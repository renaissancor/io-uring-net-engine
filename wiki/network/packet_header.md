# packet_header — 8-byte fixed packet header for the wire protocol

> **2026-05-19 evening pivot — partially superseded.** The reception
> flow at the bottom of this spec (`mem::alloc(h.size)` + drain into
> `serial_buffer`) predates the SoA + session-as-handle pivot. Current
> state: incoming frames are zero-copy `frame_view` slices into the
> recv ring (stitched into a channel-local scratch buffer only on
> wrap-straddle), not per-frame pool allocations. The 8 B header
> layout, opcode-direction convention, sequence/flags/version
> semantics, little-endian wire format, and design rationale in this
> spec are UNCHANGED. See
> `docs/discussions/2026-05-19-chat-server-data-layout.md` and the
> `project-chat-server-v1` memory entry for the current shape.

## Purpose

Define the binary header that prefixes every packet on the wire. Establishes
framing, dispatch, replay protection, extensibility, and protocol versioning
in a fixed 8 bytes — one 64-bit load on x86-64, naturally aligned for the
body that follows.

This header is the boundary between the *transport* (io_uring + TCP socket
delivering bytes) and the *content layer* (typed handlers dispatched by
opcode). Every cross-thread packet copy through the inbox (see
[[threading_model]]) starts here.

## Reference origin

| Source / convention                       | Header size | Notes                                                    |
|-------------------------------------------|-------------|----------------------------------------------------------|
| `IOCP_Rookiss/Engine/PacketHeader`        | 4 B         | `size` + `opcode`; Korean MMO minimalist tradition      |
| AION / Lineage 2 / MapleStory             | 4 B         | Same minimalist convention                              |
| HTTP/2 frame header (RFC 7540 §4.1)       | 9 B         | Length + type + flags + stream ID                       |
| Quake 3 reliable channel                  | 8 B         | Sequence + ack                                          |
| Photon (Unity)                            | 9 B+        | Op code + flags + length + sequence                     |

The 8 B size sits at the workhorse range — larger than the minimalist
Korean MMO tradition but with every byte earning its place. See
[Design rationale](#design-rationale) for why each field was chosen.

## Layout

```cpp
namespace iouring_net::net {

struct alignas(8) packet_header {
    uint16_t size;        // 2 B — total bytes INCLUDING this header; max 65535
    uint16_t opcode;      // 2 B — packet type; high bit splits direction
    uint16_t sequence;    // 2 B — rolling sender-side counter
    uint8_t  flags;       // 1 B — bitfield; all bits reserved in v1 (must be 0)
    uint8_t  version;     // 1 B — protocol version; v1 = 0
};
static_assert(sizeof(packet_header)  == 8);
static_assert(alignof(packet_header) == 8);

} // namespace iouring_net::net
```

### Byte budget

| Offset | Size | Field      | Purpose                                                          |
|--------|------|------------|------------------------------------------------------------------|
| 0–1    | 2 B  | `size`     | framing; receiver drains exactly this many bytes total           |
| 2–3    | 2 B  | `opcode`   | dispatch; routes to handler; high bit splits CMSG vs SMSG        |
| 4–5    | 2 B  | `sequence` | replay protection / in-flight ordering verification              |
| 6      | 1 B  | `flags`    | extensibility; v1 = 0, features claim bits as they land          |
| 7      | 1 B  | `version`  | protocol evolution; bump on any breaking change                  |
|        | **8 B** |         | one 64-bit load on x86-64, fully accounted for                   |

## Field semantics

### `size` — total packet length including header

`size` counts the 8 header bytes as well as the body. So `size == 8` is a
header-only packet (e.g., a keepalive). Maximum body length is
`65535 − 8 = 65527 B`. Bodies that need to exceed this use the future
`flags.fragment-more` bit; see [Open questions](#open-questions).

The reason for this convention rather than "size of body": the receiver
allocates a `serial_buffer` of exactly `size` bytes via `mem::alloc(size)`,
then drains `size` bytes from the session ring buffer. One number describes
one allocation. If `size` excluded the header, every receiver would have to
compute `size + 8`.

### `opcode` — packet type, 65536 distinct IDs

High bit partitions direction:

- `0x0000 – 0x7FFF` → `CMSG_*` (client → server) — 32 K opcodes
- `0x8000 – 0xFFFF` → `SMSG_*` (server → client) — 32 K opcodes

This convention serves three roles:

1. **Direction at a glance.** A parser can tell which side sent the packet
   from the first byte of `opcode`, without per-opcode metadata.
2. **Immediate corruption check.** If a client receives an opcode in the
   CMSG range (or vice versa), it's almost certainly garbage or off-by-one
   framing. Reject fast.
3. **Opcode-table partitioning.** CMSG and SMSG handlers can live in
   separate dispatch tables, each addressed by the low 15 bits.

32 K opcodes per direction is intentionally over-provisioned. No real game
ships more than a few hundred distinct opcodes; the headroom exists so the
convention itself never becomes a constraint.

### `sequence` — replay protection / in-flight ordering

Sender increments per packet; receiver tracks last-seen and rejects
duplicates within a small window (typically 256 or 1024 packets back).

Wraps after ~65 K packets. The wrap window is far larger than any in-flight
ordering window. **This is not an ack/window field**: TCP already provides
reliable in-order delivery; `sequence` is defense against proxy-induced
replay plus a debugging tool for out-of-order conditions.

If the project ever moves to UDP, `sequence` becomes load-bearing for
in-order delivery and may need to widen to `uint32_t` (a breaking change →
bump `version`).

### `flags` — bitfield, all bits reserved in v1

In v1, senders MUST emit `flags == 0` and receivers MUST reject any packet
with non-zero `flags`. Treat this as an extension slot for v2+ features.

Anticipated bit allocations (claimed when each feature actually lands):

| Bit | Name                     | Meaning                                              |
|-----|--------------------------|------------------------------------------------------|
| 0   | `encrypted`              | Body is encrypted; decrypt with session key          |
| 1   | `compressed`             | Body is compressed; decompress before dispatch       |
| 2   | `ack_required`           | Sender expects an explicit ack opcode in response    |
| 3   | `fragment_more`          | More fragments follow with same opcode; reassemble   |
| 4   | `debug_header_follows`   | An 8 B `packet_debug_header` follows (deferred design) |
| 5–7 | reserved                 | Reserved for future use                              |

These are *anticipated*, not *committed*. Specific bit positions get locked
in when each feature lands; this table gets updated then.

### `version` — protocol version

`v1 = 0`. Bumps on any breaking change to the production header layout,
opcode-space partition, or field semantics. Receivers route by version to
the correct parser or reject unknown versions.

Non-breaking changes that do NOT bump `version`:

- New opcodes (unknown opcodes are rejected by an existing handler).
- New `flags` bit allocations (the wire framing path is unchanged).
- Body-format changes for a specific opcode (per-opcode versioning is
  application-layer, not transport-layer).

Version-skew strategy: when v2 ships, servers run v1 and v2 dispatchers
in parallel for a deprecation window. Clients connect with their build's
version; server routes by `version`. v1 dispatcher is removed once the
deprecation window closes.

## Wire conventions

### Endianness

**Little-endian** on every multi-byte field. Matches x86-64 native — no
byte-swap on the host. One `memcpy` from wire bytes lands the struct
ready-to-read.

If a big-endian platform is ever targeted, conversion happens at the wire
boundary in serial_buffer's encode/decode, not in the header struct itself.
For now: zero conversion cost.

### Alignment

`alignof(packet_header) == 8`. After the memory pool's 16 B `MemoryHeader`
(see [[memory_pool]]) plus the 8 B `packet_header`, the body starts at
offset 24 — an 8 B boundary. Body fields up to `uint64_t` have no
misalignment penalty.

This is one reason the header is 8 B rather than 4 B: a 4 B header would
leave the body at a 4 B boundary, forcing `memcpy` or accepting misaligned
8 B loads in the body.

## Reception flow

```
[io_uring CQE: N bytes recv'd on session S]
        │
        ▼
[copy N bytes into session S's ring_buffer]
        │
        ▼
[framing loop]
   while ring_buffer.readable() >= sizeof(packet_header):
       peek packet_header h from ring_buffer       // does not consume
       if (h.flags   != 0)                         reject as malformed
       if (h.version != 0)                         route to correct dispatcher or reject
       if (h.size    < sizeof(packet_header))      reject as malformed
       if (h.size    > ring_buffer.capacity())     reject as malformed
       if (ring_buffer.readable() < h.size)        break    // need more bytes
       allocate serial_buffer via mem::alloc(h.size)
       drain h.size bytes from ring_buffer into serial_buffer
       dispatch(h.opcode, serial_buffer)
```

The framing loop runs intra-thread on the connection's worker. Dispatch is
intra-thread for opcodes whose handlers touch only this connection's state;
cross-thread (via inbox copy, see [[threading_model]]) for opcodes that
affect shared interaction-unit state on a different worker.

## Test plan

- Unit: round-trip every field — write `packet_header` to bytes, read
  back, assert equal.
- Unit: `sizeof(packet_header) == 8`, `alignof(packet_header) == 8`.
- Unit: high-bit `opcode` partition — synthesize CMSG/SMSG headers and
  verify the direction-detection helper returns the correct value.
- Unit: `flags != 0` rejection in v1.
- Unit: `version != 0` rejection in v1.
- Unit: `size < sizeof(packet_header)` rejection (cannot exist).
- Unit: `size > ring_buffer.capacity()` rejection (oversized).
- Property: random byte input never produces a successful framing
  (fuzz-style; >99% rejection rate expected).
- Integration: framing loop drains exactly `size` bytes per packet, leaves
  partial-packet bytes in the ring for the next iteration.

## Open questions

1. **Sequence dedup window.** What's the duplicate-detection window —
   256 packets? 1024? Wider window → more memory per session (one bit per
   tracked sequence number); narrower → more false negatives on benign
   reorders. Decide once we measure realistic reordering rates.
2. **`flags.fragment_more` semantics.** When a body needs to exceed
   65527 B, how does the receiver reassemble fragments? Likely: same
   opcode, sequential sequence numbers, last fragment has `fragment_more`
   = 0. Specifics (max fragments, timeout, OOM-protection) deferred until
   the first use case demands it.
3. **Debug header signaling.** `flags.bit 4` is tentatively assigned to
   "debug header follows" — see deferred design. When that lands, this
   bit and the 8 B `packet_debug_header` layout get locked together.
4. **CMSG/SMSG asymmetric features.** Some games encrypt only SMSG, some
   require ack on SMSG only. The high-bit-direction convention lets
   handlers branch on direction; specific asymmetric behaviors (per-
   direction encryption keys, per-direction ack semantics) are
   application-layer, not header.

## Design rationale — why these specific bytes

Preserving the rationale here so future contributors don't reopen settled
questions.

### Why 8 B and not 4 B?

A 4 B header (`size` + `opcode`, the Korean MMO minimalist convention)
ships everything strictly required for framing and dispatch. The extra 4 B
in this design buys:

- **One naturally-aligned 64-bit load on x86-64** (`mov rax, [rdi]` reads
  the entire header in one instruction).
- **8 B alignment for the body**, eliminating misaligned-load penalties on
  any `uint64_t` body fields.
- **`sequence`, `flags`, `version`** — forward-compat insurance for replay
  protection, feature flags, and protocol evolution.

At the project's intended scale (channel-based MMO / match-based games,
~10K–100K concurrent connections), the per-packet tax of 8 B vs 4 B is
negligible. Forward-compat insurance is worth the bytes.

### Why 8 B and not 16 B?

A 16 B header would let us include `timestamp` and `sender_thread_id` for
debug/diagnostics in every production packet. These are valuable but
production traffic doesn't need them — they're debug-build concerns.
Deferring to a separate optional debug header (signaled by `flags.bit 4`)
keeps production paying only for what it uses.

### Why no checksum / CRC?

TCP already provides CRC at the transport layer. Adding our own
application-layer CRC is defense-in-depth that costs 2–4 B per packet for
marginal benefit. Revisit only if the protocol ever runs over UDP, where
checksum becomes load-bearing.

### Why no `magic` number?

A magic-number field rejects random garbage early, but the combination of
`opcode`'s high-bit-partition + `version` field catches most malformed
input with similar effectiveness at 0 B cost.

### Why no `session_id` / `connection_id`?

The socket itself identifies the connection (the session pointer is
reachable from the io_uring completion). Including a `session_id` in every
header would duplicate information already in the runtime — bytes for no
benefit.

## See also

- [[memory_pool]] — TLS Memory pool that backs every `serial_buffer` allocation
- [[threading_model]] — per-worker io_uring topology, cross-thread inbox semantics
- [[ring_buffer]] — per-session byte ring that accumulates bytes pre-framing
- (deferred) `packet_debug_header` — optional 8 B diagnostic extension
- (deferred) `serial_buffer` — typed read/write API over the allocated packet bytes
