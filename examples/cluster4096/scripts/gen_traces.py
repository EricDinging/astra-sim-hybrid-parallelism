"""STG-in-docker trace driver for cluster4096.

Runs STG (Symbolic Tensor Graph) inside the ``astra:latest`` Docker
image to produce Chakra execution traces for every requested job shape.

Usage examples::

    # build all ~729 bw shapes (parallelism auto-detected)
    python3 gen_traces.py

    # build only two specific shapes
    python3 gen_traces.py --only 2x4x8,16x16x16

    # smoke-test: build just 2x2x2
    python3 gen_traces.py --smoke
"""

from __future__ import annotations

import os
import shlex
import subprocess

import shapes

# ---------------------------------------------------------------------------
# STG hyperparameters: everything except --dp / --tp / --pp (injected per shape)
# ---------------------------------------------------------------------------
BW_PARAMS: str = (
    "--model_type dense "
    "--dmodel 8192 --dff 16384 --batch 16 --seq 2048 --dvocal 8192 "
    "--head 32 --kvhead 8 --num_stacks 16 --weight_sharded 0 "
    "--chakra_schema_version v0.0.4"
)

# ---------------------------------------------------------------------------
# Shell script that runs inside the container.
# Reads /work/worklist.txt ("a b c" per line) and runs STG for each shape.
# BW_PARAMS and JOBS are passed via `docker run -e`.
# ---------------------------------------------------------------------------
CONTAINER_SH: str = r"""
set -eu
cd /app/STG
run_one() {
    a="$1"; b="$2"; c="$3"
    od="/work/${MODEL}/${a}x${b}x${c}"
    mkdir -p "$od"
    python3 main.py --output_dir "$od" --output_name chakra_trace \
        --dp "$a" --tp "$b" --pp "$c" ${BW_PARAMS} > "$od/gen.log" 2>&1
}
export -f run_one
xargs -P "${JOBS}" -L1 -a /work/worklist.txt bash -c 'run_one "$@"' _
"""


# ---------------------------------------------------------------------------
# Pure helpers (fully unit-tested; no docker dependency)
# ---------------------------------------------------------------------------


def default_out() -> str:
    """Absolute path to the ``tracelib`` directory next to this script."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(os.path.dirname(here), "tracelib")


def et0_path(out_dir: str, model: str, shape: tuple[int, int, int]) -> str:
    """Return the expected path for the first Chakra trace file of a shape."""
    return os.path.join(out_dir, model, shapes.fmt_shape(shape), "chakra_trace.0.et")


def shapes_to_build(
    out_dir: str,
    model: str,
    candidates: list[tuple[int, int, int]],
) -> list[tuple[int, int, int]]:
    """Drop shapes whose ``chakra_trace.0.et`` already exists."""
    return [s for s in candidates if not os.path.exists(et0_path(out_dir, model, s))]


def build_worklist_text(shape_list: list[tuple[int, int, int]]) -> str:
    """Build the worklist text file content: one ``a b c`` line per shape."""
    return "".join(f"{a} {b} {c}\n" for a, b, c in shape_list)


def select_shapes(
    model: str,
    only: str | None,
    smoke: bool,
) -> list[tuple[int, int, int]]:
    """Return the list of shapes requested by the CLI arguments.

    Priority: ``--smoke`` > ``--only`` > full set.
    """
    if smoke:
        return [(2, 2, 2)]
    if only:
        selected = []
        for tok in only.split(","):
            shape = shapes.parse_shape(tok.strip())
            if not shapes.is_legal(model, *shape):
                raise ValueError(
                    f"--only shape {shapes.fmt_shape(shape)} is not legal for "
                    f"model {model!r} (each dim must be in {shapes.DIM_DOMAIN} "
                    f"and the product <= {shapes.MAX_SIZE})"
                )
            selected.append(shape)
        return selected
    return shapes.all_legal_shapes(model)


# ---------------------------------------------------------------------------
# Docker helpers (not unit-tested here; exercised by integration smoke test)
# ---------------------------------------------------------------------------


def ensure_image(docker_argv: list[str], image: str) -> None:
    """Raise RuntimeError if ``image`` is not available locally."""
    cmd = docker_argv + ["image", "inspect", image]
    result = subprocess.run(cmd, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"Docker image {image!r} not found. Build it from the repo root:\n"
            f"    sudo docker build -t {image} ."
        )


def run_build(
    out_dir: str,
    model: str,
    shape_list: list[tuple[int, int, int]],
    jobs: int,
    docker_argv: list[str],
    image: str,
) -> None:
    """Write the worklist then launch the docker container to build traces."""
    worklist_path = os.path.join(out_dir, "worklist.txt")
    os.makedirs(out_dir, exist_ok=True)
    with open(worklist_path, "w") as fh:
        fh.write(build_worklist_text(shape_list))

    cmd = docker_argv + [
        "run",
        "--rm",
        "--ipc=host",
        "-v",
        f"{out_dir}:/work",
        "-e",
        f"MODEL={model}",
        "-e",
        f"BW_PARAMS={BW_PARAMS}",
        "-e",
        f"JOBS={jobs}",
        image,
        "bash",
        "-c",
        CONTAINER_SH,
    ]
    subprocess.run(cmd, check=True)


def verify(
    out_dir: str,
    model: str,
    shapes_list: list[tuple[int, int, int]],
) -> list[tuple[int, int, int]]:
    """Return shapes that are still missing ``chakra_trace.0.et`` after a build."""
    return [s for s in shapes_list if not os.path.exists(et0_path(out_dir, model, s))]


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(
        description="Build Chakra traces for cluster4096 job shapes via STG-in-docker."
    )
    ap.add_argument(
        "--out", default=default_out(), help="output root (default: tracelib/)"
    )
    ap.add_argument("--model", default="bw", choices=shapes.MODELS)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument(
        "--only", default=None, help="comma-separated shapes, e.g. 2x4x8,16x16x16"
    )
    ap.add_argument(
        "--smoke", action="store_true", help="build only 2x2x2 as a smoke test"
    )
    ap.add_argument(
        "--image",
        default=os.environ.get("STG_IMAGE", "astra:latest"),
        help="Docker image containing STG (default: astra:latest or $STG_IMAGE)",
    )
    ap.add_argument(
        "--docker",
        default=os.environ.get("DOCKER", "sudo docker"),
        help="Docker command (default: 'sudo docker' or $DOCKER)",
    )
    args = ap.parse_args(argv)

    docker_argv = shlex.split(args.docker)
    out_dir = os.path.abspath(args.out)
    requested = select_shapes(args.model, args.only, args.smoke)
    todo = shapes_to_build(out_dir, args.model, requested)

    print(
        f"to_build={len(todo)} already_present={len(requested) - len(todo)}"
        f" model={args.model} jobs={args.jobs}"
    )

    if not todo:
        print("Nothing to build.")
        return 0

    ensure_image(docker_argv, args.image)
    run_build(out_dir, args.model, todo, args.jobs, docker_argv, args.image)

    missing = verify(out_dir, args.model, requested)
    if missing:
        print(f"ERROR: {len(missing)} shapes missing after build:")
        for s in missing:
            print(f"  {shapes.fmt_shape(s)}")
        return 1

    print("All traces verified OK.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
