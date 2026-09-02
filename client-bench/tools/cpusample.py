#!/usr/bin/env python3
"""Sample the CPU split of every loadgen process, and of the server, during a run.

    python3 tools/cpusample.py --server-pid $(pgrep -x server-sds) &
    python3 fleet.py --nodes 3 --conns 3334 --rate 30 --duration 15 -- --server-pid ...
    # cpusample stops on its own once the loadgen processes have gone

Why this exists. `--server-pid` inside loadgen reports the server's CPU over the
traffic window, but nothing reports the *client's* — and the client is the
side that saturated in every recorded collapse. WSL2 has no `perf`, so the
evidence available for "where do the cycles go" is utime/stime from
/proc/<pid>/stat, read as ratios under identical conditions: the share of one
core each process burned, and how much of that was user versus kernel.

Method. Once a second, utime and stime for every process whose comm is
`loadgen` (or --comm) and for --server-pid if given. At the end, for each
process, the 10-sample (--window) span in which utime+stime grew fastest is
taken as the traffic window; the connect phase and the one-second drain are
mostly idle and would otherwise understate both numbers. The server's comm is
recorded so a row can never be mislabelled: `server` and `server-sds` are two
different binaries and the number means nothing without saying which one.

It reads /proc only. It writes nothing the instrument reads.
"""

import argparse
import os
import signal
import sys
import time
from collections import defaultdict


def read_stat(pid):
    """(utime, stime) in clock ticks, or None if the process is gone."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            s = f.read()
    except OSError:
        return None
    # comm is parenthesised and may contain spaces; fields start after the last ')'
    tail = s[s.rindex(")") + 2:].split()
    return int(tail[11]), int(tail[12])   # utime(14), stime(15), 1-based in proc(5)


def read_comm(pid):
    try:
        with open(f"/proc/{pid}/comm") as f:
            return f.read().strip()
    except OSError:
        return "?"


def pids_by_comm(comm):
    out = []
    for d in os.listdir("/proc"):
        if d.isdigit() and read_comm(d) == comm:
            out.append(int(d))
    return out


def busiest_window(samples, window):
    """samples: [(t, utime, stime)] sorted. Returns (wall_s, du, ds) over the
    `window`-sample span with the largest CPU growth, or None."""
    if len(samples) <= window:
        return None
    best = None
    for i in range(len(samples) - window):
        a, b = samples[i], samples[i + window]
        grow = (b[1] + b[2]) - (a[1] + a[2])
        if best is None or grow > best[0]:
            best = (grow, a, b)
    _, a, b = best
    return b[0] - a[0], b[1] - a[1], b[2] - a[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--comm", default="loadgen",
                    help="process name to sample (default loadgen)")
    ap.add_argument("--server-pid", type=int, default=0,
                    help="also sample this pid and record its comm")
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--window", type=int, default=10,
                    help="samples in the traffic window (default 10)")
    ap.add_argument("--grace", type=float, default=3.0,
                    help="seconds without any --comm process before stopping, "
                         "once at least one was seen (default 3)")
    ap.add_argument("--out", default="",
                    help="also append the report to this file")
    args = ap.parse_args()

    hz = os.sysconf("SC_CLK_TCK")
    samples = defaultdict(list)      # pid -> [(t, utime, stime)]
    server_comm = read_comm(args.server_pid) if args.server_pid else ""
    stop = False

    def on_signal(*_):
        nonlocal stop
        stop = True
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    seen_any = False
    gone_since = None
    while not stop:
        t = time.monotonic()
        pids = pids_by_comm(args.comm)
        if pids:
            seen_any = True
            gone_since = None
        elif seen_any:
            gone_since = gone_since or t
            if t - gone_since >= args.grace:
                break
        if args.server_pid:
            pids = pids + [args.server_pid]
        for pid in pids:
            st = read_stat(pid)
            if st:
                samples[pid].append((t, st[0], st[1]))
        time.sleep(args.interval)

    lines = []
    client_share, client_user = [], []
    for pid, v in sorted(samples.items()):
        if pid == args.server_pid:
            continue
        w = busiest_window(v, args.window)
        if not w:
            lines.append(f"  {args.comm} pid {pid}: too few samples ({len(v)})")
            continue
        wall, du, ds = w
        if du + ds == 0:
            continue
        share = 100.0 * (du + ds) / hz / wall
        user = 100.0 * du / (du + ds)
        client_share.append(share)
        client_user.append(user)
        lines.append(f"  {args.comm} pid {pid}: {share:5.1f}% of a core, "
                     f"user {user:4.1f}% / kernel {100 - user:4.1f}%")
    if client_share:
        n = len(client_share)
        lines.append(f"[cpu ] {args.comm} x{n}: mean {sum(client_share) / n:.0f}% of a "
                     f"core each, user {min(client_user):.0f}..{max(client_user):.0f}% "
                     f"(mean {sum(client_user) / n:.0f}%) over the busiest "
                     f"{args.window} s")
    if args.server_pid:
        w = busiest_window(samples.get(args.server_pid, []), args.window)
        if w and (w[1] + w[2]) > 0:
            wall, du, ds = w
            lines.append(f"[cpu ] server comm={server_comm} pid {args.server_pid}: "
                         f"{100.0 * (du + ds) / hz / wall:.0f}% of a core, "
                         f"user {100.0 * du / (du + ds):.0f}% / "
                         f"kernel {100.0 * ds / (du + ds):.0f}%")
        else:
            lines.append(f"[cpu ] server comm={server_comm} pid {args.server_pid}: "
                         f"too few samples")

    report = "\n".join(lines)
    print(report)
    if args.out:
        with open(args.out, "a") as f:
            f.write(report + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
