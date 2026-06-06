#pragma once
// static_vector.h
//
// Inline fixed-CAPACITY, variable-SIZE vector. N slots live inside the
// object (no heap); size() grows 0..N as emplace_back placement-news a T
// into the next free slot. Carries non-default-constructible AND
// non-movable, address-pinned types (e.g. app::handle_worker) — the reason
// it exists over std::array / std::vector / sds::malloc_vector.
//
// NOT thread-safe. Container is non-copyable AND non-movable (elements may
// be address-pinned). Overflow / out-of-range access trap via LNX_CHECK:
// capacity is a contract, not a negotiation.
//
// Full design, rationale, and test plan: wiki/sds/static_vector.md

#include <new>          // placement new
#include <utility>      // std::forward

#include "../check.h"
#include "../types.h"

namespace sds {

template <typename T, usize N>
class static_vector {
    static_assert(N >= 1, "static_vector capacity must be >= 1");

    // Union storage gives us T's alignment for free and lets us leave the
    // slots uninitialized — no std::launder, no manual alignas. Element
    // lifetimes are managed by hand: emplace_back constructs, clear/pop_back
    // and the dtor destroy.
    union {
        T _slot[N];
    };
    usize _size = 0;

public:
    // Empty mem-init list intentionally leaves _slot uninitialized.
    static_vector() noexcept {}
    ~static_vector() noexcept { clear(); }

    static_vector(const static_vector&)            = delete;
    static_vector& operator=(const static_vector&) = delete;
    static_vector(static_vector&&)                 = delete;
    static_vector& operator=(static_vector&&)      = delete;

    static constexpr usize capacity() noexcept { return N; }
    usize size()  const noexcept { return _size; }
    bool  empty() const noexcept { return _size == 0; }
    bool  full()  const noexcept { return _size == N; }

    // Construct a T in-place at the next free slot, forwarding args to its
    // ctor. Traps on overflow. Returns a reference to the new element.
    template <typename... Args>
    T& emplace_back(Args&&... args) noexcept {
        LNX_CHECK(_size < N);
        T* p = ::new (&_slot[_size]) T(std::forward<Args>(args)...);
        ++_size;
        return *p;
    }

    // Destroy the last element. Traps on empty.
    void pop_back() noexcept {
        LNX_CHECK(_size > 0);
        --_size;
        _slot[_size].~T();
    }

    // Destroy all live elements in reverse construction order, reset to 0.
    void clear() noexcept {
        while (_size > 0) {
            --_size;
            _slot[_size].~T();
        }
    }

    T&       operator[](usize i)       noexcept { LNX_CHECK(i < _size); return _slot[i]; }
    const T& operator[](usize i) const noexcept { LNX_CHECK(i < _size); return _slot[i]; }

    T&       front()       noexcept { LNX_CHECK(_size > 0); return _slot[0]; }
    const T& front() const noexcept { LNX_CHECK(_size > 0); return _slot[0]; }
    T&       back()        noexcept { LNX_CHECK(_size > 0); return _slot[_size - 1]; }
    const T& back()  const noexcept { LNX_CHECK(_size > 0); return _slot[_size - 1]; }

    T*       data()       noexcept { return _slot; }
    const T* data() const noexcept { return _slot; }

    // Iteration over the live prefix [0, _size).
    T*       begin()       noexcept { return _slot; }
    T*       end()         noexcept { return _slot + _size; }
    const T* begin() const noexcept { return _slot; }
    const T* end()   const noexcept { return _slot + _size; }
};

}  // namespace sds
