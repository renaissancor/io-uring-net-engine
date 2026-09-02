# main — the supervisor: sole spawner and root of the thread tree

> **Status:** landed
> **Source:** `src/main.cpp`
> **Namespace:** global `main()`; one helper in an anonymous namespace
> **Depends:** `roster`, `config`, `worker_ctl`, `acceptor_ctl`, `mesh`, `detail/thread_role`, `sds::static_vector`, `check`, `runtime/thread`, `types`

## Purpose

`main()` is the supervisor thread. It never runs an engine: it owns all
cross-thread storage, brings up the worker pool and then the acceptor, blocks
on a termination signal, and drives an ordered shutdown. The binary today proves
the roster and boot ordering; there is no listen socket, accept, or handoff
(not built).

## Boot sequence

`main.cpp` has no header. In execution order:

1. **Role token and name.** Set `detail::tls_role = thread_role::supervisor`,
   then `pthread_setname_np(pthread_self(), "main")`. Any stray
   `*_engine::instance()` call on this thread now traps.
2. **Signal mask.** Call the file-local `install_signal_mask()`: build a
   `sigset_t` of `SIGINT` and `SIGTERM`, `pthread_sigmask(SIG_BLOCK, ...)`,
   `LNX_CHECK(rc == 0)`, return the set. This happens before any thread exists,
   so every spawned thread inherits the blocked mask.
3. **Config.** Default-construct `app::config cfg`. There is no worker-count
   field; the roster is compile-time.
4. **LANDLORD storage.** Declare, as stack locals in `main`'s frame:
   `acceptor_to_worker_pipe to_worker[roster::k_worker_count]`,
   `worker_to_acceptor_pipe from_worker[roster::k_worker_count]`, and
   `sds::static_vector<worker_ctl, roster::k_worker_count> workers`. For each
   `i`: `workers.emplace_back(i, cfg)` then
   `workers[i].install_pipes(&to_worker[i], &from_worker[i])`.
5. **Start workers, then barrier.** `w.start()` on every worker, then for every
   worker spin on `w.is_running()` with `lnx::this_thread::yield()`. Print
   `[main] %d worker(s) running`.
6. **Start acceptor.** Construct `acceptor_ctl acceptor{cfg}`,
   `acceptor.install_pipes(to_worker, from_worker)` (whole arrays by reference),
   `acceptor.start()`, spin on `acceptor.is_running()`. Print
   `[main] acceptor running`.
7. **Supervise.** Print the up banner, then `sigwait(&term_set, &sig)`,
   `LNX_CHECK(rc == 0)`, print the received signal number.
8. **Ordered shutdown.** `acceptor.request_stop()`, `acceptor.join()`; then
   `w.request_stop()` on every worker; then `w.join()` on every worker. Print
   `[main] stopped` and `return 0`.

The one compile-time check in the file:

```cpp
static_assert(roster::k_worker_count * (sizeof(acceptor_to_worker_pipe)
                                        + sizeof(worker_to_acceptor_pipe))
                  <= 1024 * 1024,
              "mesh edges live in main's stack frame — keep the roster's "
              "total under 1 MiB or move them to static storage");
```

## Invariants

- **Mask before spawn.** Termination signals are blocked process-wide before
  the first `start()`, so only the supervisor observes them, via `sigwait`.
- **Landlord ownership.** Pipes and the worker table are constructed before any
  thread that can see them and destroyed after every thread is joined, because
  they are locals of the frame that does the joining. Ctls hold borrowed pointers.
- **Exact sizing.** Every array is sized by `roster::k_worker_count`; there are
  no spare slots and no runtime allocation decision.
- **Consumers before producer.** All workers are observed `running` before the
  acceptor is constructed or started.
- **Shutdown is reverse of boot.** Acceptor is stopped and joined first; workers
  are all asked to stop, then all joined.
- **The supervisor runs no engine.** Its role token is `supervisor`, so the
  engine singletons refuse to construct on it.

## Errors & edge cases

| Condition | Behavior |
|---|---|
| `pthread_sigmask` fails | `LNX_CHECK` trap |
| `sigwait` fails | `LNX_CHECK` trap |
| A worker or the acceptor never publishes `running` | the barrier spins forever (yielding); no timeout |
| A thread exits without publishing `stopped` | `thread_ctl::join()` traps after the OS join |
| `install_pipes` skipped or partial | trapped inside `worker_ctl::start()` / `acceptor_ctl::start()`, not here |
| Roster total pipe bytes exceed 1 MiB | compile error from the `static_assert` |
| Any signal other than `SIGINT`/`SIGTERM` | not masked, not waited on; default disposition applies |
| Normal termination | exit code 0 after a clean join of every thread |

## Notes

- The pipes are non-movable (they embed byte rings), which is why they are plain
  arrays in the frame rather than elements of a container.
- `worker_ctl` is non-default-constructible and address-pinned, which is what
  `sds::static_vector` with `emplace_back` provides here.
- The acceptor's `install_pipes` takes array references sized by the roster, so
  passing the wrong-length array fails at compile time.
- `main.cpp` is compiled only into the `uring-server` executable; everything else
  lives in `uring_server_core`, which the tests link.
- Output is `std::printf` to stdout; there is no logger thread (deferred).

## Test plan

No dedicated test. The test suite links `uring_server_core`, which excludes
`main.cpp`; boot and shutdown ordering are exercised only by running the binary.
