#include "diagnostic/profiler_scope.h"

#include <cassert>
#include <ctime>
#include <fstream>
#include <limits>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

#include <fmt/core.h>

namespace profiler {
namespace {

constexpr const char* UNIT_SUFFIX[] = {"ns", "us", "ms", "s"};
constexpr long double UNIT_DIVISOR[] = {
    1.0L,
    1'000.0L,
    1'000'000.0L,
    1'000'000'000.0L,
};

const char* unit_suffix(time_unit unit) noexcept {
    return UNIT_SUFFIX[static_cast<std::uint8_t>(unit)];
}

long double unit_divisor(time_unit unit) noexcept {
    return UNIT_DIVISOR[static_cast<std::uint8_t>(unit)];
}

long double convert_ns(std::uint64_t ns, time_unit unit) noexcept {
    return static_cast<long double>(ns) / unit_divisor(unit);
}

pid_t current_tid() noexcept {
#if defined(SYS_gettid)
    return static_cast<pid_t>(::syscall(SYS_gettid));
#else
    return ::getpid();
#endif
}

}  // namespace

std::uint64_t now_ns() noexcept {
    timespec ts{};
    const int rc = ::clock_gettime(CLOCK_MONOTONIC, &ts);
    assert(rc == 0);
    (void)rc;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<std::uint64_t>(ts.tv_nsec);
}

manager::manager() noexcept
    : _records(), _tid(current_tid()) {}

manager& manager::instance() noexcept {
    thread_local manager instance;
    return instance;
}

void manager::add(const char* scope_name, std::uint64_t enter_ns,
                  std::uint64_t leave_ns) noexcept {
    assert(scope_name != nullptr);
    _records[scope_name].push_back(record{enter_ns, leave_ns});
}

summary_data manager::summarize(const record_list& records) const noexcept {
    summary_data summary{};
    summary.call_count = records.size();

    if (records.empty()) {
        summary.min_ns = 0;
        return summary;
    }

    summary.min_ns = std::numeric_limits<std::uint64_t>::max();
    for (const record& rec : records) {
        const std::uint64_t elapsed = rec.leave_ns - rec.enter_ns;
        summary.total_ns += elapsed;
        if (elapsed < summary.min_ns) summary.min_ns = elapsed;
        if (elapsed > summary.max_ns) summary.max_ns = elapsed;
    }

    return summary;
}

void manager::print_ticks() const {
    fmt::print("----------------------------------\n");
    for (auto it = _records.begin(); it != _records.end(); ++it) {
        const record_list& records = it->second;
        if (records.empty()) continue;

        const summary_data summary = summarize(records);
        fmt::print("Scope {} Calls : {}\n", it->first, summary.call_count);
        fmt::print("Total ns     : {}\n", summary.total_ns);
        fmt::print("Min ns       : {}\n", summary.min_ns);
        fmt::print("Max ns       : {}\n", summary.max_ns);
        fmt::print("----------------------------------\n");
    }
}

void manager::print_times(time_unit unit) const {
    const char* suffix = unit_suffix(unit);

    fmt::print("----------------------------------\n");
    for (auto it = _records.begin(); it != _records.end(); ++it) {
        const record_list& records = it->second;
        if (records.empty()) continue;

        const summary_data summary = summarize(records);
        const long double total = convert_ns(summary.total_ns, unit);
        const long double avg = total / static_cast<long double>(summary.call_count);
        const long double min = convert_ns(summary.min_ns, unit);
        const long double max = convert_ns(summary.max_ns, unit);

        fmt::print("Scope {} Calls : {}\n", it->first, summary.call_count);
        fmt::print("Total Time   : {:16.4Lf} {}\n", total, suffix);
        fmt::print("Average Time : {:16.4Lf} {}\n", avg, suffix);
        fmt::print("Min Time     : {:16.4Lf} {}\n", min, suffix);
        fmt::print("Max Time     : {:16.4Lf} {}\n", max, suffix);
        fmt::print("----------------------------------\n");
    }
}

void manager::save_txt(std::string_view path, time_unit unit) const {
    std::ofstream file{std::string{path}};
    if (!file) return;

    const char* suffix = unit_suffix(unit);
    file << "----------------------------------\n";

    for (auto it = _records.begin(); it != _records.end(); ++it) {
        const record_list& records = it->second;
        if (records.empty()) continue;

        const summary_data summary = summarize(records);
        const long double total = convert_ns(summary.total_ns, unit);
        const long double avg = total / static_cast<long double>(summary.call_count);
        const long double min = convert_ns(summary.min_ns, unit);
        const long double max = convert_ns(summary.max_ns, unit);

        file << fmt::format("Scope {} Calls : {}\n", it->first, summary.call_count);
        file << fmt::format("Total Time   : {:16.4Lf} {}\n", total, suffix);
        file << fmt::format("Average Time : {:16.4Lf} {}\n", avg, suffix);
        file << fmt::format("Min Time     : {:16.4Lf} {}\n", min, suffix);
        file << fmt::format("Max Time     : {:16.4Lf} {}\n", max, suffix);
        file << "----------------------------------\n";
    }
}

void manager::save_csv(std::string_view path, time_unit unit) const {
    std::ofstream file{std::string{path}};
    if (!file) return;

    const char* suffix = unit_suffix(unit);
    file << fmt::format(
        "Scope Name,Call Count,Total Time ({}),Average Time ({}),Min Time ({}),Max Time ({})\n",
        suffix, suffix, suffix, suffix);

    for (auto it = _records.begin(); it != _records.end(); ++it) {
        const record_list& records = it->second;
        if (records.empty()) continue;

        const summary_data summary = summarize(records);
        const long double total = convert_ns(summary.total_ns, unit);
        const long double avg = total / static_cast<long double>(summary.call_count);
        const long double min = convert_ns(summary.min_ns, unit);
        const long double max = convert_ns(summary.max_ns, unit);

        file << fmt::format("{},{},{:.4Lf},{:.4Lf},{:.4Lf},{:.4Lf}\n",
                            it->first, summary.call_count, total, avg, min, max);
    }
}

void manager::save_func_csv(std::string_view path) const {
    std::ofstream file{std::string{path}};
    if (!file) return;

    file << "Scope Name,Call Count,Total ns,Min ns,Max ns\n";

    for (auto it = _records.begin(); it != _records.end(); ++it) {
        const record_list& records = it->second;
        if (records.empty()) continue;

        const summary_data summary = summarize(records);
        file << fmt::format("{},{},{},{},{}\n",
                            it->first, summary.call_count, summary.total_ns,
                            summary.min_ns, summary.max_ns);
    }
}

void manager::clear() noexcept {
    _records.clear();
}

const record_list* manager::records_for(const char* scope_name) const noexcept {
    assert(scope_name != nullptr);
    auto it = _records.find(scope_name);
    if (it == _records.end()) return nullptr;
    return &it->second;
}

scope::scope(const char* scope_name) noexcept
    : _scope_name(scope_name), _enter_ns(now_ns()), _stopped(false) {
    assert(scope_name != nullptr);
}

scope::~scope() noexcept {
    stop();
}

void scope::stop() noexcept {
    if (_stopped) return;
    _stopped = true;

    manager::instance().add(_scope_name, _enter_ns, now_ns());
}

}  // namespace profiler
