# cs_packet — typed reader for incoming packets

> **2026-05-19 evening pivot — partially superseded.** This spec
> describes `cs_packet` as a packet_pool-allocated slot whose body
> bytes are drained from the recv ring. Current state: `cs_packet` is
> a VIEW (`frame_view`) — a pointer + length into recv_ring bytes (or
> into a channel-local stitch buffer when the frame straddles the ring
> wrap). No per-frame pool allocation, no body memcpy on the happy
> path. The typed read API (`read<T>`, `read_string`, `read_bytes`)
> and lifetime contract (valid for one dispatch call) are UNCHANGED;
> only the underlying storage model changes. See
> `../design-notes/2026-05-19-chat-server-data-layout.md` and the
> `project-chat-server-v1` memory entry for the current shape.

## Purpose

`cs_packet` (client → server packet) is the **typed reader** the content
thread uses to consume one inbound packet after framing. It is allocated
from the per-content-thread [[packet_pool]] when a complete packet's bytes
are available in a session's `recv_ring_buffer`, populated by draining
those bytes, then handed to the opcode dispatcher. After the handler
runs, it's freed back to the same pool.

`cs_packet` exposes a typed read API (`read<T>()`, `read_string()`,
`read_bytes(n)`) over the bytes following the 8 B [[packet_header]] —
sequential reads via a cursor, bounds-checked in debug builds.

## Reference origin

No direct reference. The Korean MMO tradition (`SerialBuffer`,
`PacketReader`) is the conceptual ancestor; concrete shape is fresh to
this project.

## Public API sketch

```cpp
namespace iouring_net::net {

class cs_packet {
public:
    // ─── Header access (memcpy-decoded from body[0..8]) ───
    packet_header header()   const;
    uint16_t      opcode()   const { return header().opcode; }
    uint16_t      sequence() const { return header().sequence; }
    uint8_t       flags()    const { return header().flags; }
    uint8_t       version()  const { return header().version; }
    uint16_t      size()     const { return storage_.size_or_cap; }

    // ─── Typed reads — advance cursor, LNX_DCHECK bounds in debug ───
    template <class T> T            read();
    std::string_view                read_string();
    std::span<const std::byte>      read_bytes(size_t n);

    // ─── Cursor introspection ───
    size_t bytes_remaining() const;
    bool   at_end()          const;

    // ─── Pool-internal: packet_pool calls these ───
    void reset_for_recv(uint16_t actual_size);
    void* body_writable()    { return storage_.body; }   // for ring_buffer drain

private:
    packet_storage storage_;
};

} // namespace iouring_net::net
```

## Linux design

### Memory layout

Shares the [[packet_pool]] storage struct:

```cpp
struct alignas(8) packet_storage {
    uint16_t bucket_index;   //  2 B — bucket index for free path
    uint16_t size_or_cap;    //  2 B — actual size received (cs side)
    uint16_t cursor;         //  2 B — current read position within body
    uint16_t _padding;       //  2 B — keep metadata at 8 B
    std::byte body[];        //  8 B packet_header + payload
};
static_assert(sizeof(packet_storage) == 8);
```

The metadata occupies the first 8 B of the pool slot; `body[]` starts at
offset 8 and contains the wire bytes verbatim — header first, then payload.

Body alignment: 8 B from the start of `cs_packet`, which (combined with
the 16 B header from `MemoryHeader` in TLS Memory — though this lives
in the dedicated packet pool, not TLS Memory) means payload bytes start
at a 16 B aligned address. Safe for any `uint64_t` read.

### Endian

**Little-endian on the wire and on the host (x86-64 / ARM64).** No byte
swap on read. `memcpy` from wire bytes directly into a `T` works
correctly because both byte orders match.

If the project ever targets a big-endian platform, the swap happens at
the `read<T>` boundary via `std::byteswap` (C++23) or hand-rolled —
fully isolated to one function template.

### Typed read implementation

```cpp
template <class T>
T cs_packet::read() {
    static_assert(std::is_trivially_copyable_v<T>);
    LNX_DCHECK(storage_.cursor + sizeof(T) <= storage_.size_or_cap);
    T v;
    std::memcpy(&v, &storage_.body[storage_.cursor], sizeof(T));
    storage_.cursor += sizeof(T);
    return v;
}
```

Notes:

- `memcpy` handles arbitrary cursor alignment correctly. A `read<uint64_t>`
  at cursor position 11 is safe (slightly slower on some micro-
  architectures, invisible at workload scale).
- The `static_assert` rejects types with non-trivial constructors,
  destructors, or copy operators — only POD-like types make sense to
  deserialize this way.
- `LNX_DCHECK` traps to `int 3` in debug builds at the misuse site if
  the read would underrun the buffer. Release builds compile the check
  out — production reads are trusted because the framing layer
  ([[packet_header]]'s reception flow) already validated
  `size <= ring capacity` and `size >= sizeof(packet_header)`.

### String / byte-span reads

```cpp
std::string_view cs_packet::read_string() {
    // Wire format: uint16_t length, then `length` bytes
    uint16_t len = read<uint16_t>();
    LNX_DCHECK(storage_.cursor + len <= storage_.size_or_cap);
    std::string_view sv{
        reinterpret_cast<const char*>(&storage_.body[storage_.cursor]),
        len
    };
    storage_.cursor += len;
    return sv;
}

std::span<const std::byte> cs_packet::read_bytes(size_t n) {
    LNX_DCHECK(storage_.cursor + n <= storage_.size_or_cap);
    std::span<const std::byte> view{
        &storage_.body[storage_.cursor],
        n
    };
    storage_.cursor += n;
    return view;
}
```

The returned views point **into the packet's body** — valid only until
the packet is freed back to the pool. Handlers that need to retain bytes
beyond the dispatch call must copy them (typically into a TLS Memory
allocation for game state).

### Pool integration

```cpp
void cs_packet::reset_for_recv(uint16_t actual_size) {
    storage_.size_or_cap = actual_size;
    storage_.cursor      = sizeof(packet_header);   // past the 8 B header
}
```

This is called by `packet_pool::alloc_cs` immediately after popping the
slot from its bucket free list. The cursor is positioned past the header
so the first `read<T>()` lands on the first payload byte.

The bucket_index field is preserved across alloc/free cycles (set when
the slot was first built at server startup).

## Lifecycle — receive flow

```
[Network thread: io_uring CQE delivers N bytes]
   network_thread.append_to_recv_ring(session, n)
        │
        ▼
[Content thread: tick input phase]
   for each owned session S:
       while S.recv_ring.readable() >= sizeof(packet_header):
           packet_header h;
           S.recv_ring.peek(&h, sizeof(h));           // doesn't consume
           if S.recv_ring.readable() < h.size: break  // wait for more bytes
           cs_packet* p = packet_pool::alloc_cs(h.size);
           S.recv_ring.drain(p->body_writable(), h.size);
           p->reset_for_recv(h.size);
           dispatch(session_id, opcode, *p);          // handler runs
           packet_pool::free_cs(p);
```

The cs_packet exists only for the duration of one handler dispatch. It
never escapes the content thread that owns the session.

## Concurrency & ownership

- **Content-thread-local.** Allocated, used, and freed entirely on one
  content thread.
- **No cross-thread access.** Network thread fills the ring buffer with
  raw bytes but never touches a cs_packet object.
- **Single-handler lifetime.** Each cs_packet is dispatched once and freed
  immediately. No long-lived references; views returned by `read_string`
  / `read_bytes` are invalidated by the free.

## Test plan

- Unit: round-trip every supported type — write to a buffer manually, wrap
  in a fake packet, `read<T>()` returns the same value.
- Unit: `read_string()` round-trips arbitrary-length strings up to packet
  capacity.
- Unit: `bytes_remaining()` decrements correctly across mixed `read<T>`
  / `read_string` / `read_bytes` calls.
- Unit (debug build): `read<T>` past `size_or_cap` fires `LNX_DCHECK`
  (traps via `int 3` → caught by gdb).
- Integration: real recv flow — bytes pushed to ring, framing loop drains
  to cs_packet, handler reads typed fields, all values match the encoded
  values byte-for-byte.
- Endian: cross-check that `read<uint32_t>` of bytes
  `0x01 0x02 0x03 0x04` returns `0x04030201` on x86-64.

## Open questions

1. **`init()` vs constructor + placement-new on alloc.** Currently
   tentatively `init()` (here named `reset_for_recv`) because the pool's
   slot constructors run once at server startup, not per allocation.
   Placement-new on every alloc is the alternative — feels heavier and
   would require the allocator to template on init args. **Decision
   deferred; can change later without breaking the API surface.**
2. **Should `read<T>` enforce `is_standard_layout_v<T>` in addition to
   `is_trivially_copyable_v<T>`?** Standard-layout types have predictable
   member layout across compilers; trivially-copyable doesn't strictly
   guarantee that. Lean toward yes. Decide once we have a real codec
   suite to test against.
3. **Variable-length-field convention.** Currently `read_string` decodes
   `uint16_t length + bytes`. For very short strings, `uint8_t length`
   would save a byte. For very long, `uint32_t` would extend the cap.
   `uint16_t` matches the packet size field's width and is plenty for
   chat messages. Lock unless measurement suggests otherwise.
4. **`LNX_DCHECK` vs `LNX_CHECK` in release builds.** Hot path optimization
   says compile out in release; defense-in-depth says keep on. The
   measurable cost is one branch per read, likely negligible. Profile
   first.

## See also

- [[packet_header]] — 8 B wire header at `body[0..8]`
- [[packet_pool]] — pre-allocated bucket pool that hands out cs_packet slots
- [[sc_packet]] — sibling type for outgoing packets (typed writer)
- [[session]] — session_pool, ring buffers, framing source
- [[threading_model]] — content-thread-local lifetime invariant
