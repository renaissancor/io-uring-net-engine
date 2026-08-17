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
./loadgen --conns 10000 --per-room 10 --hold 10
./loadgen --conns 40000 --per-room 10 --src-ips 4 --hold 20
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

### Not built yet (phase 2)

Message send, latency histogram. The design notes are at the bottom of
`loadgen.cpp`: open-loop scheduling, latency measured against the *intended*
send time (coordinated omission), fixed filler strings per size class, and a
self-lag histogram so client saturation cannot be misread as server queueing.

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
