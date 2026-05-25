# Handle/Engine Split Discussion — 2026-05-25

Companion record to `.omc/wiki/handle-engine-split-pattern.md`.
Captures the conversation that produced the handle/engine split pattern for active-object threads, the codex review pass that surfaced 10 findings, and the three engine-construction sub-decisions (1 + 3 locked, 2 deferred to implementation).

---

## TL;DR

Locked across the discussion:

1. **Per-thread design uses a handle/engine split.** `handle_*` = cross-thread surface (atomics + SPSC inbox pointers + `lnx::thread`), lives in a process-global registry singleton. `engine_*` = single-owner body (io_uring ring, SoA columns, libpq pool), lives as a TLS Meyers singleton. Enforces Lock 7 single-owner invariant at the type level.
2. **Naming convention: category-first.** `handle_thread`, `handle_worker`, `handle_db`, `handle_supervisor`, `engine_worker`, `engine_db`. Matches the project's existing `profiler_scope`-style category-prefix rule.
3. **Composition over inheritance** for role-specific handles. `handle_worker` composes `handle_thread`; never inherits.
4. **Supervisor needs a `handle_supervisor`.** Once workers / db push messages via SPSC (not signals), supervisor is itself an active-object thread. No `engine_supervisor` in v1 — supervisor body lives in `main()`.
5. **Engine construction protocol — lazy Meyers TLS.** `static thread_local engine_* inst;` inside `instance()`. Same mechanic as `mem::packet_pool`. Engine ctor is handle-agnostic (mmap arena + io_uring init).
6. **`attach(h)` — post-construction handle wiring.** Renamed from `bind()` to avoid socket-`::bind()` confusion. Three `LNX_CHECK` guards: no-double-attach, no-null-attach, no-run-without-attach. `_handle` is a plain pointer (single-owner per Lock 7, no atomic needed).

Deferred to implementation:

- **Role-token guard mechanism** — Option A (shared `detail::tls_role` enum) vs Option B (per-engine `thread_local bool` flag). Either works; 30-min refactor to switch. Decided when the first engine is being typed.

Deferred to future discussions:

- Init failure modes (mmap / io_uring_queue_init / pthread_create / listen failure handling)
- Shutdown ordering at process exit (registry dtor vs engine TLS dtors vs `lnx::thread` dtors)
- Boot wiring order (supervisor allocates SPSC storage → creates handles → installs pointers → starts db → starts workers)

---

## How the conversation started

Opening was a status check on Phase 0 against `chat-server-v1-implementation-phases.md`. All deliverables green; `c93df64` had landed the io_uring echo smoke test the prior day. Phase 1 (multi-worker mesh) is explicitly deferred to v2 per the locked phase plan, so the natural next move was **Phase 2 — sessions + rooms with stubbed auth**.

The first concrete design question — **app::worker and lnx::thread design** — opened the rest of the session.

---

## Member variables: 5 access categories

User framed the access matrix explicitly:

> *"1 can readable by worker thread but writable by supervisor thread*
> *2 can writable by worker thread but readable only by supervisor thread*
> *3 supervisor thread initialize, fixed but worker thread can read it*
> *4 only supervisor thread can access while worker thread should not access*
> *5 only worker thread can access while supervisor thread cannot access"*

This framing became load-bearing for the rest of the design. Mapped onto storage:

| Category | Storage | Mechanism |
|---|---|---|
| 1 | Handle | `lnx::atomic*`, supervisor `store_release`, worker `load_acquire` |
| 2 | Handle | `lnx::atomic*`, worker writes (relaxed for non-causal stats), supervisor reads |
| 3 | Handle | Plain const fields; happens-before via `pthread_create` |
| 4 | Outside handle (registry / main) | Worker has no path |
| 5 | Engine | TLS singleton |

The matrix is the spine of the entire design: every per-thread field has exactly one category, and each category has exactly one storage location.

---

## The TLS-singleton turning point

The first proposal was a single `worker_registry` Meyers singleton — process-global, supervisor reaches into worker state via it. User pushed back:

> *"If singleton is made, then not only supervisor main thread but also all worker threads can access all contents inside worker_registery singleton. If you want to enforce worker thread only access its value you need another design pattern based on TLS singleton."*

Right — process-global singleton is enforcement-by-convention. The reply walked through the Seastar `engine()`-style split with a TLS-pointer-to-mmap-body sketch (over-engineered). User cut it down:

> *"TLS singleton is just singleton that declare instance inside class private part as thread_local _instance; instead of static _instance"*

The simplification matters: `static thread_local engine_worker inst;` inside `instance()` IS the TLS body. No separate pointer-to-arena dance. Same mechanic as `mem::packet_pool::instance()` already does — pattern reuse, not invention.

This collapse made the handle/engine split sharp:
- **Handle** = process-global, cross-thread surface (Meyers singleton holds N of them in the registry)
- **Engine** = per-thread, TLS singleton (Meyers `static thread_local` inside `instance()`)

Same idiom, opposite scope.

---

## Naming flip — category-first

The first sketch used `worker_handle`, `worker_engine`. User flipped to category-first:

> *"naming convention : handle_thread handle_db engine_worker engine_db sounds better"*

Matches the project memory's existing convention (`profiler_scope.md`, not `scope_profiler.md`). Files cluster by kind in directory listings: all `handle_*.h` together, all `engine_*.h` together. Adding a future role (`handle_renderer`, `engine_renderer`) drops in alphabetically without scattering.

---

## Supervisor's shape

User confirmed two points that finalized the supervisor's role:

> *"1. workers / db thread message the supervisor : probably by spsc queue rather than system*
> *2. supervisor will control all thread engines."*

Point 1 → supervisor IS an active-object thread, needs a `handle_supervisor` with inbox queues. Point 2 needed a tiny clarification: supervisor can't directly touch engine state under TLS gating. "Control" is always handle-mediated — atomic publish (`_state.store_release(draining)`) or SPSC command message into the target handle's inbox. The engine remains TLS-private; the handle is the supervisor's entire API surface.

v1 does NOT have an `engine_supervisor` — supervisor body lives in `main()`. If supervisor logic ever extracts out of main, promote to an explicit engine then.

---

## "Damn complicated wow"

A natural reflection point in the middle:

> *"So damm complicated wow"*

Honest acknowledgment back: yes, it is, and the complexity ISN'T arbitrary. Each of the five access categories exists because of a specific concurrency hazard, and using the wrong mechanism for the wrong category produces a specific class of bug (torn read on a plain bool, mutex blocking the worker tick, UB from happens-before not established, etc.). The discipline IS the design. Once a field is placed in the right bucket, the storage location and synchronization mechanism are determined; adding a new field becomes a 30-second decision instead of a re-derivation.

This was the design's load-bearing claim: the complexity is upfront, not ongoing.

---

## Foundation doc written, then codex review

After all naming + structure locked, wrote `.omc/wiki/handle-engine-split-pattern.md` capturing the pattern, the 5-category matrix, the storage flavors, the naming, composition discipline, per-role specifics, and a trampoline sketch.

User asked for a critical review via codex agents. Codex (via `codex:codex-rescue`) returned **10 findings — 1 CRITICAL, 4 HIGH, 3 MEDIUM, 2 LOW** with the verdict *needs-revision-pass*. The findings split into "just fix the doc" (5 items) and "requires new design discussion" (5 items, including the engine construction protocol the doc had flagged as TBD).

The highest-value catches:

- **CRITICAL** — the doc said "supervisor's TLS pointer is null" but Meyers TLS singletons don't have null pointers; they construct lazily on first call. A supervisor that mistakenly called `engine_worker::instance()` would silently construct a fresh 573 MiB engine on the main thread. The fix is a role-token guard at the top of `instance()` that traps before the static thread_local storage is referenced.
- **MEDIUM** — the `_state` release/acquire claim for `_kernel_tid` visibility was incomplete; the doc didn't say the reader MUST do an acquire-load on `_state` before reading `_kernel_tid` for the publish ordering to work.
- **HIGH** — the matrix only covered supervisor↔worker. Real systems also need worker↔worker (peer mesh) and worker↔db (query/reply) flows. These are not state-access patterns (they're message passing), so the fix was an ORTHOGONAL "Inter-thread communication channels" section, not new matrix rows.

User picked **Option B** of the response — apply the "just fix" set immediately AND start the engine-construction discussion in the same pass to fill in finding #6 (the TBD engine construction protocol).

7 edits applied, doc grew from 242 → 268 lines. All CRITICAL/HIGH/MEDIUM doc fixes landed; `Status:` demoted from "Locked" to "Draft (foundation)" to honestly signal what's still in design.

---

## Engine construction — three sub-decisions, one at a time

User asked to discuss one decision at a time. Three were on the table:

### Decision 1: Lazy Meyers vs eager init

Two options:

- **(A) Lazy Meyers** — `static thread_local engine_worker inst;` inside `instance()`. Ctor takes no args; engine learns its handle via separate `attach()`.
- **(B) Eager init** — supervisor pre-allocates engines in mmap arena, trampoline plants TLS pointer. Ctor can take args.

User picked **A**:

> *"lazy Meyers sounds more reasonable for portfolio and engine basics level, Option B is more close to production level optimization"*

Right framing — B is the kind of optimization that pays off at production scale where supervisor-driven boot wiring saves a few milliseconds per worker. For portfolio + first-principles learning, A's pattern reuse with `mem::packet_pool` is the simpler mental model.

### Decision 2: Role-token guard shape

Two options:

- **(A) Shared `detail::tls_role` enum** — one TLS variable across all engine types; every `instance()` checks the same enum
- **(B) Per-engine `thread_local bool` flag** — each engine has its own armed-flag

User: *"I am not really sure about this one honestly."*

This was a small enough decision that being unsure made sense. Both options are mechanically valid; the trade-off is about whether you'll often want to ask "what role is this thread?" cross-cuttingly (favors A) or whether per-engine isolation feels cleaner (favors B). Reframed as: *defer to implementation time, decide in the moment when typing the first engine, refactor if the other shape turns out cleaner*. User picked **defer**:

> *"Can we just skip this for this phase?"*

Yes — the invariants box already says "thread-local role-token guard installed at trampoline entry," which doesn't commit to either shape. The trampoline marks the install point with a `TODO(impl)` comment.

### Decision 3: `bind()` / chicken-and-egg

The Meyers limitation (ctor takes no args) means the engine constructs handle-agnostic and learns its handle via a separate post-construction call. First name was `bind()`. User caught the obvious problem:

> *"bind() might be confusing due to socket bind, in network library programming. thread controlling engine connected to originally settled metadata in supervisor might need some different name."*

True — `::bind(socket, addr)` shows up everywhere in a network-library codebase. Proposed alternatives (`attach`, `adopt`, `wire`, `set_handle`, `mount`, `register_with`). User picked **`attach`**.

The mechanic locked:
- `attach(h)` — sets `_handle = h`, with `LNX_CHECK(_handle == nullptr)` and `LNX_CHECK(h != nullptr)`
- `run_loop()` preamble — `LNX_CHECK(_handle != nullptr)`
- `_handle` storage — plain pointer, not atomic (only the worker thread writes and reads it, Lock 7 applies)

User then asked an interesting structural question:

> *"Is it possible to call codes inside entry as worker engine class constructor or thread constructor?"*

Honest answer: partially, but not fully. The engine ctor (Decision 1) already absorbs the handle-agnostic preamble (mmap, io_uring init). What can't move into a ctor is the handle-aware work — `pthread_setname_np` needs the handle's `_name`; the kernel_tid cache needs `gettid()` to run on the new thread; the release-store of `_state == running` must be the *last* step before the loop. The trampoline is the procedural glue that respects the ordering constraints. Industry pattern (Schmidt POSA2, Seastar's `reactor::run`, Akka's actor base classes) all keep this layer procedural.

An RAII helper (`worker_thread_init`) was sketched as an option for cosmetic simplification, but ruled out for v1 — the trampoline at ~8 meaningful lines is already terse; adding a class for cosmetics is more types than the savings warrant.

---

## Doc updates after locks

Two final edits closed the loop:

1. **Trampoline section** — removed "(illustrative)" qualifier, dropped the "TBD page" sentence, updated `bind()` → `attach()` throughout, added the `TODO(impl)` comment marking the role-token install point
2. **New section: "Engine construction protocol"** — captures Decisions 1 + 3 explicitly, documents that Decision 2 is deferred to implementation with the two viable shapes listed for future-self to pick from

Final doc: 337 lines, 13 sections. Status accurately reflects "engine construction protocol locked; role-token mechanism + init failure / shutdown ordering / boot wiring still in design."

---

## Open items for next session

| Priority | Topic | Notes |
|---|---|---|
| High | **Init failure modes** | mmap fails, io_uring_queue_init fails, pthread_create fails, listen fails. Needs a policy (LNX_CHECK fail-fast before publishing `running`, or introduce `failed_to_start` state with cleanup). Affects both `handle_thread` and `engine_worker` shape. |
| High | **Shutdown ordering at process exit** | Registry singleton dtor vs engine TLS dtors vs `lnx::thread` dtors. Specifically: engine dtors must not touch registry queues after the registry singleton is destroyed. |
| Medium | **Boot wiring order** | Supervisor allocates SPSC storage → creates handles → installs pointers → starts db → starts workers. Race-free sequencing. |
| Low (mechanical) | **First code: `handle_thread.h`** | Translate the locked design into a header. ~30 lines, zero design decisions left. |
| Low (mechanical) | **Refactor `worker-class-and-thread-roles.md`** | Becomes a thin pointer to `handle-engine-split-pattern.md`. Land with first code commit. |

---

## Reflection — codex review value

The codex review was the highest-leverage moment of the session. It caught one real correctness bug (the false TLS-null claim) and four documentation gaps that would have tripped up an implementer reading the doc cold. ROI: ~2 minutes of agent delegation, ~10 minutes of fixes, saved an unknown amount of confused-future-self time. Pattern worth repeating before any foundation doc lands.

The codex agent is resumable (`a69d3ee50389052dc`) if a follow-up review pass on the revised doc is wanted later.

---

# Evening addendum — implementation, latent bug, wiki revision pass (2026-05-25)

## What landed in code

Commit `bbe6851` ships the foundation skeleton from the morning's design:

- `src/app/config.h` — placeholder POD
- `src/app/detail/thread_role.{h,cpp}` — shared `tls_role` enum (Decision 2 → **Option A**, the deferred sub-decision resolved at implementation time)
- `src/app/handle_thread.{h,cpp}` — universal per-thread metadata, lifecycle observers, `request_stop()` / `join()`
- `src/app/handle_worker.{h,cpp}` — composes `handle_thread`, trampoline (role-token install → setname → kernel_tid publish → packet_pool prewarm → engine instance → `attach()` → CAS-promote → `run_loop`)
- `src/app/engine_worker.{h,cpp}` — TLS Meyers singleton, `attach()` + `run_loop()` with three `LNX_CHECK` guards
- `tests/app/handle_worker_skeleton_test.cpp` — full lifecycle + idempotence tests

89/89 tests, 1.03M assertions, clean under ASan+UBSan and TSan (via `setarch -R` per `docs/06-system-setup.md`).

## The race that wasn't in the design

The first test run hung. Cause: unconditional `_state.store_release(running)` at the end of the trampoline can clobber a concurrent `request_stop()`'s `draining` write — worker spins forever with no observation of the stop request.

Fix landed in `bbe6851`: introduced a 4th state value `starting` (ctor default), and the trampoline CAS-promotes `starting → running`. CAS-loss path (observed == `draining`) means supervisor requested stop during preamble → short-circuit to `stopped` without entering `run_loop`. The runtime three-state contract (`running → draining → stopped`) from the worker-lifecycle wiki is preserved; `starting` is a pre-running sentinel with no runtime semantics.

The wiki review (next section) found a deeper bug here that the skeleton tests dodged by accident.

## Wiki review pass — three codex agents in parallel

Three `codex:codex-rescue` agents dispatched in parallel against the three wiki pages most affected by the day's work. Verdicts:

| Page | Verdict | Key findings |
|---|---|---|
| `handle-engine-split-pattern.md` | needs revision before promotion | Status still "Draft"; Decision 2 still marked deferred; trampoline still shows TBD role-token install; struct sketch still 3-state |
| `worker-lifecycle-three-state-protocol.md` | sentinel framing recommended (not rename) | Page is "Three-State Protocol"; needs 4-state acknowledgment; transition table missing 2 rows; enum example outdated; **flagged `request_stop` doesn't handle `starting → draining`** |
| `worker-class-and-thread-roles.md` | Option B (prune + banner) | 8 obsolete line ranges identified; universal-pattern + atomic discipline + identity rationale parts still load-bearing and should be preserved |

The middle one was the highest-impact catch: the `starting → draining` gap in `request_stop()` is a real latent bug. The skeleton tests dodged it because they always `spin_until_running` before `request_stop`, but external callers had no such guarantee — a `request_stop()` call landing during trampoline preamble would silently drop. Commit `a2de49e` fixed it with TDD:

1. Added a failing test that calls `request_stop()` on a fresh handle (state == `starting`) and asserts `is_draining()`. Pre-fix it failed at the assertion as expected.
2. Replaced the single CAS with a CAS-loop accepting both `starting` and `running` as legitimate pre-stop states.
3. Re-ran: 90/90 tests pass, clean under ASan+UBSan+TSan.

## Wiki revisions applied (local-only — `.omc/` is gitignored)

`handle-engine-split-pattern.md` (337 → 393 lines):
- Status promoted from "Draft (foundation)" → "Locked (foundation implemented `bbe6851` + `a2de49e`)"
- `handle_thread` struct sketch updated: state enum now 4-state, `_state` ctor default = `starting`
- Lifecycle API description updated: `request_stop()` is a CAS-loop accepting `starting` or `running`
- Trampoline section rewrote the code block: role-token install as step 1, CAS-promote with explicit branch on CAS-loss
- Decision 3 section retitled "Role-token guard mechanism — Option A (shared `tls_role` enum)" with concrete code from `src/app/`
- Documentation roadmap table now shows ✅ for the foundation and ⬜ for per-class detail pages
- Added "Still-deferred design topics" subsection enumerating init failure / shutdown ordering / boot wiring

`worker-lifecycle-three-state-protocol.md` (228 → 270 lines):
- Title kept as "Three-State" — sentinel framing, not promotion
- Subtitle extended: "Three-State Runtime Contract (+ starting boot sentinel)"
- Updated date stamped 2026-05-25
- Cross-link to `[[handle-engine-split-pattern]]` added
- Transition table extended with `starting → running`, `starting → draining`, `starting → stopped` (short-circuit) rows
- New "Boot sentinel — why a fourth state value exists" subsection explains the race and the CAS-promote fix
- API surface pseudocode updated: 4-state enum, `is_starting()` observer, trampoline-based pseudocode with CAS-promote + CAS-loss branch
- Renamed conceptual class from `worker` to `active_object_handle` with a note that the live code is `handle_thread` / `handle_worker` / `engine_worker`

`worker-class-and-thread-roles.md` (467 → 156 lines, **net -311**):
- Option B applied: prune obsolete `app::worker` umbrella class material, keep role-agnostic discipline
- New banner at top marking the page as historical and pointing at `[[handle-engine-split-pattern]]`
- Kept: three-roles inventory, Pattern A vs Pattern B, portable definition, data-plane diagram, universal active-object patterns (#1–7), `lnx::atomic` vs `std::atomic` discipline, identity rationale (`id` / `name` / `kernel_tid`)
- Removed: `## The worker class is an active object`, universal anatomy + inbound channels, `worker.h` shape, `worker.cpp` shape, API design rules, "Lifetime: supervisor owns worker objects", "Open items deferred to implementation"
- Updated cross-references to point at `handle-engine-split-pattern` and the session-account data model

## Status of the deferred topics

Still owed for future sessions (in approximate priority order):

| Topic | Reason it's still owed |
|---|---|
| Init failure modes | `mmap` fail, `io_uring_queue_init` fail, `pthread_create` fail, `listen` fail. Need a policy: hard-fail before publishing `running`, or introduce `failed_to_start` lifecycle state. |
| Shutdown ordering at process exit | Registry singleton dtor vs engine TLS dtors vs `lnx::thread` dtors. Engine dtors must not touch registry queues after the registry singleton is destroyed. |
| Boot wiring order | Supervisor allocates SPSC storage → creates handles → installs pointers → starts db → starts workers. Race-free sequencing. |
| `peer_msg` / `aux_msg` tagged union shape | Currently the mesh inbox pointers are absent in the skeleton; types decided when first peer-comms use case lands. |

## Reflection — wiki review value, second pass

The morning's codex review caught documentation gaps (the false TLS-null claim being the biggest). The evening's wiki review pass found a real latent bug in the code (`request_stop` dropping `starting → draining` requests). Both passes were ~2 minutes of agent dispatch and ~10–20 minutes of follow-up work; both paid back many multiples of the investment.

Worth noting: the bug surfaced as a remark inside a documentation-review finding ("the spec does not cover `starting → draining`"), not as a separate code-review finding. Wiki/code consistency review is incidentally a code-correctness review when the code is implementing what the wiki says.
