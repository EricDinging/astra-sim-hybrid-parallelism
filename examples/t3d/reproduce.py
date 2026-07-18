#!/usr/bin/env python3
"""Top-level reproduce driver for the t3d (3-D torus) experiments; the
cluster dims come from cluster.json (prompted once by the prereq phase).
Thin dispatcher over the phase library scripts/sweep.py.

Usage:
  ./reproduce.py                  interactive: pick experiment(s), generate arrival traces
  ./reproduce.py prereq           set the cluster dims (prompted once, saved to
                                  cluster.json -- every later phase derives from it),
                                  then build the prerequisites: Chakra tracelib (via
                                  stage) + measured service_times.csv (both skipped
                                  if present)
  ./reproduce.py gen [exps|all]   arrival-trace generation for one experiment (or all)
  ./reproduce.py launch [exps|all]    deploy to workers, start runners, monitor
                                  (prompts for a workers file, default workers.txt --
                                  give a different file to place a new sweep on a
                                  separate worker group)
  ./reproduce.py monitor [exps|all]   re-attach to a running sweep's progress
                                  (blocks, polls every 15 min; POLL_SECS overrides; polls
                                  each sweep's own workers per assignments.csv, and
                                  rebalances: idle hosts steal queued combos from
                                  backlogged ones between polls)
  ./reproduce.py collect [exps|all]   pull results back from the workers
  ./reproduce.py post [exps|all]      (not implemented yet)
  ./reproduce.py clean            remove all results (runs/<exp>; prerequisites are kept)

`exps` is one experiment name or a comma-separated subset. Omitting it falls
back to the interactive picker, which accepts the same comma-separated form
(names or menu indices).

Env (prereq phase): STAGE_IMAGE (default astra:latest), DOCKER (default
"sudo docker"), JOBS (in-container parallelism, default nproc-2).
"""

from __future__ import annotations

import argparse
import os
import resource
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "scripts"))

import cluster  # noqa: E402
import sweep  # noqa: E402


def raise_fd_limit() -> None:
    """Raise the open-files soft limit to the hard limit, first thing. Child
    processes inherit rlimits, so every job/subtask this driver spawns (stage
    runs, simulator sims -- one fd per rank, the full torus for the biggest
    shape) sees the increased limit. A non-root process can't raise the hard
    limit itself; measure_svc.py errors out if even that is too low."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft < hard:
        resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))


PHASES = ["prereq", "gen", "launch", "monitor", "collect", "post", "clean"]
CLEANUP = "cleanup all results"


def ensure_cluster() -> None:
    """Prompt for the cluster dims on first prereq run; later runs reuse the
    saved cluster.json."""
    if os.path.isfile(cluster.path(sweep.ROOT)):
        print(f"cluster: {cluster.fmt(cluster.load(sweep.ROOT))} (from cluster.json)")
        return
    while True:
        raw = input("cluster dims (AxBxC, e.g. 16x16x16)> ").strip()
        try:
            dims = cluster.parse_dims(raw)
            break
        except ValueError as e:
            print(e, file=sys.stderr)
    cluster.save(sweep.ROOT, dims)
    print(f"saved {cluster.path(sweep.ROOT)}: {cluster.fmt(dims)}")


def pick_experiments(exps: list[str]) -> list[str]:
    """Menu selection: a single name/index, 'all', or a comma-separated
    subset (names and indices mix freely). Cleanup only stands alone."""
    names = [CLEANUP, "all", *exps]
    print("Which experiment(s) to run? (comma-separate for a subset)")
    for i, name in enumerate(names):
        print(f"  {i}) {name}")
    while True:
        raw = input("experiment> ").strip()
        toks = [t.strip() for t in raw.split(",") if t.strip()]
        toks = [
            names[int(t)] if t.isdigit() and int(t) < len(names) else t for t in toks
        ]
        if toks == [CLEANUP]:
            return toks
        if toks and all(t == "all" or t in exps for t in toks):
            return exps if "all" in toks else list(dict.fromkeys(toks))
        print("invalid selection", file=sys.stderr)


def ask_workers() -> str:
    """Prompt for the workers file to launch on (default workers.txt). Giving
    a different file places the launch on a separate worker group, e.g. when
    new servers show up mid-sweep; assignments.csv remembers the hosts, so
    monitor/collect find each sweep's own workers later."""
    while True:
        raw = input("workers file [workers.txt]> ").strip() or "workers.txt"
        path = raw if os.path.isabs(raw) else os.path.join(sweep.ROOT, raw)
        # the default may legitimately be absent (= run locally); a typed-in
        # file that doesn't exist is a typo, not a request to run locally
        if raw == "workers.txt" or os.path.isfile(path):
            return path
        print(f"{path} not found", file=sys.stderr)


def confirmed_clean() -> None:
    reply = input("remove every runs/<experiment> dir? [y/N] ")
    if reply.strip().lower() in ("y", "yes"):
        sweep.clean()
    else:
        print("aborted")


def main(argv: list[str] | None = None) -> int:
    raise_fd_limit()
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("phase", nargs="?", choices=PHASES, help="phase to run")
    ap.add_argument(
        "experiment",
        nargs="?",
        help="experiment name(s, comma-separated), or 'all'",
    )
    args = ap.parse_args(argv)

    if args.phase == "prereq":
        ensure_cluster()
        sweep.prereq(jobs=os.environ.get("JOBS"))
        return 0
    if args.phase == "clean":
        confirmed_clean()
        return 0

    # every other phase needs cluster.json; this raises SystemExit otherwise
    exps = list(sweep.experiments())

    if args.experiment:
        toks = [t.strip() for t in args.experiment.split(",") if t.strip()]
        unknown = [t for t in toks if t != "all" and t not in exps]
        if unknown or not toks:
            ap.error(f"unknown experiment(s): {', '.join(unknown) or args.experiment}")
        selected = exps if "all" in toks else list(dict.fromkeys(toks))
    else:
        selected = pick_experiments(exps)
        if selected == [CLEANUP]:
            confirmed_clean()
            return 0

    if args.phase == "launch":
        # experiments are planned jointly so a host never oversubscribes
        sweep.launch(selected, workers_file=ask_workers())
        return 0
    if args.phase == "monitor":
        sweep.monitor(selected)
        return 0

    phase_fn = {
        None: sweep.gen,  # bare ./reproduce.py: trace generation
        "gen": sweep.gen,
        "collect": sweep.collect,
        "post": sweep.postprocess,
    }[args.phase]
    for exp in selected:
        phase_fn(exp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
