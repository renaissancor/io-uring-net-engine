#pragma once
// sds/ring_buffer.h
//
// One byte-oriented ring, two synchronization policies — the unified transport
// primitive. TCP recv/send buffers (io_uring fills them) AND cross-thread mesh
// edges are the SAME structure, differing only in how the cursors are published:
//
//   ring_buffer<N, ring_sync::single>  one owning thread (producer == consumer).
//                                      recv/send path: io_uring writes the
//                                      enqueue region, the worker parses frames
//                                      off the dequeue region. Plain cursors.
//   ring_buffer<N, ring_sync::spsc>    one producer thread, one consumer thread.
//                                      mesh edge: a peer serializes a frame in,
//                                      the owner parses it out. atomic64
//                                      acquire/release; the two cursors sit on
//                                      separate cache lines (no false sharing).
//
// The byte API and the zero-copy hooks (direct_*_size + commit_*) are identical
// across both policies, so every consumer runs ONE "byte ring -> parse
// length-prefixed frames" loop regardless of whether the bytes arrived off a
// socket or off a peer thread. This is the SPSC-mesh decision applied uniformly;
// see wiki inter-thread-comms-spsc-mesh-pattern. Supersedes the earlier
// slot-typed spsc_queue<T,N> (retired) and the heap/auto-resize ring_buffer.
//
// Cursor model: enqueue_ = producer cursor (advances on enqueue), dequeue_ =
// consumer cursor (advances on dequeue). used = enqueue_ - dequeue_ ; free =
// N - used. Monotonic i64 counters (never wrap in practice); storage offset is
// `cursor & (N-1)`. Because fullness is the counter delta, ALL N bytes are
// usable — no wasted separator slot.
//
// Bounded, not growable: enqueue past capacity is REFUSED (returns 0), never
// resized. A ring bigger than a frame is the caller's sizing job; overflow is a
// backpressure signal (drop-and-close), not a grow trigger.
//
// Contract (spsc policy): AT MOST one thread enqueues, AT MOST one thread
// dequeues. Violating it is UB — no runtime guard. The policy name is the
// warning.

#include "../sync/atomic.h"
#include "../types.h"

namespace sds {

// --- synchronization policies -------------------------------------------
// A policy owns the cursor pair and exposes it by ROLE INTENT (producer view /
// consumer view), so the correct memory ordering lives here once and the ring
// body stays policy-agnostic. `p_*` = producer side, `c_*` = consumer side;
// `*_own` reads the cursor this side writes, `*_peer` reads the other side's
// (acquire, under spsc) to observe its published progress.
struct ring_sync {
    struct single {
        i64 enqueue_ = 0;
        i64 dequeue_ = 0;
        i64  p_own()  const noexcept { return enqueue_; }
        i64  p_peer() const noexcept { return dequeue_; }
        void p_publish(i64 v) noexcept { enqueue_ = v; }
        i64  c_own()  const noexcept { return dequeue_; }
        i64  c_peer() const noexcept { return enqueue_; }
        void c_publish(i64 v) noexcept { dequeue_ = v; }
    };

    struct spsc {
        lnx::cache_aligned<lnx::atomic64> enqueue_{0};   // producer-only writer
        lnx::cache_aligned<lnx::atomic64> dequeue_{0};   // consumer-only writer
        i64  p_own()  const noexcept { return enqueue_->load_relaxed(); }
        i64  p_peer() const noexcept { return dequeue_->load_acquire(); }
        void p_publish(i64 v) noexcept { enqueue_->store_release(v); }
        i64  c_own()  const noexcept { return dequeue_->load_relaxed(); }
        i64  c_peer() const noexcept { return enqueue_->load_acquire(); }
        void c_publish(i64 v) noexcept { dequeue_->store_release(v); }
    };
};

template <usize N, typename Sync = ring_sync::spsc>
class ring_buffer {
    static_assert(N >= 2, "ring_buffer capacity must be >= 2");
    static_assert((N & (N - 1)) == 0, "ring_buffer capacity must be power of 2");

    static constexpr usize MASK = N - 1;

    Sync sync_;
    alignas(lnx::CACHE_LINE_SIZE) byte buffer_[N];

    static usize umin(usize a, usize b) noexcept { return a < b ? a : b; }

    // memcpy declares src/dst __nonnull; a zero-length copy from a null pointer
    // (e.g. an empty buffer) is harmless but trips UBSan's nonnull check. Guard
    // it here so every call site can pass n == 0 freely.
    static void copy(void* dst, const void* src, usize n) noexcept {
        if (n) __builtin_memcpy(dst, src, n);
    }

public:
    ring_buffer() noexcept = default;
    ~ring_buffer()         = default;

    ring_buffer(const ring_buffer&)            = delete;
    ring_buffer& operator=(const ring_buffer&) = delete;
    ring_buffer(ring_buffer&&)                 = delete;
    ring_buffer& operator=(ring_buffer&&)      = delete;

    static constexpr usize capacity() noexcept { return N; }

    // ---- producer side --------------------------------------------------
    usize free_size() const noexcept {
        return N - static_cast<usize>(sync_.p_own() - sync_.p_peer());
    }
    bool is_full() const noexcept { return free_size() == 0; }

    // Copy-in, all-or-nothing: enqueues the whole frame or nothing (returns n
    // or 0). Whole-frame semantics keep length-prefixed framing intact — a
    // partial write would strand half a message. A frame larger than N can
    // never be enqueued (returns 0): size the ring for your max frame.
    usize enqueue(const byte* src, usize n) noexcept {
        const i64 w = sync_.p_own();
        if (static_cast<usize>(w - sync_.p_peer()) + n > N) return 0;
        const usize off   = static_cast<usize>(w) & MASK;
        const usize first = umin(n, N - off);              // contiguous run to end
        copy(buffer_ + off, src, first);
        copy(buffer_, src + first, n - first);             // wrapped remainder
        sync_.p_publish(w + static_cast<i64>(n));
        return n;
    }

    // Zero-copy enqueue (io_uring recv target). Fill up to direct_enqueue_size()
    // bytes at direct_enqueue_ptr(), then commit_enqueue(k) with what landed.
    // Size is the CONTIGUOUS run to the buffer end, so the kernel gets one flat
    // region.
    byte* direct_enqueue_ptr() noexcept {
        return buffer_ + (static_cast<usize>(sync_.p_own()) & MASK);
    }
    usize direct_enqueue_size() const noexcept {
        const i64 w = sync_.p_own();
        const usize free   = N - static_cast<usize>(w - sync_.p_peer());
        const usize to_end = N - (static_cast<usize>(w) & MASK);
        return umin(free, to_end);
    }
    void commit_enqueue(usize k) noexcept {
        sync_.p_publish(sync_.p_own() + static_cast<i64>(k));
    }

    // ---- consumer side --------------------------------------------------
    usize used_size() const noexcept {
        return static_cast<usize>(sync_.c_peer() - sync_.c_own());
    }
    bool is_empty() const noexcept { return used_size() == 0; }

    // Copy-out, advancing the dequeue cursor. Returns min(n, used_size()).
    usize dequeue(byte* dst, usize n) noexcept {
        const i64 r = sync_.c_own();
        n = umin(n, static_cast<usize>(sync_.c_peer() - r));
        const usize off   = static_cast<usize>(r) & MASK;
        const usize first = umin(n, N - off);
        copy(dst, buffer_ + off, first);
        copy(dst + first, buffer_, n - first);
        sync_.c_publish(r + static_cast<i64>(n));
        return n;
    }

    // Copy-out WITHOUT advancing — probe a frame's length prefix before you
    // commit to consuming the whole frame. Reassembles across the wrap.
    usize peek(byte* dst, usize n) const noexcept {
        const i64 r = sync_.c_own();
        n = umin(n, static_cast<usize>(sync_.c_peer() - r));
        const usize off   = static_cast<usize>(r) & MASK;
        const usize first = umin(n, N - off);
        copy(dst, buffer_ + off, first);
        copy(dst + first, buffer_, n - first);
        return n;
    }

    // Zero-copy dequeue (io_uring send source / in-place frame parse).
    // Contiguous run only; a frame straddling the wrap is read via dequeue()
    // into a scratch instead.
    const byte* direct_dequeue_ptr() const noexcept {
        return buffer_ + (static_cast<usize>(sync_.c_own()) & MASK);
    }
    usize direct_dequeue_size() const noexcept {
        const i64 r = sync_.c_own();
        const usize avail  = static_cast<usize>(sync_.c_peer() - r);
        const usize to_end = N - (static_cast<usize>(r) & MASK);
        return umin(avail, to_end);
    }
    void commit_dequeue(usize k) noexcept {
        sync_.c_publish(sync_.c_own() + static_cast<i64>(k));
    }
};

}  // namespace sds
