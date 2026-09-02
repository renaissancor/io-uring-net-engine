# CLAUDE.md — engine-uring

Operating notes for Claude. The README is for the human; this is the fast path
for *me*. Keep it short — point to docs for depth, record only what isn't
obvious from the code.

## What this is

A Linux-native **C++20 realtime interaction network engine for MMO/RTS-style
servers** on `io_uring`. First milestone is single-SessionManager + one-worker
room chat. The runtime source of truth is
`../server-uring/doc/10-realtime-server-architecture.md`. Dated design
decisions live in `../design-notes/`; `doc/<category>/<name>.md` describes
each built source unit (map: `doc/INDEX.md`).

## Build / test

```bash
rtk make build                 # default preset (Debug + ASan + UBSan)
rtk make test                  # build + ctest
rtk make build PRESET=floor    # gcc-12 floor — the hard compiler floor; keep it green
rtk make build PRESET=release  # no sanitizers
```

Build dirs are `build/<preset>/`. Run one test binary directly with an absolute
path (`cd` does not persist across Bash calls): `build/default/tests/iouring_net-test "[app][mesh]"` (in `server-uring`).

## Hard rules (these override instinct)

- **No STL.** View types, traits, and C-library funcs are fine. **Banned:**
  STL containers, smart pointers, `std::function`, exceptions, streams,
  `<format>`, and **`std::` sync types** (`std::atomic`/`mutex`/…). Use the
  project's `lnx::atomic32/64`, `lnx::mutex`, `sds::` containers. See
  `doc/04-coding-style.md`.
- **Scalar aliases are `u08`/`i08`, not `u8`/`i8`** (`src/types.h`); also
  `u16/u32/u64`, `i16/i32/i64`, `byte`, `usize`, `isize`. `u8` will not compile.
- **Namespaces:** `lnx::` = raw POSIX/Linux primitives (atomic, mutex, thread);
  `sds::` = generic data structures (personal library, domain-free); `app::` =
  domain/runtime (supervisor, acceptor, worker, mesh, session_table);
  `mem::` = pools. No umbrella namespace just to group a folder.
- **Naming:** Linux side is `snake_case`. Category-prefix filenames
  (`profiler_scope.h`, not `scope_profiler.h`).
- **`sds::ring_buffer` is non-copyable AND non-movable.** Anything embedding it
  (mailboxes, sessions) is pinned — store in `sds::static_vector` / fixed inline
  arrays owned by the supervisor (LANDLORD), never a movable container.
- **Session storage is SoA + session-as-handle + flat session_id + mmap-backed
  ring storage** (locked). Do **not** introduce a large AoS
  `struct session { recv_ring; send_ring; }` — inline rings blow up BSS. See
  `doc/10-…` §"Preserve: SoA".

## Architecture invariants (never break)

One fd → one owner thread. One room/world state → one owner thread. The owning
worker parses and executes its own packets in-thread. Cross-thread = SPSC
message passing (`sds::pipe` framed by `app/mesh.h`), never shared mutable state on the hot
path. Gameplay packets valid only after `S_ENTER_WORLD_OK`. DB/auth deferred
(fake guest identity in v1).

## Workflow

- **Verify before claiming done:** `rtk make test` green + `server` boots and
  shuts down cleanly on SIGINT. Prefer the floor preset for a final check.
- **Delegate context-free extraction/mechanical edits to Codex** (see project
  memory); keep design, review, and commits on Claude.
- **Commits:** conventional subject + git trailers (`Constraint:` / `Rejected:`
  / `Confidence:` …) per the global commit protocol. Branch off `main`; commit
  only when asked. End messages with the `Co-Authored-By` trailer.
- **`handoff.md` is an uncommitted working note — do NOT commit it** unless the
  owner explicitly says so.

## Watch out

- Once an engine loop blocks in `io_uring_wait_cqe` (today they `yield()` +
  `peek_cqe`), an atomic stop flag alone won't wake it — pair `request_stop()`
  with eventfd/timeout wake. See `doc/runtime/thread.md` §"Cooperative stop".
- Specs for the May 2026 designs that were never built (two-tier reactor,
  8-byte header, typed packets, memory tiers) were moved out of `doc/` to
  `../design-notes/unbuilt-specs-2026-05/` on 2026-09-02. Everything left in
  `doc/` describes built code; when it conflicts with a header, the header wins.
