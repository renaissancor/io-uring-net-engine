#!/usr/bin/env python3
"""Run a fleet of loadgen processes and merge their result.

Multiple processes are not only a way to offer more load. They are the only
check this instrument has against itself: a saturated client reports numbers
that look healthy, and the way that was caught was carrying identical load with
one process and with three and finding they disagreed by two orders of
magnitude. Running a fleet should therefore be the easy path, not the one that
needs a hand-written shell loop every time.

What this assigns so you do not have to:

  --node        0..N-1, which namespaces each process's nicks and rooms so the
                rooms stay disjoint. Two processes sharing a node id merge
                their rooms and silently double the fan-out.
  --src-ip-base non-overlapping 127.0.0.x ranges. Ephemeral ports are a
                per-source-IP kernel resource, so processes sharing a source IP
                divide one pool of 28,232 instead of getting one each.
  --dump        a per-node path, then merges them. Percentiles cannot be
                averaged across processes; only the raw buckets can be added.

    python3 fleet.py --nodes 3 --conns 3334 --rate 30 --duration 20
    python3 fleet.py --nodes 3 --conns 3334 --rate 30 -- --corpus --proto iouring

Anything after a bare -- is passed through to every loadgen unchanged.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
LOADGEN = os.path.join(HERE, "loadgen")
MERGE = os.path.join(HERE, "merge.py")


def main():
    ap = argparse.ArgumentParser(
        description="run N loadgen processes and merge the result")
    ap.add_argument("--nodes", type=int, default=3,
                    help="loadgen processes to run (default 3)")
    ap.add_argument("--conns", type=int, default=3334,
                    help="connections PER NODE (default 3334)")
    ap.add_argument("--rate", default="1",
                    help="messages/sec per connection (default 1)")
    ap.add_argument("--duration", type=int, default=20)
    ap.add_argument("--per-room", type=int, default=10)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--src-ips", type=int, default=1,
                    help="source IPs PER NODE; needed past ~25k conns per node")
    ap.add_argument("--keep", action="store_true",
                    help="keep the dump files instead of deleting them")
    ap.add_argument("--dump-dir", default=None,
                    help="where to write dumps (default: a temp dir)")
    ap.add_argument("passthrough", nargs="*",
                    help="args after -- are forwarded to every loadgen")
    args = ap.parse_args()

    if not os.access(LOADGEN, os.X_OK):
        sys.exit(f"{LOADGEN} not built — run make first")
    if args.nodes < 1:
        sys.exit("--nodes must be at least 1")

    # 127.0.0.x only goes to 255, and node 0 starts at 1.
    highest = 1 + args.nodes * args.src_ips - 1
    if highest > 255:
        sys.exit(f"--nodes {args.nodes} x --src-ips {args.src_ips} needs "
                 f"127.0.0.{highest}, which does not exist. Use fewer source "
                 f"IPs per node, or spread the fleet over more machines — "
                 f"which is what it is for.")

    total = args.nodes * args.conns
    if args.src_ips == 1 and args.conns > 25000:
        print(f"[fleet] warning: {args.conns} conns per node from one source "
              f"IP is near the 28,232 ephemeral port limit; pass --src-ips 2")

    dump_dir = args.dump_dir or tempfile.mkdtemp(prefix="netbench-")
    os.makedirs(dump_dir, exist_ok=True)

    extra = list(args.passthrough)
    procs, dumps = [], []
    print(f"[fleet] {args.nodes} nodes x {args.conns} conns = {total} total, "
          f"rate {args.rate}/s, {args.duration}s -> {args.host}:{args.port}")

    for node in range(args.nodes):
        base = 1 + node * args.src_ips
        dump = os.path.join(dump_dir, f"node{node}.txt")
        dumps.append(dump)
        cmd = [LOADGEN,
               "--host", args.host, "--port", str(args.port),
               "--node", str(node),
               "--conns", str(args.conns),
               "--per-room", str(args.per_room),
               "--rate", str(args.rate),
               "--duration", str(args.duration),
               "--src-ips", str(args.src_ips),
               "--src-ip-base", str(base),
               "--dump", dump] + extra
        log = open(os.path.join(dump_dir, f"node{node}.log"), "w")
        print(f"  node {node}  src-ip-base {base}  -> {dump}")
        procs.append((subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT),
                      log))

    failed = False
    for node, (p, log) in enumerate(procs):
        rc = p.wait()
        log.close()
        if rc != 0:
            failed = True
            print(f"[fleet] node {node} exited {rc}; its log is "
                  f"{os.path.join(dump_dir, f'node{node}.log')}")

    missing = [d for d in dumps if not os.path.exists(d)]
    if missing:
        print(f"[fleet] {len(missing)} nodes produced no dump — not merging, "
              f"because a partial fleet reports a fraction of the load as if "
              f"it were the whole thing.")
        return 1

    print()
    rc = subprocess.call([sys.executable, MERGE] + dumps)

    if not args.keep and not args.dump_dir:
        shutil.rmtree(dump_dir, ignore_errors=True)
    else:
        print(f"\n[fleet] dumps kept in {dump_dir}")
    return rc or (1 if failed else 0)


if __name__ == "__main__":
    sys.exit(main())
