# Supervisor Initialization & Acceptor-as-Lobby Discussion — 2026-06-06

Companion record to the `.omc/wiki/` revisions made the same day:
`supervisor-boot-thread-initialization-sequence.md` (new), and revisions to
`acceptor-role-and-connection-entry.md`, `supervisor-as-controller.md`,
`worker-class-and-thread-roles.md`.

Captures the conversation that (1) landed `sds::static_vector` as the
supervisor's worker-handle storage primitive, (2) reshaped the acceptor
role from "thin accept loop" to "accept + auth + lobby front-end," and
(3) produced the concrete supervisor boot sequence.

---

## TL;DR

Locked:

1. **Init failure handling is deferred.** Build and verify the happy-path
   boot first; add mmap/io_uring/pthread_create/listen failure policy after
   initialization is shown to work.
2. **`sds::static_vector<T, N>` is the supervisor's worker storage.** Inline
   fixed-capacity, variable-size, placement-new + manual dtor. Carries
   `handle_worker` (non-default-constructible, non-movable, address-pinned).
   Landed in commit `ed437cf`; full spec in `wiki/sds/static_vector.md`.
3. **Acceptor absorbs auth + lobby.** Server-side there is no separate
   "lobby" role — "lobby" is a *client-perspective* state. The acceptor
   accepts, authenticates, owns the connection's network I/O while the
   client picks a channel, then transfers session responsibility to the
   owning worker.
4. **One channel per connection.** A session belongs to exactly one channel
   → no multi-channel fan-out, session and its channel co-locate on one
   worker → the `hash(room_id) % N` cross-shard case disappears for v1.
5. **Post-login, the only valid client input is the channel selection.**
   Anything else is a protocol violation → drop at the door. This bounds
   the handoff: the fd handed to the worker is "clean" (no buffered
   application bytes trailing the selection message).
6. **Handoff = copy-via-inbox, not object transfer.** The fd (an int) crosses
   trivially (shared fd table); the acceptor's pre-session object is
   destroyed on the acceptor (TLS alloc==free invariant) and the worker
   constructs a fresh session seeded from a small handoff message
   `{fd, authed user id, channel, peer addr}`. This is exactly the locked
   copy-via-inbox rule — no new mechanism.
7. **Supervisor is the sole spawner and owns the listen socket.** It
   creates + binds + `listen()`s the socket (privileged, pre-drop), then
   initializes and starts every thread. Acceptor only `accept()`s
   (dequeues the backlog); it does not create the socket.
8. **Two boot ordering invariants** (see the new boot wiki page):
   landlord-allocates-all-mesh-storage-and-installs-pointers before any
   thread starts; all workers reach `running` before the acceptor starts.

Deferred:

- `channel → worker` mapping shape — default `channel_id % N`, hook left for
  an explicit assignment table. Pinned later.
- db role boot placement — boots before the acceptor (auth queries it),
  shuts down last; slots into the spine without disturbing it.
- Init failure modes (per #1).
- Acceptor admission *intelligence* (rate limits, per-IP caps, flood
  backpressure) — still purely additive to `engine_acceptor`.

---

## How it started — deferring init failure

Opening decision: ignore init-failure handling for now and implement it
later, "after checking that initialization works." Right ordering — you
can't design sensible failure policy until you've watched the successful
boot execute and know what each step touches. This scoped the rest of the
session to the happy-path boot.

Status check: the per-worker init path is already real and tested
(`bbe6851` + `a2de49e`): `start()` → trampoline (role token → setname →
publish tid → `packet_pool.prewarm()` → `engine_worker::instance()` →
`attach()` → CAS-promote `starting→running`) → `run_loop` → clean stop. The
next rung up is the *multi-worker* boot — the supervisor bringing up N
workers — which is the deferred "Boot wiring order" item.

---

## static_vector — the storage primitive the boot needs

The supervisor must own `n` `handle_worker` objects. `handle_worker` is
non-default-constructible (ctor needs `id` + `config&`), non-movable, and
**address-pinned** (its address is handed to its own engine via
`engine_worker::attach(h)`). That rules out `std::array` (needs default
ctor, fixed N), `std::vector` (banned by no-STL; relocation moves break
pinning), and `sds::malloc_vector` (trivially-copyable-only, memcpy growth).

A clarifying exchange: *"static_vector is just std::array right?"* No —
`std::array<T,N>` holds N *already-constructed* T's; `static_vector` is raw
aligned storage for N + a live count, constructing on demand via
placement-new. That distinction is the whole reason it fits a
non-default-constructible, non-movable type.

Implemented with union-of-array storage (alignment for free, no
`std::launder`), manual lifetimes (placement-new + reverse-order `~T()`),
`LNX_CHECK` on overflow/out-of-range. 8 Catch2 cases / 30 assertions green
under ASan+UBSan; the load-bearing test type mirrors `handle_worker`
(non-movable, self-counting) and asserts address stability across emplaces.
Landed `ed437cf`; documentation consolidated into `wiki/sds/static_vector.md`
rather than scattered across code comments.

---

## The acceptor reshape — "lobby is a client word"

The pivotal turn. The proposal: after the acceptor `accept()`s, it should
check whether the connection is authenticated, then act as a **lobby** —
holding the connection while the user selects world/channel/server (= a
worker), then hand it to the corresponding worker.

This departed from three then-locked acceptor decisions: thin acceptor
(no I/O on the connection), auth-on-worker, and load-based worker
assignment. Raised as a conscious change, with the key structural fork:
*is "lobby" a separate role, or folded into the acceptor?*

The resolution clarified the framing entirely:

> *"lobby is just client perspective. in server perspective, from dequeue
> by accept queue, create tcp session, notice player to be logged into
> lobby, and wait until player select 1 channel, and send session
> responsibility to corresponding worker. lobby is just inside acceptor
> thread, lobby exist for client."*

So there is no server-side lobby role — it's a client UX state. Server-side
it's all the acceptor thread. Plus two constraints:

> *"while client select what world to get in, acceptor thread controls
> network io of that session. Users should NOT be in multiple channels."*
> *"only possible input client should give after login should be what
> server it should get in."*

These three lines settled the model: acceptor owns the connection's I/O
through auth + selection; one channel per connection; the only post-login
message is the selection.

### Why the constraints matter

- **One channel per connection** → session and its channel co-locate on the
  chosen worker. The cross-shard `hash(room_id) % N` traffic the threading
  model worried about doesn't arise for v1. (A connection is an int fd; it
  can be adopted by exactly one worker, so multi-channel would have
  reintroduced cross-shard inboxes.)
- **Only-valid-input-is-selection** → kills the mid-stream-bytes hazard.
  After reading the one framed selection message, nothing trails it; the fd
  handed off is clean. Extra bytes = protocol violation → drop, and the
  acceptor is the right place to enforce that.

### The one reconciliation — copy, not move

"Send session responsibility to the worker" can't mean handing over the
session object, because of two locked invariants — and they make it clean:

- **TLS alloc-thread == free-thread:** the acceptor's pre-session was
  allocated in the acceptor's TLS pool, so it must be freed there.
- **Cross-thread = copy via inbox, never pointer passing** (threading-model
  rule #3).

So: acceptor reads selection → deregisters fd from its ring → pushes
`{fd, authed user id, channel, peer addr}` onto the worker's SPSC inbox →
destroys its pre-session. Worker pops → registers fd in its ring →
allocates a fresh session in its TLS pool, seeded from the message. The fd
crosses as an int (shared fd table); the object is recreated. The locked
rules already covered this case.

### Blast-radius note

The acceptor now holds auth credentials + pre-channel connection state —
the boundary moved from "transport-only" to "everything pre-channel." But
it still holds **no in-channel/room state and no control authority**, so it
remains isolated from both the data plane and the control root. Conscious
trade, accepted.

---

## Supervisor initialization — the boot sequence

With the topology settled (4 roles: supervisor, acceptor, workers, db;
"lobby" folded into the acceptor), the supervisor's init logic falls out:

```
main()  ── the supervisor, sole spawner ──
 1. create + bind + listen() the listen socket        [privileged; supervisor owns the fd]
 2. drop privileges                                    [if a low port was bound]
 3. build config + channel→worker map
 4. LANDLORD: allocate ALL cross-thread storage up front
      • sds::static_vector<handle_worker, N> workers   (main owns it)
      • acceptor→worker SPSC inboxes (one per worker)
      • handle_acceptor
    …install every queue pointer into the handles NOW, before any thread runs
 5. construct worker handles   → workers.emplace_back(i, cfg) ×N
 6. start workers              → w.start() ×N
 7. ── BARRIER ── spin until every worker _state == running
 8. construct + start acceptor → handed { listen_fd, &workers table, channel→worker map }
 9. supervise loop            → sigtimedwait(SIGINT/SIGTERM); observe heartbeats
10. shutdown (locked order)   → acceptor stop/join → workers stop-all then join-all → [db]
11. return 0
```

### Two ordering invariants make it correct

1. **Landlord allocates all mesh storage and installs all pointers before
   any thread starts** (step 4 before step 6). The acceptor is an SPSC
   producer into each worker's inbox; that inbox must exist before either
   endpoint runs. `main()` allocates it and writes the pointer into the
   worker handle pre-`start()`, so both worker (consumer) and later acceptor
   (producer) see a ready queue. No nullptr-inbox race.
2. **All workers reach `running` before the acceptor starts** (step 7 before
   step 8). `_state == running` is a sufficient readiness signal: the
   trampoline publishes `running` only after the engine ctor (ring init) and
   `attach()`, and the inbox pointer was installed even earlier (step 4). So
   "running" ⇒ "ring up, handle attached, inbox live" — safe for the
   acceptor to start handing off.

### Listen-socket split

Supervisor creates + binds + `listen()`s (privileged, before dropping
privileges); the acceptor only `accept()`s the backlog. On a dev/high-port
setup there's no privilege to drop, but keeping bind in the supervisor
preserves the privilege-separation story for real deployment. The
supervisor is the sole spawner — nothing spawns anything except it (root of
the supervision tree).

---

## Open items for next session

| Priority | Topic | Notes |
|---|---|---|
| High | **Skeleton boot implementation** | Steps 4–7 + 10 for workers only (no acceptor/socket yet) — the multi-worker boot test, using `sds::static_vector`. |
| Medium | **`channel → worker` mapping** | Default `channel_id % N`; decide whether an explicit table is wanted before the acceptor lands. |
| Medium | **db boot placement** | Before the acceptor (auth dependency); shutdown last. Confirm when db role is built. |
| Medium | **`engine_acceptor` shape** | Acceptor is now "worker-lite": own io_uring ring, recv/send buffers, pre-session pool, auth + selection protocol. Heavier than the old accept-loop sketch. |
| Medium | **Handoff message type** | `{fd, authed user id, channel, peer addr}` — concrete struct + SPSC element type; trivially-copyable for the inbox. |
| Deferred | **Init failure modes** | Per the opening decision — after happy-path boot works. |
| Deferred | **Acceptor admission intelligence** | Rate limits, per-IP caps, flood backpressure — additive to `engine_acceptor`. |

---

## Reflection

The session's leverage was in the acceptor reshape, and specifically in the
clarification that "lobby" is a client word, not a server role. That single
reframing collapsed a threatened 5th role into the existing acceptor and
made the three new constraints (one channel, owns-I/O-until-pick,
selection-only-input) cohere. The copy-not-move reconciliation then showed
the change needs no new mechanism — the locked TLS + copy-via-inbox rules
already describe the handoff. Init-failure-deferred kept the scope honest:
design and prove the happy path, harden after.
