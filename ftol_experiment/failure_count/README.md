# GPT-80B ftol failure-count experiment

This experiment measures iteration-time overhead as the number of failed links
in the data-parallel ring increases from 0 through the maximum supported count.

## Configuration

- Model: GPT-80B proxy (~80.1B parameters with tied embeddings)
- Shape: 99 layers, hidden size 8192, FFN size 32768, 64 attention heads,
  vocabulary size 50257
- Workload: batch 32, sequence length 1024
- Parallelism: TP=4, DP=8, PP=1 (32 GPUs); pure TP (no sequence
  parallelism) and sharded DP/FSDP
- Network: analytical congestion-aware RailNvlink; local NVLink switch per 4-GPU node, 4 rail leaves plus a non-blocking spine, 50 GB/s rail links, 900 GB/s NVLink links
- Fault tolerance: `rootFetchRing` for DP/FSDP reduce-scatter and all-gather,
  `m_f=2`, seed 1; TP collectives remain ring-based and are not failed
- Failure timing: every selected failed link is active before protocol round 1
- Sweep: 0..7 failures; 7 is the backend cap of `DP-1`

Run in the repository's ASTRA image:

```bash
docker run --rm --ipc=host \
  -v "$PWD:/app/astra-sim" -w /app/astra-sim \
  astra ./ftol_experiment/failure_count/run.sh
```

Raw logs are under `results/failures_N/output.txt`; each records the DP ring
size, applied failure count, protocol transfer count, and simulator statistics.
The machine-readable result is `results/summary.csv`.

## Results

| Failed DP links | Protocol transfers | Wall time (cycles) | Wall overhead | Comm overhead |
|---:|---:|---:|---:|---:|
| 0 | 56 | 2243437716 | 0.000000% | 0.000000% |
| 1 | 63 | 2244040046 | 0.026849% | 0.041894% |
| 2 | 70 | 2244989296 | 0.069161% | 0.093445% |
| 3 | 77 | 3650432846 | 62.716033% | 79.626104% |
| 4 | 84 | 3659323460 | 63.112327% | 80.128604% |
| 5 | 91 | 3903352188 | 73.989773% | 93.936558% |
| 6 | 98 | 5932797083 | 164.451161% | 208.770973% |
| 7 | 105 | 5873889229 | 161.825376% | 205.437742% |

With failures applied to both DP/FSDP reduce-scatter and all-gather, the
RailNvlink topology keeps same-node TP traffic on fast NVLink and maps same
local ranks onto the same rail leaf. Low failure counts add little critical-path
cost, while higher counts force repair paths onto more contended rail/spine
routes. The curve is not strictly monotonic because the seeded failed-link set
changes which repair paths contend on the critical path.
