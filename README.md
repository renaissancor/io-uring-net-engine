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
