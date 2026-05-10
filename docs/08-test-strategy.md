# 08 — Test strategy

What gets tested, at which level, with which tools. The bar is: every
subsystem in this repo has a test that locks its expected behavior
under the conditions it will face in production. Concurrency code
passes TSan; memory code passes ASan; the integration tests cover
wire-format parity with the Windows reference.

---

## Test layout

- `tests/<category>/` — Catch2 unit + component tests; the tree
  mirrors `src/<category>/` one file per source file.
- `tests/integration/` — multi-process loopback tests including the
  Linux-vs-Windows wire-format-parity test (sends pre-recorded packet
  fixtures captured from the Windows reference).
- `benchmarks/` — `nanobench`-driven micro-benchmarks; run on demand,
  not in CI by default.

---

## Test pyramid

```
            ┌───────────────────────┐
            │  Wire-format parity   │  ← cross-repo fixtures
            ├───────────────────────┤
            │   Integration (TCP)   │  ← real sockets, loopback
            ├───────────────────────┤
            │     Component         │  ← reactor + 1 listener + 1 session
            ├───────────────────────┤
            │      Unit             │  ← per-class, per-primitive
            └───────────────────────┘
```

Per-level expectations:

| Level             | Tool      | Sanitizers          | Speed         |
|-------------------|-----------|---------------------|---------------|
| Unit              | Catch2 v3 | ASan, UBSan, TSan   | <1 s total    |
| Component         | Catch2 v3 | ASan, UBSan, TSan   | <10 s total   |
| Integration (TCP) | Catch2 v3 + scripts | ASan, UBSan       | <30 s total   |
| Wire-format parity | Catch2 v3 + recorded fixtures | none | <5 s total    |

Benchmarks live in `benchmarks/` and are explicitly **not** in the
test suite. They run on demand.

---

## Per-subsystem coverage targets

| Subsystem           | Tests                                                              |
|---------------------|--------------------------------------------------------------------|
| MemoryPool          | round-trip every bucket; 8-thread × 1M alloc/free under TSan       |
| ObjectPool          | construct/destruct, weak_ptr, mixed-thread drop                    |
| RingBuffer          | wrap, peek-frame split, capacity boundary, property test           |
| SerialBuffer        | every primitive, overflow path, alignment for typed reserve        |
| Atomic / Mutex      | TSan-clean increment race; spin_mutex correctness                  |
| Lock-free stack     | targeted ABA; 8-thread × 1M push/pop; `is_always_lock_free` assert |
| Deadlock profiler   | injected 2-cycle is detected; absent-cycle case clean              |
| Leak tracker        | alloc/free balance; report shape                                   |
| Reactor             | accept, recv (incl. EOF, error), send, close; capability probe     |
| task<T>             | sync_wait, exception propagation, cancellation                     |
| Session             | full echo round trip; backpressure; mid-recv disconnect            |
| Listener / Service  | bind failure, fd leak (over 1k connect/disconnect), shutdown       |
| Packet framing      | every header value; split-header; malformed; cross-repo parity     |
| Packet handler      | id dispatch; unknown id; codec round-trip; codec size mismatch     |
| Job queue           | FIFO order; multi-thread push correctness                          |

---

## Wire-format parity test

The single most important integration test. Captures a binary fixture
of real Windows-reference packet streams and replays them through the
Linux server.

**Fixture capture.** From the Windows reference build, run a short
client/server session and `tcpdump -w fixtures/echo_session.pcap`.
Extract the TCP payload bytes (one binary file per session direction).
Check the binary fixtures into `tests/fixtures/wire/`.

**Replay test.** Spin up the Linux service with a hand-registered
packet handler that records every dispatch; replay the captured bytes
through a loopback connection; assert the dispatch sequence matches a
golden expected list.

**Why this matters.** A subtle bug in the framer (off-by-one on size,
endianness mistake, alignment glitch) breaks parity but might pass
isolated unit tests. Parity catches it.

---

## Sanitizers

| Sanitizer | When run                              | Notes                          |
|-----------|---------------------------------------|--------------------------------|
| ASan      | Every PR (linux-clang job)            | Default for unit + integration |
| UBSan     | Bundled with ASan                     | Free with `-fsanitize=address,undefined` |
| TSan      | Every PR (linux-clang-tsan job)       | Concurrency tests only         |
| MSan      | Manual / opt-in                       | Useful for the codec path; libc++ rebuild required, defer |
| Valgrind  | Manual                                | Slow; ASan covers the common cases |

ASan and TSan are mutually exclusive (cannot link the same binary with
both). Run two CI jobs.

---

## TSan policy

- Every concurrent test must pass under TSan.
- "Benign" data races (well-known false positives in third-party code)
  are silenced via per-file annotations in
  `tests/sanitizer-suppressions.txt`. Project code does not get
  suppressions — fix the race or remove the test.
- Lock-free code (the Treiber stack, the SPSC ring buffer) is the
  most TSan-fragile area. Hand-annotate with explicit memory orders
  and run on every commit that touches those files.

---

## Continuous fuzzing

Out of scope for v1, but the framing code is designed to be
fuzz-friendly:

- `peek_frame(std::span<const std::byte>)` is a pure function over
  bytes.
- `packet_codec<T>::decode(frame_view)` is pure.

Wire libFuzzer harnesses for both at v2. Catch the long tail of weird
inputs that property tests miss.

---

## Open questions

1. **Network-level integration tests in CI.** Loopback (`127.0.0.1`)
   integration tests are reliable. Multi-host tests (NIC offload,
   RSS-aware listener) require dedicated hardware — out of scope for
   open-source CI.
2. **Stress test duration.** A 60-second stress test catches more
   bugs than a 1-second one but costs CI minutes. Run a 5-second
   variant on every PR; a 10-minute variant nightly.
3. **Property-based testing.** `rapidcheck` integrates cleanly with
   Catch2. Adopt for framing, ring buffer, and codec round-trips.
4. **Coverage gates.** No coverage gate at v1. Test thoroughness is
   judged by the per-subsystem table above, not by line coverage.
