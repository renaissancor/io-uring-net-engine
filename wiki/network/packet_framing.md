# Packet framing — `[uint16 size | uint16 id][payload]`

## Purpose

Define the on-wire byte format for every packet sent through this
library: a **4-byte header** `[uint16 size | uint16 id]` followed by
payload bytes.

**This header is deliberately wider than the Windows reference's.**
The reference uses a 3-byte header
`[0x89 | uint8 payload_size | uint8 packet_type]` declared at
[`~/CLionProjects/SelectServer/TestSerialize/Protocol.h:40-45`](../../../SelectServer/TestSerialize/Protocol.h),
which caps the protocol at 256 packet IDs and 255-byte payloads.
This library's 4-byte header lifts those limits to 65 535 IDs and
65 531 byte payloads — the cost is intentional framing divergence
from the reference.

The cross-platform claim is therefore **payload-byte parity under
header normalization**, not raw-bytes parity. A Windows-recorded
trace is replayed against a Linux server (and vice versa) only after
a normalization step re-emits the 3-byte Windows header as a Linux
4-byte header. Stripping the 0x89 magic byte alone is **not**
sufficient; that mistake would leave a `[uint8 size | uint8 type]`
header that the Linux deframer misparses as the first 2 bytes of a
4-byte header.

See
[`iouring-net-server/docs/04-protocol.md`](../../../iouring-net-server/docs/04-protocol.md)
§ "Parity with the Windows reference" for the full precondition list,
the per-packet payload-byte parity statement, and what is/isn't
guaranteed (in particular: this is NOT a source-compatibility claim;
v1 does NOT support an unmodified Windows client end-to-end).

## Reference origin

- Format defined in `NextProject.md` (the planning doc); referenced in
  `SelectServer/FighterOOP/Network.cpp:377` (magic byte 0x89 validation,
  not the size/id format).
- No actual port — this is the canonical specification, not a copy.

## Wire format

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-------------------------------+-------------------------------+
|             size              |              id               |
+-------------------------------+-------------------------------+
|                                                               |
|                          payload                              |
|                                                               |
+---------------------------------------------------------------+
```

| Field    | Width   | Encoding         | Meaning                           |
|----------|---------|------------------|-----------------------------------|
| `size`   | 2 bytes | little-endian    | Total packet size in bytes (including header). 4 ≤ size ≤ 65535. |
| `id`     | 2 bytes | little-endian    | Packet ID (0..65535). Every value is legal at this layer; interpretation belongs to the product's dispatcher. |
| payload  | size−4  | application-defined | Opaque bytes; interpreted by the handler keyed on `id`. |

**Endianness.** Little-endian on the wire, matching the Windows reference.
Linux x86-64 and ARM64 little-endian are native, so reads/writes are plain
`memcpy`. A future big-endian target would need byte-swapping at the
header boundary.

**Maximum payload.** 65531 bytes (size = 65535, header = 4). Larger
payloads must be chunked at the application layer.

**Minimum size.** 4 bytes (header only, empty payload). `size < 4` is a
protocol error and the connection is dropped.

## Public API sketch

```cpp
namespace iouring_net {

struct packet_header {
    uint16_t size;
    uint16_t id;
} __attribute__((packed));
static_assert(sizeof(packet_header) == 4);

struct frame_view {
    uint16_t              size;        // including header
    uint16_t              id;
    std::span<const std::byte> payload; // size - 4 bytes
};

class packet_writer {
public:
    explicit packet_writer(uint16_t id, iouring_net::sds::serial_buffer<>& buf);

    template <class T>
    packet_writer& write(const T& v);                  // memcpy-append
    packet_writer& write_bytes(std::span<const std::byte> b);

    void finalize();                                    // back-patches size

private:
    iouring_net::sds::serial_buffer<>* buf_;
    packet_header*                     header_;
};

// Inverse: framer
std::optional<frame_view> peek_frame(std::span<const std::byte> bytes) noexcept;

} // namespace iouring_net
```

## Linux design

**Reading.** `recv_ring_buffer::peek_packet` reads the first 4 bytes
without advancing the read cursor:

```cpp
std::optional<frame_view> peek_packet() const {
    if (size() < 4) return std::nullopt;
    packet_header hdr;
    copy_out(read_pos_, &hdr, 4);                       // wrap-aware
    if (hdr.size < 4 || hdr.size > 65535)
        return std::nullopt;                            // protocol error
    if (size() < hdr.size) return std::nullopt;         // not all data yet
    return frame_view{ hdr.size, hdr.id, payload_span(hdr.size - 4) };
}
```

**Writing.** `packet_writer` reserves a 4-byte header in the
`SerialBuffer`, records its location, lets the caller append the payload,
then back-patches `header_->size = buf_->size()` in `finalize()`. This
matches the reference repo's idiom (lecture-derived).

**Validation rules.** A valid frame:
- `size >= 4`
- `size <= 65535` (implicit; `uint16_t`)
- All `size` bytes present in the buffer

A frame failing the first rule causes the session to disconnect.
Failing the second rule is impossible by type. Failing the third means
"keep waiting."

**Do not reserve or special-case `id == 0` in this layer.** Earlier
drafts reserved `id == 0` as an "uninitialized" sentinel that would
close the session, on the theory that it catches zero-init bugs in
product code. That rule conflicts with the wire-parity claim against
the Windows reference, whose schema includes
`SC_CREATE_MY_CHARACTER = 0` at
[`~/CLionProjects/SelectServer/TestSerialize/packets.json`](../../../SelectServer/TestSerialize/packets.json),
and breaks any product whose protocol legitimately uses `id == 0`.

Framing is a pure transport layer: every `uint16_t` is a legal `id`
on the wire, and the framing code path must not branch on the
numeric value. Whether `0` (or any other ID) is a *registered*
packet is the product's dispatcher's concern, handled by the
dispatcher's unknown-id handler (see
[`iouring-net-server/wiki/server/dispatch.md`](../../../iouring-net-server/wiki/server/dispatch.md)).
If a future contributor proposes re-adding an `id != 0` (or any
`id ∈ S`) rule here, the answer is: that policy lives in the
dispatcher, not the framing layer.

**Magic byte (rejected).** The reference repo's
`SelectServer/FighterOOP/Network.cpp:377` validates a 0x89 magic byte
in the header. We do **not** include this. Reasoning: a magic byte is
effective only as a partial sanity check and adds wire-format divergence
with no protocol benefit at this layer. Application-layer protocol IDs
do the same job.

## Concurrency & ownership

- Stateless. `peek_frame` and `packet_writer` operate on caller-supplied
  buffers and have no shared state.
- `packet_writer` is non-copyable, non-movable; it borrows the underlying
  `SerialBuffer` for its lifetime.

## Test plan

- Unit: round-trip every valid header value (size = 4, 100, 65535; id =
  0, 1, 32768, 65535); byte-exact match. `id = 0` is **explicitly
  included** to catch any reintroduction of the old `id != 0` rule.
- Unit: malformed header (size < 4) returns protocol error.
- Unit: split header (2 bytes available, then the other 2 arrive)
  returns `std::nullopt` then a valid frame.
- Unit: empty payload (size = 4) is valid.
- Property test: random byte streams; assert framer never reads past
  the input.
- Integration: send 1000 packets through a real session; assert all
  received with byte-exact payloads and ids in order.
- **Cross-implementation parity test:** capture a packet sequence from
  the Windows reference build, replay it through the Linux server,
  assert handler invocation matches.

## Open questions

1. **Variable-length size field.** A future v2 could promote `size` to a
   uint32 to remove the 65 KiB cap. That breaks wire-format parity, so
   it requires a versioning byte. Out of scope for v1.
2. **Compression / encryption.** Out of scope. If added, they live below
   this layer (per-session shim) and the framing is unchanged.
3. **Framing fast path.** If profiling shows the recv-side
   `peek_packet` is hot, vectorize the wrap-aware copy or specialize for
   the common-case (header is contiguous).
