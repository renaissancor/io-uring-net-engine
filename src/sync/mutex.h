#pragma once
// mutex.h

#include <cerrno>
#include <pthread.h>

// Debug-only misuse trap. Catches double-unlock, use-after-destroy, init
// failure, etc. Zero cost in release builds. No <cassert>, no std::abort.
#ifndef NDEBUG
    #define LNX_DCHECK(cond) do { if (!(cond)) __builtin_trap(); } while (0)
#else
    #define LNX_DCHECK(cond) ((void)0)
#endif

namespace lnx {

class mutex {
private:
    pthread_mutex_t _mtx;

public:
    inline mutex() noexcept {
        LNX_DCHECK(pthread_mutex_init(&_mtx, nullptr) == 0);
    }
    inline ~mutex() noexcept {
        LNX_DCHECK(pthread_mutex_destroy(&_mtx) == 0);
    }

    mutex(const mutex&)            = delete;
    mutex& operator=(const mutex&) = delete;
    mutex(mutex&&)                 = delete;
    mutex& operator=(mutex&&)      = delete;

    inline void lock() noexcept {
        LNX_DCHECK(pthread_mutex_lock(&_mtx) == 0);
    }
    inline bool try_lock() noexcept {
        int rc = pthread_mutex_trylock(&_mtx);
        LNX_DCHECK(rc == 0 || rc == EBUSY);
        return rc == 0;
    }
    inline void unlock() noexcept {
        LNX_DCHECK(pthread_mutex_unlock(&_mtx) == 0);
    }
};

class shared_mutex {
private:
    pthread_rwlock_t _mtx;

public:
    inline shared_mutex() noexcept {
        LNX_DCHECK(pthread_rwlock_init(&_mtx, nullptr) == 0);
    }
    inline ~shared_mutex() noexcept {
        LNX_DCHECK(pthread_rwlock_destroy(&_mtx) == 0);
    }

    shared_mutex(const shared_mutex&)            = delete;
    shared_mutex& operator=(const shared_mutex&) = delete;
    shared_mutex(shared_mutex&&)                 = delete;
    shared_mutex& operator=(shared_mutex&&)      = delete;

    inline void lock_exclusive() noexcept {
        LNX_DCHECK(pthread_rwlock_wrlock(&_mtx) == 0);
    }
    inline bool try_lock_exclusive() noexcept {
        int rc = pthread_rwlock_trywrlock(&_mtx);
        LNX_DCHECK(rc == 0 || rc == EBUSY);
        return rc == 0;
    }
    inline void unlock_exclusive() noexcept {
        LNX_DCHECK(pthread_rwlock_unlock(&_mtx) == 0);
    }

    inline void lock_shared() noexcept {
        LNX_DCHECK(pthread_rwlock_rdlock(&_mtx) == 0);
    }
    inline bool try_lock_shared() noexcept {
        int rc = pthread_rwlock_tryrdlock(&_mtx);
        LNX_DCHECK(rc == 0 || rc == EBUSY);
        return rc == 0;
    }
    inline void unlock_shared() noexcept {
        LNX_DCHECK(pthread_rwlock_unlock(&_mtx) == 0);
    }
};

class lock_guard {
private:
    mutex& _mtx;

public:
    explicit inline lock_guard(mutex& m) noexcept : _mtx(m) { _mtx.lock(); }
    inline ~lock_guard() { _mtx.unlock(); }

    lock_guard(const lock_guard&)            = delete;
    lock_guard& operator=(const lock_guard&) = delete;
    lock_guard(lock_guard&&)                 = delete;
    lock_guard& operator=(lock_guard&&)      = delete;
};

class unique_lock {
private:
    mutex* _mtx;
    bool _owns;

public:
    inline unique_lock() noexcept : _mtx(nullptr), _owns(false) {}
    explicit inline unique_lock(mutex& m) noexcept : _mtx(&m), _owns(false) { lock(); }
    inline ~unique_lock() {
        if (_owns && _mtx) {
            _mtx->unlock();
        }
    }

    unique_lock(const unique_lock&)            = delete;
    unique_lock& operator=(const unique_lock&) = delete;

    inline unique_lock(unique_lock&& other) noexcept
        : _mtx(other._mtx), _owns(other._owns) {
        other._mtx  = nullptr;
        other._owns = false;
    }

    inline unique_lock& operator=(unique_lock&& other) noexcept {
        if (this != &other) {
            if (_owns && _mtx) {
                _mtx->unlock();
            }

            _mtx  = other._mtx;
            _owns = other._owns;

            other._mtx  = nullptr;
            other._owns = false;
        }

        return *this;
    }

    inline void lock() {
        if (_mtx && !_owns) {
            _mtx->lock();
            _owns = true;
        }
    }

    inline void unlock() noexcept {
        if (_mtx && _owns) {
            _mtx->unlock();
            _owns = false;
        }
    }

    inline bool try_lock() noexcept {
        if (_mtx && !_owns && _mtx->try_lock()) {
            _owns = true;
            return true;
        }

        return false;
    }

    inline mutex* release() noexcept {
        mutex* released = _mtx;
        _mtx            = nullptr;
        _owns           = false;
        return released;
    }

    inline bool owns_lock() const noexcept { return _owns; }
};

class shared_lock_guard {
private:
    shared_mutex& _mtx;

public:
    explicit inline shared_lock_guard(shared_mutex& m) noexcept : _mtx(m) {
        _mtx.lock_shared();
    }
    inline ~shared_lock_guard() { _mtx.unlock_shared(); }

    shared_lock_guard(const shared_lock_guard&)            = delete;
    shared_lock_guard& operator=(const shared_lock_guard&) = delete;
    shared_lock_guard(shared_lock_guard&&)                 = delete;
    shared_lock_guard& operator=(shared_lock_guard&&)      = delete;
};

class exclusive_lock_guard {
private:
    shared_mutex& _mtx;

public:
    explicit inline exclusive_lock_guard(shared_mutex& m) noexcept : _mtx(m) {
        _mtx.lock_exclusive();
    }

    inline ~exclusive_lock_guard() { _mtx.unlock_exclusive(); }

    exclusive_lock_guard(const exclusive_lock_guard&)            = delete;
    exclusive_lock_guard& operator=(const exclusive_lock_guard&) = delete;
    exclusive_lock_guard(exclusive_lock_guard&&)                 = delete;
    exclusive_lock_guard& operator=(exclusive_lock_guard&&)      = delete;
};

} // namespace lnx

#undef LNX_DCHECK
