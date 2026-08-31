# Session/Account Data Model Discussion — 2026-05-23

Companion record to `.omc/wiki/chat-server-v1-session-account-data-model.md`.
Captures the conversation flow behind locks 1–7, the user's redirects when the
discussion drifted, and the two items (8, 9) explicitly deferred.

---

## TL;DR

Locked across the discussion:

1. **Session lifetime = TCP connection lifetime (Reading 1).** New TCP = new session. Account is the durable identity. Reconnect = re-auth.
2. **`session` and `account` are separate concepts.** session = network, account = app/DB. Drop the `_state` suffix.
3. **`class session` = connection status + ring metadata.** `class account` = DB-backed authenticated identity. Both are zero-cost handles over SoA columns.
4. **session row and account row co-located** at the same `local_idx` on the same worker.
5. **Bridge** = forward `account_id[idx]` column + reverse `account_id → local_idx` hash map.
6. **Per-worker account visibility + single-occupancy.** Each worker knows its accounts; one account is on exactly one worker. Forces a process-global `account_id → worker_id` lookup, naturally homed on db_thread.
7. **Single-owner-at-a-time invariant per session.** Rings + SoA columns + bridge entry touched by exactly one worker at any instant.

Deferred:
- **8.** Counter atomicity / cache-line padding (consequence of #7, needs own pass)
- **9.** Migration mechanism (separate topic)

---

## How the conversation started

The opening prompt asked for a ring-buffer storage architecture answering three concrete sub-questions:

1. **Storage backend** — mmap vs BSS vs hugepage?
2. **Buffer layout under SoA** — flat per-worker arrays vs alternatives?
3. **Wraparound handling** — single SQE-to-wrap, `RECVMSG` with iovec, or compact-then-recv?

The initial answer proposed:
- mmap with `MAP_POPULATE` per worker (hugepage as opt-in flag)
- Split-region SoA: recv_region[8192 × 4 KiB] + send_region[8192 × 64 KiB]; no per-slot `alignas(64)` on counters
- `RECVMSG` / `SENDMSG` with 2-element iovec when the wrap straddles; per-worker `serial_buffer` scratch for parser-side stitching

That answer was structurally fine, but the user redirected before locking it — they
wanted to first re-examine the *ownership boundary* between storage and workers.

---

## The first redirect: global vs per-worker storage

> *"I was thinking about session as global rather than universal, all ring buffers belonged to global sequential memory space instead of existing per worker thread, and each worker thread change ownership."*

This reopened a tension the wiki had moved past in 2026-05-20/21: the 2026-05-19 evening
project memory had said "migration is architecturally supported by flat session_id + SoA",
but the wiki's later lock said "sticky session assignment, never migrate."

The discussion teased apart three motivations that could drive a global-storage choice:

1. **"Architecture must not foreclose migration"** — supported by a flat process-global session_id with routing-table indirection, *regardless* of where backing storage lives.
2. **"Workers should be able to read/write each other's rings directly"** — would unwind the locked cross-thread = inbox-copy rule.
3. **"Migration should be byte-shuffle cheap"** — false framing; migration is dominated by control-plane work (drain SQEs, re-register fd/buffers, route flip, inbox quiesce), not bytes.

User confirmed motivation **(1)**: keep migration architecturally possible, not necessarily exercised, and not pretending to be cheap. This unblocked the rest of the design without unwinding any wiki commitments — the routing-table-global / storage-per-worker shape gives migration capability without paying the NUMA tax of a single global mmap region or the cost of widening the cross-thread access model.

---

## Pinning the problem: content vs. session migration

> *"In application perspective, content interaction happens inside each worker thread. However, each session should be allowed to move between each worker thread."*

Half agreed:

- **Content interaction is per-worker.** Locked, no contention.
- **"Sessions allowed to move"** — needed disambiguation between:
  - (a) Architecture must permit migration (capability)
  - (b) Migration is a routine exercised operation (load balancing, hot-room rebalance)
  - (c) Migration is cheap (~byte-shuffle cost)

User clarified later:

> *"1 Yes migration should be allowed, if you want to change chatting channel or game map you should be allowed. 2 migration method can be decided after session - application relationship is defined. 3 migration is never cheap, but must be implemented."*

So: (a) yes, (b) acknowledged as needed *somehow* (deferred design), (c) explicitly rejected (migration is hard regardless of approach).

Important: the user also said they wanted to nail the **session ↔ application** relationship *before* designing migration. That set the agenda for the next several turns.

---

## The session ↔ account split

User's framing move (Lock 2):

> *"If something is account_state it should be about account. If something is session_state it should be about session. For instance network related stuff is in session. Authentication stuff or any non network stuff about application or user info is in account."*

This crystallized a clean separation:

| `class session` | `class account` |
|---|---|
| state (lifecycle enum) | account_id |
| fd, gen, remote_addr | username_cached |
| head/tail recv & send | profile_blob_ref |
| last_packet_at, ping_deadline | current_room_id |

User then refined further when locking:

> *"3 for session it is connection status and ring buffer metadata precisely, for account DB based authentication is source, detail implement later"*

So the session class is *purely* connection status + ring metadata (nothing else), and the account class is *purely* DB-backed identity. No fields leak between them.

---

## Account identity stability

> *"Suppose id password based user account with user name and personal information. This should be fixed. And after session gets connected, user will do authentication, and if authenticated, session will get assigned to certain account. This account assigned to session will be fixed. Even though session info of client side might change in mobile environment, still account is fixed."*

This statement had two readings:

- **Reading 1:** mobile connection drops → new TCP socket → new session, same account binds again. Session lifetime = TCP connection lifetime.
- **Reading 2:** mobile connection drops → reopens → same logical session continues (resume token / ghost state).

User picked Reading 1:

> *"I think technically in server engineering perspective reading 1 should work. Even though in UX perspective making it continuous is different problem, reading 1 should be right choice."*

Locked as item 1. UX continuity is treated as a separate concern (client-side handling, room-replay layer, etc.), not bolted into the server's session model.

---

## Class shape: handles over SoA

User then collapsed the naming distinction:

> *"What you call as session_state and account_state can be just member of class session and class account"*

This needed disambiguation: "member of class" can mean AoS (class owns the bytes) or SoA + handle (class is a typed view, bytes live in worker SoA columns). The locked project-memory pivot was SoA + session-as-handle, so the read was confirmed as **zero-cost handle classes**:

```cpp
class session {
    worker* w; uint32_t idx;
public:
    int32_t fd() const { return w->sessions.fd[idx]; }
    // … resolves to SoA column reads
};
```

No data is owned by the class instance — only `(worker*, idx)`. Compiles to the same machine code as direct `worker.fd[idx]` access.

---

## Worker class and TLS

User's intuition:

> *"Worker class basically means, what thread does it belong to, and each TLS is managed by worker class?"*

Half-correct, refined:

- **Thread identity** — yes, `class worker` ↔ one OS thread, per [[worker-class-and-thread-roles]].
- **TLS management** — flipped. The locked design uses *instance members* of `class worker` for cross-thread-reachable state (queues, atomics, lifecycle), and reserves `thread_local` for singletons that handler code reaches without a `worker*` (currently `mem::packet_pool::instance()` and `profiler::manager::instance()`).

User then refined:

> *"Okay each TLS is not managed by worker class but after lnx::thread is launched and function is executed"*

That's the precise framing: TLS lifetime is bound to the **thread**, not to the worker object. The worker class's only relationship to TLS is *being the first to call `mem::packet_pool::instance()`* on the newly spawned thread inside `worker::entry()` — that triggers magic-static construction. The TLS owns itself; the worker just initialized it by being there first.

Migration implication: SoA columns (instance members) can move across workers via inbox `memcpy`. TLS-allocated packets *cannot* — they're locked to their allocating thread by the alloc-thread == free-thread rule. The migration protocol must drain in-flight packets before moving.

---

## Ring buffers under the fused model

User:

> *"Session stays there, each session ring buffer getting filled by io-uring, and each worker thread will dequeue from session ring buffers for sessions each worker holds."*

Confirmed, with one correction: the rings are **not SPSC in the classical kernel↔user sense**. Under the fused per-worker model ([[threading-model-per-worker-io-uring-copy-via-inbox]]), the worker drives both ends — submits the recv SQE, observes the CQE, advances tail; later parses from head and advances head. The kernel writes bytes into the ring storage at addresses the worker provides, but does **not** touch the head/tail counters.

This contradicts the old project-memory framing ("SPSC per session, producer/consumer roles defined by who's kernel vs content thread") which was correct under the 2026-05-17 morning architecture (separate network and content threads) but obsolete under the fused-worker lock that came after.

---

## The redirect away from room migration

The discussion drifted briefly into Architecture α (session migrates to follow room) vs Architecture β (session sticky, rooms have home workers, broadcasts route via inbox). The wiki has β locked. User pulled the discussion back:

> *"Hey, forget room. This is about thread change not room change in same thread. Latter one is not issue."*

That clarified the migration discussion in question is **session-to-different-worker movement**, not **content moves between rooms**. Room change is handled cleanly under Architecture β via inbox membership updates; it's not migration at all.

---

## The single-owner invariant

User:

> *"My point is, each thread will access each session ring buffer. While one thread access one session ring buffer other thread should NOT access that session ring buffer."*

Locked as item 7 — the load-bearing invariant for the whole data layout. Stated precisely:

```
For each session_id S, there exists AT MOST ONE worker W at any
instant such that W is authorized to access:
  - S's ring storage
  - S's SoA columns (session + account)
  - S's reverse-map entry

When W has authorization, no other worker reads or writes any of those.
```

This is what makes head/tail counters lock-free without atomics, lets ring bytes move in program order, and lets the TLS packet_pool invariant compose automatically.

---

## The single-occupancy invariant for accounts

User's refinement to item 6:

> *"Each THREAD knows what Accounts they get belonged to, one account MUST be occupied by ONE worker."*

This is the account-side mirror of item 7. An authenticated account is hosted by exactly one worker; no two workers can both believe they host the same account.

**Non-obvious consequence:** per-worker maps can answer "do I host account A?" but cannot answer "which worker hosts account A?" — needed for cross-worker kick-old. The natural home for that lookup is **db_thread**, which already serializes auth and touches all auth flows:

```
W' → db_thread:  AUTH_REQUEST
db_thread → PG:  verify hash, get account_id
db_thread:       consult account_binding_map[account_id]
db_thread → W':  "OK, account_id = A; existing binding = (W, S1) or none"
db_thread:       atomically updates map to (W', new_session_pending)
W' → W (if existing): "kick S1, reason=logged_in_elsewhere"
W  → W' :        "kicked, you may proceed"
W' :             completes binding
```

Rejected alternative: per-worker fan-out probe broadcast (scales with N).

---

## Items explicitly deferred

| # | Topic | Reason for defer |
|---|---|---|
| 8 | Counter atomicity / cache-line padding | A *consequence* of #7, but the concrete type choice (`uint64_t` vs `lnx::atomic64`) for cross-thread observable fields like stats needs its own discussion |
| 9 | Migration mechanism | Separate topic — the drain/serialize/inbox/re-materialize/ACK/route-flip protocol deserves its own design page |

---

## What this discussion DID NOT change

To be explicit about scope: this discussion did not touch any of the following locked decisions:

- Capacity constants (8192 sessions, 4 KiB recv, 64 KiB send, etc.) — see [[chat-server-v1-session-and-auth-design]]
- Six-state session lifecycle (accepted → … → closing) — same page
- Auth security non-negotiables (bcrypt/argon2, parameterized queries, pre-auth timeout, rate limit) — same page
- Worker active-object shape — see [[worker-class-and-thread-roles]]
- All-SPSC inter-thread mesh — see [[inter-thread-comms-spsc-mesh-pattern]]
- TLS `packet_pool` design — see [[memory-pool-tls-singleton-mmap-design-decision]]
- No-STL discipline, `lnx::atomic` types — project-wide
- Architecture β (sticky sessions, room home workers, cross-worker broadcast via inbox) — chat-server-v1 page §3

What it *did* change vs project memory:

- The 2026-05-19 evening "SPSC counters with `alignas(64)` per slot" framing is **superseded** under the fused-worker model. Counters are single-threaded; dense packing returns.
- The 2026-05-19 evening "frame view zero-copy or stitch" is **partially superseded** by the kernel-side iovec scatter approach (covered in the original three-question framing at the top, though not formally locked in this discussion).

---

## Where things go next

Natural follow-up threads (priority-ordered):

1. **Lock 8 examination.** Concrete types for SoA columns under Lock 7. Plain `uint64_t` for counters touched only by the owning worker; `lnx::atomic` for fields read by the supervisor (stats counters, heartbeat). Need to enumerate which fields fall in which category.
2. **Original three questions revisited.** Storage backend (mmap with MAP_POPULATE per worker), layout (split-region SoA), wraparound (RECVMSG iovec). Now that the data model is locked, can lock these too without ambiguity.
3. **db_thread account_binding_map design.** Hash table sizing, eviction discipline (on disconnect?), interaction with auth retry / kick-old timing.
4. **Lock 9 — migration mechanism.** Drain protocol, serialization shape of the migration inbox message, re-materialization on the destination, route-table flip ordering, ACK semantics.

Whichever comes first depends on what blocks Phase 2 implementation.
