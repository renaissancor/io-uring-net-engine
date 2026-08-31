#pragma once
// thread.h

#include "check.h"

#include <cerrno>
#include <cstdint>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace lnx {

class thread {
private:
    pthread_t _tid;
    bool _joinable;

public:
    inline thread() noexcept : _joinable(false) {}

    inline thread(void* (*fn)(void*), void* arg) noexcept {
        int rc = pthread_create(&_tid, nullptr, fn, arg);
        LNX_CHECK(rc == 0);
        _joinable = (rc == 0);
    }

    inline ~thread() noexcept {
        LNX_CHECK(!_joinable);
    }

    thread(const thread&)            = delete;
    thread& operator=(const thread&) = delete;

    inline thread(thread&& other) noexcept
        : _tid(other._tid), _joinable(other._joinable) {
        other._joinable = false;
    }

    inline thread& operator=(thread&& other) noexcept {
        if (this != &other) {
            LNX_CHECK(!_joinable);

            _tid      = other._tid;
            _joinable = other._joinable;

            other._joinable = false;
        }

        return *this;
    }

    inline void join() noexcept {
        LNX_CHECK(_joinable);
        int rc = pthread_join(_tid, nullptr);
        LNX_CHECK(rc == 0);
        _joinable = false;
    }

    inline void detach() noexcept {
        LNX_CHECK(_joinable);
        int rc = pthread_detach(_tid);
        LNX_CHECK(rc == 0);
        _joinable = false;
    }

    inline bool joinable() const noexcept {
        return _joinable;
    }

    inline pthread_t native_handle() const noexcept {
        return _tid;
    }
};

namespace this_thread {

inline void yield() noexcept {
    sched_yield();
}

inline void sleep_for_ns(int64_t ns) noexcept {
    timespec ts;
    ts.tv_sec  = ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;

    int rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
    LNX_CHECK(rc == 0 || rc == EINTR);
}

inline pthread_t id() noexcept {
    return pthread_self();
}

inline int kernel_tid() noexcept {
    return static_cast<int>(syscall(SYS_gettid));
}

} // namespace this_thread

} // namespace lnx
