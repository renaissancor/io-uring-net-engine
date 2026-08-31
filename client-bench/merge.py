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


def parse(path, lat, lag, scalars, meta):
    with open(path) as f:
        lines = f.read().splitlines()

    # The first line names the dump format and carries the verdict constants
    # the producing binary used. Refusing anything else is the point: a dump
    # from a different loadgen would otherwise misparse silently, and a fleet
    # verdict recomputed with thresholds no binary used describes nothing.
    if not lines or not lines[0].startswith("netbench-dump "):
        sys.exit(f"{path}: no 'netbench-dump' header — written by an older "
                 f"loadgen? Regenerate the dump or use the matching merge.py.")
    head = lines[0].split()
    if head[1] != "v1":
        sys.exit(f"{path}: dump format {head[1]}; this merge.py reads v1")
    kv = dict(p.split("=", 1) for p in head[2:])
    for key in ("lag_floor_ns", "lag_ratio"):
        if key not in kv:
            sys.exit(f"{path}: dump header is missing {key}")
        val = int(kv[key])
        if meta.setdefault(key, val) != val:
            sys.exit(f"{path}: {key}={val} but an earlier dump said "
                     f"{meta[key]} — these runs used different loadgen "
                     f"binaries and their verdicts are not comparable")

    i = 1
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
            elif key in ("server_cpu_pct", "server_kern_pct"):
                # Every node watched the SAME server over roughly the same
                # window, so these are repeated measurements of one quantity,
                # not addends. Summing them would report N x the truth.
                scalars.setdefault(key, []).append(float(val))
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

    lat, lag, scalars, meta = Hist(), Hist(), {}, {}
    for path in sys.argv[1:]:
        parse(path, lat, lag, scalars, meta)

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

    # Server CPU, if any node was given --server-pid. Nodes independently
    # sampled one server, so they should agree; a wide spread means their
    # traffic windows did not overlap and the mean describes no single run.
    cpus = scalars.get("server_cpu_pct", [])
    if cpus:
        cpu = sum(cpus) / len(cpus)
        kern = scalars.get("server_kern_pct", [0.0])
        kern = sum(kern) / len(kern)
        spread = max(cpus) - min(cpus)
        print(f"[srv ] server CPU {cpu:.0f}% of one core "
              f"(user {100 - kern:.0f}% / kernel {kern:.0f}%), "
              f"mean of {len(cpus)} node samples")
        if spread > 10:
            print(f"[srv ] those samples span {spread:.0f} points — the nodes "
                  f"did not measure the same window, so treat the mean as "
                  f"indicative only")
        if cpu < 95:
            print("[srv ] the server was NOT CPU-bound on one core; if it is "
                  "single-threaded this fleet is below its ceiling")

    # Measured fan-out, recovered the same way loadgen does it. Duplicate node
    # ids are invisible to the ownership stamp -- two processes both claiming
    # node 0 stamp identically -- but they are loud here, because their merged
    # rooms return every message twice.
    sent, frames = scalars.get("sent", 0), scalars.get("frames_in", 0)
    want = float(scalars.get("per_room", 0) or 0)
    fanout_bad, fanout = False, 0.0
    if sent and want:
        fanout = frames / sent
        print(f"[fleet] measured fan-out {fanout:.2f} (--per-room {want:.0f})")
        fanout_bad = not (want * 0.95 <= fanout <= want * 1.05)

    if scalars.get("foreign", 0):
        print("[fleet] foreign-stamped frames were excluded from the histogram; "
              "rooms are supposed to be node-private and were not.")

    # Thresholds come from the dump header, so this verdict is by construction
    # the one the binaries themselves would have reached; see traffic.cpp for
    # the rationale behind the floor and the ratio.
    #
    # The 'or lag_beyond' has no counterpart in traffic.cpp because it needs
    # none there: the C++ pct() returns the 1s range limit for a beyond-range
    # percentile, which is already over any sane floor. Our pct() returns the
    # highest occupied bucket — a value that can sit under the floor even when
    # the true p99 is past 1s — so beyond-range must clear the floor explicitly.
    lag_floor, lag_ratio = meta["lag_floor_ns"], meta["lag_ratio"]
    lat99, lat_beyond = lat.pct(0.99)
    lag99, lag_beyond = lag.pct(0.99)

    # Fan-out voids the fleet before self-lag is even consulted, for the
    # reason given in traffic.cpp: once the offered load is not the requested
    # load, asking whether the fleet kept up with it answers nothing. It was
    # an advisory line and advisory lines do not stop anyone -- orphaned
    # loadgen processes drove it to 17.4 against --per-room 10 and the run
    # reported HIGHER throughput, which reads as success.
    if fanout_bad:
        # Above and below mean opposite things; see traffic.cpp.
        if fanout > want:
            print(f"[VOID] measured fan-out {fanout:.2f} exceeds --per-room "
                  f"{want:.0f} — rooms hold more clients than requested, so "
                  f"the offered load is larger than asked and the throughput "
                  f"above is inflated. Usual causes: two nodes sharing a "
                  f"--node id, or orphaned loadgen processes still holding "
                  f"connections (check: pgrep -x loadgen).")
        else:
            print(f"[VOID] measured fan-out {fanout:.2f} is below --per-room "
                  f"{want:.0f} — deliveries that were sent never arrived, so "
                  f"the histogram is missing exactly its slowest samples. "
                  f"Something was overloaded: read fleet self-lag above to "
                  f"see which side.")
        return 3

    if lat.total == 0:
        print("[VOID] no latency samples")
        return 3
    elif lag99 * lag_ratio > lat99 and (lag99 >= lag_floor or lag_beyond):
        print(f"[VOID] fleet self-lag p99 ({lag99/1e6:.3f}ms) is not small "
              f"against latency p99 ({lat99/1e6:.3f}ms) — this run measured "
              f"the fleet, not the server. Add processes or machines.")
        return 3
    elif lag99 * lag_ratio > lat99:
        print(f"[WARN] fleet self-lag p99 ({lag99/1e6:.3f}ms) is a large "
              f"fraction of latency p99 ({lat99/1e6:.3f}ms), but is under "
              f"{lag_floor/1e6:g}ms in absolute terms. Read the server p99 as "
              f">= {(lat99-lag99)/1e6:.3f}ms; the run is usable, the headroom "
              f"is not.")
    else:
        print(f"[ OK ] fleet self-lag p99 ({lag99/1e6:.3f}ms) is small against "
              f"latency p99 ({lat99/1e6:.3f}ms); no node was the bottleneck")
    if lat_beyond:
        print("       latency p99 is past the 1s histogram range; the printed "
              "value is a floor, not a measurement")
    return 0


if __name__ == "__main__":
    # Exit 3 == VOID, so fleet.py and any wrapping script can gate on the
    # verdict without scraping stdout. 0 covers [ OK ] and [WARN] alike.
    sys.exit(main())
