# epoll-chat-study

Single-threaded, level-triggered epoll chat server. A **throwaway study build**
whose job is to teach the readiness model and to get the protocol logic right
somewhere cheap to debug, before it gets ported to io_uring in
`iouring-net-lib`.

Deliberately uses the STL, one file, and no abstractions. None of the
`iouring-net-lib` rules (no-STL, `sds::` containers, `u08` aliases) apply here.
**Do not try to make this good.** When the lessons are absorbed, it gets
deleted.

## Build and run

```bash
make                 # ASan + UBSan build (default; keep it this way)
./server 9000
```

## Test modes

The clients live in [`~/code/netbench`](../netbench) — both of them, for the
same reason: this repo is marked for deletion and they have to outlive it.

```bash
cd ~/code/netbench
python3 chatcli.py interactive --nick alice --room lobby
python3 chatcli.py verify  --clients 8  --messages 20    # content correctness
python3 chatcli.py dribble                               # one byte per send
python3 chatcli.py load    --clients 30 --messages 30
CHAT_SNDBUF=8192 ./server 9000                           # then:
python3 chatcli.py slowreader --clients 8 --messages 300
```

`CHAT_SNDBUF` exists because of a real trap — see lesson 7.

### Verified results

| test | result |
|---|---|
| `dribble` | server reassembles frames split across many `recv()` calls |
| `verify` 8×20 | 1,280/1,280 deliveries, exact bodies, 0 missing / 0 duplicate / 0 misattributed |
| `load` 30×30 | 900 chats → 27,495 frames, 100% delivery, ASan clean |
| `slowreader` | `[drop] fd=7 send buffer over cap (261948 B)`, server stays responsive |
| SIGINT | clean shutdown via `signalfd` |
| `loadgen` | see `netbench/README.md` — 2M deliveries/s with `CHAT_FLUSH=batch`, measured with a 3-process fleet |

## Clients live in `netbench`

Both the load generator (`src/`, driven by `fleet.py`) and the interactive/verify
client (`chatcli.py`) were extracted to [`~/code/netbench`](../netbench). They had to
leave: this repo is marked for deletion, and both are needed to evaluate the
io_uring server that replaces it — the baseline numbers exist precisely to be
compared against later.

```bash
cd ~/code/netbench && make
CHAT_FLUSH=batch CHAT_MAX_CONNS=60000 CHAT_QUIET=1 ./server 9000     # here
python3 fleet.py --nodes 3 --conns 3334 --rate 20 --duration 20 --port 9000
```

Use `fleet.py`, not a single `./loadgen`, for anything above ~500k
deliveries/s. One client process reports plausible numbers while saturated —
see netbench's "One process cannot verify itself".

Three environment knobs exist for it:

- `CHAT_FLUSH=batch` defers sends to one flush pass at the end of the epoll
  batch instead of calling `send()` inline per recipient. **See lesson 8 — at
  the same offered load it is worth four orders of magnitude on the p50 while
  moving the same number of frames, and it is the only fair baseline to
  compare io_uring against.**
- `CHAT_MAX_CONNS` raises the connection cap from its 4096 default. The server
  also raises its own `RLIMIT_NOFILE` to match — without that the cap just
  trades a polite refusal for lesson 6's EMFILE livelock nine thousand
  connections early.
- `CHAT_QUIET=1` suppresses the per-accept log line. Above a few thousand
  connections that line is a syscall per accept on line-buffered stdout, and
  it makes the server look slow when the measurement is really measuring
  `printf`.

### The result worth carrying forward

| | `immediate` | `batch` |
|---|---:|---:|
| ceiling | ~700k deliveries/s | **2M deliveries/s** |
| p50 at 1.4M/s | 460.1 ms | **0.073 ms** |
| p50 at 2.0M/s | — | **0.102 ms** |
| p50 at 3.0M/s | — | 18.533 ms (past the knee) |
| p99.9 at 500k/s | 12.328 ms | **0.382 ms** |
| kernel share | 92–94% | 86–93%, falling as load rises |

The `immediate` knee was **a property of the design, not the machine** — at
1.4M deliveries/s both modes moved the same 28M frames at 100% of one core,
and only the latency differed, by a factor of 15,000. Measured three times; the third
run used a **fleet of client processes**, and it withdrew the single-process
numbers above 500k/s — one loadgen process was stamping receive times per
epoll batch and was itself inside the flow-control loop, so it reported 0.136 ms
where three processes carrying the same load reported 18.533 ms. Full tables,
method, and the two mechanisms are in `netbench/README.md`.

## Protocol

4-byte header, little-endian, no byte swapping — the same shortcut the real
project takes.

```
struct { uint16_t len; uint16_t type; }   // len = payload bytes, header excluded
```

The real project's header (`iouring-net-server/docs/04-protocol.md`) is also
4 bytes, `[uint16 size][uint16 id]`, and differs in exactly one respect:
**`size` counts the header, `len` does not.** The width is the same; the
inclusive/exclusive convention is the whole porting seam, and it is what
`netbench --proto` switches. An earlier note here claimed the real project
used an 8-byte header — the protocol doc is authoritative and says otherwise.

| type | direction | meaning |
|---|---|---|
| 1 `C_SET_NICK` | → | set nickname |
| 2 `C_JOIN` | → | join room (nickname required first) |
| 3 `C_CHAT` | → | chat to current room |
| 100 `S_NOTICE` | ← | system message |
| 101 `S_CHAT` | ← | `nick: text` |

## The lessons

All eight are marked `LESSON n` in `server.cpp`, so the code and this list
cross-reference. Lessons 1–6 came out of writing the server; 7 and 8 came out
of measuring it, which is why their tags are scattered across several sites
rather than sitting at one function — lesson 8 in particular touches
`broadcast()`, the recv path, `flush_dirty()` and the main loop's tail
ordering, and none of those reads as related without the tag.

1. **The `EAGAIN` drain loop.** Under level-triggered epoll, looping `recv()`
   until `EAGAIN` is an optimization — read once and return, and `epoll_wait`
   re-reports the fd. Under **edge-triggered it is mandatory**: the edge already
   fired, unread bytes produce no further notification, and the connection
   hangs forever. This is *the* difference between the two modes.

2. **`EAGAIN` on send is not an error.** It means the kernel socket buffer is
   full. Keep the unsent tail, arm `EPOLLOUT`, return.

3. **Never leave `EPOLLOUT` permanently armed.** A socket is writable almost
   always, so a permanent `EPOLLOUT` turns `epoll_wait` into a 100%-CPU spin.
   Arm on partial send, disarm the instant the buffer drains.

4. **TCP is a byte stream, not a message stream.** One `recv()` can yield half
   a frame, or three and a half. `chatcli.py dribble` proves the parser handles
   it.

5. **Never `close()` mid-iteration.** Mark doomed, reap after the tick. Closing
   inline invalidates the container you're walking — and the fd number is
   immediately reusable, so a later event in the *same batch* can land on a new
   connection that inherited the number.

6. **The `EMFILE` trap.** Out of fds means `accept4()` keeps failing while the
   listener stays readable, so level-triggered epoll re-reports it forever: a
   100%-CPU livelock serving nobody. Fix: hold one fd in reserve, release it to
   accept the pending connection, close it politely, re-take the reserve.

7. **Backpressure is invisible on loopback by default.** The kernel auto-tunes
   `SO_SNDBUF` to ~2.5 MB, so `send()` swallows everything and you never see
   `EAGAIN` at all. The first slow-reader run pushed 1.4 MB and triggered
   nothing. Shrink `SO_SNDBUF` (hence `CHAT_SNDBUF`) *and* set the client's
   `SO_RCVBUF` **before `connect()`** — set afterwards it's cosmetic, because
   the receive window is negotiated during the handshake.

8. **Never `send()` inline while walking the room.** Queue into each
   recipient's buffer during the walk, then flush once at the end of the epoll
   batch. Sending inline holds the event loop inside syscalls while events pile
   up behind it, and the backlog — not CPU exhaustion — is what produces the
   collapse.

   The cleanest statement of the cost is that **it is not a throughput cost at
   all.** At the same offered load the two modes delivered 28,003,573 frames
   (batched) and 28,015,074 frames (inline) — the same work, in the same
   second — and reported p50 **0.031 ms** and **460.1 ms**. Inline `send()`
   does not reduce what the server can push. It makes every delivery wait out
   the backlog. And the damage appears in the tail long before the median: at
   rate 5, at 79% of one core, both modes report p50 0.022 ms while the p99.9
   is 0.382 ms batched and **12.328 ms** inline.

   Batching also gets *better* under load, because more messages accumulate per
   batch and coalesce into one `send()`. And it dissolves lesson 5b outright:
   with the flush deferred, a send failure can no longer doom a connection
   while the member set is being iterated.

   This one was found by asking whether the baseline was a fair opponent for
   io_uring. It was not — publishing the inline number would have credited
   io_uring with a win that plain epoll could take for itself.

### The bug worth the whole exercise

`reap_doomed()` originally used a range-`for` over `g_doomed`. The "X left"
notice it sends is a broadcast; a broadcast can hit the send cap on *another*
client, which dooms it, which `push_back`s onto `g_doomed` — the vector being
iterated. ASan caught the heap-use-after-free on the first slow-reader run.

This is a **cascading-cleanup** hazard, not an epoll hazard, and it will exist
verbatim in the io_uring version. Finding it here cost minutes; finding it
there would have meant untangling it from CQE ordering at the same time. That
is the entire argument for building this first.

Fix: index loop re-reading `size()` each iteration, so connections doomed
*during* the reap are cleaned up in the same pass.

## What ports to io_uring, and what doesn't

**Ports as-is** — this is the payload of the exercise:

- frame parsing and the partial-frame state machine
- room membership, broadcast fan-out, join/leave ordering
- the doomed-list / deferred-reap discipline and its cascade hazard
- backpressure policy (cap the pending buffer, drop and close)
- `MSG_NOSIGNAL` / SIGPIPE handling

**Does not port** — different failure modes entirely:

- readiness itself. io_uring is *completion*-based: no `EAGAIN` loop, no
  `EPOLLOUT` arming. Those disappear.
- buffer ownership. Here `std::string` is yours the whole time. There, a
  buffer handed to the kernel is untouchable until the CQE arrives — the
  dominant new bug class, and the reason `sds::ring_buffer` is non-movable.
- cancellation. Multishot ops must be cancelled and drained on teardown;
  `epoll_ctl(DEL)` has no equivalent.
- `std::string::erase(0, n)` per frame is an O(n) memmove. Fine here,
  replaced by `sds::ring_buffer` there.

## Scope discipline

Timeboxed to one week. Done means: accept, framing, rooms, broadcast, clean
disconnect, and a forced partial-send path. All six are verified above.
**Then stop and go back to `iouring-net-lib`.**
