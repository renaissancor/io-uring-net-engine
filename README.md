# netbench

Load generator for the chat/game servers in `epoll-chat-study`,
`iouring-net-lib`, and `iouring-net-server`. Measures connection scale and
delivery latency, and says out loud when a run measured the client instead of
the server.

## Why this is its own repo

It is a **measuring instrument, not a product**, and it has to survive the
things it measures.

- `epoll-chat-study` is explicitly throwaway. Anything durable that lives
  there dies with it — including the baseline numbers below, which exist
  precisely to be compared against later.
- `iouring-net-lib` bans the STL (`std::function`, exceptions, streams,
  `std::` sync types; `sds::` containers instead). A load generator has no
  reason to obey that, and obeying it would mean a rewrite for nothing.
- An instrument that lives inside one of the two things it compares makes the
  comparison harder to justify than it should be.

So it lives outside both, keeps the STL, and switches protocols with a flag.

## Build and run

```bash
make
./loadgen --conns 10000 --per-room 10 --rate 6 --duration 12
./loadgen --conns 40000 --src-ips 4 --rate 1 --duration 20
./loadgen --help
```

`make asan` builds a sanitised binary for correctness checks at small
`--conns`. Do not measure with it — ASan roughly halves throughput, so the
numbers describe the sanitiser.

## Protocol

Both targets use the same 4-byte little-endian header, no byte swapping. They
differ in exactly one respect:

```
epoll-chat-study   [uint16 len ][uint16 type]   len  = payload bytes
iouring-net-*      [uint16 size][uint16 id  ]   size = payload + header
```

The width is identical; what differs is whether the length field counts the
header. That is the whole porting seam, and it is what `--proto study` /
`--proto iouring` switches.

Getting it backwards desynchronises the stream by four bytes per frame, so it
is worth checking that it fails loudly. It does: `--proto iouring` against the
study server drops every connection and reports `[VOID] no latency samples`
rather than producing plausible-looking wrong numbers.

`--proto` currently switches **framing only**. The iouring-net packet IDs come
from a schema that does not exist yet; fill them in from the generated table
rather than guessing, because an unrecognised ID closes the session there and
a wrong guess would present as a connection failure rather than as a protocol
error.

## What it measures, and the two things that make the numbers real

**Open-loop scheduling.** Every connection has a fixed send deadline
(`start + m * slot`, one global sequence, `slot = 1/(N*rate)`) and fires on
schedule regardless of whether the previous reply arrived. Closed-loop
send-after-echo cannot overload a server: when the server slows the client
slows with it, the queue never builds, and the latency graph comes out
flattering and wrong.

**Latency measured from the intended send time.** The payload carries the
deadline, not the moment the write actually happened:

```
[uint16 len][uint16 type][8B intended_ts_ns][4B seq][4B client_id][filler ...]
```

Measuring from the actual send time silently deletes every delay the client
itself caused — which is exactly where the tail lives. This is coordinated
omission, and it is the default failure mode of hand-rolled load tests.

**A self-lag histogram beside the latency histogram.** The loop records how
late it was issuing each send. Without it there is no way to separate server
queueing from client saturation, and the classic wrong result is reporting
"p99 200 ms at 100k connections" when the client was the thing dying. If
self-lag p99 is not small against latency p99, the run prints `[VOID]` and its
numbers do not describe the server.

Filler is fixed strings per size class, not per-message RNG: randomness in the
hot loop is client CPU, and client CPU lands in the measurement as server
latency. Content is irrelevant — TCP does not compress — but length class is
not, since small frames are syscall-bound and frames over the MSS take the
segmentation path.

## Connection scale

| run | result |
|---|---|
| 10k conns, 1 source IP | 10,000 established, 0 failed, 0.10 s, 0 attrition over 10 s |
| 40k conns, 1 source IP | **28,232 established, 11,768 × `EADDRNOTAVAIL`** |
| 40k conns, 4 source IPs | 40,000 established, 0 failed, 0.45 s, 0 attrition over 20 s |

**28,232 is not a coincidence.** `net.ipv4.ip_local_port_range` is
`32768 60999`, and `60999 - 32768 + 1 = 28232`. A connection is identified by
the 4-tuple `(src IP, src port, dst IP, dst port)`; against a single server
`IP:port` the last two are fixed, so one source IP produces only as many
distinct tuples as it has ephemeral ports.

This is a **client-side** limit. The server's local port stays 9000 for every
connection — `accept()` returns a new fd, not a new port — so the server side
varies `(src IP, src port)` and is bounded by fds and memory, not ports.
Switching to UDP would not change this in either direction.

Ephemeral ports are a per-source-IP kernel resource, so extra *processes* on
one box do not buy extra ports. `--src-ips` binds across 127.0.0.1..n (Linux
treats all of 127.0.0.0/8 as local); past that it takes more machines. Opening
any other field of the tuple works too — several destination ports would do
the same job.

Three settings that had to be right before any of this worked:

- **`RLIMIT_NOFILE`**, raised at startup on both sides. The 1024 default means
  the run dies at the 1024th connection and it looks like a network problem.
- **`SO_LINGER{1,0}`** on the client. The active closer eats TIME_WAIT, and
  that is this process: 28k sockets held for 60 s means the *next* run fails
  for no visible reason. A real client must never do this — RST discards the
  send buffer.
- **`TCP_NODELAY`** on both sides. Nagle would hold small frames for up to
  40 ms, indistinguishable from server latency in the histogram.
- **Room sharding** via `--per-room`. Join broadcasts a notice to the room, so
  N clients in one room is O(N²) frames; 40k in a single room is 800M notices
  and the connect phase never finishes.

## Baseline: single-threaded epoll server

`epoll-chat-study`, 10k connections, rooms of 10, loopback, same machine.
Delivered messages per second is `conns × rate × per-room`. The server is
single-threaded, so one core is the ceiling.

| rate | delivered/s | server CPU | latency p50 | latency p99 | self-lag p99 |
|---:|---:|---:|---:|---:|---:|
| 1 | 100k | — | 0.022 ms | 0.152 ms | 0.011 ms |
| 5 | 500k | 78.5% | 0.022 ms | 13.0 ms | 0.034 ms |
| 6 | 600k | 93.2% | 0.026 ms | 13.4 ms | 0.042 ms |
| 7 | 700k | **99.7%** | **45.4 ms** | 176.6 ms | 0.426 ms |
| 8 | 800k | 100% | 142.3 ms | 371.2 ms | 0.603 ms |
| 10 | 1.0M | 100% | 276.4 ms | 735.4 ms | 0.936 ms |
| 14 | 1.4M | 100% | 446.5 ms | >1000 ms | 1.499 ms |

**The knee is at ~600–700k deliveries/s, exactly where one core runs out.**
p50 moves from 26 µs to 45 ms — a factor of 1700 — for a 17% increase in
offered load. Nothing was dropped and no connection was lost at any rate: the
server degrades by queueing, not by failing.

Self-lag stays two to three orders of magnitude below latency throughout, so
none of these rows are measuring the load generator.

### Where the time goes

Sampled at rate 6, the saturation point:

```
user   0.42s   ( 7.0% of wall)
sys    5.09s   (84.8% of wall)
split: 8% user / 92% kernel
```

**92% of the server's CPU is kernel time** — essentially one `send()` per
delivery plus the `recv()` and `epoll_wait()` around it. Application logic
(framing, room lookup, string assembly) is 8%.

This is the io_uring argument measured rather than assumed: the cost being
attacked is syscall transitions, and 92% of the budget sits in the part
io_uring can batch. It is also the honest ceiling — a perfect port cannot
recover more than that 92%, and the 8% of userspace work does not go away.

**Re-run this split first against the io_uring server.** If it does not move,
the port did not do what it was for.

## Caveats

- Loopback only. No NIC, no driver path, and client and server share the CPU.
- The histogram tops out at 1 s. Rows showing `>1000 ms` have samples
  excluded, which is also why rate 20 reports a *lower* p50 than rate 14 —
  past saturation the percentiles stop being comparable.
- Steady-state connections, not churn. Repeated connect/disconnect is a
  different and harder workload this does not touch.
- Fan-out is fixed at rooms of 10 throughout. Large rooms are a separate
  experiment.
- `--size-mix` exists but the table above is fixed 64-byte filler.
