#!/usr/bin/env python3
"""Plot TP and DP scaling: wall time + speedup, with OOM markers."""

import os
import re
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

SCRIPT_DIR  = os.path.dirname(os.path.realpath(__file__))
TP_RESULTS  = os.path.join(SCRIPT_DIR, "results", "tp_scaling")
DP_RESULTS  = os.path.join(SCRIPT_DIR, "results", "dp_scaling")
OUT_PNG     = os.path.join(SCRIPT_DIR, "scaling.png")

SCALE_VALS = [4, 8, 16, 32, 64]

# ── Parse result directories ───────────────────────────────────────────────────
def parse_wall_time(output_path):
    """Return wall-time cycles from sys[0] in an output.txt, or None on failure."""
    try:
        with open(output_path) as f:
            for line in f:
                m = re.search(r"sys\[0\], Wall time:\s*(\d+)", line)
                if m:
                    return int(m.group(1))
    except OSError:
        pass
    return None

tp_data, dp_data = {}, {}   # gpu_count -> cycles (None = OOM/missing)

# Baselines (tp=1, dp=1) — used for speedup only, not plotted on x-axis
tp_base_t = parse_wall_time(os.path.join(TP_RESULTS, "tp1", "output.txt"))
dp_base_t = parse_wall_time(os.path.join(DP_RESULTS, "dp1", "output.txt"))

for n in SCALE_VALS:
    tp_data[n] = parse_wall_time(os.path.join(TP_RESULTS, f"tp{n}", "output.txt"))
    dp_data[n] = parse_wall_time(os.path.join(DP_RESULTS, f"dp{n}", "output.txt"))

def prepare(data):
    items   = sorted(data.items())
    valid_g = [g for g, t in items if t is not None]
    valid_t = [t for g, t in items if t is not None]
    oom_g   = [g for g, t in items if t is None]
    return valid_g, valid_t, oom_g

tp_vg, tp_vt, tp_og = prepare(tp_data)
dp_vg, dp_vt, dp_og = prepare(dp_data)

def speedup(valid_g, valid_t, base_t):
    """Speedup relative to base_t. Returns (gpus, speedups) for valid points only."""
    if not base_t:
        return [], []
    return valid_g, [base_t / t for t in valid_t]

tp_sg, tp_st = speedup(tp_vg, tp_vt, tp_base_t)
dp_sg, dp_st = speedup(dp_vg, dp_vt, dp_base_t)

# ── Summary table ──────────────────────────────────────────────────────────────
print("TP Scaling (DP=1):")
print(f"  {'GPUs':>6}  {'Wall time (cycles)':>22}  {'Speedup vs TP=1':>16}")
for g in SCALE_VALS:
    t = tp_data.get(g)
    sp = (tp_base_t / t) if (t and tp_base_t) else None
    t_str  = f"{t:,}" if t else ("OOM" if g in tp_og else "—")
    sp_str = f"{sp:.2f}×" if sp else "—"
    print(f"  {g:>6}  {t_str:>22}  {sp_str:>16}")

print("\nDP Scaling (TP=1):")
print(f"  {'GPUs':>6}  {'Wall time (cycles)':>22}  {'Speedup vs DP=1':>16}")
for g in SCALE_VALS:
    t = dp_data.get(g)
    sp = (dp_base_t / t) if (t and dp_base_t) else None
    t_str  = f"{t:,}" if t else ("OOM" if g in dp_og else "—")
    sp_str = f"{sp:.2f}×" if sp else "—"
    print(f"  {g:>6}  {t_str:>22}  {sp_str:>16}")

# ── Figure: 2 rows × 2 cols ────────────────────────────────────────────────────
fig, axes = plt.subplots(2, 2, figsize=(12, 8))
fig.suptitle(
    "LLaMA-8B Scaling  —  NVIDIA Vera Rubin NVL72\n"
    "batch=64 · seq=2048 · PP=1",
    fontsize=12, fontweight="bold",
)

(ax_tp_t, ax_dp_t), (ax_tp_s, ax_dp_s) = axes

BLUE, GREEN = "#1f77b4", "#2ca02c"

def _oom_y(valid_t, scale=1.18):
    return max(valid_t) * scale if valid_t else 1.0

def draw_time(ax, all_g, valid_g, valid_t, oom_g, color, title):
    ax.plot(valid_g, [t / 1e9 for t in valid_t],
            "o-", color=color, linewidth=2.2, markersize=8,
            markerfacecolor="white", markeredgewidth=2, label="Simulated")
    if oom_g:
        y_oom = _oom_y(valid_t) / 1e9
        ax.scatter(oom_g, [y_oom] * len(oom_g),
                   marker="X", color="red", s=160, zorder=6, label="OOM")
        for g in oom_g:
            ax.annotate("OOM", xy=(g, y_oom),
                        xytext=(0, 8), textcoords="offset points",
                        ha="center", fontsize=8.5, color="red", fontweight="bold")
    ax.set_title(title, fontsize=10, pad=6)
    ax.set_ylabel("Wall time (×10⁹ cycles)", fontsize=9)
    ax.set_xticks(all_g)
    ax.set_xticklabels([str(g) for g in all_g])
    ax.set_xlim(all_g[0] * 0.5, all_g[-1] * 1.5)
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: str(int(x))))
    ax.xaxis.set_minor_formatter(ticker.NullFormatter())
    ax.legend(fontsize=8)
    ax.grid(True, linestyle="--", alpha=0.35)
    ax.spines[["top", "right"]].set_visible(False)

def draw_speedup(ax, speedup_g, speedup_t, all_g, oom_g, color, title):
    # Ideal line (relative to 1 GPU)
    if all_g:
        ax.plot(all_g, [g / 1 for g in all_g],
                "--", color="gray", linewidth=1.5, alpha=0.8, label="Ideal")
    # Simulated speedup
    ax.plot(speedup_g, speedup_t,
            "o-", color=color, linewidth=2.2, markersize=8,
            markerfacecolor="white", markeredgewidth=2, label="Simulated")
    for g, s in zip(speedup_g, speedup_t):
        ax.annotate(f"{s:.2f}×", xy=(g, s),
                    xytext=(0, 8), textcoords="offset points",
                    ha="center", fontsize=8.5, color=color, fontweight="bold")
    if oom_g:
        y_oom = max(speedup_t) * 1.18 if speedup_t else 1.0
        ax.scatter(oom_g, [y_oom] * len(oom_g),
                   marker="X", color="red", s=160, zorder=6, label="OOM")
        for g in oom_g:
            ax.annotate("OOM", xy=(g, y_oom),
                        xytext=(0, 8), textcoords="offset points",
                        ha="center", fontsize=8.5, color="red", fontweight="bold")
    ax.set_title(title, fontsize=10, pad=6)
    ax.set_xlabel("Number of GPUs", fontsize=9)
    ax.set_ylabel("Speedup vs 1 GPU", fontsize=9)
    ax.set_xticks(all_g)
    ax.set_xticklabels([str(g) for g in all_g])
    ax.set_xlim(all_g[0] * 0.5, all_g[-1] * 1.5)
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: str(int(x))))
    ax.xaxis.set_minor_formatter(ticker.NullFormatter())
    ax.legend(fontsize=8)
    ax.grid(True, linestyle="--", alpha=0.35)
    ax.spines[["top", "right"]].set_visible(False)

draw_time(ax_tp_t, SCALE_VALS, tp_vg, tp_vt, tp_og, BLUE,  "TP Scaling — Wall Time  (DP=1)")
draw_time(ax_dp_t, SCALE_VALS, dp_vg, dp_vt, dp_og, GREEN, "DP Scaling — Wall Time  (TP=1)")
draw_speedup(ax_tp_s, tp_sg, tp_st, SCALE_VALS, tp_og, BLUE,  "TP Scaling — Speedup vs TP=1")
draw_speedup(ax_dp_s, dp_sg, dp_st, SCALE_VALS, dp_og, GREEN, "DP Scaling — Speedup vs DP=1")

plt.tight_layout()
plt.savefig(OUT_PNG, dpi=150, bbox_inches="tight")
print(f"\nSaved: {OUT_PNG}")
