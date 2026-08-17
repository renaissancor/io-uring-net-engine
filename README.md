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

python3 client.py interactive --nick alice --room lobby
```

## Test modes

```bash
python3 client.py dribble                       # frames split one byte per send
python3 client.py load --clients 30 --messages 30
CHAT_SNDBUF=8192 ./server 9000                  # then:
python3 client.py slowreader --clients 8 --messages 300
```

`CHAT_SNDBUF` exists because of a real trap — see lesson 7.

### Verified results

| test | result |
|---|---|
| `dribble` | server reassembles frames split across many `recv()` calls |
| `load` 30×30 | 900 chats → 27,495 frames, 100% delivery, ASan clean |
| `slowreader` | `[drop] fd=7 send buffer over cap (261770 B)`, server stays responsive |
| SIGINT | clean shutdown via `signalfd` |

## loadgen — connection scale

`loadgen.cpp` is the C++ load generator. Unlike the rest of this repo it is
**not throwaway**: it has to measure the io_uring server too, so the framing
constants are parameterised rather than hardcoded. When this study build is
deleted, `loadgen.cpp` moves to `iouring-net-server`.

Single-threaded on purpose. Scale out with processes and machines, not
threads — same core scaling, no shared state, and it forces the multi-box
design from day one.

```bash
make loadgen
CHAT_MAX_CONNS=60000 CHAT_QUIET=1 ./server 9000
./loadgen --conns 10000 --per-room 10 --rate 1 --duration 10
./loadgen --conns 40000 --per-room 10 --src-ips 4 --rate 1 --duration 20
```

### Verified results

| run | result |
|---|---|
| 10k conns, 1 source IP | 10,000 established, 0 failed, 0.10s, 0 attrition over 10s |
| 40k conns, 1 source IP | **28,232 established, 11,768 × `EADDRNOTAVAIL`** |
| 40k conns, 4 source IPs | 40,000 established, 0 failed, 0.45s, 0 attrition over 20s |
| `load` 30×30 after the above | 27,495 frames — no regression from the server changes |

Server RSS at 40k held connections: ~17.8 MB (~450 B/conn userspace). Kernel
socket buffers are not in RSS; that is the number that actually scales, hence
the small `SO_RCVBUF`/`SO_SNDBUF` defaults on both sides.

### What the numbers say

**28,232 is not a coincidence.** `net.ipv4.ip_local_port_range` is
`32768 60999`, and `60999 - 32768 + 1 = 28232`. A connection is identified by
the 4-tuple `(src IP, src port, dst IP, dst port)`; against a single server
`IP:port` the last two are fixed, so one source IP can only produce as many
distinct tuples as it has ephemeral ports.

This is a **client-side** limit and always was. The server's local port stays
9000 for every connection — `accept()` returns a new fd, not a new port — so
the server side varies `(src IP, src port)` and is bounded by fds and memory,
not ports. Switching to UDP would not have changed this in either direction.

Ephemeral ports are a per-source-IP kernel resource, so extra *processes* on
one box do not buy extra ports. `--src-ips` binds across 127.0.0.1..n (Linux
treats all of 127.0.0.0/8 as local); past that it takes more machines.

Three things that had to be right before any of this worked:

- **`RLIMIT_NOFILE`** on *both* sides. The 1024 default means the run dies at
  the 1024th connection, and on the server it reaches the EMFILE livelock of
  lesson 6 nine thousand connections early.
- **`SO_LINGER{1,0}`** on the client. The active closer eats TIME_WAIT, and
  that is the load generator: 28k sockets held for 60s means the *next* run
  fails for no visible reason. The runs above are back-to-back with no wait.
  A real client must never do this — RST discards the send buffer.
- **Room sharding.** Join broadcasts a notice to the room, so N clients in one
  room is O(N²) frames. 40k in a single room is 800M notices and the connect
  phase never finishes. `--per-room` keeps fan-out out of a connection-scale
  test; fan-out is a separate experiment with its own knob.

## The baseline number

This is what the whole exercise was for: the number io_uring has to beat.

```bash
./loadgen --conns 10000 --per-room 10 --rate 6 --duration 12
```

Each message is broadcast to its room, so delivered messages per second is
`conns × rate × per-room`. Server is single-threaded, so one core is the
ceiling. Loopback, same machine.

| rate | delivered/s | server CPU | latency p50 | latency p99 | self-lag p99 |
|---:|---:|---:|---:|---:|---:|
| 1 | 100k | — | 0.022 ms | 0.152 ms | 0.011 ms |
| 5 | 500k | 78.5% | 0.022 ms | 13.0 ms | 0.034 ms |
| 6 | 600k | 93.2% | 0.026 ms | 13.4 ms | 0.042 ms |
| 7 | 700k | **99.7%** | **45.4 ms** | 176.6 ms | 0.426 ms |
| 8 | 800k | 100% | 142.3 ms | 371.2 ms | 0.603 ms |
| 10 | 1.0M | 100% | 276.4 ms | 735.4 ms | 0.936 ms |
| 14 | 1.4M | 100% | 446.5 ms | >1000 ms | 1.499 ms |

**The knee is at ~600–700k deliveries/s, and it is exactly where one core
runs out.** p50 goes from 26 µs to 45 ms — a factor of 1700 — for a 17%
increase in offered load. Nothing was dropped and no connection was lost at
any rate; the server degrades by queueing, not by failing.

Self-lag stays two to three orders of magnitude below latency throughout, so
none of these rows are measuring the load generator. That check is not
decoration: without it the rate-14 row is indistinguishable from a client that
was simply too slow to keep up, and reporting it as a server result would have
been wrong.

### Where the time actually goes

Sampled at rate 6, the saturation point:

```
user   0.42s   ( 7.0% of wall)
sys    5.09s   (84.8% of wall)
split: 8% user / 92% kernel
```

**92% of the server's CPU is kernel time.** At 600k deliveries/s that is
essentially one `send()` per delivery plus the `recv()` and `epoll_wait()`
traffic around it — the application logic (framing, room lookup, string
assembly) accounts for 8%.

This is the entire argument for the io_uring port, and it is now measured
rather than assumed: the cost being attacked is syscall transitions, and 92%
of the budget sits in the part io_uring can batch. It also sets the honest
ceiling — even a perfect result cannot recover more than that 92%, and the 8%
of userspace work does not go away.

The same measurement is the first thing to re-run against the io_uring server.
If its user/kernel split does not move, the port did not do what it was for.

### Caveats on these numbers

- Loopback only. No NIC, no driver path, and client and server share the CPU.
- The histogram tops out at 1 s. Rows where p99 shows `>1000 ms` have samples
  excluded, which is also why rate 20 reports a *lower* p50 than rate 14 —
  past saturation the percentiles stop being comparable.
- Steady-state connections, not churn. Repeated connect/disconnect is a
  different and harder workload that this does not touch.
- `--size-mix` (rotating 16/32/256/1000-byte payloads) exists but the table
  above is fixed 64-byte filler.

## Protocol

4-byte header, little-endian, no byte swapping (same shortcut the real project
takes with its 8-byte header).

```
struct { uint16_t len; uint16_t type; }   // len = payload bytes, header excluded
```

| type | direction | meaning |
|---|---|---|
| 1 `C_SET_NICK` | → | set nickname |
| 2 `C_JOIN` | → | join room (nickname required first) |
| 3 `C_CHAT` | → | chat to current room |
| 100 `S_NOTICE` | ← | system message |
| 101 `S_CHAT` | ← | `nick: text` |

## The lessons

Marked `LESSON n` in `server.cpp`.

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
   a frame, or three and a half. `client.py dribble` proves the parser handles
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
