# 05 — Codegen

The `proto/packets.json` schema is the source of truth for the wire
protocol. The codegen pipeline turns it into C++:

```
                       proto/packets.json (input)
                                 │
                                 ▼
                         codegen/rpc_gen.py   (orchestrator)
                  ┌──────────┬───┴───┬──────────┐
                  ▼          ▼       ▼          ▼
            ids_gen.py  layout_gen.py  stub_gen.py  proxy_gen.py
                  │          │       │          │
                  ▼          ▼       ▼          ▼
       generated/packet_ids.h        server_stub.{h,cpp}
                  packet_layout.h     client_proxy.{h,cpp}
                       ▲       ▲
                       │       │
                       │       └─ included by client_proxy.h
                       └───────── included by server_stub.h
```

`packet_layout.h` is the shared source of truth for wire-size and
field-offset constants. Both the stub (decoder) and proxy (encoder)
`#include` it, which is why the encoder and decoder can never drift —
they read the same constants.

The architecture ports
`~/CLionProjects/SelectServer/TestSerialize/{rpc_gen,stub_gen,proxy_gen}.py`
to Linux, with five deviations from the reference:

- Python 3.10+ idioms (the reference predates `match`).
- UTF-8 file I/O throughout (reference is Windows codepage-dependent).
- **Framing header upgraded**: the reference emits a 3-byte
  `[0x89][u8 payload_size][u8 type]` header (see
  `~/CLionProjects/SelectServer/TestSerialize/Protocol.h:40-45`); this
  codegen emits the Linux 4-byte `[u16 total_size][u16 id]` header
  defined in [`04-protocol.md`](04-protocol.md). The 0x89 magic byte is
  dropped, and the size/type fields are widened to `uint16_t`,
  giving 65 535 IDs and 65 531-byte payloads vs the reference's
  256 / 255. Per-packet payload bytes remain identical (parity
  unit); the wrapping frame header is intentionally different.
- The wire-layout constants are split into a dedicated
  `layout_gen.py` so encoder and decoder share constants by
  construction (a shared `packet_layout.h`).
- Schema validation lives in the orchestrator (reference has none).

---

## Schema reference — `proto/packets.json`

```json
{
  "config": {
    "handler_prefix": "handle_",
    "namespace":      "iouring_server"
  },

  "types": {
    "uint8_t":  1,
    "uint16_t": 2,
    "uint32_t": 4,
    "int8_t":   1,
    "int16_t":  2,
    "int32_t":  4
  },

  "c2s": [
    {
      "name":    "CS_MOVE_START",
      "id":      10,
      "handler": "MoveStart",
      "fields": [
        { "name": "dir", "type": "uint8_t"  },
        { "name": "x",   "type": "uint16_t" },
        { "name": "y",   "type": "uint16_t" }
      ]
    },
    ...
  ],

  "s2c": [
    {
      "name":  "SC_MOVE_START",
      "id":    11,
      "send":  "BroadcastExcept",
      "fields": [
        { "name": "id",  "type": "uint32_t" },
        { "name": "dir", "type": "uint8_t"  },
        { "name": "x",   "type": "uint16_t" },
        { "name": "y",   "type": "uint16_t" }
      ]
    },
    ...
  ]
}
```

### Differences from the reference schema

| Field                  | Reference (`SelectServer`) | This project        | Why                                                              |
|------------------------|----------------------------|---------------------|------------------------------------------------------------------|
| `config.header_code`   | `"0x89"`                   | **removed**         | Magic byte dropped — see [`04-protocol.md`](04-protocol.md)      |
| `config.handler_class` | `"Logic::GetInstance()"`   | **removed**         | Handlers are free functions, not singleton-class methods         |
| `config.handler_prefix`| `"On"`                     | `"handle_"`         | Matches the project's C++ style guide (snake_case)               |
| `config.namespace`     | (none)                     | `"iouring_server"`  | Generated code is namespaced; reference dumped into global scope |
| `c2s[].type`           | numeric ID                 | renamed to `id`     | `type` collides with C++ vocabulary; `id` is unambiguous         |
| `s2c[].send`           | `"Unicast"`/`"Broadcast"`  | preserved           | Reference's send-mode tag retained; resolved at handler level    |

### Field invariants enforced by `rpc_gen.py`

The orchestrator validates the schema before invoking the emitters:
- Every packet has unique `id` within its direction. Cross-direction
  uniqueness is also enforced as a **debug/readability invariant** —
  v1 does not technically require it (server and client dispatchers
  are separate tables and could legally share an ID), but enforcing
  uniqueness keeps grep, log lines, and stack-trace inspection
  unambiguous. This rule is independent of any v2 range scheme. See
  [`04-protocol.md`](04-protocol.md) § "Packet ID ranges" for the
  v1-vs-v2 distinction.
- Every `type` referenced in `fields` exists in `types`.
- `name` is a valid C++ identifier (regex `[A-Z][A-Z0-9_]*`).
- No packet's total payload size exceeds `65 531` bytes.
- `s2c[].send` is one of `Unicast`, `Broadcast`, `BroadcastExcept`.

A failed invariant is a hard error: `rpc_gen.py` exits non-zero with
a message naming the offending packet, and the CMake configure step
fails (see [`03-cmake.md`](03-cmake.md)).

---

## What each generator emits

### `codegen/rpc_gen.py` (orchestrator)

Mirrors the reference at
`~/CLionProjects/SelectServer/TestSerialize/rpc_gen.py:1` (14 lines —
just imports the other two modules and calls them in sequence).

```python
# Pseudo:
import json, pathlib, sys
import ids_gen, layout_gen, stub_gen, proxy_gen

def main(schema_path, out_dir):
    schema = json.loads(pathlib.Path(schema_path).read_text("utf-8"))
    validate(schema)
    ids_gen.emit(schema,    out_dir)     # packet_ids.h
    layout_gen.emit(schema, out_dir)     # packet_layout.h  (must run before stub/proxy)
    stub_gen.emit(schema,   out_dir)     # server_stub.{h,cpp}
    proxy_gen.emit(schema,  out_dir)     # client_proxy.{h,cpp}

if __name__ == "__main__":
    # argparse: --schema PATH, --out PATH
    ...
```

`ids_gen` and `layout_gen` are sibling helper modules — both are
small (~40 lines each) and emit a single header. They run before
`stub_gen` and `proxy_gen` because the stub/proxy headers `#include`
their outputs.

### `codegen/stub_gen.py`

Reference: `~/CLionProjects/SelectServer/TestSerialize/stub_gen.py`
(76 lines).

Emits two files:

**Wire layout vs C++ struct layout — the rule that drives the codegen.**

The generated `*_body` struct is a *carrier* for handler ergonomics, not
a wire image. `CS_MOVE_START_body` declared as
`{ uint8_t dir; uint16_t x; uint16_t y; }` has `sizeof == 6` on every
ABI we support (1 byte of padding between `dir` and `x`), while the
on-the-wire payload is **5 bytes**. **The decoder never decodes a
whole struct with one `memcpy`.** Instead, the generator emits per-field
loads at compile-time-known wire offsets. The C++ struct's `offsetof`
and `sizeof` are deliberately *not* the source of truth; the schema is.

For every packet, the generator emits a `_wire_size` constant and a
table of `_wire_offset_*` constants computed from the schema's field
declaration order and the type-width map. The handler-facing struct
remains naturally aligned; the wire walk is independent of it.

**Wire constants live in a shared header.** All `_wire_size` and
`_wire_offset_*` constants are emitted into a single
`generated/packet_layout.h` that both `server_stub.{h,cpp}` and
`client_proxy.{h,cpp}` include. Stub and proxy share the same
constants by construction — encode and decode cannot drift because
they pull from the same source. The proxy never has to depend on
stub declarations.

**`generated/packet_layout.h`** (shared by stub and proxy)
```cpp
#pragma once
#include <cstddef>

namespace iouring_server::generated {

// Wire constants — emitted from the schema, independent of any C++
// layout. The fact that sizeof(<Pkt>_body) != <Pkt>_wire_size is
// expected and load-bearing.

inline constexpr std::size_t CS_MOVE_START_wire_size       = 5;
inline constexpr std::size_t CS_MOVE_START_wire_offset_dir = 0;
inline constexpr std::size_t CS_MOVE_START_wire_offset_x   = 1;
inline constexpr std::size_t CS_MOVE_START_wire_offset_y   = 3;
// ... one block per packet, both c2s and s2c ...

} // namespace iouring_server::generated
```

**`generated/server_stub.h`**
```cpp
#pragma once
#include <span>
#include <cstddef>
#include <cstdint>
#include <iouring_net/session.h>
#include <iouring_net/task.h>
#include "packet_ids.h"
#include "packet_layout.h"

namespace iouring_server::generated {

// One handler-facing POD per c2s packet — laid out for the C++
// compiler, NOT for the wire.
struct CS_MOVE_START_body {
  uint8_t  dir;
  uint16_t x;
  uint16_t y;
};
// ...

// Generated stubs that the dispatcher table points at
iouring_net::task<void>
stub_CS_MOVE_START(iouring_net::session& s,
                    uint16_t id,
                    std::span<const std::byte> body);
// ... `id` is the dispatcher-passed packet ID, always equal to
// `packet_id::CS_MOVE_START` for this stub. It is unused by the stub
// itself but kept in the signature so the dispatcher's `reject_unknown`
// slot (which shares the same function-pointer type) can report which
// id was rejected.

// User-implemented (declared here, defined in server/handlers/*.cpp).
// Signature is canonical — must match wiki/server/handlers.md exactly:
//   (session&, uint16_t id, X_body).
iouring_net::task<void>
handle_CS_MOVE_START(iouring_net::session& s,
                      uint16_t id,
                      CS_MOVE_START_body body);
// ...

// Called once at startup from server/main.cpp.
// packet_dispatcher lives in iouring_server::packet_dispatcher (NOT
// inside iouring_server::generated). The forward-declaration with
// `class` is intentional: it declares the SAME type as the one in
// the dispatcher header, because we open the parent namespace here.
} // namespace iouring_server::generated

namespace iouring_server {
  class packet_dispatcher;                // real definition in server/dispatch.h
}

namespace iouring_server::generated {

void register_all(::iouring_server::packet_dispatcher&);

} // namespace iouring_server::generated
```

**`generated/server_stub.cpp`**
```cpp
#include "server_stub.h"
// Pull in the dispatcher's real definition. The product CMake adds
// the server/ source dir to iouring_server_stub's include path, so
// "<server/dispatch.h>" resolves via the source tree, NOT via a
// relative `..` walk out of the build-tree generated/ directory.
#include <server/dispatch.h>
#include <cstring>

namespace iouring_server::generated {

iouring_net::task<void>
stub_CS_MOVE_START(iouring_net::session& s,
                    uint16_t /*id*/,                  // always packet_id::CS_MOVE_START here
                    std::span<const std::byte> body)
{
  // Wire-size check — NOT sizeof(struct). The struct's sizeof is
  // typically 6 because of padding; the wire payload is 5.
  if (body.size() != CS_MOVE_START_wire_size) {
    // Wire-size mismatch is a protocol error. The stub closes the
    // session via session::reject_and_close(); the per-session loop
    // in lifecycle.cpp observes the closed session and tears down
    // its coroutine on the next recv attempt. See wiki/server/
    // dispatch.md § "Failure routing" and wiki/server/lifecycle.md
    // § "Per-session loop" for the full path.
    s.reject_and_close(iouring_net::framing_error::bad_payload_size);
    co_return;
  }

  // Per-field memcpy at schema-derived offsets. memcpy handles
  // unaligned wire reads correctly on every supported ABI and
  // compiles to single loads when the destination is aligned.
  //
  // Zero-initialize so any padding the C++ ABI inserts is defined.
  // Without `{}` the padding is indeterminate and propagates through
  // pass-by-value — MSan can flag that even though no field is
  // actually unread.
  CS_MOVE_START_body parsed{};
  std::memcpy(&parsed.dir, body.data() + CS_MOVE_START_wire_offset_dir,
              sizeof(parsed.dir));
  std::memcpy(&parsed.x,   body.data() + CS_MOVE_START_wire_offset_x,
              sizeof(parsed.x));
  std::memcpy(&parsed.y,   body.data() + CS_MOVE_START_wire_offset_y,
              sizeof(parsed.y));

  co_await handle_CS_MOVE_START(s, id, parsed);
}
// ... one per c2s packet

void register_all(::iouring_server::packet_dispatcher& d)
{
  d.register_handler(static_cast<uint16_t>(packet_id::CS_MOVE_START),
                     &stub_CS_MOVE_START);
  // ...
}

} // namespace iouring_server::generated
```

Why per-field, not whole-struct:

- **Padding immunity.** `sizeof(CS_MOVE_START_body)` is 6 on every
  supported ABI; the wire is 5. A whole-struct `memcpy` would mismatch
  by one byte and reject every well-formed frame.
- **No `__attribute__((packed))`.** Packed structs invite UB on
  unaligned member access (a packed `uint16_t` taken by reference
  becomes a misaligned pointer on architectures stricter than x86).
  Per-field copy avoids the trap entirely.
- **No `offsetof` dependency.** The wire layout is derived from the
  schema and emitted as `_wire_offset_*` constants. The C++ struct's
  `offsetof` is irrelevant to the codec — different compilers /
  different ABIs cannot break wire parity.
- **Endianness is a single-point edit.** Today little-endian wire ==
  native CPU; if we ever target a big-endian platform, the per-field
  memcpy sites are the only place that changes. The v2 evolution is to
  swap the raw `std::memcpy` for generator-emitted `load_le<T>` /
  `store_le<T>` helpers — the per-field pattern already has the right
  shape for that change.

**Do not replace this with a single whole-struct `memcpy` "for
performance."** Compilers already lower fixed-size per-field `memcpy`
calls to direct loads/stores at `-O2`; there is no codegen win on
top of that, and the padding bug it reintroduces is a parser-wide
correctness regression. ASan/UBSan will not catch it because the
arithmetic is internally consistent — the proxy and stub disagree
about a byte that "exists" in C++ but not on the wire.

**Why not generate the constants with `offsetof` against a packed
struct?** Because that brings the alignment problem back through a
different door. The schema-derived constants are 100% under the
generator's control and never depend on the C++ compiler at all.

**v2 variable-width fields.** The "compile-time constant offset for
every field" pattern only works for v1's fixed-width fields. When v2
adds `string` (length-prefixed UTF-8) and `bytes` (length-prefixed
opaque), fixed offsets after the first variable-width field stop
being computable at compile time. The generalized v2 form:
- Emit `X_min_wire_size` (sum of fixed-width prefix only).
- Emit a per-packet `encoded_size(<fields...>)` helper for the
  encoder.
- Decoder becomes a cursor walk: fixed fields read at compile-time
  offsets, variable fields read a length, bounds-check
  (`length <= remaining`), advance the cursor, materialize either an
  owning `std::string` / `std::vector<std::byte>` or a span bound to
  the recv buffer's lifetime.

The v2 generator changes shape; the v1 per-field idiom remains a
correct *subset* of it (fixed-only packets keep the constant
offsets). No v1→v2 rewrite of the encoder/decoder is wasted.

### `codegen/proxy_gen.py`

Reference: `~/CLionProjects/SelectServer/TestSerialize/proxy_gen.py`
(94 lines).

Emits `generated/client_proxy.{h,cpp}`. The proxy is the inverse of
the stub: client code calls structured functions, the proxy serializes
+ sends.

**`generated/client_proxy.h`**
```cpp
#pragma once
#include <span>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <iouring_net/session.h>
#include <iouring_net/session_handle.h>            // opaque routing handle
#include <iouring_net/task.h>
#include <iouring_net/error/expected.h>            // project's expected alias
#include "packet_ids.h"
#include "packet_layout.h"                          // shared wire constants

namespace iouring_server::generated {

// Send is routed via session_handle so the proxy is safe for both
// "send to my own session" (client: `my_session.handle()`) and "send
// to a peer" (server broadcast: handle iterated from service).
// Under v1 the handle's send() is a direct call; under v2 it hops
// to the recipient's reactor. The proxy code is unchanged either way.
iouring_net::task<iouring_net::expected<void, std::error_code>>
send_CS_MOVE_START(iouring_net::session_handle h,
                    uint8_t dir, uint16_t x, uint16_t y);
// ... one per c2s packet

// For s2c packets, the receiving stub still takes session& (its own
// receiving session is on the receiver's reactor by construction).
iouring_net::task<void>
stub_SC_MOVE_START(iouring_net::session& s,
                    uint16_t id,
                    std::span<const std::byte> body);
iouring_net::task<void>
handle_SC_MOVE_START(iouring_net::session& s,
                      uint16_t id,
                      SC_MOVE_START_body body);

} // namespace iouring_server::generated

// packet_dispatcher is defined in iouring_server::packet_dispatcher
// (NOT in generated::). Forward-declare in the parent namespace so
// the register_all signature below resolves correctly.
namespace iouring_server { class packet_dispatcher; }

namespace iouring_server::generated {

void register_all(::iouring_server::packet_dispatcher&);

} // namespace iouring_server::generated
```

**`generated/client_proxy.cpp`**
```cpp
#include "client_proxy.h"
#include <cstring>

namespace iouring_server::generated {

iouring_net::task<iouring_net::expected<void, std::error_code>>
send_CS_MOVE_START(iouring_net::session_handle h,
                    uint8_t dir, uint16_t x, uint16_t y)
{
  // Same wire constants the decoder uses, so encode/decode cannot drift.
  constexpr std::size_t header_size = 4;  // size(2) + id(2)
  constexpr std::size_t frame_size  = header_size + CS_MOVE_START_wire_size;

  std::byte buf[frame_size];
  const uint16_t size = static_cast<uint16_t>(frame_size);
  const uint16_t id   = static_cast<uint16_t>(packet_id::CS_MOVE_START);

  // Header
  std::memcpy(&buf[0], &size, sizeof(size));
  std::memcpy(&buf[2], &id,   sizeof(id));

  // Payload — per-field at schema-derived offsets, exact mirror of the
  // stub's decode. The handler-side struct's offsetof is intentionally
  // never consulted; the wire offsets ARE the source of truth.
  std::memcpy(&buf[header_size + CS_MOVE_START_wire_offset_dir],
              &dir, sizeof(dir));
  std::memcpy(&buf[header_size + CS_MOVE_START_wire_offset_x],
              &x,   sizeof(x));
  std::memcpy(&buf[header_size + CS_MOVE_START_wire_offset_y],
              &y,   sizeof(y));

  // Route via session_handle. In v1 this resolves to a direct
  // session::send on the same reactor. In v2, h.send(...) hops to
  // the recipient's reactor — the proxy code is unchanged.
  co_return co_await h.send(std::span(buf, frame_size));
}
// ...

} // namespace iouring_server::generated
```

---

## CMake integration (summary)

Codegen runs at *configure* time so that the IDE indexer sees the
generated headers before any compile happens. See
[`03-cmake.md`](03-cmake.md) § "Codegen integration" for the full
`cmake/codegen.cmake` script.

Re-run triggers (declared via `CMAKE_CONFIGURE_DEPENDS`):
- `proto/packets.json` mtime changes
- Any of `codegen/*.py` mtime changes

A schema edit triggers reconfigure → regenerate → rebuild. The
developer ergonomic loop is one `cmake --build build` away.

---

## Style mandates for the generators

These are non-negotiable across script revisions:

1. **Deterministic output.** Same `packets.json` → byte-identical
   generated files. No timestamps in headers, no `dict` iteration
   order leakage (use `sorted()` where order isn't fixed by the
   schema).
2. **No template engine.** The reference uses string concatenation;
   we do too. Pulling in Jinja for 200 lines of generation is
   overkill, and it adds a build-time PyPI dependency.
3. **Single-pass emission.** The generators do not parse C++ they
   previously wrote. If a refactor needs information from prior
   output, factor it through the schema instead.
4. **`-Wall -Wextra -Werror` clean.** Generated code passes the same
   warning bar as hand-written code. Unused-variable warnings, etc.,
   are bugs in the generator, not noise to silence.
5. **Comments in generated files name the schema entry.** The first
   line of every emitted function carries
   `// generated from packets.json: CS_MOVE_START (id=10)`. A grep
   from a stack trace back to the schema must be one step.

---

## Extending the schema — runbook for adding a new packet

1. Edit `proto/packets.json`. Add the entry to either `c2s` or `s2c`.
2. `cmake --build build` — codegen reruns automatically.
3. Implement the corresponding handler in `server/handlers/` (for c2s)
   or `client/` (for s2c). The forward declaration in the generated
   header tells the compiler what to expect.
4. Add an integration test under `tests/integration/` that exercises
   the new packet round-trip.
5. Commit `proto/packets.json` and the new handler in the same change.
   Do **not** commit anything under `generated/`.

---

## Reference origin

- `~/CLionProjects/SelectServer/TestSerialize/packets.json` — schema
  format
- `~/CLionProjects/SelectServer/TestSerialize/rpc_gen.py` (14 lines)
- `~/CLionProjects/SelectServer/TestSerialize/stub_gen.py` (76 lines)
- `~/CLionProjects/SelectServer/TestSerialize/proxy_gen.py` (94 lines)
- `~/CLionProjects/SelectServer/TestSerialize/RPC.md` — sparse Korean
  notes; not load-bearing for the port

---

## Cross-references

- [`04-protocol.md`](04-protocol.md) — the wire bytes the generators
  must produce.
- [`03-cmake.md`](03-cmake.md) — how the scripts are invoked.
- [`wiki/codegen/pipeline.md`](../wiki/codegen/pipeline.md) — per-script
  implementation spec.
- [`wiki/proto/packets.md`](../wiki/proto/packets.md) — schema as
  living document; what each field of each packet means.
