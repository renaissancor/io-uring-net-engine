# sc_packet — typed writer for outgoing packets

> **2026-05-19 evening pivot — partially superseded.** This spec
> describes `sc_packet` as a packet_pool-allocated staging buffer that
> is memcpy'd into the send_ring at finalize time. Current direction:
> the outgoing path may write directly into a per-channel staging
> scratch (or directly into the send_ring's contiguous write region)
> and skip the staging-to-ring memcpy. The typed write API (`init`,
> `write<T>`, `write_string`, `finalize`), wire format, and
> content-thread-local lifetime are UNCHANGED. The packet_pool
> allocation step is being reconsidered. See
> `docs/discussions/2026-05-19-chat-server-data-layout.md` and the
> `project-chat-server-v1` memory entry for the current shape.

## Purpose

`sc_packet` (server → client packet) is the **typed writer** the content
thread uses to construct one outbound packet. It is allocated from the
per-content-thread [[packet_pool]] when a handler needs to send a reply
or broadcast, stamped with a header via `init()`, populated by typed
writes (`write<T>()`, `write_string()`, `write_bytes()`), finalized
(which patches the packet_header's `size` field), then its wire bytes
are copied into the owning session's `send_ring_buffer` and the packet
is freed back to the pool.

`sc_packet` is the symmetric companion of [[cs_packet]] — same storage
layout, opposite direction.

## Reference origin

No direct reference. Korean MMO `PacketWriter` / `SerialBuffer` writer
patterns are the conceptual ancestor; concrete shape is fresh.

## Public API sketch

```cpp
namespace iouring_net::net {

class sc_packet {
public:
    // ─── Per-use setup: stamps packet_header into body[0..8] ───
    void init(uint16_t opcode, uint16_t sequence,
              uint8_t flags = 0, uint8_t version = 0);

    // ─── Typed writes — advance cursor, LNX_DCHECK bounds in debug ───
    template <class T> void write(const T& v);
    void write_string(std::string_view sv);
    void write_bytes(std::span<const std::byte> bytes);

    // ─── Finalize: patches header.size in body[0..2]; returns total wire bytes ───
    uint16_t finalize();

    // ─── Capacity introspection ───
    uint16_t bytes_written() const { return storage_.cursor; }
    uint16_t capacity()      const { return storage_.size_or_cap; }
    uint16_t bytes_remaining() const;

    // ─── For the send-ring drain path ───
    std::span<const std::byte> wire_bytes() const;

    // ─── Pool-internal: packet_pool calls these ───
    void reset_for_send(uint16_t bucket_capacity);

private:
    packet_storage storage_;
};

} // namespace iouring_net::net
```

## Linux design

### Memory layout

Shares the [[packet_pool]] storage struct with [[cs_packet]]:

```cpp
struct alignas(8) packet_storage {
    uint16_t bucket_index;   //  2 B — bucket index for free path
    uint16_t size_or_cap;    //  2 B — bucket capacity (sc side)
    uint16_t cursor;         //  2 B — current write position within body
    uint16_t _padding;       //  2 B — keep metadata at 8 B
    std::byte body[];        //  8 B packet_header + payload
};
static_assert(sizeof(packet_storage) == 8);
```

`size_or_cap` here is the bucket's full capacity (e.g., 256 B for a
slot in the 256-byte bucket), not the packet's actual size. The
**actual** size lives in the `packet_header.size` field at `body[0..2]`,
patched by `finalize()`. `cursor` is the current write position;
`cursor == finalize()'s return value` is the total wire bytes emitted.

### Endian

**Little-endian on the wire and on the host (x86-64 / ARM64).** No
byte swap on write. `memcpy` from `T` directly to wire bytes works
because both byte orders match.

### `init()` — stamp the header

```cpp
void sc_packet::init(uint16_t opcode, uint16_t sequence,
                     uint8_t flags, uint8_t version) {
    // size starts at 0 — patched by finalize()
    packet_header h{0, opcode, sequence, flags, version};
    std::memcpy(&storage_.body[0], &h, sizeof(packet_header));
    storage_.cursor = sizeof(packet_header);   // ready to write payload
}
```

After `init`:

- `body[0..8]` contains the header with `size = 0` (placeholder).
- `cursor = 8` (positioned at the first payload byte).
- The handler now writes typed payload fields via `write<T>` etc.

### Typed write implementation

```cpp
template <class T>
void sc_packet::write(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    LNX_DCHECK(storage_.cursor + sizeof(T) <= storage_.size_or_cap);
    std::memcpy(&storage_.body[storage_.cursor], &v, sizeof(T));
    storage_.cursor += sizeof(T);
}
```

Notes:

- `LNX_DCHECK` traps in debug if the write would overrun the bucket
  capacity. Release builds compile the check out — production trust is
  that handlers don't write more than their allocated bucket holds. If
  the handler underestimates, allocate from a larger bucket up-front.
- The `static_assert` rejects non-trivially-copyable types.
- `memcpy` handles arbitrary cursor alignment — same as cs_packet.

### String / byte-span writes

```cpp
void sc_packet::write_string(std::string_view sv) {
    LNX_DCHECK(sv.size() <= 65535);
    write<uint16_t>(static_cast<uint16_t>(sv.size()));
    write_bytes(std::as_bytes(std::span{sv.data(), sv.size()}));
}

void sc_packet::write_bytes(std::span<const std::byte> bytes) {
    LNX_DCHECK(storage_.cursor + bytes.size() <= storage_.size_or_cap);
    std::memcpy(&storage_.body[storage_.cursor], bytes.data(), bytes.size());
    storage_.cursor += static_cast<uint16_t>(bytes.size());
}
```

Wire format mirrors [[cs_packet]]'s reader convention: `uint16_t length`
followed by the bytes. Read and write share the length-prefix convention
so a string written by `sc_packet::write_string` reads back via
`cs_packet::read_string` on the peer.

### `finalize()` — patch the size field

```cpp
uint16_t sc_packet::finalize() {
    LNX_DCHECK(storage_.cursor >= sizeof(packet_header));
    uint16_t total = storage_.cursor;
    // Patch packet_header.size in-place (first 2 bytes of body)
    std::memcpy(&storage_.body[0], &total, sizeof(uint16_t));
    return total;
}
```

`finalize` rewrites only the 2-byte size field. The rest of the header
(`opcode`, `sequence`, `flags`, `version`) was already correct after
`init()`. No re-encoding cost.

After `finalize`, the packet is ready to ship: `wire_bytes()` returns a
view of `body[0..cursor]`, which gets copied into the session's send ring
buffer.

### `wire_bytes()` — for the ring-buffer append

```cpp
std::span<const std::byte> sc_packet::wire_bytes() const {
    return {storage_.body, storage_.cursor};
}
```

The content thread's tick loop appends these bytes to the owning
session's `send_ring_buffer`. The bytes are copied — the sc_packet is
freed immediately after, so the buffer must own its own copy.

### Pool integration

```cpp
void sc_packet::reset_for_send(uint16_t bucket_capacity) {
    storage_.size_or_cap = bucket_capacity;
    storage_.cursor      = 0;                 // init() will set to sizeof(header)
}
```

Called by `packet_pool::alloc_sc` after popping the slot from its bucket
free list. The handler then calls `init()` to stamp the header.

## Lifecycle — send flow

```
[Content thread: opcode handler runs]
   sc_packet* p = packet_pool::alloc_sc(estimated_max_size);
   p->init(SMSG_CHAT_MSG, session.next_send_seq++);
   p->write<uint32_t>(speaker_id);
   p->write_string(message_text);
   p->write<uint64_t>(timestamp);
   uint16_t total = p->finalize();
   session.send_ring.append(p->wire_bytes());     // copy out into ring
   packet_pool::free_sc(p);
        │
        ▼
[Content thread: tick output phase ends → tick loop continues to next session]

[Network thread: io_uring CQE wakes up after send_ring fill]
   io_uring_prep_send(sqe, session.fd,
                      session.send_ring.contiguous_bytes(),
                      session.send_ring.contiguous_size());
   io_uring_submit();
        │
        ▼
[Eventually: io_uring CQE for send completion]
   session.send_ring.advance_read(bytes_sent);
   // if more bytes in ring: submit another send SQE
```

The sc_packet exists only inside the handler call frame plus a few
instructions. The pool slot is freed before the tick advances to the
next session.

## Concurrency & ownership

- **Content-thread-local.** Allocated, populated, finalized, and freed
  entirely on one content thread.
- **No cross-thread access.** Network thread reads bytes from the send
  ring buffer but never touches an sc_packet object.
- **Short lifetime.** Each sc_packet lives microseconds — built within
  one handler call, copied to ring, freed. The pool sees high turnover
  in the active buckets.

## Test plan

- Unit: round-trip a series of typed writes followed by reads (via a
  cs_packet view) — values match byte-for-byte.
- Unit: `init` + `finalize` with zero payload writes — `total` equals
  `sizeof(packet_header)`, header bytes correct.
- Unit: `write_string("")` and `write_string("hello")` round-trip via
  `cs_packet::read_string`.
- Unit (debug build): `write<T>` past `size_or_cap` fires `LNX_DCHECK`.
- Unit: `wire_bytes()` after `finalize()` matches the expected wire
  encoding byte-for-byte.
- Integration: handler builds sc_packet, appends to send ring, network
  thread reads from ring, peer receives byte-for-byte match.
- Endian: cross-check that `write<uint32_t>(0x04030201)` produces wire
  bytes `0x01 0x02 0x03 0x04` (little-endian) on x86-64.

## Open questions

1. **`init()` vs constructor + placement-new on alloc.** Currently
   tentatively `init()` because the pool's slot constructors run once at
   server startup. Placement-new on every alloc is the alternative.
   **Decision deferred; can change later without breaking the API
   surface.** (Same open question as [[cs_packet]].)
2. **Bucket-size estimation.** Handlers must call `alloc_sc(N)` with an
   accurate-or-overestimate of the bytes they'll write. Under-estimating
   trips `LNX_DCHECK` on a later `write<T>`. Two policy options:
   (a) handlers know exactly how many bytes they'll write, allocate
   precisely; (b) allocate from the next bucket up generously, accept
   wasted bucket space. Mix likely in practice — short fixed packets do
   (a), variable-length ones do (b).
3. **Auto-finalize on first `wire_bytes()` call?** Could remove the
   manual `finalize()` step. Drawback: hides the size-patch step from
   the caller, harder to debug if someone calls `wire_bytes()` without
   intending to finalize. Lean toward keeping `finalize()` explicit.
4. **Multi-packet builders.** Some replies span multiple sc_packets (a
   broadcast to N recipients). Each is built independently and appended
   to its session's send ring. No special multi-packet shape needed in
   sc_packet itself — the handler just builds one per recipient.
5. **`LNX_DCHECK` vs `LNX_CHECK` in release builds.** Same trade-off as
   [[cs_packet]]. Profile to decide.

## See also

- [[packet_header]] — 8 B wire header at `body[0..8]`
- [[packet_pool]] — pre-allocated bucket pool that hands out sc_packet slots
- [[cs_packet]] — sibling type for incoming packets (typed reader)
- [[session]] — session_pool, send_ring_buffer that consumes wire_bytes
- [[threading_model]] — content-thread-local lifetime invariant
