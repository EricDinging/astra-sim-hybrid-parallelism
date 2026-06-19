#!/usr/bin/env python3
"""Slot-limited worker pool over the farm hosts (spec section 8).

Items: one per line in --items (whitespace-separated fields; LINE ORDER IS
PRIORITY — callers put the longest work first). For each item the pool runs

    bash <handler> <host> <field1> <field2> ...

on a host with a free slot (--slots per host, default 2; workers are bound
to hosts, so the cap is structural). Handler exit 0 -> the item line is
appended to <log-dir>/done.txt; nonzero -> <log-dir>/failed.txt, pool
continues and exits 1 at the end. Items already in done.txt are skipped, so
reruns are idempotent (failed.txt is truncated at start). Per-item output:
<log-dir>/<slug>.log.
"""

import argparse
import os
import queue
import re
import subprocess
import sys
import threading


def slug(item):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", item)[:120]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--hosts",
        required=True,
        help="space-separated list; an entry may carry a per-host slot count "
        'as "user@host=N" (default: --slots)',
    )
    ap.add_argument("--slots", type=int, default=2)
    ap.add_argument("--items", required=True)
    ap.add_argument("--handler", required=True)
    ap.add_argument("--log-dir", required=True)
    args = ap.parse_args()
    hosts = []  # (host, slots)
    for ent in args.hosts.split():
        if "=" in ent:
            h, n = ent.rsplit("=", 1)
            hosts.append((h, int(n)))
        else:
            hosts.append((ent, args.slots))
    if not hosts:
        sys.exit("no hosts")
    os.makedirs(args.log_dir, exist_ok=True)
    done_path = os.path.join(args.log_dir, "done.txt")
    fail_path = os.path.join(args.log_dir, "failed.txt")
    done = set()
    if os.path.isfile(done_path):
        with open(done_path) as f:
            done = {line.rstrip("\n") for line in f}
    open(fail_path, "w").close()
    with open(args.items) as f:
        items = [line.strip() for line in f if line.strip()]
    todo = [i for i in items if i not in done]
    nslots = sum(n for _, n in hosts)
    print(
        f"[pool] {len(todo)} to run ({len(items) - len(todo)} already "
        f"done), {len(hosts)} hosts / {nslots} slots",
        flush=True,
    )

    q = queue.Queue()
    for i in todo:
        q.put(i)
    lock = threading.Lock()
    failed = []

    def worker(host):
        while True:
            try:
                item = q.get_nowait()
            except queue.Empty:
                return
            log = os.path.join(args.log_dir, slug(item) + ".log")
            with open(log, "w") as lf:
                r = subprocess.run(
                    ["bash", args.handler, host] + item.split(),
                    stdout=lf,
                    stderr=subprocess.STDOUT,
                )
            with lock:
                if r.returncode == 0:
                    with open(done_path, "a") as f:
                        f.write(item + "\n")
                    print(f"[pool] done ({host}): {item}", flush=True)
                else:
                    failed.append(item)
                    with open(fail_path, "a") as f:
                        f.write(item + "\n")
                    print(
                        f"[pool] FAIL ({host}, rc={r.returncode}): {item} -- see {log}",
                        file=sys.stderr,
                        flush=True,
                    )

    threads = [
        threading.Thread(target=worker, args=(h,), daemon=True)
        for h, n in hosts
        for _ in range(n)
    ]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    if failed:
        print(f"[pool] {len(failed)} item(s) failed", file=sys.stderr)
        sys.exit(1)
    print("[pool] all items done")


if __name__ == "__main__":
    main()
