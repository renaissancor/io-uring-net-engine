# 08 — Test strategy

What gets tested, at which level, with which tools. The bar is: every
subsystem in this repo has a test that locks its expected behavior
under the conditions it will face in production. Concurrency code
passes TSan; memory code passes ASan; the integration tests cover
**header-normalized trace-replay parity** against the Windows
reference (payload-byte parity per packet — the framing header is
deliberately wider than the reference's, so raw frame bytes do
*not* match; see § "Wire-format parity test" below).

---

## Test layout

- `tests/<category>/` — Catch2 unit + component tests; the tree
  mirrors `src/<category>/` one file per source file.
- `tests/integration/` — multi-process loopback tests including the
  Linux-vs-Windows wire-format-parity test, which **normalizes** the
  reference's 3-byte `[0x89][u8 size][u8 type]` header to this
  library's 4-byte `[u16 size][u16 id]` header on each captured frame
  before replay.
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
| Session             | full echo round trip; backpressure; mid-recv disconnect            |
| Listener / Service  | bind failure, fd leak (over 1k connect/disconnect), shutdown       |
| Packet framing      | every header value; split-header; malformed; cross-repo parity     |
| Session handle      | non-owning + generation-stale detection; copy semantics            |
| Job queue           | FIFO order; multi-thread push correctness                          |
| mesh framing        | whole-frame post/recv; partial consumes nothing; zero-length body; full = backpressure; FIFO across types; wrap-straddling frames **(landed)** |
| session_table       | id minting; generation guard; state/owner transitions; remove + slot reuse; full-table refusal **(landed)** |

### Realtime-server architecture tests

Tied to the three-thread milestone (see
[`10-realtime-server-architecture.md`](10-realtime-server-architecture.md)):

- **SessionManager/Worker handoff** — accepted fd becomes worker-owned;
  `S_ENTER_WORLD_OK` emitted only after worker adoption + room join; worker
  close notifies the SessionManager; stale-generation messages discarded.
- **Room chat** — join room; broadcast to room members; leave removes
  membership; a packet before `S_ENTER_WORLD_OK` is rejected.
- **Thread-mesh** — SPSC acceptor→worker preserves message order;
  worker→acceptor preserves close-notification order.

The `io_uring` echo smoke stays a low-level transport smoke, not the
end-to-end product milestone. Future: world migration quiesces the old owner
before adoption by the new owner; DB auth validates `session_id + generation`;
a full logger queue does not block a worker.

Packet dispatch / handler-table / codec tests are **not** part of
the library test pyramid in v1 — the dispatcher and codec are
product-side (see
[`iouring-net-server/docs/06-test-strategy.md`](../../iouring-net-server/docs/06-test-strategy.md)
§ "Layer 1 — unit"). The library tests stop at `frame_view`; the
product owns everything above that.

---

## Wire-format parity test — header-normalized trace replay

The single most important integration test. Captures real
Windows-reference packet streams and replays them through the Linux
server **after a header-normalization step** that converts the
reference's 3-byte `[0x89][u8 size][u8 type]` header to this library's
4-byte `[u16 size][u16 id]` header. Parity is a **payload-byte**
claim, not a frame-byte claim — see
[`../doc/network/packet_framing.md`](../doc/network/packet_framing.md)
§ "Purpose" and
[`iouring-net-server/docs/04-protocol.md`](../../iouring-net-server/docs/04-protocol.md)
§ "Parity with the Windows reference" for why.

**Fixture capture.** From the Windows reference build, run a short
client/server session and `tcpdump -w fixtures/echo_session.pcap`.
Extract the TCP payload bytes (one binary file per session direction).
The captured bytes are **raw Windows frames** —
`[0x89][u8 size][u8 type][payload...]` repeated. Check the binary
fixtures into `tests/fixtures/wire/` alongside a `.meta.yml` sibling
naming the reference build's schema version.

**Normalization step.** A small helper (Python script or C++ test
fixture) reads each captured byte stream, parses the 3-byte Windows
header, validates `code == 0x89`, and re-emits each frame as the
Linux 4-byte header form:

```
windows:  0x89 | u8 payload_size | u8 type | payload[payload_size]
linux:    u16 total_size (= payload_size + 4) | u16 id (= type) | payload[payload_size]
```

This is **not** "strip 0x89." A bare-strip leaves the bytes
`[u8 size][u8 type]` which the Linux deframer misparses as
`u16 size = (type << 8) | size`. Either do the full header normalization
or expect every test to fail.

**Replay test.** Spin up the Linux service with a hand-registered
packet handler that records every dispatch; pump the normalized
bytes through a loopback connection; assert the dispatch sequence
and decoded payload bytes match a golden expected list.

**Verified-field-offsets test.** Separately from the replay, the
test harness asserts each schema-emitted `_wire_offset_<field>`
matches the actual byte position of that field in a Windows-recorded
payload. Guards against schema-vs-reference drift in field
serialization order.

**Why this matters.** A subtle bug in the framer (off-by-one on
size, endianness mistake, alignment glitch) breaks parity but might
pass isolated unit tests. Parity replay catches it. The
verified-offsets test catches schema drift the replay test would
miss (a packet with one wrong-position field can still round-trip
within the same build).

---

## Sanitizers

| Sanitizer | When run                              | Notes                          |
|-----------|---------------------------------------|--------------------------------|
| ASan      | Every PR (floor job, gcc-12)          | Default for unit + integration |
| UBSan     | Bundled with ASan                     | Free with `-fsanitize=address,undefined` |
| TSan      | Planned (separate job, lands with reactor) | Concurrency tests only      |
| MSan      | Manual / opt-in                       | Useful for the codec path; libc++ rebuild required, defer |
| Valgrind  | Manual                                | Slow; ASan covers the common cases |

ASan and TSan are mutually exclusive (cannot link the same binary with
both). Run two CI jobs.

For ptrace/sandboxed environments where LeakSanitizer cannot attach,
`ctest --preset default` already sets `LSAN_OPTIONS=detect_leaks=0`.
Use `ctest --preset default-lsan` when strict leak checks are available.

---

## TSan on Ubuntu 24.04 (and other high-ASLR kernels)

ThreadSanitizer hardcodes its address-space layout at link time. On
Ubuntu 24.04 the kernel default is `vm.mmap_rnd_bits=32` (32 bits of
ASLR entropy), which can place mmap regions outside TSan's reserved
ranges. The binary then aborts on first run with:

```
FATAL: ThreadSanitizer: unexpected memory mapping 0x<addr>-0x<addr>
```

This is **not a project bug** — it's a kernel-config / TSan-design
mismatch documented upstream. Two equivalent workarounds:

```bash
# Per-process — no root, no global state change. Recommended for ctest.
setarch "$(uname -m)" -R ctest --preset tsan

# Or, system-wide — once per host, requires root, persists across reboots
# (until reset by Ubuntu kernel updates):
sudo sysctl -w vm.mmap_rnd_bits=28
```

`tests/CMakeLists.txt` uses `catch_discover_tests(... DISCOVERY_MODE
PRE_TEST EXTRA_ARGS --allow-running-no-tests)` so the build itself
stays green, and policy-classified runtime paths in smoke tests can
be treated as non-failures without destabilizing CTest. The TSan
workaround is only needed when tests *run*. CMake 3.29's
`TEST_LAUNCHER` would let us auto-prefix every test with `setarch -R`;
until that is the floor, the invocation is manual.

CI workaround: the `linux-gcc-tsan` GHA job does
`echo 0 | sudo tee /proc/sys/kernel/randomize_va_space` (or the
`sysctl -w vm.mmap_rnd_bits=28` form) once before `ctest --preset
tsan`. See `.github/workflows/floor.yml` for the pattern when the
TSan job lands.

---

## TSan policy

- Every concurrent test must pass under TSan.
- "Benign" data races (well-known false positives in third-party code)
  will be silenced via per-file annotations in a project-level
  suppressions file (added when the TSan job lands). Project code does
  not get suppressions — fix the race or remove the test.
- Lock-free code (the Treiber stack, the SPSC ring buffer) is the
  most TSan-fragile area. Hand-annotate with explicit memory orders
  and run on every commit that touches those files.

---

## Continuous fuzzing

The library's framing code is designed to be fuzz-friendly:
`peek_frame(std::span<const std::byte>)` is a pure function over
bytes — wire a libFuzzer harness against it. (The matching
deframer-fuzz harness on the product side is in
[`iouring-net-server/docs/06-test-strategy.md`](../../iouring-net-server/docs/06-test-strategy.md)
§ "Fuzz harness".)

Codec-level fuzzing is **product-side**: the codec lives in
`iouring-net-server/generated/` and tests against schema invariants
(field widths, wire offsets) rather than library framing. Don't
duplicate it here.

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
   Catch2. Adopt for framing and ring buffer round-trips.
   (Codec property tests are product-side; see
   [`iouring-net-server/docs/06-test-strategy.md`](../../iouring-net-server/docs/06-test-strategy.md).)
4. **Coverage gates.** No coverage gate at v1. Test thoroughness is
   judged by the per-subsystem table above, not by line coverage.
