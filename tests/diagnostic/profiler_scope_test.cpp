// tests/diagnostic/profiler_scope_test.cpp
//
// Unit tests for profiler::manager and profiler::scope.

#include "diagnostic/profiler_scope.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file{path};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

std::filesystem::path temp_report_path(std::string_view filename) {
    return std::filesystem::temp_directory_path()
         / ("iouring_net_" + std::to_string(::getpid()) + "_" + std::string{filename});
}

}  // namespace

TEST_CASE("profiler_scope: explicit add and summarize", "[diagnostic][profiler]") {
    profiler::record_list records;
    records.push_back(profiler::record{100, 150});
    records.push_back(profiler::record{200, 260});
    records.push_back(profiler::record{300, 320});

    const profiler::summary_data summary = profiler::manager::instance().summarize(records);

    REQUIRE(summary.call_count == 3);
    REQUIRE(summary.total_ns == 130);
    REQUIRE(summary.min_ns == 20);
    REQUIRE(summary.max_ns == 60);
}

TEST_CASE("profiler_scope: empty summary is zeroed", "[diagnostic][profiler]") {
    const profiler::record_list records;
    const profiler::summary_data summary = profiler::manager::instance().summarize(records);

    REQUIRE(summary.call_count == 0);
    REQUIRE(summary.total_ns == 0);
    REQUIRE(summary.min_ns == 0);
    REQUIRE(summary.max_ns == 0);
}

TEST_CASE("profiler_scope: RAII scope records elapsed interval",
          "[diagnostic][profiler]") {
    auto& manager = profiler::manager::instance();
    manager.clear();

    {
        profiler::scope measured{"profiler_scope_raii"};
        std::this_thread::sleep_for(1ms);
    }

    const profiler::record_list* records =
        manager.records_for("profiler_scope_raii");

    REQUIRE(records != nullptr);
    REQUIRE(records->size() == 1);
    REQUIRE(records->front().leave_ns >= records->front().enter_ns);

    const profiler::summary_data summary = manager.summarize(*records);
    REQUIRE(summary.call_count == 1);
    REQUIRE(summary.total_ns > 0);
}

TEST_CASE("profiler_scope: stop is idempotent", "[diagnostic][profiler]") {
    auto& manager = profiler::manager::instance();
    manager.clear();

    {
        profiler::scope measured{"profiler_scope_stop_once"};
        measured.stop();
        measured.stop();
    }

    const profiler::record_list* records =
        manager.records_for("profiler_scope_stop_once");

    REQUIRE(records != nullptr);
    REQUIRE(records->size() == 1);
}

TEST_CASE("profiler_scope: nested scopes record independently",
          "[diagnostic][profiler]") {
    auto& manager = profiler::manager::instance();
    manager.clear();

    {
        profiler::scope outer{"profiler_scope_outer"};
        {
            profiler::scope inner{"profiler_scope_inner"};
            std::this_thread::sleep_for(1ms);
        }
    }

    const profiler::record_list* outer =
        manager.records_for("profiler_scope_outer");
    const profiler::record_list* inner =
        manager.records_for("profiler_scope_inner");

    REQUIRE(outer != nullptr);
    REQUIRE(inner != nullptr);
    REQUIRE(outer->size() == 1);
    REQUIRE(inner->size() == 1);
    REQUIRE(outer->front().enter_ns <= inner->front().enter_ns);
    REQUIRE(outer->front().leave_ns >= inner->front().leave_ns);
}

TEST_CASE("profiler_scope: thread-local managers isolate records",
          "[diagnostic][profiler]") {
    auto& main_manager = profiler::manager::instance();
    main_manager.clear();

    profiler::scope main_scope{"profiler_scope_tls"};

    std::size_t worker_count = 0;
    pid_t worker_tid = 0;
    std::thread worker{[&] {
        auto& worker_manager = profiler::manager::instance();
        worker_manager.clear();
        worker_tid = worker_manager.thread_id();

        {
            profiler::scope worker_scope{"profiler_scope_tls"};
        }

        const profiler::record_list* records =
            worker_manager.records_for("profiler_scope_tls");
        worker_count = records == nullptr ? 0 : records->size();
    }};
    worker.join();

    main_scope.stop();

    const profiler::record_list* main_records =
        main_manager.records_for("profiler_scope_tls");

    REQUIRE(main_records != nullptr);
    REQUIRE(main_records->size() == 1);
    REQUIRE(worker_count == 1);
    REQUIRE(worker_tid != 0);
    REQUIRE(worker_tid != main_manager.thread_id());
}

TEST_CASE("profiler_scope: CSV reports include header and scope row",
          "[diagnostic][profiler]") {
    auto& manager = profiler::manager::instance();
    manager.clear();

    manager.add("profiler_scope_csv", 100, 250);

    const std::filesystem::path path = temp_report_path("profiler_scope_test.csv");
    manager.save_csv(path.string(), profiler::time_unit::nanosec);

    const std::string contents = read_file(path);
    std::filesystem::remove(path);

    REQUIRE(contents.find("Scope Name,Call Count,Total Time (ns)") != std::string::npos);
    REQUIRE(contents.find("profiler_scope_csv,1,150.0000") != std::string::npos);
}

TEST_CASE("profiler_scope: raw function CSV reports nanosecond summary",
          "[diagnostic][profiler]") {
    auto& manager = profiler::manager::instance();
    manager.clear();

    manager.add("profiler_scope_func_csv", 100, 130);
    manager.add("profiler_scope_func_csv", 200, 250);

    const std::filesystem::path path = temp_report_path("profiler_scope_func_test.csv");
    manager.save_func_csv(path.string());

    const std::string contents = read_file(path);
    std::filesystem::remove(path);

    REQUIRE(contents.find("Scope Name,Call Count,Total ns,Min ns,Max ns") != std::string::npos);
    REQUIRE(contents.find("profiler_scope_func_csv,2,80,30,50") != std::string::npos);
}
