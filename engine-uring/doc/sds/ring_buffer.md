# ring_buffer — bounded byte ring, two sync policies

> **Status:** landed
> **Source:** `src/sds/ring_buffer.h`
> **Namespace:** `sds`
> **Depends:** `sync/atomic`, `types`

## Purpose

One byte-oriented ring, two synchronization policies — the unified transport
primitive. A TCP recv/send buffer (io_uring fills it) and a cross-thread mesh
edge are the **same structure**, differing only in how the cursors are
published. Every consumer therefore runs one "byte ring → parse length-prefixed
frames" loop regardless of whether the bytes arrived off a socket or off a peer
thread.

## API

```cpp
namespace sds {

// Cursor-pair policies, exposed by role intent (producer view / consumer view)
// so the correct memory ordering lives in one place and the ring body stays
// policy-agnostic.
struct ring_sync {
    struct single;   // producer == consumer (one owning thread). Plain cursors.
    struct spsc;     // one producer thread, one consumer thread. atomic64
                     // acquire/release; cursors on separate cache lines.
};

template <usize N, typename Sync = ring_sync::spsc>
class ring_buffer {
    static_assert(N >= 2, "capacity must be >= 2");
    static_assert((N & (N - 1)) == 0, "capacity must be power of 2");
public:
    ring_buffer() noexcept;
    // non-copyable, non-movable

    static constexpr usize capacity() noexcept;   // == N

    // ---- producer side ----
    usize free_size() const noexcept;
    bool  is_full()   const noexcept;
    // All-or-nothing: enqueues the whole frame or nothing (returns n or 0).
    // A frame larger than N can never be enqueued (returns 0).
    usize enqueue(const byte* src, usize n) noexcept;
    // Gather enqueue: publishes `a` immediately followed by `b` as ONE
    // all-or-nothing frame (returns na + nb, or 0 if the PAIR does not fit).
    // Lets a length-prefixed frame go in without staging it in a scratch
    // buffer: pass the header as `a` and the body as `b`. Both regions land
    // before the single cursor publish, so a reader sees the whole frame or
    // none of it.
    usize enqueue2(const byte* a, usize na, const byte* b, usize nb) noexcept;
    // Zero-copy enqueue (io_uring recv target): fill up to direct_enqueue_size()
    // bytes at direct_enqueue_ptr(), then commit_enqueue(k). Size is the
    // CONTIGUOUS run to the buffer end (one flat region for the kernel).
    byte* direct_enqueue_ptr() noexcept;
    usize direct_enqueue_size() const noexcept;
    void  commit_enqueue(usize k) noexcept;

    // ---- consumer side ----
    usize used_size() const noexcept;
    bool  is_empty()  const noexcept;
    usize dequeue(byte* dst, usize n) noexcept;        // returns min(n, used_size())
    usize peek(byte* dst, usize n) const noexcept;     // copy-out WITHOUT advancing
    const byte* direct_dequeue_ptr() const noexcept;   // zero-copy send source
    usize direct_dequeue_size() const noexcept;        // contiguous run only
    void  commit_dequeue(usize k) noexcept;
};

}  // namespace sds
```

## Invariants

- **Bounded, never grows.** Enqueue past capacity is refused (returns 0), never
  resized — overflow is a backpressure signal (drop-and-close), not a grow
  trigger. Sizing the ring larger than the max frame is the caller's job.
- **All N bytes usable.** Fullness is a monotonic-counter delta
  (`used = enqueue_ - dequeue_`), so there is no wasted separator slot.
- **`single` policy:** exactly one thread both produces and consumes (recv/send
  path: io_uring writes the enqueue region, the worker parses the dequeue
  region). Plain cursors, no atomics.
- **`spsc` policy:** at most one thread enqueues, at most one dequeues.
  `atomic64` acquire/release; the two cursors sit on separate cache lines (no
  false sharing). Violating the one-producer/one-consumer contract is **UB — no
  runtime guard**; the policy name is the warning.
- **Power-of-two N** (`static_assert`): storage offset is `cursor & (N-1)`.
- Non-copyable and non-movable → callers store it in pinned storage
  (`sds::static_vector` / fixed inline arrays), never a movable container.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `enqueue` n > `free_size()` | returns `0`, writes nothing (all-or-nothing) |
| `enqueue` frame larger than N | always returns `0` |
| `enqueue2` `na + nb` > `free_size()` | returns `0`, writes nothing — the PAIR is atomic, so a fitting header alone must not land |
| `enqueue2` `nb == 0` with null `b` | legal — equivalent to `enqueue(a, na)` |
| `dequeue` / `peek` n > `used_size()` | clamped to `used_size()` |
| zero-length copy (`n == 0`, possibly null src) | guarded — no `memcpy` call (UBSan-clean) |
| frame straddling the wrap via `direct_dequeue_ptr` | contiguous run only; read the split frame via `dequeue()` into scratch instead |

## Notes

- Storage is `alignas(CACHE_LINE) byte buffer_[N]`; the policy object owns the
  cursor pair. Cursors are monotonic `i64` counters (never wrap in practice);
  the physical offset is `cursor & (N-1)`.
- The byte API and the zero-copy hooks (`direct_*` + `commit_*`) are identical
  across both policies — that is the whole point: one framing loop, two
  transports.
- `enqueue` and `enqueue2` share a private `write_at(cursor, src, n)` helper
  that splits a region across the wrap **without** publishing; each public entry
  point advances the producer cursor exactly once, after every region has
  landed. That single publish is what makes a multi-region write atomic.
- `sds::pipe<N>` is the alias for the `spsc` policy, used for thread-mesh
  edges — see `doc/sds/pipe.md` for why the mesh role is named separately.
- Supersedes the retired growable `char*` `RingBuffer` port and the earlier
  slot-typed `spsc_queue<T,N>`.

## Test plan

`tests/sds/ring_buffer_test.cpp`:
- deterministic `single`-policy cases: starts empty; enqueue/dequeue round-trip;
  whole-N usable then all-or-nothing refusal; frame > N refused whole;
  peek-without-consume; wrap reassembly; `direct_*` contiguous-region sizing.
- `enqueue2` cases: joins two regions into one frame; all-or-nothing on the
  PAIR; empty second region equals plain `enqueue`; wrap falling inside the
  FIRST region; wrap falling inside the SECOND region; a sweep asserting the
  result matches a pre-assembled `enqueue` at every wrap offset.
- `spsc`-policy multi-threaded FIFO stress (ported from the retired
  `spsc_queue` suite), TSan-checked via the `tsan` preset.

## Done when

- [x] Builds on `default` and `floor` presets
- [x] Tests pass under ASan+UBSan; spsc stress clean under TSan
- [x] This spec matches the built API

## Rationale

- `.omc/wiki/inter-thread-comms-spsc-mesh-pattern.md` — SPSC-everywhere mesh decision
- `../design-notes/2026-05-19-chat-server-data-layout.md` — recv/send ring sizing
- Supersedes the pre-`810ae50` growable-ring design still described in git history.
