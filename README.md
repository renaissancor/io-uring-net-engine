# netbench

Client-side tools for the chat/game servers in `epoll-chat-study`,
`iouring-net-lib`, and `iouring-net-server`. Two of them, and the split
between them is the point:

| | | |
|---|---|---|
| `loadgen.cpp` | **instrument** | connection scale and delivery latency, and it says out loud when a run measured the client instead of the server |
| `chatcli.py` | **judge** | whether the messages are *correct* — right body, right sender, right room, exactly once |

They do not overlap. `loadgen` embeds a 16-byte blob, reads the timestamp back
out and discards the rest, so it will happily report 100% delivery at a 0.1 ms
p99 while every message arrives at the wrong client with the wrong body.
Nothing it measures can catch that. `chatcli.py verify` exists for exactly
that gap, and `chatcli.py interactive` is the human-eyeball version of the
same question.

Neither has any performance requirement in the judging role, which is why one
is C++ and the other is Python.

## Why this is its own repo

These are **tools, not products**, and they have to survive the things they
point at.

- `epoll-chat-study` is explicitly throwaway. Anything durable that lives
  there dies with it — including the baseline numbers below, which exist
  precisely to be compared against later.
- `iouring-net-lib` bans the STL (`std::function`, exceptions, streams,
  `std::` sync types; `sds::` containers instead). A load generator has no
  reason to obey that, and obeying it would mean a rewrite for nothing.
- A tool that lives inside one of the two things it compares makes the
  comparison harder to justify than it should be.

So they live outside both, keep the STL, and switch protocols with a flag.

## Build and run

```bash
make
CHAT_FLUSH=batch CHAT_MAX_CONNS=20000 CHAT_QUIET=1 ./server 9000  # in the study repo
./loadgen --conns 10000 --per-room 10 --rate 20 --duration 20
./loadgen --conns 40000 --src-ips 4 --rate 1 --duration 20
./loadgen --help

python3 chatcli.py interactive --nick alice --room lobby
python3 chatcli.py verify --clients 8 --messages 20
python3 chatcli.py dribble
python3 chatcli.py slowreader --clients 8 --messages 300
```

Both take `--proto study|iouring`.

`make asan` builds a sanitised binary for correctness checks at small
`--conns`. Do not measure with it — ASan roughly halves throughput, so the
numbers describe the sanitiser.

## chatcli modes

| mode | what it answers |
|---|---|
| `interactive` | does this behave like a chat server to a human |
| `verify` | does every message reach every room member exactly once, with the exact body and correct sender |
| `load` | frame counting only — delivery, not content |
| `slowreader` | does a client that never reads get dropped without taking the server down |
| `dribble` | does the parser survive frames split one byte per `send()` |

`verify` varies payload length across 8/63/64/65/200/900 bytes so framing
boundaries get exercised rather than one comfortable size, and it checks for
three distinct failures a throughput test reports as a clean 100%: messages
that never arrived, messages delivered twice, and bodies or senders that do
not match anything that was sent.

`dribble` matters more for the io_uring port than it looks. The partial-frame
state machine is one of the things that ports verbatim, and there it has to
survive completion semantics on top — a buffer handed to the kernel is
untouchable until the CQE arrives.

### Verified against the epoll server

| mode | result |
|---|---|
| `verify` 8×20 | 1,280/1,280 deliveries, exact bodies, 0 missing / 0 duplicate / 0 misattributed |
| `verify` with wrong `--proto` | 80/80 missing, `VERIFY FAIL` — fails loudly rather than plausibly |
| `dribble` | server reassembled frames split one byte per send |
| `load` 30×30 | 27,495 frames, matching the study repo's recorded result |
| `slowreader` | `[drop] fd=7 send buffer over cap (261948 B)`, server stayed responsive to a fresh client |

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

**There are two baselines, and the difference between them is larger than
anything io_uring is expected to deliver.** The study server takes
`CHAT_FLUSH=immediate|batch`:

- `immediate` — a broadcast calls `send()` once per recipient, inline, while
  walking the room. One syscall per delivery. The naive shape.
- `batch` — the room walk only appends to each recipient's buffer, and one
  flush pass at the end of the epoll batch sends what accumulated. Several
  messages bound for the same connection collapse into one `send()`.

Publishing only the first would credit io_uring for batching that epoll can do
perfectly well. A weak control group is not a control group.

### immediate — one send() per delivery

| rate | delivered/s | server CPU | user/kernel | p50 | p99 |
|---:|---:|---:|---:|---:|---:|
| 5 | 500k | 79% | 8 / 92 | 0.022 ms | 0.612 ms |
| 7 | 700k | **99%** | 6 / 94 | **85.9 ms** | 196.5 ms |
| 8 | 800k | 100% | — | 142.3 ms | 371.2 ms |
| 10 | 1.0M | 100% | — | 276.4 ms | 735.4 ms |
| 14 | 1.4M | 100% | — | 446.5 ms | >1000 ms |

### batch — one send() per connection per epoll batch

| rate | delivered/s | server CPU | user/kernel | p50 | p99 | self-lag p99 |
|---:|---:|---:|---:|---:|---:|---:|
| 5 | 500k | 78% | 7 / 93 | 0.021 ms | 0.172 ms | 0.03 ms |
| 8 | 800k | 98% | 13 / 87 | 0.023 ms | 0.262 ms | 0.04 ms |
| 14 | 1.4M | 97% | 11 / 89 | 0.031 ms | 0.484 ms | 0.09 ms |
| 20 | 2.0M | 97% | 15 / 85 | 0.048 ms | 0.811 ms | 0.30 ms |
| 30 | 3.0M | 99% | 14 / 86 | 0.103 ms | 7.697 ms | 1.12 ms |
| 45 | — | — | — | `[VOID]` | `[VOID]` | client died first |

### What the comparison says

**The `immediate` knee at ~700k was a property of the design, not of the
machine.** At rate 7 both modes sit at ~98% of one core, and immediate reports
p50 85.9 ms while batch reports 0.021 ms — a factor of 4000 at identical
offered load and identical CPU. What produced the 86 ms was not CPU
exhaustion; it was the event loop being held inside inline syscalls while
events piled up behind it.

**Batching gets *more* effective as load rises.** rate 8 and rate 20 both sit
at ~98% CPU, but rate 20 delivers 2.5× the messages. Higher load means more
messages accumulate per epoll batch, so more of them coalesce into one
`send()`, so the cost per delivery falls. The user/kernel split shifts from
7/93 to 15/85 across that range, which is exactly the signature of syscalls
being removed while the memcpy work stays.

**The real ceiling is 2–3M deliveries/s**, roughly 4× the naive figure, and
past that it cannot be measured from one client process — at rate 45 the load
generator saturated first and correctly refused to report, which is what
`--src-ips` and multiple processes and machines exist for.

### Where the time goes

At the fair baseline the server spends **85–89% of its CPU in kernel time**,
falling as coalescing improves. Application logic — framing, room lookup,
string assembly — is the remaining 11–15%.

That is the io_uring argument measured rather than assumed: the cost being
attacked is syscall transitions, and that is where the budget sits. It is also
the honest ceiling, and the honest ceiling moved twice today — first because
92% was measured against a naive baseline, and again because the throughput
bar rose from 700k to 2–3M.

**Re-run this split first against the io_uring server, and against `batch`,
never against `immediate`.** If the split does not move, the port did not do
what it was for.

## Caveats

- Loopback only. No NIC, no driver path, and client and server share the CPU.
- The histogram tops out at 1 s. Rows showing `>1000 ms` have samples
  excluded, so past saturation the percentiles stop being comparable between
  rows.
- `batch` flushes at the end of an epoll batch — microseconds — not on a
  fixed-rate tick. A 30 Hz tick would add up to 33 ms and is a different
  experiment.
- Steady-state connections, not churn. Repeated connect/disconnect is a
  different and harder workload this does not touch.
- Fan-out is rooms of 10 in the tables above. Varying it 10 → 100 → 500 at a
  fixed 500k deliveries/s moved the user/kernel split only from 7/93 to 4/96,
  so the split is not an artifact of small rooms. It *is* an artifact of chat:
  per-recipient work here is a memcpy, where a game server would add AoI
  filtering and per-recipient serialization. Re-measure when gameplay packets
  land.
- `--size-mix` exists but every table above is fixed 64-byte filler.
