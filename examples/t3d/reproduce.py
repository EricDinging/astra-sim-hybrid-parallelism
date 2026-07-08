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
  ./reproduce.py gen [exp|all]    arrival-trace generation for one experiment (or all)
  ./reproduce.py launch [exp|all]     (not implemented yet)
  ./reproduce.py collect [exp|all]    (not implemented yet)
  ./reproduce.py post [exp|all]       (not implemented yet)
  ./reproduce.py clean            remove all results (runs/<exp>; prerequisites are kept)

Omitting the experiment argument falls back to the interactive picker.

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


PHASES = ["prereq", "gen", "launch", "collect", "post", "clean"]
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


def pick_experiment(exps: list[str]) -> str:
    names = [CLEANUP, "all", *exps]
    print("Which experiment(s) to run?")
    for i, name in enumerate(names):
        print(f"  {i}) {name}")
    while True:
        raw = input("experiment> ").strip()
        if raw in names:
            return raw
        if raw.isdigit() and 0 <= int(raw) < len(names):
            return names[int(raw)]
        print("invalid selection", file=sys.stderr)


def confirmed_clean() -> None:
    reply = input("remove every runs/<experiment> dir? [y/N] ")
    if reply.strip().lower() in ("y", "yes"):
        sweep.clean()
    else:
        print("aborted")


def for_each(choice: str, fn, exps: list[str]) -> None:
    for exp in exps if choice == "all" else [choice]:
        fn(exp)


def main(argv: list[str] | None = None) -> int:
    raise_fd_limit()
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("phase", nargs="?", choices=PHASES, help="phase to run")
    ap.add_argument("experiment", nargs="?", help="experiment name, or 'all'")
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

    choice = args.experiment or pick_experiment(exps)
    if choice == CLEANUP:
        confirmed_clean()
        return 0
    if choice != "all" and choice not in exps:
        ap.error(f"unknown experiment: {choice}")

    if args.phase == "launch":
        # experiments are planned jointly so a host never oversubscribes
        sweep.launch(exps if choice == "all" else [choice])
        return 0

    phase_fn = {
        None: sweep.gen,  # bare ./reproduce.py: trace generation
        "gen": sweep.gen,
        "collect": sweep.collect,
        "post": sweep.postprocess,
    }[args.phase]
    for_each(choice, phase_fn, exps)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
