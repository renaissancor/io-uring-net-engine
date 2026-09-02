# client-bench

Client-side tools for the chat/game servers in `server-epoll`,
`engine-uring`, and `server-uring`. Two of them, and the split
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

## Why this is its own component

These are **tools, not products**. They share the repository with the servers
they point at, but not the directory, and the boundary is load-bearing:

- A tool that lives *inside* one of the two things it compares makes the
  comparison harder to justify than it should be. Sitting beside both, and
  switching between them with `--proto`, is what lets one instrument produce
  numbers that are comparable at all.
- `engine-uring` bans the STL (`std::function`, exceptions, streams, `std::`
  sync types; `sds::` containers instead). A load generator has no reason to
  obey that, and obeying it would mean a rewrite for nothing. Here the STL
  stays — **out of the hot loop**. Since 2026-09-02 the receive path parses in
  place from a per-connection slot in one `mmap` slab and the send path hands a
  stack-built frame straight to `send()`; `std::string` still holds an unsent
  tail and everything off the hot path. Nothing from the engine is linked: an
  instrument that shared a `ring_buffer` with the io_uring server would share
  its defects too.
- Nothing here has a performance requirement in its *judging* role, which is
  why one half is C++ and the other is Python.

The measured numbers do not live here either — they are in
[`../result-notes/`](../result-notes/), so that changing the tool cannot
quietly restate its own results.

## Layout

Code map with reading order, what each unit owns and what it fails on:
[`doc/INDEX.md`](doc/INDEX.md). Dated findings that outlive the numbers:
[`../result-notes/`](../result-notes/).

```
src/wire.*        frame header, the study/iouring seam, blob layout
src/config.*      the whole command line, in one struct
src/conn.h        per-connection state, clock, stop flag
src/corpus.*      realistic chat text, built once at startup
src/corpus_data.* the message pool, bucketed by byte length (generated)
src/histogram.*   1us buckets, printing, and the dump merge.py reads
src/netutil.*     fd limits, source binding, non-blocking read/write
src/connect.*     phase 1: establish and shard into rooms
src/traffic.*     phase 2: open-loop schedule, sampling, verdict
src/main.cpp      argument handling and the two phase calls

tools/mkcorpus.py .corpus-src/*.txt -> src/corpus_data.cpp
fleet.py          run N loadgen processes, assign identities, merge
merge.py          add raw histograms; percentiles cannot be averaged
chatcli.py        the judge: content correctness, not throughput

doc/INDEX.md      code map: what each unit owns and breaks on
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

# was the SERVER at its limit? nothing else here observes the server at all
./loadgen --conns 10000 --rate 30 --server-pid $(pgrep -x server)
python3 fleet.py --nodes 3 --conns 3334 --rate 30 -- --server-pid $(pgrep -x server)

./loadgen --help

python3 chatcli.py interactive --nick alice --room lobby
python3 chatcli.py verify --clients 8 --messages 20
python3 chatcli.py dribble
python3 chatcli.py slowreader --clients 8 --messages 300
```

Both take `--proto study|iouring`.

The study repo's `make` builds the **measurement** binary; `make asan` builds
the sanitised one under a separate name. It was the other way round until
2026-08-30, which meant the obvious command produced a server roughly half as
fast with nothing in the output to say so.

### Verdicts and exit codes

`loadgen` and `merge.py` exit **3** when the verdict is `[VOID]` — the run
produced numbers and they do not describe the server. `[ OK ]` and `[WARN]`
both exit 0. Three things void a run, checked in this order:

1. **Fan-out drift** past ±5% of `--per-room`. First, because the offered load
   is then not the requested one. Orphaned `loadgen` processes are the usual
   cause and they read as *higher* throughput — check `pgrep -x loadgen`.
2. **Connection loss** past 0.5% of `--conns`. Same reason: a connection that
   dies mid-run stops offering load, so the throughput is an average over a
   load that changed while it was measured. Usually the server shedding —
   `server-epoll` closes a connection whose send buffer passes 256 KiB. Fan-out
   cannot catch this: a closed connection leaves both sides of `frames_in/sent`
   at once, so the ratio barely moves. A run that lost 1,260 of 10,008
   connections measured fan-out 9.87, comfortably inside the band.
3. **Self-lag** too large against latency p99.

`--server-pid` reports the server's CPU over the traffic window:

```
[srv ] server CPU 100% of one core (user 14% / kernel 86%) over the traffic window
```

**Read that line for its low values.** 100% of a core is *not* saturation here —
this server held 100% from 3M to 10M deliveries/s, because a longer sweep
batches more messages into each syscall. Under ~95% the run is definitively
below the ceiling; at 100% you have learned almost nothing. The signal that
works is a pair: achieved < offered with self-lag **small** means the server is
the limit, with self-lag **large** means the client is. See
[`result-notes/2026-08-30-what-limits-the-server.md`](../result-notes/2026-08-30-what-limits-the-server.md).

### Payload: fixed length or real chat

`--size` / `--size-mix` send fixed-length filler. `--corpus` sends realistic
chat text instead — 4096 lines built once from a fixed seed, with the length
distribution conversations actually have: measured 2–979 bytes, mean 51.7,
39.7% reactions and 34.4% one-liners.

The point of that shape is the tail. Messages over 200 bytes are **3.8% of
frames but 25.0% of bytes**, which is the property a fixed `--size` cannot
reproduce at any single value — pick 64 and you understate the volume, pick 512
and you understate the frame count.

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
distinction untested. The `k_xlong` class exists to sit right under that cap
(902–980 bytes) rather than approach it by accident, though it is only 0.3% of
draws: it is the boundary case, and a corpus that served it often would be
measuring the cap instead of chat.

The pool itself is generated, not hand-edited:

```bash
python3 tools/mkcorpus.py     # .corpus-src/*.txt -> src/corpus_data.cpp
```

`.corpus-src/` is the authoring format — one message per line, plain UTF-8 —
and `src/corpus_data.cpp` is the generated artefact. Both are committed, so the
text is stored twice; the alternative is a generator with no inputs or a Python
step in a C++ build, and neither is worth saving 292 KB. The script buckets
by **byte** length rather than by filename, because a person writing Korean
counts characters while the protocol counts bytes and the two differ by 3x.

It also reports a 4-gram diversity figure per bucket, which is there for a
specific failure: asking a generator for "400 unique lines" is satisfied by 400
permutations of one sentence with the nouns swapped. Distinct lines are cheap,
distinct language is not, and only the n-gram measure separates them. That
figure is sampled at a **fixed 4-gram count**, not a fixed line count — the
ratio falls as any natural-language sample grows, so comparing an 1997-line
bucket of 32-byte lines against a 50-line bucket of 959-byte lines is only
meaningful once the sample size is held constant. All five buckets currently
measure 0.89–0.91.

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
[`../result-notes/2026-08-17-interactive-client-defects.md`](../result-notes/2026-08-17-interactive-client-defects.md):
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
server-epoll   [uint16 len ][uint16 type]   len  = payload bytes
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

## The numbers this produced

The single-threaded epoll baseline, its two flush modes, the fleet-vs-single
process divergence, and the user/kernel split all live in
[`../result-notes/`](../result-notes/) — results are kept apart from the tool
that produced them so that changing the tool cannot quietly restate them.

What stays here is the *method*: what is measured, what makes a run count, and
the traps above that had to be right before any number was meaningful.
