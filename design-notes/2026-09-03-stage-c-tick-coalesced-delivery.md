---
status: accepted
note: stage C of 2026-09-02 § 7, shaped by result-notes/2026-09-03 § 1.1 — one send per player per tick, coalesced not overwritten, so the client's gates keep their meaning. Ran the same evening (result-notes/2026-09-03-the-tick-budget-stage-c) — P4, P5, P7 confirmed, P6 failed (the burst)
---
# 2026-09-03 — Stage C: tick-coalesced delivery, and why it is coalesced rather than a snapshot

[`result-notes/2026-09-03-the-tick-budget-stage-a.md`](../result-notes/2026-09-03-the-tick-budget-stage-a.md)
§ 1 measured the epoll server's I/O term at ~16 µs per active connection per
tick and found nine tenths of it in the ten `send()` calls the chat shape
issues per input, one per recipient per wake, with one input per wake. § 1.1
predicted that sending each player one frame per tick would cut the term to
~2.7 µs by construction, and argued this has to be built *before* the
io_uring comparison, or the comparison credits io_uring with batching a tick
already provides. This note is the design of that change.

## 1. The shape

Under `CHAT_TICK_MODE=coalesce` (requires `CHAT_TICK_HZ`), a chat frame no
longer produces ten `send()` calls in the batch it arrived in. The handler
appends the frame's payload — the same `"nick: " + blob` bytes the broadcast
would have carried — to a **per-room tick buffer**, as one `[u16 len][bytes]`
entry. At the tick, after the logic walk, every room's buffer is wrapped in
one frame of type `s_tick = 102` and queued to every member, sender
included; the batch flush that follows sends it. One `send()` per player per
tick, carrying everything the room said during the period.

Everything else is unchanged: `CHAT_TICK_MODE` unset (or `immediate`) is the
stage A server byte for byte; `CHAT_TICK_HZ` unset is the stage 0 server.
The `sds::` sibling and the io_uring server take the same mode when they take
the tick.

## 2. Why coalesced, not a snapshot

§ 7.2 of the hypothesis describes the game shape as a *snapshot*: each
entity's latest state, latest input wins, earlier inputs within a tick are
overwritten. That is what a simulation does, and it is not what this stage
builds, for one reason: **the instrument's gates are delivery-counting.**

`merge.py` voids a run whose measured fan-out leaves the ±5 % band around
`--per-room`, and that gate is what caught orphaned processes and shedding
servers in every earlier note. Under latest-wins, two inputs landing in one
tick deliver one; at 30 inputs/s against a 30 Hz tick with scheduling jitter
that happens a few percent of the time, fan-out reads 9.6, and the run voids
for a reason that is the design, not a fault. Relaxing the gate for one mode
would be the instrument bending to the result.

Coalescing keeps every delivery: each input appears in exactly one tick
frame to exactly `F` recipients, so fan-out is still 10.00 and zero loss is
still zero. The **I/O shape** — the thing stage C exists to measure — is
identical to the snapshot's: one wake per input on the receive side, one
`send()` per player per tick on the send side. What differs is payload size
(all inputs of the period, not the latest), which at 10 members × ~100 B is
about 1 KB per frame and adds ~0.4 µs at the fitted 0.4 ns/byte. The
snapshot proper, with entities and latest-wins, is the shaped tick of
§ 7.2 and comes with its own client, later.

## 3. The client

`consume_frames` learns the `s_tick` type: the payload is a sequence of
`[u16 len][entry]` and each entry is parsed exactly as a chat payload is
today — scan to `": "`, check the blob's node stamp, sample
`recv_ts − intended`. Each entry counts as one delivery in `frames_in`, so
the fan-out ratio keeps its definition (deliveries per message sent).

The latency changes meaning and **is not comparable with any earlier row**.
It now includes the wait for the next tick: p50 should sit near half a
period plus half the tick, p99 near a period plus the tick. It is the
latency a player feels under a tick-based server, which is what
`2026-09-02` § 7.4 asked for as the secondary number; the primary number is
still the server's own phase histogram.

## 4. Predictions, before the loop runs

From stage A's constants (`c_drain` 1.25 µs, `c_send` 1.43 µs for ~100 B,
0.4 ns/byte), at `W = 64`:

| N | stage A I/O per input | I/O share | predicted stage C per input | predicted share |
|---:|---:|---:|---:|---:|
| 300 | 16.3 µs | 15 % | ~3.1 µs | ~3 % |
| 1,000 | 15.6 µs | 47 % | ~3.1 µs | ~9 % |
| 3,000 | 10.9 µs (saturated) | 98 % | ~3.1 µs | ~28 %, no longer saturated |
| 10,000 | 3.1 µs (saturated) | 92 % | ~3.1 µs | ~90 %, still at the edge |

- **P4.** The flush term falls from 14.3 ms to about 1.9 ms at `N = 1000`
  (1,000 sends of ~1 KB); the drain stays ~1.1–1.25 ms; the tick phase gains
  the room walk, ~0.2 ms. If the flush is above 4 ms the prediction failed.
- **P5.** Wakes per period stay ≈ `N_a`. Coalescing moves the send side
  only; the receive side still pays one wake, two `recv()`, per input. That
  is the term left for stage B to attack.
- **P6.** At 10k the two shapes cost the same: stage A was already coalesced
  there by saturation. The difference is the queue — stage A's client p50
  was 18–23 ms of backlog; stage C's should be ~17 ms of tick wait with the
  server no longer behind. Same number, opposite meaning; if it is not
  distinguishable in the phase histogram (flush well under the period,
  overruns 0), the row says nothing and this note says so now.
- **P7.** Fan-out 10.00 ± 0.05 and zero loss in every unsaturated cell; the
  coalescing changes no delivery count.
- The 3,000 row is the one that should visibly change regime: stage A had
  it at 98 % I/O with the server at 100 % CPU; stage C predicts ~30 % and
  headroom for `L ≈ 7 µs` before the tick overruns.

## 5. Build notes

- The tick buffer is one `std::string` per room in `server.cpp`; the header
  stays free of rooms and of the STL. `tick::maybe_tick` takes a callback
  that runs inside the timed tick phase, after the logic walk; the room
  walk is that callback, so its cost lands in the tick histogram where
  § 7.1 puts "build snapshots".
- One frame per member is capped at 32 KiB (the client's receive slot is
  64 KiB and a `u16` length field ends at 65,535). A room whose buffer
  would exceed the cap drops the input and counts it; the fan-out gate then
  voids the run, which is the correct outcome for a room that talks faster
  than a tick can carry.
- `queue_send` today drops any payload over 1,024 B silently; the tick
  frame bypasses that cap and uses its own.
- The sweep is stage A's `W = 64` grid — `N ∈ {300, 1000, 3000, 10000}` ×
  `L ∈ {0, 10, 30, 100} µs` — two passes, `ticksweep.py --mode coalesce`,
  read against the stage A rows in the same table.

## 6. Rejected

- **Latest-wins snapshot now.** § 2: it voids the fan-out gate by design and
  measures the same I/O shape. Deferred to the shaped tick.
- **Per-member entries with client-side dedup.** Needed only under
  latest-wins; under coalescing every entry is new.
- **Sending the tick frame from inside the header.** Would put rooms and
  `std::string` into a header whose whole value is being the same bytes in
  three servers.
- **Relaxing the fan-out band for tick mode.** An instrument that changes
  its gate per mode is not an instrument.

## Rationale links

- [`../result-notes/2026-09-03-the-tick-budget-stage-a.md`](../result-notes/2026-09-03-the-tick-budget-stage-a.md)
  § 1–1.1 — the decomposition and the 2.7 µs prediction this stage tests.
- [`2026-09-02-where-io-uring-becomes-meaningful.md`](2026-09-02-where-io-uring-becomes-meaningful.md)
  § 7.2–7.4 — the snapshot shape this stage approximates and the latency
  redefinition it adopts; § 9 for the knobs-first staging.
- [`2026-09-03-working-set-knob-for-the-tick-budget-experiment.md`](2026-09-03-working-set-knob-for-the-tick-budget-experiment.md)
  § 5 — stage A/B/C as first laid out.
- `../client-bench/README.md` § "Verdicts and exit codes" — the fan-out gate
  § 2 refuses to bend.
