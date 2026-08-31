# engine-uring

A Linux-native C++20 realtime interaction network engine for MMO/RTS-style
servers, built on `io_uring`. The first demo is room-based chat; the engine
target is persistent TCP sessions, world-thread ownership, packet routing, and
lock-free thread-to-thread messaging on Linux `io_uring`.

The reason this runtime exists is not only throughput — it is to keep the
packet-to-authoritative-state path short, bounded, and inspectable under
realtime interaction load. It carries the engine-primitives lessons of the
Windows IOCP family of reference projects (architectural inspiration, not a
compatibility goal) onto a 2026-relevant Linux stack.

See [`../server-uring/doc/10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md)
for the runtime shape, ownership invariants, and the first three-thread
milestone (the runtime itself lives in `../server-uring/`, consuming this
engine through its install prefix). (No coroutines are implemented yet — the runtime path is hand-written
`io_uring` SQEs; the identity is deliberately not tied to coroutines until such
code exists.)

Landed so far: the CMake scaffolding + dependency graph (`liburing`, `{fmt}`,
`Catch2`, `tl::expected`) + CI floor job, the primitive layer
(`sds::ring_buffer`, `mem::packet_pool`, `sync::` atomics/mutex,
`diagnostic::profiler_scope`), and — now in `../server-uring/` — the 3-role supervisor/acceptor/worker boot
spine and the thread-mesh runtime (`app::spsc_mailbox`, `app::session_table`).
The data path on top — protocol framing, worker-side session storage, rooms, and
the per-worker `io_uring` chat loop — is the current work. Per-file design specs
live under `doc/<category>/<name>.md`; the runtime architecture lives in
`../server-uring/doc/10-realtime-server-architecture.md`.

---

## Quick start

Verified on Ubuntu 24.04 / WSL2 (kernel 6.6+) with `g++-12` or newer and
`liburing-dev >= 2.5`. See `doc/06-system-setup.md` for the full
distro-aware install runbook.

```bash
make hello                     # configure + build + run examples/hello
make test                      # configure + build + ctest (default preset)
make test-sds                  # configure + build + run only sds:: tests
make test  PRESET=floor        # same, but on the gcc-12 floor preset
make build PRESET=release      # release build, no sanitizers
make help                      # list every target
```

The `make` targets are thin wrappers around `cmake --preset …`,
`cmake --build --preset …`, and `ctest --preset …`; the raw CMake
commands work too. Build directories live under `build/<preset>/` and
are gitignored.

---

## Why this exists

Three predecessor repositories live at `~/CLionProjects/`:

| Repo               | What it is                                              |
|--------------------|---------------------------------------------------------|
| `IOCP_Rookiss`     | Engine primitives: 47-class memory pool, sync, deadlock |
| `SelectServer`     | `select()`-based game servers (FighterOOP, StarServer)  |
| `WindowsLibrary`   | Utility layer: atomics, mutexes, threads, smart ptrs    |

All three are Windows-bound. The systems-level lessons in them — memory
pools, lock-free stacks, per-entity job queues, packet framing — are
OS-agnostic at the conceptual layer. This project preserves those lessons
while replacing the Windows surface with `io_uring`, POSIX, and modern C++.

The Windows repos remain as **read-only architectural references**. Nothing
in this repo links against them.

---

## Scope

v1 milestone is **a single SessionManager + one WorldThread carrying room
chat** (connect → room select → chat → disconnect), not merely an echo server
over one connection. The `io_uring` echo smoke remains a low-level transport
test, not the product milestone. See
[`../server-uring/doc/10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md).

In scope for v1:

1. **Fused worker-owner data plane** — one `io_uring` loop per worker owns its
   fds, recv/send rings, and room state; recv completion, framing, handler
   execution, and state mutation all run on that one thread. No separate network
   thread; no coroutines.
2. **Three-role runtime** — supervisor (spawn/shutdown), SessionManager/acceptor
   (accept + session authority map), worker (owns adopted fds + rooms), wired by
   SPSC mailboxes.
3. **Room chat over the custom frame** — `[uint16 size | uint16 id][payload]`;
   join / chat / leave, with gameplay packets gated behind `S_ENTER_WORLD_OK`.
4. **Primitive layer** — `mem::packet_pool` (47 size classes, ported from
   `IOCP_Rookiss`), `sds::ring_buffer`, and the `sync::` primitives
   (project-owned `lnx::atomic*` / `lnx::mutex`, **no `std::` sync types** — see
   the No-STL policy in `doc/04-coding-style.md`).

Out of scope for v1: real login/auth, database/persistence, TLS, multiple
workers, world migration, serious benchmark claims, and Windows compatibility
shims. (Room chat itself is *in* scope — it is the v1 milestone, a testbed for
the future interaction-space model.)

---

## Reading order

Documentation is split into three trees:

- **`doc/`** — the code documentation you build from. Project guides at the root
  (`doc/INDEX.md` is the entry point: build order + dependency graph), and
  per-source-file specs mirroring `src/` 1:1 under `doc/<path>.md`. This is the
  **source of truth** — an agent can implement a unit from its spec alone.
- **`../design-notes/`** — the dated brainstorm journal (the *why*: rationale,
  alternatives, rejected ideas). Read for context; never a build dependency.
- **`src/`** — the code.

Start with `doc/INDEX.md`, then `doc/00-overview.md` for scope and the layered
map, and `../server-uring/doc/10-realtime-server-architecture.md` for the runtime shape.

```
doc/
├── INDEX.md                          # build order + dependency graph (start here)
├── TEMPLATE.md                       # per-file spec template
├── README.md                         # reading-path index
├── 00-overview.md … 08-test-strategy.md                  # project guides
└── sds/  memory/  sync/  diagnostic/  runtime/  network/  # per-file specs, mirror src/

../design-notes/                      # dated decision journal (append-only, repo root)
```

The relocated per-file specs under `doc/<category>/` are still in their original
prose; reformatting them to `TEMPLATE.md` is ongoing (see `doc/INDEX.md`). The
`app::` runtime layer moved to `../server-uring/` and is documented there
(`doc/10-realtime-server-architecture.md`) plus the decision records under
`../design-notes/`.

---

## Status

**Landed and tested** (full suite runs under ASan+UBSan; also builds on the
gcc-12 floor preset):

- Build scaffolding — CMake, presets, deps graph, CI floor job, devcontainer,
  `examples/hello/`.
- Primitive layer — `sds::ring_buffer<N, Sync>`, `sds::static_vector`,
  `sds::cstr_hash_map`, `sds::malloc_vector`, `mem::packet_pool`, `sync::`
  atomics/mutex, `diagnostic::profiler_scope`.
- Runtime spine — 3-role supervisor boot (main / acceptor / worker), the
  handle/engine split, and an `io_uring` echo *transport* smoke. The spine and
  the thread mesh (`app::spsc_mailbox`, `app::session_table`) migrated to
  [`../server-uring/`](../server-uring/) with their tests; the engine keeps the
  transport smoke and everything below the app layer.

**In progress** — the room-chat data path: protocol framing, worker-side
session storage (SoA/mmap), rooms, and the per-worker `io_uring` chat loop, ending
at the single-SessionManager + one-WorldThread milestone in
[`../server-uring/doc/10-realtime-server-architecture.md`](../server-uring/doc/10-realtime-server-architecture.md).
