#pragma once
// spsc_queue.h
//
// Single-producer / single-consumer lock-free FIFO over a fixed-size ring.
// Used as the worker-mesh and worker↔aux inbox primitive (see
// inter-thread-comms-spsc-mesh-pattern wiki).
//
// Contract: AT MOST one thread calls push / try_push, AT MOST one thread
// calls try_pop. Violating this is undefined behaviour — there is no
// runtime check. The type name is the warning.

#include <type_traits>

#include "../check.h"
#include "../sync/atomic.h"
#include "../types.h"

namespace sds {

template <typename T, usize N>
class spsc_queue {
    static_assert(N >= 2, "spsc_queue capacity must be >= 2");
    static_assert((N & (N - 1)) == 0, "spsc_queue capacity must be power of 2");
    static_assert(std::is_trivially_copyable_v<T>,
                  "spsc_queue<T>: T must be trivially copyable (messages are byte-copied)");

    // Monotonic counters. Indices into slots_ are `counter & (N - 1)`.
    // i64 internally; never wraps in practice (~292 years at 1B ops/s).
    // head_: producer writes, consumer reads (acquire to see slot writes).
    // tail_: consumer writes, producer reads (acquire to see slot reads).
    lnx::cache_aligned<lnx::atomic64> head_;
    lnx::cache_aligned<lnx::atomic64> tail_;
    alignas(lnx::CACHE_LINE_SIZE) T slots_[N];

public:
    inline spsc_queue() noexcept : head_(0), tail_(0) {}
    inline ~spsc_queue() = default;

    spsc_queue(const spsc_queue&)            = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;
    spsc_queue(spsc_queue&&)                 = delete;
    spsc_queue& operator=(spsc_queue&&)      = delete;

    static constexpr usize capacity() noexcept { return N; }

    // Producer side. try_push returns false on full; push traps on full.
    inline bool try_push(const T& v) noexcept {
        const i64 h = head_->load_relaxed();
        const i64 t = tail_->load_acquire();
        if (static_cast<u64>(h - t) == N) return false;
        slots_[static_cast<u64>(h) & (N - 1)] = v;
        head_->store_release(h + 1);
        return true;
    }
    inline void push(const T& v) noexcept {
        LNX_CHECK(try_push(v));
    }

    // Consumer side. No assert-on-empty variant — empty is the normal
    // polling result, not an error.
    inline bool try_pop(T& out) noexcept {
        const i64 t = tail_->load_relaxed();
        const i64 h = head_->load_acquire();
        if (h == t) return false;
        out = slots_[static_cast<u64>(t) & (N - 1)];
        tail_->store_release(t + 1);
        return true;
    }

    // Snapshot observers. Both atomics are loaded relaxed; the returned
    // value may already be stale by the time the caller branches on it.
    // Use for metrics and wake-decision heuristics, not for correctness
    // gating.
    inline u64 size() const noexcept {
        const i64 h = head_->load_relaxed();
        const i64 t = tail_->load_relaxed();
        return static_cast<u64>(h - t);
    }
    inline bool empty() const noexcept { return size() == 0; }
    inline bool full()  const noexcept { return size() == N; }
};

}  // namespace sds
