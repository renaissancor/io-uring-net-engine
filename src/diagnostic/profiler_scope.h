#pragma once
// profiler_scope.h

#include "sds/cstr_hash_map.h"
#include "sds/malloc_vector.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <sys/types.h>

namespace profiler {

enum class time_unit : std::uint8_t {
    nanosec,
    microsec,
    millisec,
    sec,
};

struct record {
    std::uint64_t enter_ns = 0;
    std::uint64_t leave_ns = 0;
};

struct summary_data {
    std::uint64_t total_ns = 0;
    std::uint64_t min_ns = UINT64_MAX;
    std::uint64_t max_ns = 0;
    std::size_t   call_count = 0;
};

using record_list = sds::malloc_vector<record>;

std::uint64_t now_ns() noexcept;

class manager {
private:
    sds::cstr_hash_map<record_list> _records;
    pid_t                           _tid;

    manager() noexcept;
    ~manager() = default;

public:
    manager(const manager&)            = delete;
    manager& operator=(const manager&) = delete;
    manager(manager&&)                 = delete;
    manager& operator=(manager&&)      = delete;

    static manager& instance() noexcept;

    void add(const char* scope_name, std::uint64_t enter_ns,
             std::uint64_t leave_ns) noexcept;

    summary_data summarize(const record_list& records) const noexcept;

    void print_ticks() const;
    void print_times(time_unit unit = time_unit::microsec) const;
    void save_txt(std::string_view path, time_unit unit = time_unit::microsec) const;
    void save_csv(std::string_view path, time_unit unit = time_unit::microsec) const;
    void save_func_csv(std::string_view path) const;

    void clear() noexcept;

    pid_t thread_id() const noexcept { return _tid; }
    std::size_t scope_count() const noexcept { return _records.size(); }
    const record_list* records_for(const char* scope_name) const noexcept;
};

class scope {
private:
    const char*   _scope_name;
    std::uint64_t _enter_ns;
    bool          _stopped;

public:
    explicit scope(const char* scope_name) noexcept;
    ~scope() noexcept;

    scope(const scope&)            = delete;
    scope& operator=(const scope&) = delete;
    scope(scope&&)                 = delete;
    scope& operator=(scope&&)      = delete;

    void stop() noexcept;
};

}  // namespace profiler
