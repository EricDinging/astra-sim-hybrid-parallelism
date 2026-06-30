# GPT-80B ftol DP GPU-count experiment

This experiment keeps the root-fetch failure count fixed and scales the data-parallel group size. By default it runs `FAILURES=2` with `TP=4`, `PP=1`, and `DP=4,8,16`, corresponding to 16, 32, and 64 total GPUs.

The network backend is analytical congestion-aware `RailNvlink`: 4 GPUs per node, local NVLink scale-up at 450 GB/s, 4 rail leaf switches, a non-blocking spine, and 50 GB/s rail links.

Run in Docker from the repository root:

```bash
docker run --rm --ipc=host --user "$(id -u):$(id -g)" -e HOME=/tmp \
  -v "$PWD:/app/astra-sim" -w /tmp astra-sim:latest \
  bash /app/astra-sim/ftol_experiment/gpu_count/run.sh
```

The machine-readable result is `results/summary.csv`. Per-case logs are under `results/dp*/failures_*/output.txt`.
