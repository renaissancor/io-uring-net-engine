---
status: accepted
---
# 2026-09-02 — Drift review of the design notes

The author, returning after a stretch of day-job weeks: *"several concepts
are mixed as I had a lot of idea about this project but forgot as I work in
company."* Two independent read-throughs of every note, spec, and README were
made — one over 2026-05-14 → 05-23, one over 05-25 → 09-01 plus the imported
`game-server/` set, `doc/10`, `threading_model.md`, and `result-notes/`. They
produced 67 cited findings. This note keeps the citations and groups them by
root cause, because the 67 collapse into six, and it records the purpose
statement the author gave in response, which closes the first of them.

Paths are relative to the repository root. Line numbers are as of commit
`b15e077`.

## 0. The purpose, stated

Asked which of the repository's several purpose statements was the real one,
the author answered:

> This repo should show my code optimization efforts, meaningful loop logic
> structure, and a network engine to use in a game server. All details are
> targeting those.

Three things, in that order: **optimisation work that can be shown**, **loop
structure that is worth reading**, and **an engine a game server could use**.
Every other statement in the repository is an instrument for one of those, not
a rival purpose. In particular the epoll-versus-io_uring measurement is the
instrument for the first, and
[`2026-09-02-where-io-uring-becomes-meaningful.md`](2026-09-02-where-io-uring-becomes-meaningful.md)
is the hypothesis that instrument tests. The chat server is the smallest
workload that exercises the second and third.

## 1. The purpose was stated five ways, none citing the others

- Root README: one measurement question — where the epoll server saturates
  and how far io_uring moves that line (`README.md:7-15`).
- `server-uring/doc/10-realtime-server-architecture.md:12-18` and
  `engine-uring/README.md:3-6`: a realtime interaction engine for MMO/RTS
  servers, chat as testbed for the `InteractionSpace` model.
- `design-notes/2026-05-19-portfolio-strategy.md:3` (banner): chat server
  only is the complete deliverable; MMO work moved to a separate repo.
- `doc/10 §12` (`:176-180`) and `engine-uring/README.md:99-100`: "serious
  benchmark claims" are out of scope — while the root README's headline is a
  benchmark and `server-epoll/README.md:7-10` says every engine claim is a
  comparison against its numbers.
- `design-notes/game-server/docs/00-overview.md:176`: "a network demo, not a
  game."

Related: "product" means `server-uring` (`README.md:24`), the never-built game
server (`game-server/README.md:26-29`), and a design criterion
(`2026-05-25-handle-engine-split.md:149-151`). `engine-uring/README.md:14-17`
says the runtime lives in `server-uring`, and the same file's Scope section
(`:77-98`) lists the runtime as in scope for the engine's v1.
`engine-uring/CLAUDE.md` names `doc/10` as if it were an engine doc; it lives
in `server-uring/doc/`.

**Closed by § 0.** The remaining work is mechanical: point the root README's
opening and `doc/10 §12` at § 0, and counter-banner the portfolio note.

## 2. The binding decisions lived outside the repository

- Seven notes cite `.omc/wiki/` pages as the locked record. The directory was
  gitignored (`.gitignore:49`) and is not in any commit.
- "The 2026-05-17 architecture" is cited five times
  (`2026-05-19-chat-server-data-layout.md:28, :45, :119, :684`;
  `2026-05-23-session-account-data-model.md:175`) and had no note.
- The 2026-05-21 chat-only decision narrowed the deliverable and existed only
  as a banner (`2026-05-19-portfolio-strategy.md:3`) and one line
  (`game-server/README.md:52-53`).
- Every note from 05-23 onward treats the wiki as ground truth
  (`2026-05-23-session-account-data-model.md:3, :154, :173, :249-256`;
  `2026-05-25-handle-engine-split.md:3`; `2026-06-06-...:3-6`).

**Recovered.** The memory files under the old repository names survived:
`~/.claude/projects/-home-stephen-code-iouring-net-lib/memory/` holds
`project_architecture_v1.md` (the 05-17 pivot, with its 05-19 update),
`project_library_product_split.md` (the 05-17 monorepo decision),
`project_portfolio_scope.md` (the 05-21 scope revision), and
`project_chat_server_v1.md` (the 05-23 → 05-24 eight locks, including the
record that "channel" was renamed to "worker" everywhere). Two of these are
now transcribed as
[`2026-05-17-architecture-pivot-and-monorepo-reconstructed.md`](2026-05-17-architecture-pivot-and-monorepo-reconstructed.md)
and
[`2026-05-21-chat-only-scope-reconstructed.md`](2026-05-21-chat-only-scope-reconstructed.md).
The eight-locks record is largely already in the repo as the 05-23 note; its
"supersedes" section is the missing rename record and is quoted in § 3.

## 3. The threading model has three unreconciled layers

**The two-tier doc still claims to be normative.**
`engine-uring/doc/runtime/threading_model.md:3-14` is bannered as replaced,
but `:18-26` says every other doc "MUST be consistent with this document";
`:67-75` still lists two-tier as "what v1 implements"; `:108-148` and
`:260-271` specify network↔content SPSC rings per session; and
`packet_header.md:24-25` links it as current. The banner names the 05-19
evening model as the successor, not the supervisor/acceptor/worker model of
`doc/10 §3` (`:35-57`), and says the acceptor owns no ring, which
`2026-08-21-phase2-architecture-pass.md:66-69` contradicts.

**Nobody wrote why single-tier won.** `threading_model.md:150-165` gives five
reasons to reject single-tier. `doc/10 §2` (`:20-33`) and
`engine-uring/README.md:84-88` assert single-tier without answering them. The
answer exists — `2026-05-19-server-architecture.md:13-19, :29-38` — and the
reconstructed 05-17 note now points at it.

**"Worker" flipped meaning.** Kernel-side io_uring thread with "no separate
user-managed worker thread" (`2026-05-19-server-architecture.md:17, :234,
:277-279`), then a user-space compute pool in the same note (`:451, :468`),
then the content thread from 05-21 onward
(`2026-05-21-packet-pool-review.md:10, :17, :54`;
`2026-05-23-...:69, :135`). The rename is recorded only in the surviving
memory: *"'channel' terminology replaced by 'worker' everywhere. `channel`
retained only as queue terminology."*

**Synonyms never reconciled.** Owner thread: strand
(`2026-05-19-server-architecture.md:36`), channel / content thread (`:14,
:105-107`), zone (`:246`), worker, WorldThread (`doc/10:47`). Accept role:
accept/gateway thread, accept thread, SessionManager / Acceptor, acceptor as
lobby. Interaction unit (`threading_model.md:30-45`), InteractionSpace
(`doc/10:17`), room (`doc/10 §6-7`). Channel = room
(`2026-05-19-server-architecture.md:14`; `portfolio-strategy.md:37`) versus a
channel hosting 16 rooms (`chat-server-data-layout.md:664-665`) versus a
client-selected key mapped `channel_id % N` onto a worker
(`2026-06-06-...:31-33, :53-55`). `doc/10` never uses "channel"; "world" in
`doc/10 §3/§6` (`:56, :87`) is undefined relative to "room".

**Roles.** Five (content, accept/gateway, registry, broadcaster, janitor:
`2026-05-19-server-architecture.md:626-633`) → three plus later db and logger
(`doc/10:37-57`) → "4 roles" with db booting before the acceptor
(`2026-06-06-...:179-180, :55-57`). Broadcaster and janitor vanish with no
note. Session/account ownership has three homes (`...server-architecture.md:671-701`;
`2026-05-23-...:18, :219-230`; `doc/10:44-47, :70-73`).

**Loop.** Both 05-19 notes assume a periodic scan-every-session tick
(`server-architecture.md:531`; `chat-server-data-layout.md:488-506`).
`doc/10 §8` (`:126`) is "completion-driven, not scan-every-session-every-tick."
Unflagged. The 2026-09-02 hypothesis note's § 7 needs both — completion-driven
input, periodic tick — and says so.

**Wake model.** Four positions: yield + `peek_cqe` spin with timeout as
first-pass (`doc/10 §9:144-154`; `thread.md:77-88`); "every wake reason is a
CQE" including the tick as a multishot timeout SQE
(`2026-08-21-architecture-review.md:19-29, :43-59`); bounded
`submit_and_wait_timeout` as "load-bearing", explicitly rejecting the tick as
a CQE (`2026-08-21-phase2-architecture-pass.md:71-79`); busy-poll as "library
reality" (`game-server/docs/08:21`), which `phase2:77-78` and
`2026-09-01-...:40-44` both reject. The two 08-21 notes disagree on the same
day, and the 09-01 disposition calls item 1 undecided without citing the
phase-2 decision (`2026-09-01-...:40-45`). `doc/10 §9` still lists MSG_RING
as a peer option (`:151-152`) that `phase2:16-19, :86-88` ruled out for stop.

**Mesh names.** `handle_worker` / `engine_worker`
(`2026-05-25:12-13`; `2026-06-06:83-86`; `game-server/docs/08:63-64`) → code
has `worker_ctl` / `worker_engine`. `app::spsc_mailbox` is named as live in
`engine-uring/README.md:25, :153`, `server-uring/README.md:5`,
`engine-uring/CLAUDE.md`, and `phase2:39-42`; `server-uring/doc/mesh.md:20-22`
says it is retired for `sds::pipe` + `app/mesh.h`, and no source for it
exists.

## 4. Six notes are superseded and unmarked

The journal (`design-notes/README.md:33-46`) marks only the 05-14 note.

| note | superseded by | where it says so today |
|---|---|---|
| `2026-05-15-session-log.md` | build order never happened (`:32-36`; `packet_pool` first per `2026-05-21:3`); `LNX_DCHECK` rule reversed (`:24`; deleted per `2026-05-19-chat-server-data-layout.md:699-700`) | nowhere |
| `2026-05-19-server-architecture.md` | `ObjectPool<Session>` (`:129-139`), POD migration (`:504-518`), MPSC inboxes (`:91, :653`), broadcaster/janitor (`:632-633, :705-728`) — all replaced by 05-19 evening, 05-23, `doc/10`; Part 3 table `:180-186` called "misleading" by its own `:703` | nowhere; index says "Server architecture" |
| `2026-05-19-chat-server-data-layout.md` | global mmap (`:18, :75-79`) → per-worker (`2026-05-23:59`); "migration nearly trivial" (`:94-97`) → "never cheap" (`2026-05-23:50, :57, :77`); constants (`:142-146`) → `2026-05-23:249`; kernel-producer SPSC (`:189-203, :235-272`) → single-owner dense counters (`2026-05-23:173-175, :260`) | nowhere |
| `2026-05-21-packet-pool-review.md:17` | `app::worker` entry point → handle/engine split (`2026-05-25:11-13`) | nowhere |
| `2026-05-25-handle-engine-split.md` | `handle_*`/`engine_*` names (`:12-13`) and registry singleton (`:76`) → `*_ctl`/`*_engine` and compile-time `roster.h:9-19` | nowhere |
| `2026-06-06-supervisor-init-and-acceptor-lobby.md` | acceptor owns I/O while client picks a channel (`:26-37, :134-136`) → `doc/10 §7:98-107` "v1 does NOT do that". `phase2:211-214` asked for a banner | nowhere |
| `game-server/docs/08-architecture-pivot.md` | busy-poll (`:21`), 65536-slot dispatcher (`:40-43`) → `phase2:182-190` static 7-opcode table; "this document wins" (`:4-5`) | provenance blockquote only; `game-server/README.md:33-59` body still reads as current with build instructions against `../iouring-net-lib` |

Also: the 05-23 note misattributes what it supersedes (`:175` blames the
05-17 model for kernel-producer SPSC; both 05-19 notes asserted it too).
Coroutines are an open question (`server-architecture.md:821`), the headline
pitch (`portfolio-strategy.md:65`), and rejected two days earlier
(`chat-server-data-layout.md:683-690`), all dated 05-19. Phase 2's DECIDED
items have no disposition: `S_ENTER_SELECTING` fold (`phase2:203-204` vs
`doc/10:90-91, :109`), state renames (`:118-121` vs `doc/10:84-94`), kernel
floor 6.0 (`:101-103` vs `engine-uring/doc/00:212`, `02:78`, `06:140`,
`game-server/README.md:87` all 5.19).

And the directory violates its own "never revised" rule (`README.md:6, :20`):
05-21 has inline resolutions dated 05-23 (`:10, :15-19, :83-85`); the
portfolio note has a 05-21 banner. The rule needs a stated exception for
banners, or a convention that replaces them — § 7.

## 5. The measurement story contradicts itself in the READMEs

- "The ceiling is 2M deliveries/s" is stated as fact in `README.md:39`,
  `server-epoll/README.md:93-95`, `result-notes/README.md:74-77`.
  `result-notes/2026-08-30-...:76-80` withdrew it ("never reached in any
  dimension ... all client-limited") and `2026-09-01-...` measured the real
  ceiling as byte-bound at 1.78 GB/s. `README.md:48-52` carries the 1.78 GB/s
  line four paragraphs below the 2M line without saying which is the line
  io_uring must move. `result-notes/README.md:74-77` has no pointer to the
  correction.
- `README.md:43-45`: "that kernel share is the io_uring argument stated as a
  measurement ... the cost being attacked is syscall transitions."
  `2026-08-30-...:58-88` found kernel share fell 86 % → 77 % while CPU held
  100 %, and `:28-45` found latency is a sweep period set by connection count.
  `doc/10 §2:22-24` motivates io_uring by tick predictability instead. The
  2026-09-02 hypothesis note reframes the argument as tick budget, which is
  consistent with the measurement; the README sentence is not.
- `server-epoll/README.md:7-10` says the server is permanent; `:60-61` says
  "this repo is marked for deletion."
- Where the baseline lives: `result-notes/README.md:8-11, :15` (here);
  `server-epoll/README.md:55, :107-109` (client-bench README).
- `result-notes/README.md` is index and undated measurement record at once
  (`:15, :26-132`), against the rule in `design-notes/README.md:20-22`.

## 6. The normative spec drifted from the code and nothing broke

`design-notes/README.md:24-26` says `doc/10` lives beside the code because
"something is supposed to break when the code drifts from it." It drifted:

- `doc/10:56-57, :176-178` scopes v1 to one worker; `server-uring/README.md:36-38`
  documents `-DIOURING_NET_WORKER_COUNT=4` and `roster.h:9` allows 1..8;
  `phase2:54-57` plans `room_id % worker_count` fan-out.
- Wire header: 4 bytes in `engine-uring/README.md:92`, `server-epoll/README.md:118-125`
  (which says "the protocol doc is authoritative" — no such doc exists in
  `server-uring/doc/`), and `game-server/docs/00:126-135`; 8 bytes in the only
  header spec `engine-uring/doc/network/packet_header.md:1, :47-49` (banner
  `:3-13` says UNCHANGED) and in `phase2:161-171`, which then calls
  `packet_header.md` "banned" (`:215-217`).
- `doc/10:3-6` calls itself "durable source of truth" and "live design doc,
  not a historical log" in the same breath.
- `game-server/docs/08:63-68` calls "is the `app/` layer exported or
  product-owned" the biggest open question; `engine-uring/README.md:24-25`
  and `server-uring/README.md:3-8` record that it moved into the product. No
  document says the question closed.
- "LANDLORD" keeps its name while its boundary moves
  (`2026-06-06:187-191` → `phase2:25-28, :58-59`); `doc/10` never uses the
  word. `2026-09-01-...:71-81` introduces a new direction (vertical slice
  first) inside a disposition note, indexed only as "what landed."

## 7. The one decision left open: how to mark status between notes

There is no Markdown standard for relationships between documents. There are
three widely used conventions, and they compose:

- **YAML front matter** — a fenced `---` block at the top holding key/value
  metadata. Not CommonMark, but rendered by GitHub (as a table), Jekyll, Hugo,
  MkDocs, Docusaurus, Obsidian, and every static-site tool. This is the
  universal carrier for machine-readable fields.
- **ADR / MADR status** — Architecture Decision Records (Nygard 2011; MADR is
  the current template) carry `status: proposed | accepted | deprecated |
  superseded by ADR-NNNN`. This is exactly the field § 4 is missing, and it
  is the one convention a reviewer from any team will recognise on sight.
- **Wikilinks** `[[name]]` — Obsidian, Foam, Dendron, Zettelkasten tooling.
  Not CommonMark; GitHub does not render them. The memory files already use
  them, which is fine there and would be a regression here.

**Recommendation:** front matter on every note with `status` and, when
superseded, `superseded_by:` naming the successor file. Keep the flat
chronological directory and the "never revised" rule, amended to permit
exactly one kind of edit: the front-matter status block. A 20-line lint that
checks every `superseded` note names an existing successor and that the
journal table agrees with the front matter turns § 4 into something that
cannot silently recur. The reconstructed notes and this one already carry
the shape a future lint would read.

This is the only item in this review that needs the author's decision. Every
other item is transcription or a mechanical edit whose content the citations
above already specify.

## 8. Disposition, same day

The author answered § 7 with "okay, can we organize?", and the mechanical
items were done on 2026-09-02 in the working tree. Line citations above are
pinned to `b15e077` and describe the state *before* these changes.

- **§ 1** — the purpose in § 0 is now the opening of the root `README.md`;
  `doc/10` § 1 points at it and § 12 no longer lists benchmark claims as a
  non-goal but defers them to the data path and the hypothesis note.
- **§ 2** — two reconstructed notes written from the surviving memory files.
- **§ 3** — `doc/10` gained a glossary (§ 3a) fixing worker / acceptor /
  supervisor / room / world / mesh and naming the retired synonyms; the
  two-tier `threading_model.md` left the engine's doc tree (below); every
  `spsc_mailbox` mention in the READMEs and `CLAUDE.md` now names
  `sds::pipe` + `app/mesh.h`; `doc/10` § 3 and § 5 no longer cite
  `src/app/main.cpp` or `handoff.md`. The wake-model disagreement between the
  two 08-21 notes is *not* resolved here; it is a decision, not a cleanup.
- **§ 4** — every dated note carries a status block; the journal shows the
  status word; `check_status.py` enforces both. The rule in `README.md` now
  permits exactly that edit.
- **§ 5** — root `README.md` and `result-notes/README.md` carry the withdrawn
  2M ceiling and kernel-share readings as corrected-beside, pointing at the
  08-30 and 09-01 notes and at the tick-budget framing; `server-epoll/README.md`
  no longer says it is marked for deletion and points its protocol paragraph
  at `client-bench/src/wire.h`.
- **§ 6, doc trees** — the contract is now "a `doc/<unit>.md` describes the
  built code", stated in `engine-uring/doc/README.md`, `INDEX.md`, and
  `TEMPLATE.md`. Fourteen specs for code that was never built moved to
  [`unbuilt-specs-2026-05/`](unbuilt-specs-2026-05/) with a README saying
  what exists instead of each. The engine gained docs for `types`,
  `error/expected`, and the landed `memory/packet_pool`; the product gained
  docs for its thirteen undocumented units and a rewritten `INDEX.md`. The
  header-width contradiction dissolved with the move: every remaining
  document describes the 4-byte header that is on the wire.

Still open after this pass, each a decision rather than a cleanup: the wake
model (bounded `submit_and_wait_timeout` versus tick-as-CQE); the phase-2
DECIDED items that `doc/10` has not absorbed; the kernel floor (5.19 in the
guides, 6.0 in the phase-2 decision); and the fate of `game-server/docs/08`'s
"this document wins" clause over a design that is not being built.
