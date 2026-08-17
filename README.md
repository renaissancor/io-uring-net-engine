# netbench

Client-side tools for the chat/game servers in `epoll-chat-study`,
`iouring-net-lib`, and `iouring-net-server`. Two of them, and the split
between them is the point:

| | | |
|---|---|---|
| `src/` + `fleet.py` | **instrument** | connection scale and delivery latency, and it says out loud when a run measured the client instead of the server |
| `chatcli.py` | **judge** | whether the messages are *correct* — right body, right sender, right room, exactly once |

They do not overlap. `loadgen` embeds a 20-byte blob, reads the timestamp and
the node stamp back out and discards the rest, so it will happily report 100%
delivery at a 0.1 ms p99 while every message arrives at the wrong client with
the wrong body.
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

## Layout

Code map with reading order, what each unit owns and what it fails on:
[`doc/INDEX.md`](doc/INDEX.md). Dated findings that outlive the numbers:
[`design/`](design/).

```
src/wire.*        frame header, the study/iouring seam, blob layout
src/config.*      the whole command line, in one struct
src/conn.h        per-connection state, clock, stop flag
src/corpus.*      realistic chat text, built once at startup
src/histogram.*   1us buckets, printing, and the dump merge.py reads
src/netutil.*     fd limits, source binding, non-blocking read/write
src/connect.*     phase 1: establish and shard into rooms
src/traffic.*     phase 2: open-loop schedule, sampling, verdict
src/main.cpp      argument handling and the two phase calls

fleet.py          run N loadgen processes, assign identities, merge
merge.py          add raw histograms; percentiles cannot be averaged
chatcli.py        the judge: content correctness, not throughput

doc/INDEX.md      code map: what each unit owns and breaks on
design/           dated findings; the traps outlive the numbers
```

## Build and run

```bash
make
CHAT_FLUSH=batch CHAT_MAX_CONNS=20000 CHAT_QUIET=1 ./server 9000  # in the study repo

# one process — fine at low rate, unverified at high rate (see below)
./loadgen --conns 10000 --per-room 10 --rate 5 --duration 20

# a fleet — assigns node ids, source-IP ranges and dumps, then merges
python3 fleet.py --nodes 3 --conns 3334 --rate 30 --duration 20
python3 fleet.py --nodes 3 --conns 3334 --rate 30 -- --corpus

./loadgen --help

python3 chatcli.py interactive --nick alice --room lobby
python3 chatcli.py verify --clients 8 --messages 20
python3 chatcli.py dribble
python3 chatcli.py slowreader --clients 8 --messages 300
```

Both take `--proto study|iouring`.

### Payload: fixed length or real chat

`--size` / `--size-mix` send fixed-length filler. `--corpus` sends realistic
chat text instead — 4096 lines built once from a fixed seed, with the length
distribution conversations actually have (mostly reactions, a thin tail of
paragraphs; measured 1–445 bytes, mean 34.9).

The no-RNG-in-the-hot-loop rule is unchanged, only front-loaded: the corpus is
assembled at startup and the send path does one index and returns a reference.
The seed is fixed rather than time-based so two runs, and two nodes of one
fleet, put identical bytes on the wire.

**Fixed length stays the default**, because it is the one the recorded
baselines used and because it keeps frame size a controlled variable. Reach for
`--corpus` when the question is how the server behaves under a realistic size
mix, not when comparing two servers.

The text is Korean, so it is UTF-8 multi-byte — one character is three bytes.
That is deliberate: the protocol's invariant is the 1 KB byte cap with the
character limit derived from it, and an ASCII corpus would leave that
distinction untested.

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

`interactive` is not decoration — it is the only mode a human drives, and the
first time one did, it crashed. Two defects that no synthetic mode could
reach are recorded in
[`design/2026-08-17-interactive-client-defects.md`](design/2026-08-17-interactive-client-defects.md):
stdin is a byte stream and an IME can split a UTF-8 character across reads,
and the server's payload cap applies to `"nick: " + text` so the real limit is
per-user and has two invisible cliffs.

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
"p99 200 ms at 100k connections" when the client was the thing dying.

The verdict has three levels, and the third one had to be added after the
bare ratio test misfired:

| verdict | condition | meaning |
|---|---|---|
| `[ OK ]` | `lag99 * 5 <= lat99` | the client was not the bottleneck |
| `[WARN]` | ratio fails but `lag99 < 1 ms` | usable; read server p99 as `>= lat99 - lag99` |
| `[VOID]` | ratio fails and `lag99 >= 1 ms` | this run measured the client |

Self-lag inflates measured latency roughly additively, so what decides
whether a run is usable is whether subtracting it would change the
conclusion — not the ratio by itself. Near the knee both numbers shrink to
the same scale and a bare ratio starts flipping on scheduler noise: three
back-to-back runs at rate 20 measured self-lag 0.133 / 0.143 / 0.131 ms
against latency 0.726 / 0.658 / 0.604 ms, and `lag99 * 5 > lat99` called the
first `[ OK ]` and the other two `[VOID]`. Those are the same measurement.
Below a millisecond of client jitter a sub-millisecond server p99 stays
sub-millisecond even fully corrected, so the honest output is the corrected
lower bound rather than a discarded run. Above the floor the correction is
load-bearing — a run reporting latency p99 10.478 ms with self-lag p99
4.640 ms is genuinely void, and gets the same word for a real reason.

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

Numbers below are the **third** measurement, and the first one taken with a
fleet rather than a single client process. The single-process ladder recorded
earlier was wrong above rate 5, for two compounding reasons documented under
"One process cannot verify itself" — treat any number here that predates the
fleet as withdrawn.

Method: `python3 fleet.py --nodes 3 --conns 3334 --rate <r> --duration 20`
against a fresh server per point.

### immediate — one send() per delivery

| rate | delivered/s | server CPU | user/kernel | p50 | p99 | p99.9 |
|---:|---:|---:|---:|---:|---:|---:|
| 5 | 500k | 79% | 7 / 92 | 0.022 ms | 0.454 ms | **12.328 ms** |
| 7 | 700k | **100%** | 7 / 92 | **67.8 ms** | 187.4 ms | 210.4 ms |
| 14 | 1.4M | 100% | 7 / 92 | **460.1 ms** | >1000 ms | >1000 ms |

### batch — one send() per connection per epoll batch

Three processes, ~10k connections total, so the client is not in the way.

| rate | delivered/s | p50 | p99 | self-lag p99 | one process reported |
|---:|---:|---:|---:|---:|---:|
| 5 | 500k | 0.023 ms | 0.237 ms | 0.001 ms | 0.022 ms — agrees |
| 8 | 800k | 0.056 ms | 0.455 ms | 0.003 ms | 0.025 ms — **2x low** |
| 14 | 1.4M | 0.073 ms | 0.770 ms | 0.008 ms | 0.035 ms — **2x low** |
| 20 | 2.0M | 0.102 ms | 1.480 ms | 0.021 ms | 0.056 ms — **2x low** |
| 30 | 3.0M | 18.533 ms | 38.701 ms | 0.024 ms | 0.136 ms — **136x low** |

**The ceiling is 2M deliveries/s.** Below it the server holds a sub-millisecond
p50; at 3M it is 18.5 ms and past the knee. The earlier "2M measured, 3M
observed" reading came from a client that could not see the queue it was
creating.

### One process cannot verify itself

This is the finding worth keeping, above any particular number.

A saturated load generator does not report that it is saturated. It reports
plausible server numbers that happen to be wrong, and the self-lag guard —
built exactly to catch this — did not fire, because it watches sends and the
damage was on the receive side.

Two separate mechanisms, found by carrying identical load with one process and
with three and asking why they disagreed:

**Receive-side coordinated omission.** `recv_ts` was stamped once per
`epoll_wait` batch and shared by every frame read in that batch. The frames at
the end of a long ready list are the late ones, and dating them from when the
walk began deletes precisely the delay that saturation caused. More
connections per process means longer batches means more deleted. At 3M/s the
batch stamp reported p50 0.109 ms from one process and 18.868 ms from three
carrying the same connections and the same load — with the server at 100% CPU
and the same user/kernel split in both, so the server was doing identical work.
Fixed: stamped per socket.

**The client is inside the system under test.** Even correctly stamped, a
client that cannot read fast enough closes its receive windows, and TCP
backpressure then stops the server from building the queue it would build
against a client that keeps up. Nothing is mis-measured; the server is simply
not being asked the question you thought you asked. This one has no fix in
code — it is what `fleet.py`, `--src-ips` and more machines are for.

**So: a single-process number at high rate is unverified, not wrong-by-default
but unverified.** Confirm it with a fleet run at the same connection count
before quoting it. The two agreed exactly at rate 5 and diverged by 2x from
rate 8 onward.
### Where the time goes

At the fair baseline the server spends **86–93% of its CPU in kernel time**,
and the number *falls as load rises* — 93% at rate 5, 90% at rate 14, 86% at
rate 30 — because coalescing removes syscalls while the memcpy work stays.
Application logic (framing, room lookup, string assembly) is the remaining
7–14%.

Read the direction, not just the value: the kernel share is highest where the
server is least busy. Quoting a single figure hides that, which is how the
first session ended up with "92%" as if it were a constant.

That is the io_uring argument measured rather than assumed: the cost being
attacked is syscall transitions, and that is where the budget sits. It is also
the honest ceiling, and the honest ceiling has moved twice — first because 92%
was measured against a naive baseline, and again because the throughput bar
rose from 700k to 2M measured / 3M observed.

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
