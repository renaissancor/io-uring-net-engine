# server_sds — the epoll control group on the engine's primitives

> **Status:** landed
> **Source:** `server_sds.cpp` (one translation unit; built by `CMakeLists.txt` as `server-sds`)
> **Namespace:** none (a program, not a library); consumes `sds::`, `LNX_CHECK`
> **Depends:** `sds/ring_buffer`, `sds/cstr_hash_map`, `sds/malloc_vector`, `check`, `types` — from an **installed** engine prefix via `find_package(iouring_net)`

## Purpose

`server.cpp` with its containers swapped and nothing else moved: the same
protocol, lessons, environment knobs, 256 KiB send cap and `[drop] … over cap`
line, so that the STL-versus-`sds::` delta is a measured row and the epoll
control shares its data structures with the io_uring server. It is also the
probe `server-uring/README.md` still owed — bytes round-tripping through the
*installed* `sds::ring_buffer` — so the seam fails loudly if the public
`FILE_SET` drifts. The numbers are in
[`../../result-notes/`](../../result-notes/); this file describes the code.

## API

A program. Its interface is the command line and the environment, identical to
`server.cpp` plus one knob:

```
./server-sds [port=9000]

CHAT_FLUSH=immediate|batch   send() inline per recipient, or one flush per epoll batch (LESSON 8)
CHAT_MAX_CONNS=<n>           connection cap (default 4096); also sizes the slab and RLIMIT_NOFILE
CHAT_QUIET=1                 suppress the per-accept log line
CHAT_SNDBUF=<bytes>          force SO_SNDBUF on accepted sockets (LESSON 7 test affordance)
CHAT_SHORT_READ=1            stop the recv() drain at a short read instead of looping to EAGAIN
```

Banner: `epoll chat server (sds) listening on :<port> (level-triggered, single thread, no STL)`.
Startup also prints `[cfg ] slab: <slots> slots x <KiB> = <MiB> virtual …` and
`[cfg ] recv drain = …`, so a log names the build and its drain mode.

Wire protocol, message types and the `[drop]`/`[disc]`/`[warn]`/`[err ]` log
lines are those of `server.cpp` (see `../README.md` § Protocol).

## Structures

| in `server.cpp` | here | notes |
|---|---|---|
| `std::unordered_map<int, conn>` | `conn` slots in one `mmap(MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_NORESERVE)` slab, indexed by fd; `u08 g_live[]` liveness | `g_slots = CHAT_MAX_CONNS + 64`, stride `sizeof(conn)` rounded to a page (292 KiB). A slot is placement-new'd on accept and destroyed + `madvise(MADV_DONTNEED)` on close. An accepted fd `>= g_slots` is refused like the cap |
| `std::string in` | `sds::ring_buffer<32768, ring_sync::single>` inline in `conn` | filled by `enqueue()` from a 64 KiB stack scratch; parsed by `peek()` of the header, then in place via `direct_dequeue_ptr()`/`commit_dequeue()` when contiguous, else `dequeue()` into a 1032 B scratch |
| `std::string out` + `erase(0, n)` | linear queue `tx[k_send_cap + 1028]` with `[tx_head, tx_tail)` | reset to 0 when drained; compacted to 0 when the tail hits the region end while under the cap; over the cap → doom, same threshold and message as `server.cpp:162` |
| `std::unordered_map<string, unordered_set<int>>` | `room` records (`name[64]`, `head`, `count`, free list) + `sds::cstr_hash_map<u32>` keyed by the record's own `name` | membership is an intrusive doubly-linked list through `conn::room_prev/room_next` (by fd): O(1) join and leave, no allocation per member |
| `std::vector<int> g_doomed/g_dirty` | `sds::malloc_vector<int>` reserved to `g_slots` at boot | `push_checked()` traps if `push_back` did not grow the vector (soft-OOM contract) |
| `std::string nick/room/line/hello/bye` | `char nick[32]`, `room::name[64]`, one static `g_line` scratch | `"nick: payload"` and the notices are assembled once per inbound message and copied once per recipient |

## Invariants

- **Same syscall shape as `server.cpp` by default.** `recv()` into a 64 KiB
  scratch for `min(64 KiB, ring free)` bytes, looping to `EAGAIN`; one `send()`
  per dirty connection per batch. The ring is *not* filled through
  `direct_enqueue_ptr()`: its contiguous run shrinks near the wrap and would
  split one `recv()` into several, putting a syscall difference inside a
  data-structure comparison. The one divergence: a connection with more than
  ~31 KiB unread needs a second `recv()`; the measured inbound backlog per
  connection at the 1024 B ceiling is ≈6.6 KB.
- **Cap semantics equal the baseline's.** `used + frame > 256 KiB` dooms with
  the same log line; under the cap a frame always fits after at most one
  compaction, because the region is the cap plus one maximum frame.
- **Borrowed keys outlive the map entry.** `room_release()` erases the key
  *before* the record returns to the free list, so a reused slot's new name is
  never compared against a stale pointer.
- **A slot is constructed only while its connection lives.** `g_live[fd]`
  gates every access; `slot(fd)` on a dead fd is a bug, not a lookup miss.
  `conn::tx` is deliberately left uninitialised by the constructor — zeroing it
  would fault in 257 KiB per accept.
- **No allocation per message.** The only allocations after boot are one
  `cstr_hash_map` node per room *creation* (join time) and its `delete` when
  the room empties.
- Single-threaded; `static` scratch buffers rely on that.

## Errors & edge cases

| Condition | Behaviour |
|---|---|
| send queue would pass 256 KiB | `[drop] fd=N send buffer over cap (used B), closing`; doom (reap after the batch) |
| ring full before a frame completes (`free_size() == 0`) | `[warn] … recv backlog too large, closing`; doom. Cannot happen with well-formed frames (parse leaves < 1 frame) |
| frame `len > 1024` | `[warn] … oversize frame, closing`; doom |
| unknown type | `[warn] … unknown type, closing`; doom |
| accepted fd `>= g_slots`, or live count `>= CHAT_MAX_CONNS` | `[drop] connection cap reached, refusing fd=N`; closed |
| `EMFILE`/`ENFILE` on accept | LESSON 6 reserve-fd shedding, as `server.cpp` |
| `mmap` of the slab fails | `perror("mmap slab")`, exit 1. 60,064 slots are 17.1 GB virtual (`MAP_NORESERVE` is not charged under `vm.overcommit_memory=0`) |
| `malloc_vector::push_back` fails to grow | `LNX_CHECK` trap (`SIGTRAP`); both lists are reserved to `g_slots` at boot so this is a bug, never load |
| `slowreader` (immediate mode, `CHAT_SNDBUF=8192`) | drops **all 8** clients, as `server.cpp` does today with the current `chatcli.py` (the README's single-drop line predates it); both servers stay responsive to a fresh client |

## Notes

- **RSS.** The receive ring's pages are all touched over a connection's life
  (~32 KiB resident per connection, ~320 MB at 10k, released on close); the
  send queue touches only its peak depth because it resets to offset 0 when
  drained. Compared with `server.cpp`'s few KB per connection this is a
  deliberate trade for zero growth and zero `memmove` on the hot path.
- **Includes are `<sds/…>`, `<check.h>`, `<types.h>`** — the layout the
  engine installs under `<prefix>/include/iouring_net/`, which the imported
  target adds as an include directory. `<iouring_net/sds/…>` resolves against a
  *different* prefix's stale headers if one is on the include path (the July
  `~/.local` install carries the retired non-template `ring_buffer`), and fails
  to compile in a way that looks like an API mismatch.
- `std::printf` stays for logging (off the hot path); `std::string_view` and
  `memcpy` are within `engine-uring/doc/04-coding-style.md` § permitted.
- Build: `make sds` (measurement, `-O2 -DNDEBUG`) and `make sds-asan`
  (correctness). Both need `IOURING_NET_PREFIX` (default `../build/prefix`);
  `make sds` prints the two install commands when it is missing.

## Test plan

`../../client-bench/chatcli.py` against `server-sds-asan` (ASan + UBSan) and
`server-sds`, 2026-09-02:
- `verify --clients 8 --messages 20`: 1,280/1,280, 0 missing / 0 duplicate /
  0 misattributed — 12 of 13 runs; one early run reported 28 missing and did
  not reproduce in twelve repeats across both builds (recorded, not explained).
- `verify --clients 4`: pass. `dribble`: frames split one byte per `send()`
  reassembled. `load --clients 30 --messages 30`: 27,495 frames, the recorded
  baseline count.
- `slowreader` (`CHAT_SNDBUF=8192`): `[drop] … over cap` fires, server stays
  responsive; 8 drops on both `server` and `server-sds` (see Errors).
- `loadgen --conns 500 --rate 20 --duration 5` against the ASan build:
  fan-out 10.03, 0 lost, no sanitizer report; SIGINT → `[stop] clean shutdown`.
- Fleet rows (STL vs sds under identical load) are in the result note.
