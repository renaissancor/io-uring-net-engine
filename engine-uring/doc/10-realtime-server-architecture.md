# 10 — Realtime Server Architecture

Durable source of truth for the runtime shape. This is a **live design doc**,
not a historical log — keep it short enough to stay current. Dated design
discussions live under `../design-notes/`; decision records live in the
`.omc/wiki/` and `doc/` trees.

---

## 1. Project identity

`engine-uring` is a **Linux-native C++20 realtime interaction network engine
for MMO/RTS-style servers**, built to keep session I/O, packet parsing, and
authoritative world-state mutation on the same owner thread so per-interaction
latency is predictable.

The first visible demo is **room-based chat**. That is a testbed for the
`InteractionSpace` model, not the ceiling of the architecture.

## 2. Why C++ / Linux / io_uring

The decisive reason is **realtime interaction latency**, not raw throughput:
packet-to-state-mutation latency, tail latency, and per-tick predictability,
with direct control over memory, syscalls, wakeups, and ownership boundaries.

- **C++** — direct control over the packet-to-state-mutation path, memory
  layout, wakeups, and per-tick behavior.
- **io_uring** — a Linux-native completion path that fuses into the owner
  thread's event loop, cutting syscall/wakeup/buffer-movement overhead.
- **worker owns fd + room/world state** — recv completion → commit bytes →
  parse frame → execute handler → mutate authoritative state, all on one thread.
- **SPSC message passing** — cross-thread coordination with no mutex and no
  shared mutable state on the hot path.

## 3. Thread roles

```
Supervisor / Main
  sole spawner; owns config + queues + thread control blocks; sigwait + ordered
  shutdown. Never runs an engine.  (src/app/main.cpp)

SessionManager / Acceptor
  owns listen/accept; mints session_id + generation; fake-auths guest
  sessions; owns the session_id -> state/owner authority map; hands the fd to
  a worker; receives SessionClosed back.  (acceptor_ctl / acceptor_engine)

WorldThread / Worker
  owns active session fds, per-session recv/send rings, rooms/realtime state,
  and its own io_uring; parses packets and executes handlers in-thread.
  (worker_ctl / worker_engine)

LoggerThread   — later. One SPSC queue per producer; never blocks a worker.
Db / AuthThread — later. Ticket validation off the hot path.
```

v1 boots **three roles, one worker**. Multiple workers, world migration, DB
auth, and the logger thread are later passes.

## 4. Ownership invariants (non-negotiable)

1. One TCP session fd has exactly one owner thread at a time.
2. One world/zone/room state is mutated by exactly one owner thread.
3. The worker that owns a session dequeues its packets and runs the handler in
   that same thread — no "parse on thread A, execute on thread B" split.
4. Cross-thread communication is message passing through SPSC byte pipes
   (`sds::pipe`, framed by `app/mesh.h`), never shared mutable state on the
   hot path.
5. Gameplay/chat packets are valid only **after** `S_ENTER_WORLD_OK`.
6. DB/auth/security are deferred; v1 uses a fake guest identity.

The SessionManager stays authoritative for `session_id -> state` and
`session_id -> owner` even after handoff. **Authority is the mapping, not the
I/O** — it does not keep fd read/write ownership once a worker adopts the fd.

## 5. First three-thread milestone

Supervisor + one SessionManager + one Worker carrying a single client through
connect → room select → chat → disconnect, with the SessionManager's authority
map staying correct throughout. See §"Final Test Target" in `handoff.md` and
the test rows in `doc/08-test-strategy.md`.

## 6. V1 client state machine

```
Connected  -> SelectingWorld -> InWorld -> Disconnected
                                    ^
                            (Transferring: later — world migration)
```

- **Connected** — worker has adopted the fd; may send `S_WELCOME` /
  `S_ENTER_SELECTING`.
- **SelectingWorld** — worker awaits `C_SELECT_ROOM`.
- **InWorld** — worker emitted `S_ENTER_WORLD_OK`; chat/gameplay now legal.
- **Disconnected** — fd released; worker posts `SessionClosed` to the acceptor.

## 7. V1 session handoff protocol

The clean long-term design lets the SessionManager own a pre-world
`SelectingWorld` state before worker adoption. **v1 does NOT do that** — it does
not add pre-world recv/framing to the acceptor, because that forces
acceptor-side session buffers and protocol parsing too early. v1 flow:

```
client connects
acceptor accepts fd
acceptor mints session_id + generation + fake account_id
acceptor posts adopt_session(fd, id, generation, account) to worker 0
worker becomes SOLE fd owner; installs the session
worker sends S_WELCOME / S_ENTER_SELECTING
client sends C_SELECT_ROOM
worker joins room; sends S_ENTER_WORLD_OK
client sends C_CHAT   -> worker broadcasts S_CHAT to room members
client leaves/disconnects
worker posts session_closed to the acceptor
acceptor removes the mapping
```

Mesh vocabulary is `app::message.h`; transport is `sds::pipe<N>` (a bounded
byte stream, no message boundaries — the same mechanism as `sds::ring_buffer`,
named for its mesh role). Framing lives in `app/mesh.h`: `mesh_post` publishes
`[header|body]` atomically via `pipe::enqueue2`, and the reader runs the SAME
length-prefix parse loop the socket path uses.

## 8. Worker packet loop

Completion-driven, not scan-every-session-every-tick:

```
while running:
  drain acceptor -> worker pipe (adopt / stop)
  submit pending io_uring work
  drain CQEs:
    recv completion -> commit bytes to that session's recv ring
                    -> parse ALL complete frames -> execute handlers in-thread
    send completion -> advance send state -> submit remainder if any
  run room/world tick placeholder
  heartbeat++
```

The CQE identifies the session; parse only that session's newly-arrived frames.
If a ready-queue is ever needed, push session ids onto it only when a CQE or
mesh event makes them ready — do not poll idle sessions.

## 9. Blocking event-loop wakeup caveat

The current skeleton loops use `lnx::this_thread::yield()` and non-blocking
`io_uring_peek_cqe`, so a cooperative stop flag is observed promptly. **Once a
loop blocks** in `io_uring_wait_cqe`/`submit_and_wait`, an atomic stop flag
alone is insufficient: if no completion arrives, the thread sleeps in the kernel
and never observes the request. Pair `request_stop()` with a wake strategy —
eventfd wake, bounded-timeout wait, an io_uring timeout SQE, or a message-ring
wake. A bounded timeout is acceptable for the first pass if the latency/CPU
tradeoff is documented; eventfd wake is the cleaner end state. See
`doc/runtime/thread.md` § "Cooperative stop + blocking waits".

## 10. Future DB / auth model

```
SessionManager -> DbThread : AuthQuery (short-lived single-use ticket)
DbThread       -> Postgres over a Unix socket
DbThread       -> SessionManager : AuthResult (validates session_id + generation)
SessionManager -> Worker : adopt_session
```

Ticket validation consumes the ticket transactionally; never block the
WorldThread or SessionManager on a DB call.

## 11. Future logger model

One SPSC queue **per producer** (`Worker[i]`, `Acceptor`, `Supervisor`, later
`DbThread`). Producer timestamp = event time; logger timestamp = write time;
their difference measures logging backlog. A full logger queue must never block
a worker.

## 12. Non-goals for the current pass

Real login/auth, PostgreSQL, DDoS/session-interruption hardening, TLS,
persistence, multiple workers, world migration, open-world partitioning, final
RTS/MMO game logic, serious benchmark claims, and a Windows-native IOCP client
are all out of scope. **Prove the three-thread ownership model and room chat
first.** The io_uring echo smoke (`tests/net/echo_smoke_test.cpp`) remains a
low-level transport test, not the product milestone.
