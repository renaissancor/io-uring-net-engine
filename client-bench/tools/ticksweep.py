#!/usr/bin/env python3
"""ticksweep.py — run the tick-budget experiment's stage A grid.

For every cell (N connections, L ns of logic per entity per tick, W bytes per
entity) this starts a fresh server with the tick enabled, runs fleet.py
against it at a fixed per-connection rate, stops the server, and collects two
things per cell: the server's own per-tick phase report (the primary
instrument) and the fleet verdict (the gate that says whether the row counts).

    python3 tools/ticksweep.py --out ../results/ticksweep-<date>

The grid is design-notes/2026-09-03 § 5: N in {300, 1000, 3000, 10000} x
L_entity in {0, 10, 30, 100} us at W = 64, plus W in {512, 4096} at
L_entity in {0, 30}. Two passes over the whole grid, so drift across the
session shows up as a pass-to-pass difference rather than hiding inside a
cell. Never rebuild the server or loadgen while this is running.
"""
import argparse
import csv
import os
import re
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
FLEET = os.path.join(BENCH, "fleet.py")

# connections -> (nodes, conns per node). The 10k row is the 3-node 3M-row
# shape every result note used; below it one node is well inside its range.
FLEET_SHAPE = {300: (1, 300), 1000: (1, 1000), 3000: (3, 1000), 10000: (3, 3334)}


def grid():
    cells = []
    for n in (300, 1000, 3000, 10000):
        for l_us in (0, 10, 30, 100):
            cells.append((n, l_us, 64))
    for n in (300, 1000, 3000, 10000):
        for l_us in (0, 30):
            for w in (512, 4096):
                cells.append((n, l_us, w))
    return cells


def wait_listening(proc, log_path, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        with open(log_path) as f:
            if "listening on" in f.read():
                return True
        time.sleep(0.05)
    return False


def parse_server(log):
    out = {}
    m = re.search(r"burn calibrated at ([0-9.]+) iters/ns", log)
    out["iters_per_ns"] = m.group(1) if m else ""
    m = re.search(r"\[tick\] (\d+) ticks, (\d+) overruns \(([0-9.]+)%\)", log)
    if m:
        out["ticks"], out["overruns"], out["overrun_pct"] = m.groups()
    for label, key in (("io drain", "drain"), ("tick", "tick"), ("flush", "flush")):
        m = re.search(rf"^  {re.escape(label)}\s+n=\d+\s+p50=([0-9.]+\+?)\s+p90=([0-9.]+\+?)\s+p99=([0-9.]+\+?)",
                      log, re.M)
        if m:
            out[f"{key}_p50"], out[f"{key}_p90"], out[f"{key}_p99"] = m.groups()
    m = re.search(r"^  wakes\s+p50=(\d+)\s+p90=(\d+)\s+p99=(\d+)", log, re.M)
    if m:
        out["wakes_p50"], out["wakes_p90"], out["wakes_p99"] = m.groups()
    out["closes"] = str(len(re.findall(r"send buffer over cap", log)))
    return out


def parse_fleet(log):
    out = {}
    m = re.search(r"^\[merged\] ([0-9,]+) deliveries/s", log, re.M)
    out["achieved"] = m.group(1).replace(",", "") if m else ""
    m = re.search(r"delivery latency\s+n=\d+\s+min=[0-9.]+ms\s+p50=([0-9.]+\+?)\s+p90=([0-9.]+\+?)\s+p99=([0-9.]+\+?)", log)
    if m:
        out["lat_p50"], out["lat_p90"], out["lat_p99"] = m.groups()
    m = re.search(r"^\[(VOID|WARN| OK )\]", log, re.M)
    out["verdict"] = m.group(1).strip() if m else "none"
    m = re.search(r"self-lag p99 \(?([0-9.]+)ms", log)
    out["lag_p99"] = m.group(1) if m else ""
    m = re.search(r"server cpu[^0-9]*([0-9.]+)%", log, re.I)
    out["server_cpu"] = m.group(1) if m else ""
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default=os.path.join(BENCH, "..", "server-epoll", "server"))
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--hz", type=int, default=30)
    ap.add_argument("--rate", default="30")
    ap.add_argument("--duration", type=int, default=20)
    ap.add_argument("--passes", type=int, default=2)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default=None,
                    help="comma list of N:L:W cells to run instead of the grid")
    ap.add_argument("--only-w64", action="store_true",
                    help="run only the W = 64 cells of the grid")
    ap.add_argument("--mode", choices=("immediate", "coalesce"), default="immediate",
                    help="CHAT_TICK_MODE: stage A (immediate) or stage C (coalesce)")
    args = ap.parse_args()

    if subprocess.run(["pgrep", "-x", "loadgen"], capture_output=True).returncode == 0:
        sys.exit("loadgen processes are running; refusing to start (orphans inflate fan-out)")
    os.makedirs(args.out, exist_ok=True)

    cells = grid()
    if args.only:
        cells = [tuple(int(x) for x in c.split(":")) for c in args.only.split(",")]
    if args.only_w64:
        cells = [c for c in cells if c[2] == 64]

    fields = ["mode", "pass", "N", "L_us", "W", "iters_per_ns", "ticks", "overruns", "overrun_pct",
              "drain_p50", "drain_p90", "drain_p99", "tick_p50", "tick_p90", "tick_p99",
              "flush_p50", "flush_p90", "flush_p99", "wakes_p50", "wakes_p90", "wakes_p99",
              "closes", "achieved", "lat_p50", "lat_p90", "lat_p99", "lag_p99",
              "server_cpu", "verdict"]
    csv_path = os.path.join(args.out, "summary.csv")
    new = not os.path.exists(csv_path)
    with open(csv_path, "a", newline="") as cf:
        wr = csv.DictWriter(cf, fieldnames=fields)
        if new:
            wr.writeheader()
        for p in range(1, args.passes + 1):
            for (n, l_us, w) in cells:
                tag = f"{args.mode}-p{p}-N{n}-L{l_us}-W{w}"
                nodes, per_node = FLEET_SHAPE[n]
                env = dict(os.environ,
                           CHAT_FLUSH="batch", CHAT_QUIET="1",
                           CHAT_MAX_CONNS=str(n + 200),
                           CHAT_TICK_HZ=str(args.hz),
                           CHAT_TICK_MODE=args.mode,
                           LOGIC_NS_PER_ENTITY_TICK=str(l_us * 1000),
                           LOGIC_BYTES_PER_ENTITY=str(w),
                           CHAT_TICK_DUMP=os.path.join(args.out, f"{tag}.tickdump"))
                slog = os.path.join(args.out, f"{tag}.server.log")
                flog = os.path.join(args.out, f"{tag}.fleet.log")
                print(f"[{tag}] server up", flush=True)
                with open(slog, "w") as sf:
                    srv = subprocess.Popen([args.server, str(args.port)], env=env,
                                           stdout=sf, stderr=subprocess.STDOUT)
                if not wait_listening(srv, slog):
                    print(f"[{tag}] server did not come up; see {slog}")
                    srv.kill()
                    continue
                time.sleep(0.5)
                cmd = [sys.executable, FLEET, "--nodes", str(nodes), "--conns", str(per_node),
                       "--rate", args.rate, "--duration", str(args.duration),
                       "--port", str(args.port), "--", "--server-pid", str(srv.pid)]
                with open(flog, "w") as ff:
                    rc = subprocess.call(cmd, stdout=ff, stderr=subprocess.STDOUT)
                time.sleep(0.5)
                srv.send_signal(signal.SIGTERM)
                try:
                    srv.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    srv.kill()
                    print(f"[{tag}] server did not stop on SIGTERM; killed")
                row = {"mode": args.mode, "pass": p, "N": n, "L_us": l_us, "W": w}
                row.update(parse_server(open(slog).read()))
                row.update(parse_fleet(open(flog).read()))
                wr.writerow({k: row.get(k, "") for k in fields})
                cf.flush()
                print(f"[{tag}] fleet rc={rc} {row.get('verdict')}  drain p50 {row.get('drain_p50')}  "
                      f"tick p50 {row.get('tick_p50')}  flush p50 {row.get('flush_p50')}  "
                      f"overruns {row.get('overrun_pct')}%  lat p99 {row.get('lat_p99')}", flush=True)
                time.sleep(1.0)   # let the kernel reap 10k sockets before the next accept storm
    print(f"done; {csv_path}")


if __name__ == "__main__":
    main()
