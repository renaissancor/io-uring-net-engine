# PacketHandler — DEFERRED out of library v1

**Status (2026-05-14): not part of `iouring-net-lib` v1.** This file
documents the prior design and the reasons it was pulled out of the
library, with pointers to the product-side specs that now own
packet-handler concepts.

## Why deferred

An earlier draft of this spec put `packet_handler_table`,
`packet_codec<Pkt>`, an unhandled-id default, and codec policy
(struct-equality decode, `is_trivially_copyable_v` constraint,
variable-length-payload TODO) inside `iouring-net-lib::net::`. A peer
review surfaced two problems with that placement:

1. **All of those decisions are product policy, not transport.** A
   different product on top of the same `io_uring` transport could
   want a wholly different dispatcher (priority-keyed, route-prefix,
   capability-based) and a wholly different codec (Cap'n Proto,
   FlatBuffers, hand-rolled bitfield). The library has no business
   pre-committing the product's stack to one shape.

2. **The codec example in this file recommended a whole-struct
   `memcpy` based on `sizeof(Pkt)`.** That is the exact padding bug
   the product-side codegen had to fix
   ([`iouring-net-server/docs/05-codegen.md`](../../../iouring-net-server/docs/05-codegen.md)
   § "Wire layout vs C++ struct layout"). Keeping it documented as
   the library way kept the trap loaded.

The boundary criteria in
[`../../docs/09-project-split.md`](../../docs/09-project-split.md)
§ "What lives where" now read: framing is library; dispatch and
codec are product.

## What the library v1 actually ships

Strictly the **framing primitive** — see
[`packet_framing.md`](packet_framing.md):

- `[uint16 size | uint16 id][payload]` wire format
- `frame_view{ size, id, payload_span }` decode of a single complete
  frame from a `SerialBuffer`/`ring_buffer`
- `packet_writer` that reserves the 4-byte header in a buffer, lets
  the caller append payload bytes, then back-patches `size`
- Validation: `size >= 4`, all `size` bytes present, every `uint16_t`
  id legal (every numeric value is transport-legal; semantic policy
  is dispatcher-side)

Nothing else from the original `packet_handler` design ships with
the library. No table, no codec template, no
`is_trivially_copyable_v` constraint, no unhandled-id behavior, no
exception policy.

## Where the deferred concepts moved

| Original library concept                | New owner                                                                     |
|-----------------------------------------|-------------------------------------------------------------------------------|
| `handler_fn` typedef                    | `iouring-net-server`: `wiki/server/dispatch.md` § "Interface"                  |
| `packet_handler_table` / `dispatch`     | `iouring-net-server`: `wiki/server/dispatch.md` (`packet_dispatcher`)          |
| Unhandled-id default                    | `iouring-net-server`: `wiki/server/dispatch.md` § "Why default-rejector …"     |
| `packet_codec<Pkt>::decode` / `encode`  | `iouring-net-server`: `docs/05-codegen.md` § "Wire layout vs C++ struct layout" |
| `is_trivially_copyable_v` requirement   | Dropped. Codegen emits per-field memcpy at schema-derived offsets — see above. |
| Variable-length codec strategy          | `iouring-net-server`: `docs/05-codegen.md` § "v2 variable-width fields"        |
| Codegen pipeline                        | `iouring-net-server`: `docs/05-codegen.md`, `wiki/codegen/pipeline.md`         |
| Test plan (handler registration, etc.)  | `iouring-net-server`: `docs/06-test-strategy.md`, `wiki/server/dispatch.md`    |

## Why this file is not just deleted

Keeping it as a `DEFERRED` note has three purposes:

1. Grep traffic from older docs/branches lands somewhere informative
   rather than 404'ing.
2. The "why deferred" reasoning is the historical artifact that
   prevents the next contributor from re-proposing the same library
   abstraction without reading the boundary criteria first.
3. The migration table above is the canonical pointer from library
   wiki → product wiki for everything formerly here.

If at some point a *second* product wants to consume this library
and there is enough shared dispatch shape to factor out, that
factoring lives at *the second product's join time*, not now. A
library-side abstraction with one user is exactly the over-abstraction
this defer was about.

## Cross-references

- [`packet_framing.md`](packet_framing.md) — what the library v1
  actually owns for the network layer.
- [`../../docs/09-project-split.md`](../../docs/09-project-split.md)
  § "What lives where" — the boundary criteria that put dispatch
  product-side.
- [`../../docs/00-overview.md`](../../docs/00-overview.md) §
  "Subsystem inventory" — lists `packet_handler` with status
  **Deferred** and a back-pointer to this note. That row is the
  intentional inventory entry; do not delete it. Any inventory row
  listing `packet_handler` with status `New` or `Port` (without
  Deferred) is stale.
- [`../../../iouring-net-server/wiki/server/dispatch.md`](../../../iouring-net-server/wiki/server/dispatch.md)
  — the product-side dispatcher.
- [`../../../iouring-net-server/docs/05-codegen.md`](../../../iouring-net-server/docs/05-codegen.md)
  — the product-side codec generator.
