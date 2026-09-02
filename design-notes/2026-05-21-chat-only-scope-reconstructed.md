---
status: superseded
reconstructed: 2026-09-02
superseded_by:
  - 2026-09-02-design-notes-drift-review.md
still_holds: lift architecture decisions verbatim into game work; re-derive only entity pool, tick scheduler, AOI, snapshot
---
# 2026-05-21 — Chat-only scope; game server to a separate repo (reconstructed 2026-09-02)

> **Reconstructed, not contemporaneous.** This decision narrowed the whole
> deliverable and exists in the repository only as a banner on
> [`2026-05-19-portfolio-strategy.md`](2026-05-19-portfolio-strategy.md) and
> one line in [`game-server/README.md`](game-server/README.md). Its cited
> source, `.omc/wiki/chat-server-v1-session-and-auth-design.md`, was
> gitignored and did not survive. This note is transcribed on 2026-09-02 from
> `~/.claude/projects/-home-stephen-code-iouring-net-lib/memory/project_portfolio_scope.md`
> (revised 2026-05-21) and
> `~/.claude/projects/-home-stephen-code-iouring-net-server/memory/repo-role-and-seam-status.md`.
> The library commit for the decision is `908a7aa`.

## The decision

The 2026-05-17 portfolio plan bundled a chat server, a primitive MMO, and an
in-process renderer thread into one repo. Revised on this day:

> **Chat server is good enough as a standalone deliverable for this repo.
> Game server moves to a new repo.**

**Why**, per the record: the chat server alone fully demonstrates the v1
architecture (per-worker io_uring, TLS memory pool, SoA sessions, PG auth,
heartbeat, kick-old). Bundling a primitive MMO and a renderer thread in the
same repo blurs the focus and stretches a solo timeline. "Cleaner story: each
repo demonstrates one cohesive thing."

## What stays here (then `iouring-net-lib`, now `engine-uring` + `server-uring`)

- Core chat server: room-based, PG auth, heartbeat, kick-old, drop-and-close
  backpressure.
- Operational tooling: logger, profiler timeline, stress test client,
  monitoring, dump files, eBPF kernel capture. The record singles out eBPF
  capture as "the distinctive portfolio winner" — most servers stop at "I
  wrote io_uring code"; few demonstrate kernel-level understanding.
- Optional chat v2 (deferred): global broadcast, whisper, channel switching
  without disconnect — gated on whether the inbox subsystem is built.

## What leaves (then a future repo, later `iouring-net-server`)

- Primitive MMO at agar.io / slither.io level — explicitly *not* an RPG.
- 2D world, tick-driven, multi-zone sharding.
- Standalone game client as a separate process.
- In-process renderer thread as an observability artifact: N-panel window,
  double-buffered snapshot per content thread, ~60 Hz without blocking
  content threads.

The record notes the renderer and MMO design were "preserved as future
reference, not active scope."

## Rules the record gave for applying it

- Gate features against chat-only scope: anything that smells like game
  state, tick simulation, or rendering belongs to the game repo.
- Wiki pages that reference a "game v2 extension" (per-session SoA scratch,
  larger packet bucket) are historical reasoning, not forward-looking spec.
- When the game repo starts, lift the architecture decisions verbatim
  (per-worker io_uring, TLS pool, SPSC mesh) — they are general. Re-derive
  only the game-specific parts: entity pool, tick scheduler, AOI, snapshot.

Timeline estimate recorded: 3–4 months for a production-grade chat server
with full observability; the game repo on its own timeline afterwards.

## What happened next

- **2026-07-04** — the separate repo's v0 seam scaffold landed
  (`server_core` static lib, thin main, Catch2 harness, 8/8 tests green under
  ASan+UBSan), consuming the library from `~/.local`. Its docs 00–07 predated
  the pivot; `docs/08-architecture-pivot.md` was its reconciliation.
- **2026-07-09** — this repo's identity pivoted to "realtime interaction
  engine" (`a6d01a2`), which reintroduced MMO/RTS as the target with chat as
  the testbed — the opposite emphasis from this note's chat-only banner.
  Neither document cited the other.
- **2026-08-31** — everything was consolidated into one repository again
  (`117a17c`), and the separate repo's document set was imported whole as
  [`game-server/`](game-server/) on 2026-09-01.

So the scope moved: two repos (05-14) → one repo (05-17) → chat here, game
elsewhere (05-21) → engine-with-chat-as-testbed (07-09) → one repo holding
engine, product, control group, and instrument (08-31). The 2026-09-02 drift
review records that these five statements coexisted with no cross-references,
and the purpose statement that supersedes them is in
[`2026-09-02-design-notes-drift-review.md`](2026-09-02-design-notes-drift-review.md).
