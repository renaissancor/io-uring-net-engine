#pragma once
// atomic.h

#include <cstddef>
#include <cstdint>

namespace lnx {

inline constexpr std::size_t CACHE_LINE_SIZE = 64;

template <typename T>
struct cache_aligned {
    alignas(CACHE_LINE_SIZE) T value;

    template <typename... Args>
    explicit inline cache_aligned(Args&&... args) noexcept
        : value(static_cast<Args&&>(args)...) {}

    inline T* operator->() noexcept { return &value; }
    inline const T* operator->() const noexcept { return &value; }
    inline T& get() noexcept { return value; }
    inline const T& get() const noexcept { return value; }
};

class atomic32 {
private:
    int32_t _value;

public:
    explicit inline atomic32(int32_t val = 0) noexcept : _value(val) {}
    inline ~atomic32() = default;

    atomic32(const atomic32&)            = delete;
    atomic32& operator=(const atomic32&) = delete;
    atomic32(atomic32&&)                 = delete;
    atomic32& operator=(atomic32&&)      = delete;

    inline int32_t load() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_SEQ_CST);
    }
    inline void store(int32_t v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_SEQ_CST);
    }

    inline int32_t load_relaxed() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_RELAXED);
    }
    inline int32_t load_acquire() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_ACQUIRE);
    }

    inline void store_relaxed(int32_t v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_RELAXED);
    }
    inline void store_release(int32_t v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_RELEASE);
    }

    inline int32_t fetch_add(int32_t val) noexcept {
        return __atomic_fetch_add(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t fetch_sub(int32_t val) noexcept {
        return __atomic_fetch_sub(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t fetch_and(int32_t val) noexcept {
        return __atomic_fetch_and(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t fetch_or(int32_t val) noexcept {
        return __atomic_fetch_or(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t fetch_xor(int32_t val) noexcept {
        return __atomic_fetch_xor(&_value, val, __ATOMIC_SEQ_CST);
    }

    inline int32_t increment() noexcept {
        return __atomic_add_fetch(&_value, 1, __ATOMIC_SEQ_CST);
    }
    inline int32_t decrement() noexcept {
        return __atomic_sub_fetch(&_value, 1, __ATOMIC_SEQ_CST);
    }
    inline int32_t exchange(int32_t val) noexcept {
        return __atomic_exchange_n(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t compare_exchange(int32_t desired, int32_t expected) noexcept {
        int32_t observed = expected;
        __atomic_compare_exchange_n(
            &_value, &observed, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return observed;
    }

    inline atomic32& operator=(int32_t val) noexcept {
        store(val);
        return *this;
    }

    inline int32_t operator++() noexcept { return increment(); }
    inline int32_t operator--() noexcept { return decrement(); }
    inline int32_t operator++(int) noexcept {
        return __atomic_fetch_add(&_value, 1, __ATOMIC_SEQ_CST);
    }
    inline int32_t operator--(int) noexcept {
        return __atomic_fetch_sub(&_value, 1, __ATOMIC_SEQ_CST);
    }
    inline int32_t operator+=(int32_t val) noexcept {
        return __atomic_add_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t operator-=(int32_t val) noexcept {
        return __atomic_sub_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t operator&=(int32_t val) noexcept {
        return __atomic_and_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t operator|=(int32_t val) noexcept {
        return __atomic_or_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int32_t operator^=(int32_t val) noexcept {
        return __atomic_xor_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
};

class atomic64 {
private:
    int64_t _value;

public:
    explicit inline atomic64(int64_t val = 0) noexcept : _value(val) {}
    inline ~atomic64() = default;

    atomic64(const atomic64&)            = delete;
    atomic64& operator=(const atomic64&) = delete;
    atomic64(atomic64&&)                 = delete;
    atomic64& operator=(atomic64&&)      = delete;

    inline int64_t load() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_SEQ_CST);
    }
    inline void store(int64_t v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_SEQ_CST);
    }

    inline int64_t load_relaxed() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_RELAXED);
    }
    inline int64_t load_acquire() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_ACQUIRE);
    }

    inline void store_relaxed(int64_t v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_RELAXED);
    }
    inline void store_release(int64_t v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_RELEASE);
    }

    inline int64_t fetch_add(int64_t val) noexcept {
        return __atomic_fetch_add(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t fetch_sub(int64_t val) noexcept {
        return __atomic_fetch_sub(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t fetch_and(int64_t val) noexcept {
        return __atomic_fetch_and(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t fetch_or(int64_t val) noexcept {
        return __atomic_fetch_or(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t fetch_xor(int64_t val) noexcept {
        return __atomic_fetch_xor(&_value, val, __ATOMIC_SEQ_CST);
    }

    inline int64_t increment() noexcept {
        return __atomic_add_fetch(&_value, 1, __ATOMIC_SEQ_CST);
    }
    inline int64_t decrement() noexcept {
        return __atomic_sub_fetch(&_value, 1, __ATOMIC_SEQ_CST);
    }
    inline int64_t exchange(int64_t val) noexcept {
        return __atomic_exchange_n(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t compare_exchange(int64_t desired, int64_t expected) noexcept {
        int64_t observed = expected;
        __atomic_compare_exchange_n(
            &_value, &observed, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return observed;
    }

    inline atomic64& operator=(int64_t val) noexcept {
        store(val);
        return *this;
    }

    inline int64_t operator++() noexcept { return increment(); }
    inline int64_t operator--() noexcept { return decrement(); }
    inline int64_t operator++(int) noexcept {
        return __atomic_fetch_add(&_value, 1, __ATOMIC_SEQ_CST);
    }
    inline int64_t operator--(int) noexcept {
        return __atomic_fetch_sub(&_value, 1, __ATOMIC_SEQ_CST);
    }
    inline int64_t operator+=(int64_t val) noexcept {
        return __atomic_add_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t operator-=(int64_t val) noexcept {
        return __atomic_sub_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t operator&=(int64_t val) noexcept {
        return __atomic_and_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t operator|=(int64_t val) noexcept {
        return __atomic_or_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline int64_t operator^=(int64_t val) noexcept {
        return __atomic_xor_fetch(&_value, val, __ATOMIC_SEQ_CST);
    }
};

class atomic_ptr {
private:
    void* _value;

public:
    explicit inline atomic_ptr(void* val = nullptr) noexcept : _value(val) {}
    inline ~atomic_ptr() = default;

    atomic_ptr(const atomic_ptr&)            = delete;
    atomic_ptr& operator=(const atomic_ptr&) = delete;
    atomic_ptr(atomic_ptr&&)                 = delete;
    atomic_ptr& operator=(atomic_ptr&&)      = delete;

    inline void* load() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_SEQ_CST);
    }
    inline void store(void* v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_SEQ_CST);
    }

    inline void* load_relaxed() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_RELAXED);
    }
    inline void* load_acquire() const noexcept {
        return __atomic_load_n(&_value, __ATOMIC_ACQUIRE);
    }

    inline void store_relaxed(void* v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_RELAXED);
    }
    inline void store_release(void* v) noexcept {
        __atomic_store_n(&_value, v, __ATOMIC_RELEASE);
    }

    inline void* exchange(void* val) noexcept {
        return __atomic_exchange_n(&_value, val, __ATOMIC_SEQ_CST);
    }
    inline void* compare_exchange(void* desired, void* expected) noexcept {
        void* observed = expected;
        __atomic_compare_exchange_n(
            &_value, &observed, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return observed;
    }

    inline atomic_ptr& operator=(void* val) noexcept {
        store(val);
        return *this;
    }
};

inline void memory_barrier() noexcept {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static_assert(sizeof(atomic32) == sizeof(int32_t));
static_assert(sizeof(atomic64) == sizeof(int64_t));
static_assert(sizeof(atomic_ptr) == sizeof(void*));
static_assert(alignof(cache_aligned<atomic32>) == CACHE_LINE_SIZE);

}  // namespace lnx
