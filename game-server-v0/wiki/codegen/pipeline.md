# `codegen/` — generator pipeline

## Purpose

The five Python scripts that turn `proto/packets.json` into C++.
This wiki page is the **per-script specification**;
[`../../docs/05-codegen.md`](../../docs/05-codegen.md) is the
high-level contract this implements.

## Script roles

| Script                  | LOC (target) | Role                                                       |
|-------------------------|--------------|------------------------------------------------------------|
| `codegen/rpc_gen.py`    | ~50          | Orchestrator: load schema, validate, call ids/layout/stub/proxy |
| `codegen/ids_gen.py`    | ~40          | Emit `generated/packet_ids.h` — strongly typed enum        |
| `codegen/layout_gen.py` | ~40          | Emit `generated/packet_layout.h` — wire-size + offset constants shared by stub and proxy |
| `codegen/stub_gen.py`   | ~120         | Emit `generated/server_stub.{h,cpp}`                       |
| `codegen/proxy_gen.py`  | ~140         | Emit `generated/client_proxy.{h,cpp}`                      |

Line counts are budgets, not strict caps. The Windows reference's
totals (14 + 76 + 94 = 184) are the floor; the Linux ports add
~80 lines for schema validation, namespace wrapping, and the
shared wire-layout header (`layout_gen.py`).

## `rpc_gen.py` (orchestrator)

```python
# Public CLI
python3 codegen/rpc_gen.py \
        --schema proto/packets.json \
        --out    build/generated/
```

### Behavior

1. Read and JSON-parse `--schema`.
2. Validate (see [Validation](#validation) below). Fail fast on
   violation; print the offending entry with `key=value` context.
3. Call `ids_gen.emit(schema, out_dir)`         → `packet_ids.h`.
4. Call `layout_gen.emit(schema, out_dir)`      → `packet_layout.h`
   (wire constants shared by stub and proxy).
5. Call `stub_gen.emit(schema, out_dir)`        → `server_stub.{h,cpp}`.
6. Call `proxy_gen.emit(schema, out_dir)`       → `client_proxy.{h,cpp}`.
7. Print a one-line summary: `generated 6 files for N packets`
   (`packet_ids.h`, `packet_layout.h`, `server_stub.{h,cpp}`,
   `client_proxy.{h,cpp}`).
8. Exit 0.

### Validation

The orchestrator owns *all* schema validation. The emitters trust
their inputs.

| Check                                           | Error message                                                  |
|-------------------------------------------------|----------------------------------------------------------------|
| Required top-level keys present                 | `schema missing key: {key}`                                    |
| `gen_version` matches script's expected         | `unsupported schema version: {got} (expected {expected})`      |
| Packet `name` matches `[A-Z][A-Z0-9_]*`         | `invalid packet name: {name}`                                  |
| C2S `name` starts with `CS_`                    | `c2s name must start with CS_: {name}`                         |
| S2C `name` starts with `SC_`                    | `s2c name must start with SC_: {name}`                         |
| `id` in [0, 65535]                              | `packet id out of range: {name}={id}`                          |
| `id` unique within direction                    | `duplicate c2s id: {a.name} and {b.name} both = {id}`          |
| `id` unique across directions (v1)              | `c2s/s2c id collision: {a.name} and {b.name} both = {id}`      |
| Field `type` exists in `types`                  | `unknown type in {pkt}.{field}: {type}`                        |
| Field `name` matches `[a-z][a-zA-Z0-9_]*`       | `invalid field name in {pkt}: {field.name}`                    |
| Total packet body ≤ 65531                       | `payload too large: {name} = {n} bytes (max 65531)`            |
| S2C `send` ∈ {Unicast, Broadcast, BroadcastExcept} | `invalid send mode in {name}: {send}`                        |

## `ids_gen.py`

Emits `generated/packet_ids.h`:

```cpp
// generated from packets.json — do not edit
#pragma once
#include <cstdint>

namespace iouring_server::generated {

enum class packet_id : uint16_t {
  CS_MOVE_START = 10,
  CS_MOVE_STOP  = 12,
  // ... all c2s ...
  SC_CREATE_MY_CHARACTER = 0,
  SC_CREATE_OTHER_CHARACTER = 1,
  // ... all s2c ...
};

// Lookup helpers
constexpr std::string_view name_of(packet_id id) noexcept;
constexpr bool             is_c2s(packet_id id) noexcept;
constexpr bool             is_s2c(packet_id id) noexcept;

} // namespace iouring_server::generated
```

`name_of` is a `switch` with `case packet_id::X: return "X";` for each
entry; `is_c2s`/`is_s2c` are also switches. No `unordered_map` —
constexpr from end to end so headers stay zero-cost.

## `stub_gen.py`

Emits `generated/server_stub.{h,cpp}`. The contract (POD per packet,
`stub_X` decoder, forward-declared `handle_X`, `register_all`) is in
[`../../docs/05-codegen.md`](../../docs/05-codegen.md) § "Generated stub".

### Specific responsibilities

- **POD struct emission** is mechanical: for each `c2s` packet, iterate
  fields, emit `<type> <name>;` lines, wrap in `struct X_body { ... };`.
  The struct is a *handler-facing carrier*, not a wire image — it gets
  whatever padding the C++ ABI applies and is intentionally NOT laid out
  to match the wire.
- **Wire-constant emission**: for each packet, compute `_wire_size` and
  `_wire_offset_<field>` from the schema by walking fields in order and
  summing `types[field.type]` widths. Emit them into a single shared
  `generated/packet_layout.h` (NOT into the stub or proxy header), so
  stub and proxy include the same source of truth and cannot drift.
  These constants are the **only** source of truth for wire layout —
  never reference `offsetof` or `sizeof` of the body struct in generated
  code. Both `server_stub.h` and `client_proxy.h` `#include
  "packet_layout.h"` at the top.
- **Stub function emission**: produce a `body.size() == _wire_size`
  check (mismatch → `s.reject_and_close(framing_error::bad_payload_size);
  co_return;`), then `<Pkt>_body parsed{};` (the `{}` zero-init is
  load-bearing — without it, padding inserted by the C++ ABI is
  indeterminate and MSan can flag the pass-by-value to the handler),
  then one `std::memcpy(&parsed.<field>, body.data() +
  _wire_offset_<field>, sizeof(parsed.<field>))` per field, then a
  `co_await handle_X(s, parsed)`. Padding bytes between fields are
  never read; the only legitimate way to expose data is via a named
  field.
- **Forward declarations of `handle_X`** in the header so concrete
  handlers can `#include` it without seeing implementation.
- **`register_all` body** is a sequence of `d.register_handler(id,
  &stub_X);` lines, sorted by ID for determinism.

### Emission style

- One C++ TU output (`server_stub.cpp`), not split per packet — keeps
  build artifact count down.
- Indentation: 2 spaces, matching the project's `.clang-format`.
- Every emitted function starts with
  `// generated from packets.json: {NAME} (id={id})`.

## `proxy_gen.py`

Emits `generated/client_proxy.{h,cpp}`. Inverse of `stub_gen`:

- For each `c2s` packet, emits a `send_X(session_handle, fields...)`
  coroutine that serializes a frame and calls `h.send(...)`. The
  send is routed through the library's `session_handle` so the proxy
  is identical for client-side use (pass `my_session.handle()`) and
  server-side broadcast (pass a peer handle from
  `service::for_each_session`). The encoder uses the **same
  `_wire_size` and `_wire_offset_*` constants** the decoder uses —
  both stub and proxy `#include` the shared
  `generated/packet_layout.h` produced by `layout_gen.py`. Encode/
  decode therefore cannot drift: a schema change bumps both sides
  through one set of constants.
- For each `s2c` packet, emits the *symmetric* POD, stub, and
  `handle_X` forward declaration — so the client side can hook
  server-pushed packets the same way the server hooks client packets.
- Emits a `register_all(packet_dispatcher&)` for the `s2c` stubs.

The proxy is **structurally a mirror** of the stub. Differences:
- Server proxies of `s2c` packets exist (for handlers that send to
  the client); client proxies of `c2s` packets exist (for sending).
- The client links the proxy library; the server links the stub
  library; both register their respective subset with their local
  dispatcher.

## Determinism mandate

The generated output for a given `packets.json` must be **byte-
identical** across runs. Sources of nondeterminism to police:

| Source                       | Mitigation                                                |
|------------------------------|-----------------------------------------------------------|
| `dict` iteration order       | `sorted()` everywhere order isn't fixed by the schema     |
| Timestamps in headers        | Don't emit them. The header says "do not edit", not "regenerated at …" |
| File-system ordering         | `pathlib.Path.iterdir()` results unsorted; never used     |
| Python version differences   | Pin floor to 3.10; CI builds on 3.10 and 3.12             |
| Locale-dependent formatting  | Never use `%d`/`,` thousands separators; only f-strings   |

CI's reproducibility envelope hashes `generated/` after each build
(see [`../../docs/07-ci.md`](../../docs/07-ci.md) § "Reproducibility
envelope"). A mismatch fails the build.

## Atomicity and failure modes

The generator never writes directly into the final `generated/`
directory. It writes into a staging directory
(`generated.stage/`), CMake validates a **manifest** of expected
outputs (all six files present and non-empty), then atomically
renames the staging dir over the final one. The full sequence lives
in [`../../docs/03-cmake.md`](../../docs/03-cmake.md) §
"Codegen integration — `cmake/codegen.cmake`"; the contract is:

| Concern                                                       | Guarantee                                                              |
|---------------------------------------------------------------|------------------------------------------------------------------------|
| Generator crashes after writing some files                    | Staging dir is discarded next configure; final dir untouched           |
| Generator skips a file in the manifest                        | "Exists + non-empty" check fails configure with an explicit FATAL_ERROR |
| Generator writes an empty file                                | "Non-empty" check fails configure                                      |
| Generator writes an extra file outside the manifest           | "Exactly these files" stage-listing check fails configure              |
| Schema rename (e.g., CS_FOO → CS_BAR) leaves stale `.cpp`     | Staging dir is wiped before each run; manifest check catches strays    |
| Half-emitted state visible **to a single in-tree build**      | Validation happens on staging; the in-tree build only consumes `generated/` after a successful swap |
| Half-emitted state visible **to a concurrent uncooperating reader** (parallel CMake, IDE indexer) | **Not** prevented — `REMOVE_RECURSE` + `RENAME` has a brief gap where `generated/` is absent. Concurrent shared build-dir access is out of contract; `file(LOCK)` serializes cooperating CMake invocations only. |
| Reconfigure triggered by editing `layout_gen.py`              | `CMAKE_CONFIGURE_DEPENDS` lists all five scripts                       |
| Reconfigure triggered by editing `packets.json`               | `CMAKE_CONFIGURE_DEPENDS` lists the schema                             |

**Filesystem locality.** `file(RENAME)` uses `rename(2)`, which only
works across paths on the same filesystem. The staging dir and final
dir both live under `${CMAKE_BINARY_DIR}` by construction, so this
holds on every supported target (ext4, btrfs, xfs, tmpfs).

**Why we do not provide cross-process atomic replacement.** POSIX
`rename(2)` cannot atomically replace a **non-empty** directory; that
needs Linux-specific `renameat2(RENAME_EXCHANGE)`, which CMake does
not expose, or symlink-indirection where `generated/` is a symlink the
swap redirects atomically. Both are out of scope for v1 — the in-tree
build flow is single-process, so cooperative `file(LOCK)` plus
"out-of-contract for shared concurrent access" is the chosen
trade-off.

**Failure mode that is NOT covered:** the generator emits a file
that is **syntactically valid C++ but semantically wrong** (e.g.,
wrong wire offset). The manifest check cannot detect this; the
[`../../docs/06-test-strategy.md`](../../docs/06-test-strategy.md) §
"Layer 1 — unit" stub-decode tests are the catchnet. The
verified-offsets test (`verify_offsets.py`) is the deeper line of
defense against semantic codegen bugs.

## Local development workflow

```bash
# After editing proto/packets.json:
cmake --build build
# → CMake reconfigures, codegen runs, build proceeds.

# Force a clean regeneration:
rm -rf build/generated
cmake --build build

# Run codegen standalone (for debugging the emitters):
python3 codegen/rpc_gen.py --schema proto/packets.json --out /tmp/gen
diff -r build/generated /tmp/gen   # should be empty
```

## Reference origin

- `~/CLionProjects/SelectServer/TestSerialize/rpc_gen.py:1` (14 lines) — orchestrator
- `~/CLionProjects/SelectServer/TestSerialize/stub_gen.py:1` (76 lines) — server emitter
- `~/CLionProjects/SelectServer/TestSerialize/proxy_gen.py:1` (94 lines) — client emitter

The Linux port adds: validation (the reference has none), namespace
wrapping (the reference dumps into global scope), `ids_gen.py`
(reference inlines IDs as `#define` macros into `Protocol.h`), UTF-8
file I/O throughout (reference is Windows codepage-dependent).

## Cross-references

- [`../../docs/05-codegen.md`](../../docs/05-codegen.md) — the
  high-level codegen contract.
- [`../proto/packets.md`](../proto/packets.md) — the schema's content.
- [`../../docs/03-cmake.md`](../../docs/03-cmake.md) § "Codegen
  integration" — how CMake invokes these.
