# server-uring — the product, growing on the engine

The chat/game server built **on top of** `engine-uring`. The runtime layer —
the 3-role supervisor / acceptor / worker boot spine, the SPSC thread mesh
(`app::spsc_mailbox`), and the SessionManager `app::session_table` authority
map — lives here with its tests, moved out of the engine tree the day the
boundary became consumable. The room-chat data path on top of it is the
current work.

The boundary rule is unchanged and now enforced by the build: the product
consumes the engine through `find_package(iouring_net)` against an install
prefix, and never through a relative include into `../engine-uring/`. If a
header is missing, the fix is amending the engine's public `FILE_SET` — a
reviewable one-line boundary change — not reaching around it.

## Build

```bash
# 1. install the engine to a prefix
cmake -S ../engine-uring -B build/engine -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine
cmake --install build/engine --prefix "$PWD/prefix"

# 2. configure this against that prefix — the ONLY path to the engine
cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/prefix"
cmake --build build/server
ctest --test-dir build/server
```

There is deliberately **no top-level `CMakeLists.txt`** tying the components
together — the moment one exists, `add_subdirectory(../engine-uring)` becomes
the path of least resistance and the install/export contract stops being
tested by anything. The round trip above runs in CI
(`.github/workflows/ci.yml`, job `engine-and-server`) on every push.

The worker-count knob moved here with the roster:
`-DIOURING_NET_WORKER_COUNT=4` at configure time produces a variant binary so
a worker-count sweep stays cheap to benchmark (see `src/roster.h`).

## Layout

- `src/` — the runtime: supervisor/acceptor/worker engines and controls,
  thread roles, session table, mesh, roster. `main.cpp` builds `uring-server`.
- `tests/` — Catch2 suite for the mesh, session table, and worker control,
  linking the same objects the server ships.
- `doc/` — [`10-realtime-server-architecture.md`](doc/10-realtime-server-architecture.md)
  (the runtime shape, ownership invariants, v1 milestone) and per-unit specs
  as they land.

## What the design docs say, and how much of it survives

[`../design-notes/2026-05-14-project-split.md`](../design-notes/2026-05-14-project-split.md)
specifies the boundary criteria, what belongs on each side, and the contract
surface. Read it for the boundary. Ignore its repo topology: it argued for a
separate repository, that argument was rejected, and the banner at the top of
it says so.

Still owed from the discarded earlier scaffold: a probe that round-trips
bytes through the *installed* `sds::ring_buffer`, so the seam fails loudly at
test time instead of silently resolving headers from the source tree.
