# ring_buffer — growable circular byte buffer

## Purpose

The recv-side and send-side byte buffer used by every `Session`. Decouples
io_uring recv completions from packet-framing logic (recv side) and packet
serialization from io_uring send submissions (send side).

v1 (this document): a direct snake_case port of the Windows
`RingBuffer` — `char*` storage, modulo wrap arithmetic, auto-resize on
enqueue overflow, raw enqueue/dequeue/peek byte API. **Not** SPSC,
**not** power-of-two-restricted, **not** span-shaped. The aspirational
io_uring-zero-copy redesign lives in the open-questions section.

Specializations `recv_ring_buffer` and `send_ring_buffer` (frame-aware
peek on recv side, iovec gather on send side) will be ported from
`SelectServer/FighterServer/RingBuffer.{h,cpp}` in a follow-up.

## Reference origin

- `WindowsLibrary/Library/Include/RingBuffer.h:5` — base class header.
- `WindowsLibrary/Library/Sources/RingBuffer.cpp:6` — implementation.

The Linux port:

- Snake_case rename throughout (`RingBuffer` → `ring_buffer`,
  `Enqueue` → `enqueue`, etc.).
- LLVM-style trailing-underscore fields (`_buffer` → `buffer_`).
- `int` cursors/sizes → `std::size_t` (no negative values possible,
  matches `size()`/`capacity()` conventions on Linux).
- `memcpy_s` → `std::memcpy` (Linux has no `memcpy_s`; bounds were
  already verified by surrounding code).
- `min` macro → `std::min`.
- Explicit `delete` of both copy and move ctors/assignments (raw owning
  pointer; default move would shallow-copy and double-free).
- Debug `assert()` on `move_head`/`move_tail` to catch out-of-bounds
  cursor advances at test time.

## Public API (v1, implemented)

```cpp
namespace sds {

class ring_buffer {
public:
    ring_buffer() = delete;
    explicit ring_buffer(std::size_t buffer_capacity);
    ~ring_buffer();

    // Non-copyable, non-movable (raw owning pointer)
    ring_buffer(const ring_buffer&)            = delete;
    ring_buffer& operator=(const ring_buffer&) = delete;
    ring_buffer(ring_buffer&&)                 = delete;
    ring_buffer& operator=(ring_buffer&&)      = delete;

    bool is_empty() const noexcept;
    bool is_full()  const noexcept;

    std::size_t head_index() const noexcept;
    std::size_t tail_index() const noexcept;
    std::size_t capacity()   const noexcept;
    std::size_t used_size()  const noexcept;
    std::size_t free_size()  const noexcept;   // capacity - used - 1 (separator slot)

    // Cursor manipulation (asserts on bounds violation in debug builds).
    std::size_t move_head(std::size_t offset) noexcept;
    std::size_t move_tail(std::size_t offset) noexcept;

    // Contiguous-region sizes for zero-copy peek/poke.
    // direct_enqueue_size: bytes writable starting at &buffer_[tail_] before the wrap.
    // direct_dequeue_size: bytes readable starting at &buffer_[head_] before the wrap.
    std::size_t direct_enqueue_size() const noexcept;
    std::size_t direct_dequeue_size() const noexcept;

    // Byte-copy I/O. enqueue() auto-grows on overflow.
    std::size_t enqueue(const char* src, std::size_t size) noexcept;
    std::size_t dequeue(char* dst, std::size_t size) noexcept;
    std::size_t peek(char* dst, std::size_t size) const noexcept;
};

}  // namespace sds
```

## Linux design

**Storage.** Single `char*` allocated with `new char[capacity_]`. Capacity
floor of 4 enforced in the constructor (so the `+1` separator slot is
always meaningful). `bad_alloc` is treated as fatal — caller has no path
to recover from buffer construction failure.

**Wrap arithmetic.** Modulo `% capacity_` on every cursor advance. Not the
fastest possible — power-of-two capacity plus `& mask` would save a div —
but matches the Windows source 1:1 and is correct for any capacity. See
open question #1.

**Full/empty distinction via `+1` separator slot.** `is_full()` returns
true when `(tail_ + 1) % capacity_ == head_`, so the buffer always
reserves one unused slot. Wastes one byte; gains a single-cursor full/
empty test without an extra flag. Classical tradeoff.

**Auto-resize.** `enqueue` checks `bytes > free_size()` and doubles
`capacity_` (repeatedly) until the new bytes fit. The old contents are
copied into a fresh allocation starting at index 0, with `head_ = 0` and
`tail_ = used_size()`. Cost: O(n) on overflow. Suitable for v1 because
sessions don't expect to be on the io_uring fixed-buffer hot path yet;
once they are, auto-resize must be replaced with backpressure — see open
question #2.

**Cursor safety.** `move_head` asserts `offset <= used_size()`, `move_tail`
asserts `offset <= free_size()`. With `std::size_t`, negative inputs are
impossible at the type level; oversize inputs are caught at test time.
Released-build behavior on assertion violation is UB — that's intentional
(callers are internal subsystems, not user code).

**No internal locking.** v1 is single-threaded by contract. The intended
use site is a session owned by one reactor thread, with the same thread
producing and consuming. Multi-thread access requires external
synchronization; **the SPSC / lock-free variant is out of scope for v1**.

**Zero-byte short-circuit.** `enqueue`, `dequeue`, and `peek` return 0
immediately when `bytes == 0` and skip all buffer access. The C standard
makes `memcpy(dst, nullptr, 0)` well-defined, but UBSan flags it anyway —
and callers passing `std::vector::data()` from an empty vector (e.g. the
randomized property test) hit exactly this case. Short-circuiting is
both UBSan-clean and one branch faster than the no-op memcpy path.

## Concurrency & ownership

- v1: **single-threaded.** Caller (e.g., `Session`) is responsible for
  ensuring only one thread touches a given `ring_buffer` at a time.
  Currently this is trivially satisfied — both producer and consumer for
  a session's recv and send buffers run on the session's reactor thread.
- v2 (future): a lock-free SPSC variant with `std::atomic<size_t>` cursors
  for the case where producer and consumer live on different threads
  (e.g., a worker thread completing a job and pushing into the session's
  send buffer). Migration path documented in open question #3.
- Lifetime: owned by the consumer. Currently the planned consumer is
  `Session`, which creates two `ring_buffer`s (recv + send) at
  construction and destroys them at destruction. No transfer of ownership
  between threads.

## Test plan

Currently **12 Catch2 cases** in `tests/sds/ring_buffer_test.cpp`
(the randomized property case contributes ~20k assertions per run, so
the total assertion count varies):

- `construct empty` — sizes/flags on fresh buffer.
- `capacity floor of 4` — constructor clamps below-floor input.
- `enqueue/dequeue round-trip` — basic write/read.
- `enqueue wraps around physical boundary` — fill, drain, fill again
  past the wrap point; assert reassembly.
- `peek does not consume` — peek leaves used_size unchanged.
- `peek across wrap boundary returns reassembled bytes` — peek handles
  split data.
- `auto-resize on enqueue overflow` — bytes > capacity triggers regrow.
- `dequeue caps at used_size` — over-request returns clamped count.
- `zero-size operations are no-ops` — bytes==0 on enqueue/dequeue/peek
  returns 0 without touching state; safe even with nullptr src (i.e.
  empty `std::vector::data()`).
- `direct_enqueue_size respects wrap` — contiguous-region size after
  partial fill.
- `move_head / move_tail advance cursors` — cursor primitives work
  independently of byte I/O.
- `randomized operations match deque reference` — 1000 random
  enqueue/dequeue/peek operations cross-checked against a
  `std::deque<char>` oracle (deterministic seed `0xC0FFEE`).

Future tests:

- ASan/LSan stress: 1M auto-resize triggers, sustained random ops for
  N seconds, verify no leaks. (LSan is currently flaky on WSL/io_uring
  hosts — workaround: `ASAN_OPTIONS=detect_leaks=0`; revisit on a
  non-WSL host.)
- UBSan integer-overflow probe on `resize_buffer` with huge inputs.

## Open questions

1. **Power-of-2 capacity + mask optimization.** Replace
   `pos % capacity_` with `pos & mask_` on every cursor advance — a 1-cycle
   AND instead of a 20-40-cycle DIV. Requires either (a) constraining
   capacity to powers of 2 (lose auto-grow flexibility), or (b) keeping
   modulo for capacity changes and only masking on hot-path access (more
   complex). Deferred until profiling shows the modulo is meaningful in
   real io_uring workloads.

2. **Auto-resize vs. io_uring fixed buffers.** When the recv/send buffer
   is registered with `io_uring_register_buffers`, the buffer address is
   pinned and cannot move. Auto-resize breaks that contract. v2 needs
   either a `fixed_ring_buffer` sibling (no resize, returns
   "would-block" instead of growing) or a constructor flag
   `ring_buffer{cap, growable_t::no}`. Decision deferred until the
   io_uring reactor design lands.

3. **SPSC variant.** For cross-thread producer/consumer, cursors must
   become `std::atomic<std::size_t>` with relaxed loads + release stores
   on the producer side, acquire loads on the consumer side. The
   `assert()` bounds checks in `move_head`/`move_tail` would become
   compare-and-swap loops. Worth introducing as a separate
   `spsc_ring_buffer` class rather than retrofitting the existing one.

4. **`std::span<const std::byte>` API for io_uring zero-copy.** Wiki
   originally sketched `writable_contig` / `readable_contig` returning
   spans, allowing `io_uring_prep_recv(sqe, fd, ring.writable_contig()
   .data(), ring.writable_contig().size(), 0)` without intermediate
   copy. The current `direct_enqueue_size()` + `tail_index()` pair
   already exposes the same information; a thin `writable_span()`
   wrapper is a 5-line follow-up when the reactor needs it.

5. **`recv_ring_buffer` / `send_ring_buffer` specializations.** Port from
   `SelectServer/FighterServer/RingBuffer.{h,cpp}`. Adds: frame-aware
   peek (recv side parses `uint16 size | uint16 id` header before
   returning), and `collect_iovec()` (send side returns up to two iovec
   entries for `io_uring_prep_writev`).

6. **`std::unique_ptr<char[]>` migration.** Replace the raw `char*`
   buffer with `std::unique_ptr<char[]>`. Mechanical change:
   - Constructor uses `std::make_unique_for_overwrite<char[]>(capacity_)`
     (C++20, available on gcc-12+) to skip zero-init and match the
     current `new char[]` performance.
   - `resize_buffer` copies the old contents into a fresh unique_ptr
     then does `buffer_ = std::move(new_buffer)`; the old buffer is
     destructed automatically.
   - All `buffer_ + tail_` access sites become `buffer_.get() + tail_`.
   - Removes the manual `delete[]` from the destructor.
   - **Bonus: safely re-enable move ctor / assignment as `= default`.**
     The `= delete` move declarations exist only because the raw owning
     pointer made a shallow-copy move unsafe; with `unique_ptr`, the
     default move correctly transfers ownership and leaves the source
     in a destructible empty state. Useful if a future `Session`
     factory wants to construct ring_buffers and move them into place.

7. **`std::byte` vs `char` for storage.** Switch `char buffer_[]` to
   `std::byte buffer_[]`. Semantic improvement (a ring of bytes is not
   a ring of characters), but pays for itself only at the network
   boundary:
   - `recv()`/`send()` take `void*` — no cast needed.
   - `io_uring_prep_recv` and friends likewise take `void*`.
   - Catch2 string-equality assertions in the existing tests would
     need `reinterpret_cast<const char*>(buf.data())` everywhere.
   - Future packet code that writes serialized bytes would either
     `reinterpret_cast<std::byte*>(payload)` or work in bytes
     end-to-end.

   **Decision (current): keep `char`.** Defer `std::byte` until either
   (a) a real bug appears that byte-typing would have prevented, or
   (b) the network layer commits to byte-typed APIs end-to-end. Do
   `std::unique_ptr<char[]>` (open question #6) first — that's the
   higher-value mechanical change.
