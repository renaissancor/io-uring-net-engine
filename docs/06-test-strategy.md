# 06 — Test strategy

The library's test strategy is the floor; this document specifies what
the product **adds** above that. See
[`iouring-net-lib/docs/08-test-strategy.md`](../../iouring-net-lib/docs/08-test-strategy.md)
for unit/property/stress conventions that this repo inherits unchanged.

The bar:
- **Every `c2s` packet has at least one integration test** that
  exercises the round-trip on a real loopback socket.
- **Every `s2c` push** has a test that asserts the client receives it.
- **The deframer survives a fuzz harness** of randomly truncated /
  duplicated / reordered byte streams.

---

## Three layers

```
┌─────────────────────────────────────────────────────────────┐
│ Layer 3: end-to-end replay                                  │
│   Two-process: real server binary + real client binary      │
│   Replays a recorded packet trace, asserts ordering/timing  │
│   Slow, runs nightly                                        │
├─────────────────────────────────────────────────────────────┤
│ Layer 2: in-process integration                             │
│   Single process, threads: server + client                  │
│   Loopback socket, real reactor, real codec                 │
│   Fast (<1s per case), runs on every PR                     │
├─────────────────────────────────────────────────────────────┤
│ Layer 1: unit                                               │
│   No I/O. Generated stub/proxy correctness, schema invariants│
│   Sub-millisecond, runs on every commit                     │
└─────────────────────────────────────────────────────────────┘
```

### Layer 1 — unit

Lives under `tests/unit/`. Catch2 test cases that exercise pure
functions emitted by the codegen.

- **Stub-decode correctness.** For each generated `stub_X`, feed it
  a hand-crafted byte buffer, capture the parsed `X_body`, assert
  every field equals the expected value. Catches alignment bugs in
  the generator, endianness regressions, schema-vs-code drift.
- **Proxy-encode correctness.** For each generated `send_X`, capture
  the bytes it emits (via a mock `session` that records writes),
  assert byte-equality against a golden buffer. Pair with the
  stub-decode test on the same bytes — round-trip identity.
- **Schema invariants.** A test harness imports the schema via
  Python (or a generated C++ table) and asserts: unique IDs, valid
  identifiers, no duplicates, payload-size budget.

These tests do not link against `iouring_net::iouring_net`. They link
against `iouring_server_stub` / `iouring_client_proxy` only. Bug
isolation: a codec test failing without any I/O code involved is
unambiguous.

### Layer 2 — in-process integration

Lives under `tests/integration/`. Each test:
1. Spins up an `iouring_net::service` on `127.0.0.1:0` (kernel-assigned port).
2. Registers handlers for the packets under test.
3. Spawns a per-thread `iouring_net::service` for a client coroutine.
4. Calls `send_X` from the client, expects a specific `SC_Y` push back.
5. Asserts the round-trip completes within a deadline (default 100 ms).

Example skeleton:
```cpp
TEST_CASE("CS_MOVE_START → SC_MOVE_START broadcast", "[integration]") {
  test_server srv;                     // fixture: brings up service
  test_client clt(srv.endpoint());     // fixture: connects + handshakes

  REQUIRE(co_await clt.send_CS_MOVE_START(/*dir=*/1, /*x=*/100, /*y=*/200));

  auto pkt = co_await clt.expect<SC_MOVE_START_body>(timeout(100ms));
  REQUIRE(pkt);
  CHECK(pkt->dir == 1);
  CHECK(pkt->x   == 100);
  CHECK(pkt->y   == 200);
}
```

The fixtures live in `tests/integration/support/` and abstract the
"bring up a service, attach a client" boilerplate. New packet tests
should fit in ~10 lines.

### Layer 3 — end-to-end replay

Lives under `tests/e2e/`. Two-process orchestrated via a shell
harness:

```
tests/e2e/run.sh
  ↓
  1. start `server --port 0 --pid-file /tmp/srv.pid`
  2. wait for "listening" line on stdout
  3. start `client --replay tests/e2e/traces/move-attack.pkts`
  4. assert client exit code == 0
  5. assert server log matches tests/e2e/expected/move-attack.log
  6. tear down server
```

Replay files (`.pkts`) are line-protocol: one line per packet, plus
expected response lines. Recorded with a `--record` mode on the
client that captures every send/recv with timestamps.

These tests are slow (process spawn + socket setup overhead per case)
and run **nightly only**. CI on every PR runs Layers 1 + 2.

---

## Cross-platform replay (the parity proof)

The payload-byte parity claim against the Windows reference (see
[`04-protocol.md`](04-protocol.md) § "Parity with the Windows
reference") is proven by **header-normalized trace replay**: feeding
Windows-recorded packet traces through a small normalization step
and then through the Linux deframer, asserting handler outputs and
decoded payload bytes match.

```
tests/e2e/cross-platform/
├── traces/
│   ├── windows-recorded.pkts        # raw bytes captured from the reference
│   └── linux-recorded.pkts          # captured from the Linux client (control)
├── normalize.py                     # 3-byte Windows → 4-byte Linux header
├── compare.py                       # decoded-payload + dispatch sequence asserts
└── verify_offsets.py                # asserts schema _wire_offset_* == captured byte positions
```

### Header normalization — what `normalize.py` does

The Windows reference frames each packet as
`[0x89][u8 payload_size][u8 type][payload]` (Protocol.h:40-45). The
Linux deframer expects `[u16 total_size][u16 id][payload]` (a wider
header — see [`04-protocol.md`](04-protocol.md) § "The framing header
is intentionally different"). Normalization rewrites the header per
frame:

```python
# pseudo
for frame in windows_recorded:
    code, size, type_ = frame[0], frame[1], frame[2]
    assert code == 0x89                                # validates capture
    payload = frame[3 : 3 + size]
    total = 4 + size
    out.write(u16_le(total) + u16_le(type_) + payload)
```

After normalization the payload bytes are unchanged; the wrapping
header is upgraded in place. Bare "strip 0x89" is **not** a valid
normalization — it leaves `[u8 size][u8 type]` which the Linux
deframer misparses as the first two bytes of a 4-byte header.

### Replay assertions — what `compare.py` checks

1. **Dispatch sequence equality.** The Linux server's dispatcher
   logs every `(packet_id, payload_size)` it handles; replay
   produces the same sequence as the original Windows session
   (recorded in the trace's `.meta.yml` sibling).
2. **Decoded-payload byte equality.** For each packet, decoded
   field values match the values logged by the Windows reference
   session (also in `.meta.yml`).
3. **Re-encode round-trip.** The Linux client's proxy, called with
   the decoded field values, produces a frame whose **payload bytes**
   match the original Windows frame's payload bytes (post-header).
   This proves encode and decode are inverses on a real recording.

### Verified offsets — what `verify_offsets.py` checks

A separate harness asserts each codegen-emitted `_wire_offset_<field>`
matches the actual byte position of that field in a Windows-recorded
payload. This guards against drift between the schema's field
declaration order and the reference's serialization order in
`Packet::Put` (Packet.h:42-47). The reference uses straight per-field
`memcpy` in declaration order today; if a future reference change
introduced reordering or packing, this assertion catches it instead
of letting the replay test silently succeed on offsetting-canceling
bugs.

**Cadence.** Despite living in the cross-platform replay directory,
`verify_offsets.py` is **PR-gated** — it runs in the unit/integration
job (Layer 1 + 2), not nightly-only. The reasoning: this harness
exclusively reads pre-checked-in capture fixtures and parses
`packet_layout.h`, so it is fast (no socket setup, no two-process
orchestration), and its failure mode (schema drift) is exactly the
kind of regression we want to fail on the PR that introduced it.
The slow part of cross-platform replay — actually replaying the
trace through a running Linux server — stays nightly.

### Failure routing

A failing cross-platform replay is a hard regression — it means the
payload-byte parity guarantee in
[`04-protocol.md`](04-protocol.md) is broken. **Block release on
it.** A failing `verify_offsets.py` is a related but separate
regression: schema and reference have drifted; the next step is to
diff the schema against the reference's `Packet.h` and `Proxy.cpp`
to find which field moved.

---

## Fuzz harness

`tests/fuzz/deframer_fuzz.cpp` is a libFuzzer harness that hands raw
bytes to the library's `packet_framing` and asserts the deframer
doesn't crash, doesn't read past the input buffer, and doesn't
infinite-loop. Per the library's fuzz convention (see
[`iouring-net-lib/docs/08-test-strategy.md`](../../iouring-net-lib/docs/08-test-strategy.md)
when it covers fuzz).

The product owns this harness because the failure mode it guards
against ("a malicious or buggy client") only manifests at the
product/wire boundary; the library's unit tests use well-formed
inputs.

Sanitizers: ASan + UBSan are mandatory under fuzz; TSan is optional
because the fuzz target is single-threaded.

---

## Sanitizer matrix (inherits library)

| Preset       | Compiler   | Sanitizers   | Layers   | Cadence  |
|--------------|------------|--------------|----------|----------|
| `default`    | g++-13     | ASan + UBSan | 1, 2     | every PR |
| `floor`      | g++-12     | none         | 1, 2     | every PR |
| `tsan`       | g++-13     | TSan         | 1, 2     | every PR |
| `release`    | g++-13     | none         | 1, 2     | every PR |
| `fuzz`       | clang-17   | ASan + UBSan + libFuzzer | fuzz     | nightly  |
| `e2e`        | g++-13     | none         | 3        | nightly  |

TSan on Ubuntu 24.04 needs the same workaround documented in the
library's `tests/CMakeLists.txt` (`DISCOVERY_MODE PRE_TEST`); the
product inherits it.

---

## Test data — where it lives

- **Hand-crafted byte buffers** for unit tests: inline in the test
  `.cpp` files. No separate data directory. If the buffer is more
  than 20 bytes, factor it into a constexpr `std::array` at file
  scope.
- **Recorded traces** for E2E: under `tests/e2e/traces/`. Each trace
  is a `.pkts` file with a `.meta.yml` sibling describing what was
  recorded and against which schema version.
- **Golden expected outputs** for replay: under `tests/e2e/expected/`.
  Same naming pattern.

Schema version is recorded so an old trace replayed against a
new schema fails fast with a clear message instead of producing
spurious mismatches.

---

## Coverage targets (aspirational)

- Layer 1: 95%+ line coverage of `generated/` (one stub-decode and
  one proxy-encode test per packet is the floor).
- Layer 2: 100% packet coverage (every c2s ID and every s2c push has
  at least one test). This is the gating bar; missing coverage blocks
  release of a new packet.
- Layer 3: subjective. One nightly trace per major user-facing flow.

Coverage is measured via `gcovr` over the `default` and `floor`
presets. Combine reports before publishing.

---

## What is explicitly NOT in the test plan

- **Performance regression tests.** Microbenchmarks live in
  `tests/bench/` (Catch2 `BENCHMARK`), not in the gating CI. A
  benchmark dropping by 20% is informational, not blocking.
- **Mutation testing.** Out of scope for v1.
- **Stress tests against a large client farm.** Aspiration; tools
  exist (`tcpkali`, `wrk`) but the harness around them isn't built.
  See `wiki/server/lifecycle.md` § "Load-test posture."

---

## Cross-references

- [`iouring-net-lib/docs/08-test-strategy.md`](../../iouring-net-lib/docs/08-test-strategy.md)
  — library test pyramid that this builds on.
- [`07-ci.md`](07-ci.md) — which presets run on which trigger.
- [`05-codegen.md`](05-codegen.md) § "Style mandates" — the
  determinism rule that makes byte-golden tests possible.
