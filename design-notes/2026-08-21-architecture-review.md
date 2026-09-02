---
status: accepted
amended_by:
  - 2026-09-01-architecture-review-disposition.md
---
# 2026-08-21 — Pre-data-path architecture review

Recommendations from a Claude review of the skeleton on
`feat/pipe-mesh-and-roster` (`1d529c4`). Recording without acting; discuss
individually. The common thread: each item is a decision the io_uring data
path will calcify around, so it is cheapest to settle **before** that code
lands. Each item carries **Evidence** (exact locations in this tree) and a
**Guide** (how to implement when accepted).

**Scope of code under review:**
- `src/app/roster.h`, `src/app/mesh.h`, `src/app/message.h`, `src/app/main.cpp`
- `src/app/{worker,acceptor}_{ctl,engine}.*`, `src/app/thread_ctl.*`
- `src/sds/pipe.h`, `src/sds/ring_buffer.h`
- `doc/10-realtime-server-architecture.md` §4, §7–§9

**Status at review:** clean tree, all pushed; skeleton phase — engines empty
(no listen socket, no io_uring, no session storage on the worker).

## 1. Unify every wake reason as a CQE

`doc/10` §9 frames the blocking-wait problem as a *shutdown* caveat, but it
is wider: once a worker blocks in `io_uring_wait_cqe`, an `adopt_session`
frame posted to its admission pipe just sits there until an unrelated socket
completion happens to wake the thread — an **admission-latency hole**, not
just a stop hole.

**Recommendation:** adopt one principle and write it into `doc/10` §8: *a
worker sleeps in exactly one place, and every wake reason is a CQE* — socket
I/O, mesh doorbell, stop, and the world tick.

**Evidence**
- `src/app/worker_engine.cpp:40-46` — the loop's stated Phase 2 body is
  `io_uring_submit + io_uring_peek_cqe`; the moment peek becomes a blocking
  wait, nothing wakes it but a completion.
- `src/app/mesh.h:56-61` — `mesh_post` writes bytes and returns; the mesh
  contract has **no doorbell channel anywhere**, so a sleeping consumer is
  invisible to producers.
- `doc/10` §8 (lines 129-142) — "drain acceptor→worker pipe" is the top of
  the loop, i.e. it only runs when something else already woke the thread.
- `doc/10` §9 + `doc/runtime/thread.md` §"Cooperative stop" — the caveat is
  currently scoped to `request_stop()` only; admission latency is not named.

**Guide**
- One `eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)` per worker, created by the
  supervisor alongside the pipes (LANDLORD) and installed via the same
  pre-`start()` path as `install_pipes()`.
- Arm it in the worker's own ring: `io_uring_prep_poll_multishot(sqe, efd,
  POLLIN)` (survives across fires; drain with a non-blocking 8-byte read on
  each CQE), or a re-armed 8-byte read SQE. Multishot poll avoids re-arm
  churn.
- Producer side: after a successful `mesh_post`, `u64 one = 1; write(efd,
  &one, 8)`. Never blocks in practice (with `EFD_NONBLOCK` a saturated
  counter returns `EAGAIN`, which is fine — the consumer is already waking).
- Stop: `request_stop()` publishes draining, then writes the same eventfd.
- Tick: an `io_uring_prep_timeout` SQE in the same ring —
  `IORING_TIMEOUT_MULTISHOT` (kernel ≥ 6.4; dev kernel is 6.6) or re-armed
  per expiry. Chat does not need a tick deadline; the stated RTS identity
  does, and a blocking wait conflicts with a tick deadline unless the tick
  IS a completion.
- Dispatch: tag `user_data` (e.g. low bits = kind: recv/send/doorbell/tick;
  high bits = session slot).
- **Pitfall:** `io_uring_register_eventfd(3)` is the *opposite* direction
  (the ring signals an eventfd on completions) — do not reach for it here.
- On acceptance update: `doc/10` §8 (loop shape) + §9 (rewrite around the
  principle), `doc/runtime/thread.md` §"Cooperative stop", and the mesh
  contract comment in `src/app/mesh.h` (post + doorbell must pair).

## 2. Define fd ownership while in flight in the mesh

`adopt_session_msg` carries a live fd, so there are moments when **neither**
the acceptor nor a worker owns it. Both unowned windows are reachable today.

**Recommendation:** add an explicit invariant to `doc/10` §4: **the frame
owns the fd.**

**Evidence**
- `src/app/message.h:40` — `int fd` rides inside the frame.
- `src/app/mesh.h:24-26` — backpressure returns `false`, "a signal for the
  caller (drop-and-close)"; but no owner is named for the fd, and per
  `doc/10` §7 (lines 106-108) the id/generation/account are minted **before**
  the post, so the authority map already holds an entry that nothing retires
  on the failure path.
- `src/app/main.cpp:125-132` — shutdown stops the acceptor first (correct:
  producer before consumer), then workers; frames still sitting in
  `to_worker[i]` at that moment contain fds nobody will adopt.
- `src/app/worker_engine.cpp:48-49` — the drain phase is a placeholder; no
  pipe drain exists, so those fds would leak (or collide with recycled fd
  numbers) once the real path lands.

**Guide**
- Invariant text (proposed `doc/10` §4 item 7): *"A mesh frame carrying an
  fd owns that fd. A producer whose post fails closes it; a consumer that
  dequeues it becomes its owner; a consumer entering drain must consume its
  inbound pipes to empty before publishing stopped."*
- Acceptor failure path, in order: `close(fd)` → retire id/generation from
  the authority map → count a drop stat. Retire before anything can reuse
  the slot.
- Worker drain phase: loop `mesh_try_recv` on the admission pipe until
  empty; for each `adopt_session` frame, `close(fd)` + post
  `session_closed` (writable by construction — see item 3). Only then
  publish stopped.
- The `main.cpp` ordering already guarantees the pipe cannot refill during
  drain (acceptor joined first), so drain-to-empty is terminal — worth
  stating in the §7 flow.

## 3. Make close-notify unable to fail, by construction

Backpressure-drop is right for admissions, but a dropped `session_closed`
leaks an authority-map entry forever, and a worker must never block.

**Recommendation:** tie the constants together with a `static_assert` and
record the rule that makes it sufficient: **at most one in-flight close
frame per session.**

**Evidence**
- `src/app/config.h:23` — `k_session_capacity = 256`.
- `src/app/mesh.h:107-108` — `worker_to_acceptor_pipe = sds::pipe<16 * 1024>`.
- `src/app/message.h:50-55` — `session_closed_msg` ≈ 16 B body → ~20 B
  frame; worst case 256 × 20 = 5,120 B ≤ 16,384 B. Holds today **by
  coincidence, not by contract** — nothing fails if someone shrinks the pipe
  or grows the session cap.
- `src/sds/ring_buffer.h:112` — `static constexpr usize capacity()` already
  exists, and all N bytes are usable (fullness is the counter delta,
  `ring_buffer.h:26-28`), so the assert is expressible today with no code
  change.

**Guide**
- Next to the alias in `src/app/mesh.h` (needs `config.h` or the constant
  hoisted somewhere both can see):

  ```cpp
  static_assert(config::k_session_capacity
                    * mesh_frame_size(sizeof(session_closed_msg))
                <= worker_to_acceptor_pipe::capacity(),
                "close-notify must never drop: one in-flight close per "
                "session must fit the pipe");
  ```

- Prerequisite protocol rule for `doc/10` §7: a worker posts exactly one
  `session_closed` per adopted session (slot cannot be reused until the
  acceptor has consumed the close — the generation check at
  `src/app/message.h:47-49` already points this direction).
- This is the roster philosophy ("everything that can fail has failed by
  the boot barrier", `src/app/roster.h:16-18`) applied to a queue.

## 4. Decide the recv-ring wrap strategy before the parse loop exists

`sds::ring_buffer` is plain modular, so parse-in-place is impossible for any
frame that straddles the wrap — the socket parse loop would need a copy-out
fallback on a hot path. The alternative is the mirrored ("magic") ring:
map the same physical pages twice back-to-back so **every** frame is
contiguous and the parser is span-based, zero-copy, always.

**Recommendation:** decide mirrored-vs-modular for the **session recv/send
rings** (not the mesh pipes — 20 B mesh frames are fine with copy-out)
before the parser is written. Rejecting mirroring is a fine outcome; record
the decision either way.

**Evidence**
- `src/sds/ring_buffer.h:93-100` — writes split across the wrap.
- `src/sds/ring_buffer.h:186` — dequeue "Reassembles across the wrap."
- `src/sds/ring_buffer.h:198` — peek is "Contiguous run only; a frame
  straddling the wrap is read via dequeue()" — i.e. the zero-copy path
  disappears exactly when a frame wraps.
- The locked memory design already puts session ring storage on mmap
  (project memory "chat server v1 data layout"), so the substrate for
  mirroring is planned anyway.

**Guide**
- Classic recipe: `memfd_create` + `ftruncate(N)`; reserve `2N` of address
  space (`mmap` `PROT_NONE`); `MAP_FIXED`-map the same memfd at `base` and
  `base + N`. Requires N to be a page-size multiple — 64 KiB rings qualify.
- With 256 sessions × 2 rings, slice **one** memfd at offsets instead of one
  fd per ring to keep the fd count at 1.
- Cost: 2× virtual address space per ring (irrelevant at these sizes), a
  slightly more involved storage policy in `ring_buffer` (the mmap-backed
  variant the SoA plan already implies).
- Payoff: `peek` can return a raw `[ptr, len]` span for any complete frame;
  the §8 parse loop becomes span-based with no staging buffer.
- If rejected: the parse loop owns a `k_frame_max` staging buffer and the
  copy-out fallback becomes the documented path. Either way, record in
  `.omc/wiki` (decision record) + `doc/sds/ring_buffer.md`.

## 5. `start()` should trap on missing pipes

**Recommendation:** `LNX_CHECK` the mesh edges in `start()`, matching the
posture the engines already take with `attach()`.

**Evidence**
- `src/app/worker_ctl.h:33-40` — "Must be called BEFORE start()" is comment
  only; the pointers are plain fields whose cross-thread publication rides
  on `pthread_create`'s happens-before. Correct when the order is followed;
  a **silent data race** when it is not.
- `src/app/worker_ctl.cpp:28-33` — `start()` spawns without checking
  `_from_acceptor` / `_to_acceptor`. Same for `acceptor_ctl`.
- `src/app/worker_engine.cpp:30-31` — `attach()` already traps on
  double/null attach; this extends the same rule one layer out.

**Guide**
- Two `LNX_CHECK`s at the top of each `start()`
  (`LNX_CHECK(_from_acceptor != nullptr)` etc. — acceptor checks its edge
  arrays). One line each; no doc change needed beyond the header comment
  gaining "checked in start()".

## 6. Put TSan in the verify loop; cover `enqueue2` cross-thread

*(Revised during evidence collection — the original recommendation "add a
tsan preset and a two-thread stress test" was **wrong on both counts**:
`CMakePresets.json` already has configure+test presets named `tsan`, and a
1M-frame two-thread FIFO stress test already exists.)*

The SPSC pipe's acquire/release cursors are the load-bearing concurrency
claim of the whole mesh, and the default preset is ASan+UBSan — neither
sees ordering bugs. The infrastructure exists; two real gaps remain.

**Evidence**
- `CMakePresets.json` — `tsan` configure and test presets exist;
  `Makefile:18` documents `PRESET=tsan`.
- `tests/sds/ring_buffer_test.cpp:325-354` — 1M-frame SPSC stress with a
  real producer thread, FIFO-order asserted. **Gap (a):** it exercises
  single-region `enqueue` only; `enqueue2` — the two-region single-publish
  primitive that mesh frame atomicity depends on (`src/app/mesh.h:59`) —
  has **no cross-thread test**.
- `CLAUDE.md` §Workflow names default + floor as the verify gates; no CI
  config exists in the tree. **Gap (b):** nothing runs the tsan preset
  routinely.
- Side observation: that stress test uses `std::thread` and
  `std::atomic<bool>` (`ring_buffer_test.cpp:330-331`) while
  `tests/runtime/thread_test.cpp` uses `lnx::thread` — decide whether tests
  are exempt from the no-STL sync rule and record it in
  `doc/04-coding-style.md`, or align the test.

**Guide**
- Add a mesh-level torture case (in `tests/app/mesh_test.cpp`): producer
  thread `mesh_post_msg`s a mixed-size sequence through a small
  `sds::pipe`, consumer runs `mesh_try_recv`, assert type/size/content
  sequence exactly. Small pipe forces wrap + full-pipe backpressure paths.
- Run it under `rtk make test PRESET=tsan`; on acceptance add tsan to the
  pre-merge checklist in `CLAUDE.md` §Workflow (alongside floor).
