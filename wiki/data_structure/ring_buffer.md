# RingBuffer — circular byte buffer for socket I/O

## Purpose

The recv-side and send-side buffer used by every `Session`. Decouples
`io_uring` recv completions from packet-framing logic (recv side) and packet
serialization from `io_uring` send submissions (send side).

Two specializations: `RecvRingBuffer` for the recv side (kernel writes via
`io_uring_prep_recv`, framer reads packets out), and `SendRingBuffer` for
the send side (handler writes serialized packets, reactor pulls bytes for
`io_uring_prep_send`). Both are SPSC by construction — there is exactly one
producer and one consumer per buffer, both pinned to the session's reactor
thread (in v1).

## Reference origin

- `WindowsLibrary/Library/Include/RingBuffer.h:5` — base class.
- `SelectServer/.../RingBuffer.h:56, 71` — `RecvRingBuffer` and
  `SendRingBuffer` specializations with framing-aware `Peek`/`Read` methods.

## Public API sketch

```cpp
namespace iouring_net::buf {

class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);     // capacity must be power of 2

    // Producer side
    std::span<std::byte> writable_contig();   // contiguous writable region
    void commit_write(size_t n);              // advance write cursor

    // Consumer side
    std::span<const std::byte> readable_contig() const;
    void commit_read(size_t n);

    size_t size() const;                       // bytes currently buffered
    size_t capacity() const;
    bool   empty() const { return size() == 0; }
    bool   full()  const { return size() == capacity(); }

    void clean();                              // drop all data (debug only)

private:
    std::byte* buffer_;
    size_t     capacity_;                      // power of 2
    size_t     mask_;                          // capacity_ - 1
    size_t     write_pos_{0};
    size_t     read_pos_{0};
};

class RecvRingBuffer : public RingBuffer { /* framing-aware peek */ };
class SendRingBuffer : public RingBuffer { /* gather-list build */ };

} // namespace iouring_net::buf
```

The `writable_contig` / `readable_contig` shape is intentionally
`io_uring`-friendly: `io_uring_prep_recv` takes a single `(buf, len)` pair,
so we expose the contiguous region directly. When the wrap point splits
data, the caller handles the second region in a follow-up SQE.

## Linux design

**Power-of-two capacity.** `mask_ = capacity_ - 1`; index reads use
`buffer_[pos & mask_]`. No modulo on the hot path. Default capacity 64 KiB
for recv, 64 KiB for send.

**Cursor representation.** `write_pos_` and `read_pos_` are monotonically
increasing 64-bit counters; `size()` is `write_pos_ - read_pos_`. This is
the classic Lamport SPSC ring trick — no separate "is full" flag needed.

**Storage.** Allocated via `mem::alloc` (the project memory pool). For send
buffers larger than 2 KiB, this falls through to `std::aligned_alloc(64, ..)`.

**`io_uring` integration.** Recv ring buffers can optionally be registered
via `io_uring_register_buffers` for fixed-buffer recv. Provided buffers
(kernel 5.19+) are an alternative; see
`wiki/network/io_uring_reactor.md`.

**`RecvRingBuffer::peek_packet`.** Inspects the first 4 bytes after
`read_pos_` to extract the framing header (`uint16 size | uint16 id`) and
returns either:
- `std::nullopt` — not enough bytes buffered yet.
- `Frame{size, id, payload_span}` — header parsed, payload may still wrap.

The framer commits the read only after the handler has processed the
payload.

**`SendRingBuffer::collect_iovec`.** Returns up to two `iovec` entries
(start..end-of-buffer, then beginning..wrap-point) for the next
`io_uring_prep_writev` (or two linked `io_uring_prep_send` SQEs).

## Concurrency & ownership

- v1: SPSC, both ends on the session's reactor thread. No atomic cursors
  needed — relaxed reads/writes are correct.
- v2 (out of scope): if we ever need cross-thread send (e.g., a worker
  thread completing a job and posting a response), the send ring becomes
  MPSC and cursors become `std::atomic<uint64_t>`. Document the migration
  path explicitly.
- Lifetime: owned by `Session`. Created in `Session::Session(...)`, destroyed
  in `~Session()`. The `io_uring_register_buffers` registration must be
  released **before** the session destructs; reactor handles that.

## Test plan

- Unit: write 100 bytes, read 50, write 100 more, read 150 — assert correct
  byte sequence across the wrap.
- Unit: `peek_packet` with split header (header bytes 1-2 before wrap, 3-4
  after) — assert no false-positive frame return until all 4 bytes available.
- Unit: capacity-1 fill, full/empty boundary cases.
- Property test: random write/read sequences with size totals up to 100 ×
  capacity; final byte stream matches input.
- Integration: end-to-end echo with 1 KiB, 16 KiB, and 64 KiB messages (the
  last forces multiple wraps inside one packet).

## Open questions

1. **Capacity-per-session vs. capacity-per-listener-template.** Default
   64 KiB per session is fine for low fan-out. For 100k connections, that's
   12 GiB of recv buffers. Provide a `Service::ConnectionDefaults` knob.
2. **Provided buffers.** Kernel 5.19+ supports `IOSQE_BUFFER_SELECT` —
   the kernel hands us a buffer index per recv completion, eliminating
   per-session recv buffer allocation entirely. Worth pursuing once
   single-buffer flow works. **Decision: implement single-buffer first;
   add provided-buffer mode in a follow-up.**
3. **Backpressure.** Send ring full → ? Block the producer coroutine on a
   "writable" awaitable, or drop the connection. Current plan: block (the
   producer is a coroutine, so this is a clean suspension). Documented in
   `wiki/network/session.md`.
