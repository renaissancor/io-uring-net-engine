# 04 — Wire protocol

The bytes on the wire. The framing is the library's authority — see
[`iouring-net-lib/wiki/network/packet_framing.md`](../../../engine-uring/doc/network/)
when it lands. This document specifies the **product's contract**
with that framing: ID ranges, payload conventions, size budgets, and
the parity claim against the Windows reference.

---

## Frame layout

```
 byte 0  1  2  3   4 ...                                size-1
       ┌────────┬────────┬───────────────────────────────────┐
       │  size  │   id   │              payload              │
       └────────┴────────┴───────────────────────────────────┘
        uint16    uint16        size - 4 bytes
       (LE)      (LE)
```

- **`size`** — total frame length in bytes, including the 4-byte
  header. Range: `[4, 65535]`. `size == 4` means an empty payload
  (header-only ping).
- **`id`** — packet ID, looked up in the dispatcher table. See [ID
  ranges](#packet-id-ranges) below.
- **`payload`** — `size - 4` bytes of packet-specific body. Layout is
  determined entirely by the schema entry for `id`.

**Endianness:** little-endian for the header and for every integral
field in payloads. Matches the Windows reference and matches
`x86_64` / `aarch64` native order on every supported deployment
target, so no byte-swapping is needed in practice.

**Alignment:** payload reads happen through **per-field `memcpy` at
schema-derived wire offsets**, not via a whole-struct copy. The
generated `*_body` POD is a handler-facing carrier (naturally aligned,
with whatever padding the C++ ABI chooses); the wire bytes are walked
independently using `_wire_offset_*` constants emitted from the schema.
This is load-bearing: a packet like `{ uint8_t dir; uint16_t x; uint16_t
y; }` is **5 bytes on the wire** but `sizeof` for the handler struct is
typically **6** because of compiler-inserted padding. A whole-struct
`memcpy` would mismatch and reject every well-formed frame. See
[`05-codegen.md`](05-codegen.md) § "Wire layout vs C++ struct layout"
for the codegen rule.

---

## Magic byte: explicitly NOT carried over

The Windows reference (`SelectServer/TestSerialize/packets.json:3`)
declares a `header_code` of `0x89` that prefixes every frame. **We do
not carry this byte.**

Rationale (mirrors the library's
[`wiki/network/packet_framing.md`](../../../engine-uring/doc/network/)
decision):
- The packet ID already gates "is this byte stream meaningful?" — an
  unrecognized ID closes the session.
- A magic byte exists to detect protocol misalignment after a partial
  read, but TCP guarantees in-order delivery; the ring-buffer-based
  deframer either has the full frame or doesn't. A misalignment would
  be a library bug, not a wire condition.
- Dropping the byte saves one branch per frame and matches the
  protocol's intent.

If a future deployment needs framing over a non-TCP transport
(UDP, QUIC datagrams), the magic byte argument may resurface and a
versioned framing variant gets added then. Not v1.

---

## Packet ID ranges

### v1 — flat 16-bit namespace, no receiver-side range enforcement

In v1, packet IDs are **a flat `uint16_t` namespace**. There is no
direction-by-range rule and no receiver-side range check. The only
authority over "is this ID legal in this direction?" is the
dispatcher's registration table:

- The server dispatcher registers handlers for the C2S IDs in
  `packets.json`; an inbound frame whose `id` is not in that table
  closes the session via the unknown-id handler (see
  [`wiki/server/dispatch.md`](../wiki/server/dispatch.md)).
- The client dispatcher registers handlers for the S2C IDs; same rule
  in the other direction.

v1 IDs (`10, 12, 20, 22, 24` for C2S; `0, 1, 2, 11, 13, 21, 23, 25,
30, 251` for S2C) are inherited from the Windows reference for
wire-parity. **These IDs are legal at every layer**; nothing in v1
treats them as illegal or out-of-range.

### v2 — design target for a structured range scheme (not v1 enforcement)

A future v2 schema migration plans to remap into structured
ranges:

```
0x0000 ───────── 0x00FF   reserved (control / handshake)
0x0100 ───────── 0x7FFF   c2s (client → server)
0x8000 ───────── 0xFEFF   s2c (server → client)
0xFF00 ───────── 0xFFFE   reserved (diagnostics / debug)
0xFFFF                    reserved (sentinel, must never appear on wire)
```

Under v2, direction would be encoded in the ID range itself, allowing
receivers to fast-reject misdirected packets before the dispatcher
lookup. **This is a v2 design target, NOT a v1 rule.** A v1
implementation that adds a range check on receive will reject every
v1 packet (S2C IDs `0, 1, 2, 11, …` violate the proposed C2S range).
Do not add range-based rejection until the schema is renumbered.

**Migration noise is intentional.** v2's primary schema breaks parity
with Windows-reference clients. A reviewer following the cross-platform
proof should test against v1 binaries. Any v2 compatibility mode must
explicitly *select* the v1 schema (`--schema v1`, or equivalent) —
v2 is not parity-by-default. The v2 schema gets its own document at
that point; this section will then move from "design target" to
"active rule."

The generated `packet_ids.h` exposes the IDs as a
strongly-typed enum:

```cpp
enum class packet_id : uint16_t {
  CS_MOVE_START = 10,
  CS_MOVE_STOP  = 12,
  // ...
  SC_DAMAGE     = 30,
  SC_SYNC       = 251,
};
```

---

## Payload conventions

### Field types

| Schema type | Byte width | C++ type    | Notes                                            |
|-------------|------------|-------------|--------------------------------------------------|
| `uint8_t`   | 1          | `uint8_t`   |                                                  |
| `uint16_t`  | 2          | `uint16_t`  | Little-endian on wire                            |
| `uint32_t`  | 4          | `uint32_t`  | Little-endian on wire                            |
| `int8_t`    | 1          | `int8_t`    | Two's complement                                 |
| `int16_t`   | 2          | `int16_t`   | Little-endian, two's complement                  |
| `int32_t`   | 4          | `int32_t`   | Little-endian, two's complement                  |

`uint64_t` / `int64_t` are deliberately omitted from v1. If a packet
needs a 64-bit value, split it into two 32-bit fields and have the
handler reconstruct — this is a forcing function against schema
sprawl.

Variable-length fields (strings, byte arrays) are **not in v1**. The
Windows reference doesn't use them either, so wire-parity holds. v2
adds `string` (length-prefixed UTF-8) and `bytes` (length-prefixed
opaque) types.

### Field ordering

Fields appear in the payload in the order they are declared in the
schema's `fields` array. The generators do not reorder for alignment;
the schema author owns layout.

### Per-packet size budget

| Constraint                              | Limit       | Enforcement              |
|-----------------------------------------|-------------|--------------------------|
| Maximum frame size                      | 65 535      | `size` is `uint16_t`     |
| Maximum payload size                    | 65 531      | derived                  |
| Recommended max payload                 | 1 400 bytes | one MTU minus headers    |
| Hard error if exceeded                  | 65 532+     | deframer closes session  |

Schemas that would generate a payload exceeding 1 400 bytes should
either split into multiple packets or be re-thought. The library's
`packet_framing` will not refuse a 65 KB payload, but the network
performance argument flips when one packet stalls the queue for an
RTT.

---

## Parity with the Windows reference — what it means and what it doesn't

### The framing header is intentionally different — and that matters

A common mistake (made in early drafts of this doc) is to call the
Linux framing "the Windows reference framing minus 0x89." It is not.

The **Windows reference** framing is a **3-byte header**, as
declared at
[`~/CLionProjects/SelectServer/TestSerialize/Protocol.h:40-45`](../../SelectServer/TestSerialize/Protocol.h)
and emitted by
[`Proxy.cpp:9-15`](../../SelectServer/TestSerialize/Proxy.cpp):

```
byte 0     1            2            3 ...
    ┌──────┬────────────┬────────────┬─────────────────┐
    │ 0x89 │ payload    │ packet     │   payload       │
    │ magic│ size (u8)  │ type (u8)  │   (size bytes)  │
    └──────┴────────────┴────────────┴─────────────────┘
```

The **Linux framing** designed in this repo is a **4-byte header**,
as defined in [§ "Frame layout"](#frame-layout) above:

```
byte 0   1    2   3    4 ...
    ┌────────┬────────┬─────────────────┐
    │ total  │ packet │     payload     │
    │  size  │   id   │  (size-4 bytes) │
    │ u16 LE │ u16 LE │                 │
    └────────┴────────┴─────────────────┘
```

This is a **deliberate upgrade**: the Linux header gives us 65 535
packet IDs (vs Windows' 256) and 65 531 byte payloads (vs Windows'
255). The framing primitive in
[`iouring-net-lib`](../../../engine-uring/doc/network/)
ships this layout, not the reference's.

Stripping 0x89 alone does NOT yield a Linux-parseable frame; you
also have to re-encode the size and id fields. The
"header-normalization" step in the replay harness does both.

### Parity unit: payload bytes per packet name, not whole frames

Given the header divergence, the parity claim is **per-packet
payload-byte parity**:

> For a packet `X` with the same field values, the payload bytes
> emitted by the Linux client's proxy are byte-identical to the
> payload bytes that follow the 3-byte Windows header on the wire
> in the reference's `Proxy.cpp`.

That is, fields serialize identically; only the wrapping header
differs. The replay harness validates this by normalizing the
Windows frame's header to the Linux header and then comparing
payload bytes for the named packet.

### Preconditions for parity (P1–P6)

The payload-byte parity claim holds **only** when:

1. **Schema is identical.** Same `packets.json`: same packet names,
   same `id` numeric values (subject to P3), same field declaration
   order, same field widths. Adding, removing, renumbering, or
   reordering fields on either side breaks parity for that packet.
2. **Schema IDs fit the Windows constraint.** Every `id` in the
   schema is ≤ 255 (Windows `type` is `uint8_t`). The v1 schema
   satisfies this (max id = 251 for `SC_SYNC`). When v2 raises IDs
   into the 16-bit space, Windows-trace parity for affected packets
   ends.
3. **Schema payload sizes fit the Windows constraint.** Every
   packet's `_wire_size` is ≤ 255 (Windows `size` is `uint8_t`).
   The v1 schema's largest packet is 10 bytes; well within range.
4. **Host platform is little-endian.** Both the Windows reference
   build (MSVC x64) and the Linux target (x86_64 or aarch64 LE,
   per [§ "Endianness"](#frame-layout)) are LE; per-field wire
   offsets match native byte order with no swap. A future big-endian
   target needs explicit `load_le<T>` helpers (see
   [`05-codegen.md`](05-codegen.md) § "Endianness").
5. **Field byte positions verified against captured payloads.** The
   codegen emits `_wire_offset_<field>` from the schema; the
   cross-platform test compares these against the actual byte
   positions of each field in a recorded Windows payload. This
   guards against any schema-vs-reference drift in field
   serialization (e.g., `Packet::Put` in
   [`Packet.h:42-47`](../../SelectServer/TestSerialize/Packet.h)
   does straight `memcpy` per field, matching our per-field design;
   but if a future reference change introduced bit-packing or
   reordering, this assertion would catch it).
6. **Two's-complement encoding for signed fields.** v1's schema
   uses unsigned types only. If `int8_t` / `int16_t` / `int32_t`
   get added, the parity tests must include boundary fixtures
   (-128, 0, 127, etc.) to lock in two's-complement encoding.

### What follows from those preconditions, and only that

- **(A) Header-normalized trace replay is supported in v1.** A
  Windows-recorded packet trace is fed through a normalization step
  that parses `[0x89][u8 size][u8 type]`, re-emits
  `[u16 total_size=size+4][u16 id=type]`, then hands the result to
  the Linux deframer. Payload bytes pass through unchanged. The
  Linux dispatcher fires the correct handler. This is the supported
  cross-platform proof. See
  [`06-test-strategy.md`](06-test-strategy.md) § "Cross-platform
  replay" for the harness.

### What we explicitly do NOT guarantee

- **(B) Stripping 0x89 alone is NOT sufficient.** "Strip the magic
  byte and replay" misses the `uint8 size | uint8 type` → `uint16
  size | uint16 id` recoding. The Linux deframer will misparse the
  resulting bytes. **Use the full header normalization or do not
  call it parity.**
- **(C) An unmodified Windows client speaking end-to-end to the
  Linux server is NOT supported in v1.** The Windows client emits
  the 3-byte Windows header on every frame; the Linux server's
  deframer parses bytes 0–3 as a 4-byte Linux header and rejects.
  Two paths could lift this restriction:
  - Reconfigure the Windows client build to emit the Linux header
    layout, OR
  - Add a deframer compatibility mode on the Linux side that
    detects 0x89 and decodes a Windows-format frame.
  v1 ships neither. If demonstrating live cross-platform interop
  becomes important, building the second path (a compat deframer)
  is the lower-risk option since it does not require touching the
  reference build system.
- **(D) The reverse direction (Linux peer → unmodified Windows
  peer) is NOT supported in v1.** Same reason. Would require a
  reference-compatible writer/gateway that emits
  `[0x89][u8 payload_size][u8 type]` and stays within Windows'
  255-byte limits.
- *Compile-time* interop between the projects. The Windows
  reference's `Proxy.cpp` is `__declspec`-decorated and uses
  `WSASend`; our `client_proxy.cpp` is a `co_await session.send(...)`.
  They produce the same payload bytes (under preconditions), not
  the same source.
- Schema-evolution compatibility. Adding a field on either side is
  a breaking change for the other; no version negotiation in v1.
- Runtime ABI parity — `__declspec(align)`, MSVC packing pragmas,
  and similar Windows-build artifacts are not preserved.

---

## Framing primitive — library side

The product never parses headers itself. The library provides
(when implemented):

```cpp
// from iouring_net/packet_framing.h (planned)
struct frame_view {
  uint16_t id;
  std::span<const std::byte> body;  // size - 4 bytes
};

iouring_net::task<expected<frame_view, framing_error>>
read_frame(iouring_net::session& s);
```

The dispatcher feeds `frame.id` and `frame.body` into the generated
stub. Misframing (size < 4, payload truncated, etc.) produces a
`framing_error`; the session closes per
[`01-architecture.md`](01-architecture.md) § "Failure model."

---

## Cross-references

- [`01-architecture.md`](01-architecture.md) — how the dispatcher
  consumes `frame_view`.
- [`05-codegen.md`](05-codegen.md) — how the generators produce
  POD bodies that match this layout.
- [`wiki/proto/packets.md`](../wiki/proto/packets.md) — the schema
  file's structure and how to add a new packet.
- [`iouring-net-lib/wiki/network/packet_framing.md`](../../../engine-uring/doc/network/)
  — the framing primitive's spec (library-side; not yet implemented).
