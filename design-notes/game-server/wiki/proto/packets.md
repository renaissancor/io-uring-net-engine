# `proto/packets.json` — wire schema

## Purpose

The source of truth for the wire protocol. Every byte that crosses
the socket is described by an entry here. Editing this file regenerates
all of `generated/` and triggers an integration-test rebuild.

For *what the bytes look like on the wire*, see
[`../../docs/04-protocol.md`](../../docs/04-protocol.md). For *how
this file becomes C++*, see [`../../docs/05-codegen.md`](../../docs/05-codegen.md).
This wiki page documents the file's **content and conventions**.

## Top-level shape

```json
{
  "config":  { ... },        // global codegen settings
  "types":   { ... },        // type-name → byte-width map
  "c2s":     [ ... ],        // packets the client sends; server handles
  "s2c":     [ ... ]         // packets the server sends; client handles
}
```

### `config`

| Key                | Value                                | Effect                                                  |
|--------------------|--------------------------------------|---------------------------------------------------------|
| `handler_prefix`   | `"handle_"`                          | Prefix for hand-written handler names                   |
| `namespace`        | `"iouring_server"`                   | Wrap all generated code in this C++ namespace           |
| `gen_version`      | `"1.0"`                              | Schema-format version; codegen rejects unknown          |

Removed vs reference: `header_code` (no magic byte; see protocol doc),
`handler_class` (free functions, not class methods).

### `types`

Whitelist of allowed field types and their byte widths. The codegen
refuses to emit a packet whose field cites a type not in this map.

```json
"types": {
  "uint8_t":  1,
  "uint16_t": 2,
  "uint32_t": 4,
  "int8_t":   1,
  "int16_t":  2,
  "int32_t":  4
}
```

64-bit types and variable-length types (`string`, `bytes`) are
intentionally absent in v1 — see [`../../docs/04-protocol.md`](../../docs/04-protocol.md)
§ "Field types".

### Packet entries

```json
{
  "name":    "CS_MOVE_START",       // C++ identifier; UPPER_SNAKE_CASE
  "id":      10,                    // wire ID, 0–65535, unique within direction
  "handler": "MoveStart",           // c2s only: documentation tag, not load-bearing
  "send":    "BroadcastExcept",     // s2c only: routing hint for handlers
  "fields": [
    { "name": "dir", "type": "uint8_t"  },
    { "name": "x",   "type": "uint16_t" },
    { "name": "y",   "type": "uint16_t" }
  ]
}
```

#### Per-field rules

- `name` is a valid C++ identifier — `[a-z][a-zA-Z0-9_]*`. The
  generator does not transform names; what you write is what hits
  the POD struct.
- `type` is a string key into `types`. Codegen fails if the type
  doesn't exist.
- Field order in the JSON array is the field order in the POD struct
  and on the wire. **Do not rely on alphabetical or any other
  reordering** — the schema author owns layout.

#### Per-packet rules

- `name` is `UPPER_SNAKE_CASE`. C2S packets start with `CS_`, S2C
  with `SC_`, by convention enforced by `rpc_gen.py`'s validator.
- `id` is unique within `c2s` and within `s2c`. Cross-direction
  uniqueness is required in v1 (will be relaxed once the ID-range
  scheme in [`../../docs/04-protocol.md`](../../docs/04-protocol.md)
  is enforced).
- `handler` (c2s) is purely documentary in this port — kept because
  it shortens grep across the reference and this repo. The generator
  ignores it.
- `send` (s2c) is one of `Unicast`, `Broadcast`, `BroadcastExcept`.
  Indicates the typical routing of this packet so handler authors
  pick the right helper from `server/handlers/shared.h`. Not enforced
  by the codegen — a handler is free to send a "Broadcast"-tagged
  packet to one peer.

## v1 inventory

The v1 schema mirrors `~/CLionProjects/SelectServer/TestSerialize/packets.json`
for wire-parity:

| Direction | ID  | Name                    | Fields                                   |
|-----------|-----|-------------------------|------------------------------------------|
| c2s       | 10  | `CS_MOVE_START`         | dir:u8, x:u16, y:u16                     |
| c2s       | 12  | `CS_MOVE_STOP`          | dir:u8, x:u16, y:u16                     |
| c2s       | 20  | `CS_ATTACK1`            | dir:u8, x:u16, y:u16                     |
| c2s       | 22  | `CS_ATTACK2`            | dir:u8, x:u16, y:u16                     |
| c2s       | 24  | `CS_ATTACK3`            | dir:u8, x:u16, y:u16                     |
| s2c       | 0   | `SC_CREATE_MY_CHARACTER`| id:u32, dir:u8, x:u16, y:u16, hp:u8      |
| s2c       | 1   | `SC_CREATE_OTHER_CHARACTER` | id:u32, dir:u8, x:u16, y:u16, hp:u8  |
| s2c       | 2   | `SC_DELETE_CHARACTER`   | id:u32                                   |
| s2c       | 11  | `SC_MOVE_START`         | id:u32, dir:u8, x:u16, y:u16             |
| s2c       | 13  | `SC_MOVE_STOP`          | id:u32, dir:u8, x:u16, y:u16             |
| s2c       | 21  | `SC_ATTACK1`            | id:u32, dir:u8, x:u16, y:u16             |
| s2c       | 23  | `SC_ATTACK2`            | id:u32, dir:u8, x:u16, y:u16             |
| s2c       | 25  | `SC_ATTACK3`            | id:u32, dir:u8, x:u16, y:u16             |
| s2c       | 30  | `SC_DAMAGE`             | attackerID:u32, targetID:u32, hp:u8      |
| s2c       | 251 | `SC_SYNC`               | id:u32, x:u16, y:u16                     |

Note the IDs cluster around the reference's numeric scheme. A future
v2 schema migration is the design target for remapping into the range
scheme in [`../../docs/04-protocol.md`](../../docs/04-protocol.md) §
"Packet ID ranges" — v1 ships these IDs as-is, with no range
enforcement on either receiver.

## Evolution rules

| Change                                           | Compatibility                          |
|--------------------------------------------------|----------------------------------------|
| Add a new packet                                 | Forward-compatible — new ID, new code  |
| Add a field to an existing packet                | **Breaking**. Bump server & client.    |
| Remove a field                                   | **Breaking**.                          |
| Reorder fields                                   | **Breaking**.                          |
| Renumber `id`                                    | **Breaking** — wire parity lost.       |
| Rename `name`                                    | Source-breaking (handler renames) but wire-compatible |
| Change `send` mode                               | Documentation-only                     |

There is **no schema version negotiation** in v1. Bumping the schema
is a coordinated rebuild. v2 might add a version handshake; not now.

## Reference origin

- `~/CLionProjects/SelectServer/TestSerialize/packets.json` — the
  port source. Same shape, magic byte and handler-class removed,
  type-name `type` field renamed to `id` to avoid C++ keyword
  shadowing.

## Cross-references

- [`../../docs/04-protocol.md`](../../docs/04-protocol.md) — the wire
  encoding this schema describes
- [`../../docs/05-codegen.md`](../../docs/05-codegen.md) — what the
  generators do with this file
- [`../codegen/pipeline.md`](../codegen/pipeline.md) — per-script
  contracts
- [`../server/handlers.md`](../server/handlers.md) — what
  hand-written code corresponds to each `c2s` entry
