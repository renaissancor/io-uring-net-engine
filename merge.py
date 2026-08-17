#!/usr/bin/env python3
"""Merge loadgen --dump files into one fleet-wide report.

Percentiles from separate processes cannot be combined. The mean of two p99s
is not the fleet p99, and it is not any other statistic either -- it has no
interpretation at all. The only correct merge is over the raw buckets, which
is what --dump exists to hand over.

    ./loadgen --node 0 --dump /tmp/n0.txt ... &
    ./loadgen --node 1 --dump /tmp/n1.txt ... &
    wait
    python3 merge.py /tmp/n0.txt /tmp/n1.txt

The verdict is recomputed on the merged histograms rather than taken from any
single process, because a fleet is only as trustworthy as its worst member:
one saturated node inflates the numbers everyone else contributed to.
"""

import sys
from collections import Counter

LAG_FLOOR_NS = 1_000_000   # keep in step with loadgen.cpp


class Hist:
    def __init__(self):
        self.counts = Counter()
        self.total = 0
        self.overflow = 0
        self.min_ns = None
        self.max_ns = 0
        self.bucket_ns = 1000

    def merge(self, other_counts, total, overflow, min_ns, max_ns, bucket_ns):
        if self.total and bucket_ns != self.bucket_ns:
            sys.exit(f"bucket width mismatch: {bucket_ns} vs {self.bucket_ns}")
        self.bucket_ns = bucket_ns
        self.counts.update(other_counts)
        self.total += total
        self.overflow += overflow
        self.max_ns = max(self.max_ns, max_ns)
        if total:
            self.min_ns = min_ns if self.min_ns is None else min(self.min_ns, min_ns)

    def pct(self, p):
        """Returns (nanoseconds, beyond_range)."""
        if self.total == 0:
            return 0, False
        want = p * self.total
        seen = 0
        for b in sorted(self.counts):
            seen += self.counts[b]
            if seen >= want:
                return b * self.bucket_ns, False
        # Everything below the cut sits in overflow: the true value is larger
        # than the histogram can express, so say so instead of inventing it.
        return len(self.counts) and max(self.counts) * self.bucket_ns, True


def parse(path, lat, lag, scalars):
    with open(path) as f:
        lines = f.read().splitlines()

    i = 0
    while i < len(lines):
        parts = lines[i].split()
        if not parts:
            i += 1
            continue

        if parts[0] == "hist":
            label = parts[1]
            kv = dict(p.split("=", 1) for p in parts[2:])
            counts = Counter()
            i += 1
            while i < len(lines) and lines[i] != "end":
                b, c = lines[i].split()
                counts[int(b)] += int(c)
                i += 1
            target = lat if label == "latency" else lag
            target.merge(counts, int(kv["total"]), int(kv["overflow"]),
                         int(kv["min"]), int(kv["max"]), int(kv["bucket_ns"]))
        elif len(parts) == 2:
            key, val = parts
            if key in ("sent", "frames_in", "bytes_in", "unparsed",
                       "foreign", "backpressed", "lost_conns", "conns"):
                scalars[key] = scalars.get(key, 0) + int(val)
            elif key in ("rate", "duration", "per_room"):
                scalars.setdefault(key, val)
            elif key == "node":
                scalars.setdefault("nodes", []).append(val)
        i += 1


def show(label, h):
    if h.total == 0:
        print(f"  {label:<18} (no samples)")
        return
    cells = []
    for name, p in (("p50", .50), ("p90", .90), ("p99", .99),
                    ("p99.9", .999), ("p99.99", .9999)):
        ns, beyond = h.pct(p)
        cells.append(f"{name}={ns/1e6:.3f}{'+' if beyond else ''}")
    print(f"  {label:<18} n={h.total}  min={h.min_ns/1e6:.3f}ms  "
          + "  ".join(cells) + f"  max={h.max_ns/1e6:.3f}ms")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: merge.py <dump> [<dump> ...]")

    lat, lag, scalars = Hist(), Hist(), {}
    for path in sys.argv[1:]:
        parse(path, lat, lag, scalars)

    nodes = scalars.get("nodes", [])
    if len(set(nodes)) != len(nodes):
        dupes = [n for n in set(nodes) if nodes.count(n) > 1]
        print(f"[fleet] node id reused: {', '.join(sorted(dupes))} — "
              f"those processes shared rooms, so their fan-out and offered "
              f"load are not what the command line asked for.")

    dur = float(scalars.get("duration", 0) or 0)
    print(f"\n[merged] {len(nodes)} nodes ({','.join(nodes)})  "
          f"conns={scalars.get('conns', 0)}  "
          f"per_room={scalars.get('per_room', '?')}  "
          f"rate={scalars.get('rate', '?')}/s  duration={scalars.get('duration', '?')}s")
    print(f"[merged] sent={scalars.get('sent', 0)} "
          f"frames_in={scalars.get('frames_in', 0)} "
          f"bytes_in={scalars.get('bytes_in', 0)} "
          f"unparsed={scalars.get('unparsed', 0)} "
          f"foreign={scalars.get('foreign', 0)} "
          f"backpressed={scalars.get('backpressed', 0)} "
          f"lost_conns={scalars.get('lost_conns', 0)}")
    if dur > 0:
        print(f"[merged] {scalars.get('frames_in', 0)/dur:,.0f} deliveries/s "
              f"across the fleet")

    show("delivery latency", lat)
    show("client self-lag", lag)

    # Measured fan-out, recovered the same way loadgen does it. Duplicate node
    # ids are invisible to the ownership stamp -- two processes both claiming
    # node 0 stamp identically -- but they are loud here, because their merged
    # rooms return every message twice.
    sent, frames = scalars.get("sent", 0), scalars.get("frames_in", 0)
    want = float(scalars.get("per_room", 0) or 0)
    if sent and want:
        fanout = frames / sent
        print(f"[fleet] measured fan-out {fanout:.2f} (--per-room {want:.0f})")
        if not (want * 0.95 <= fanout <= want * 1.05):
            print("[fleet] fan-out is not what was requested — the offered "
                  "load above is not the load the command line asked for.")

    if scalars.get("foreign", 0):
        print("[fleet] foreign-stamped frames were excluded from the histogram; "
              "rooms are supposed to be node-private and were not.")

    lat99, lat_beyond = lat.pct(0.99)
    lag99, lag_beyond = lag.pct(0.99)
    if lat.total == 0:
        print("[VOID] no latency samples")
    elif lag99 * 5 > lat99 and (lag99 >= LAG_FLOOR_NS or lag_beyond):
        print(f"[VOID] fleet self-lag p99 ({lag99/1e6:.3f}ms) is not small "
              f"against latency p99 ({lat99/1e6:.3f}ms) — this run measured "
              f"the fleet, not the server. Add processes or machines.")
    elif lag99 * 5 > lat99:
        print(f"[WARN] fleet self-lag p99 ({lag99/1e6:.3f}ms) is a large "
              f"fraction of latency p99 ({lat99/1e6:.3f}ms), but is under 1ms "
              f"in absolute terms. Read the server p99 as "
              f">= {(lat99-lag99)/1e6:.3f}ms; the run is usable, the headroom "
              f"is not.")
    else:
        print(f"[ OK ] fleet self-lag p99 ({lag99/1e6:.3f}ms) is small against "
              f"latency p99 ({lat99/1e6:.3f}ms); no node was the bottleneck")
    if lat_beyond:
        print("       latency p99 is past the 1s histogram range; the printed "
              "value is a floor, not a measurement")


if __name__ == "__main__":
    main()
