#!/usr/bin/env python3
"""
plot_vl2.py  –  Reproduce key figures from the VL2 paper (SIGCOMM 2009)
                using NS-3 simulation output.

Usage:
    python3 plot_vl2.py --results ./results   # directory with .dat files
    python3 plot_vl2.py                       # defaults to current directory

Produces:
    fig9_shuffle.pdf     – Aggregate goodput during all-to-all shuffle  (paper Fig 9)
    fig10_fairness.pdf   – Jain fairness index over time                (paper Fig 10)
    fig11_isolation.pdf  – Performance isolation (two services)         (paper Fig 11)
    fig13_failure.pdf    – Goodput under link failures                  (paper Fig 13)
"""

import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np

# ── style ─────────────────────────────────────────────────────────────────────
plt.rcParams.update({
    "font.size":        10,
    "axes.titlesize":   11,
    "axes.labelsize":   10,
    "legend.fontsize":  9,
    "lines.linewidth":  1.5,
    "figure.dpi":       150,
})

# ── helpers ────────────────────────────────────────────────────────────────────

def load_dat(path):
    """Load a whitespace-delimited .dat file (skip comment lines starting with #).
    Returns a numpy array with all columns (at least 4: time gbps flows fairness;
    experiment 3 adds svc1_gbps and svc2_gbps as columns 5 and 6)."""
    data = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            data.append(list(map(float, line.split())))
    if not data:
        return None
    return np.array(data)


def savefig(fig, name, outdir):
    path = os.path.join(outdir, name)
    fig.savefig(path, bbox_inches="tight")
    print(f"  Saved {path}")
    plt.close(fig)


# ── figure 9: all-to-all shuffle ──────────────────────────────────────────────

def plot_exp1(dat_path, outdir):
    arr = load_dat(dat_path)
    if arr is None:
        print(f"  [skip] {dat_path} is empty")
        return
    t, gbps, flows, fair = arr[:, 0], arr[:, 1], arr[:, 2], arr[:, 3]

    fig, ax1 = plt.subplots(figsize=(6, 3.5))
    ax2 = ax1.twinx()

    ax1.plot(t, gbps,  color="steelblue",  label="Aggregate goodput")
    ax2.plot(t, flows, color="firebrick", linestyle="--", label="Active flows")

    ax1.set_xlabel("Time (s)")
    ax1.set_ylabel("Aggregate goodput (Gbps)", color="steelblue")
    ax2.set_ylabel("Active flows",             color="firebrick")
    ax1.tick_params(axis="y", labelcolor="steelblue")
    ax2.tick_params(axis="y", labelcolor="firebrick")

    peak = gbps.max() if len(gbps) else 0
    ax1.set_title(
        f"Aggregate goodput during all-to-all shuffle\n"
        f"(peak ≈ {peak:.1f} Gbps — Paper Fig. 9)"
    )

    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2, loc="upper right")

    fig.tight_layout()
    savefig(fig, "fig9_shuffle.pdf", outdir)


# ── figure 10: VLB fairness ───────────────────────────────────────────────────

def plot_exp2(dat_path, outdir):
    arr = load_dat(dat_path)
    if arr is None:
        print(f"  [skip] {dat_path} is empty")
        return
    t, gbps, flows, fair = arr[:, 0], arr[:, 1], arr[:, 2], arr[:, 3]

    fig, ax = plt.subplots(figsize=(6, 3.5))
    ax.plot(t, fair, color="darkorange", label="Jain fairness index (all flows)")
    ax.axhline(y=0.98, color="gray", linestyle=":", label="0.98 threshold")
    ax.set_ylim(0.88, 1.02)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Jain's Fairness Index")
    ax.set_title("VLB fairness: traffic split to Intermediate switches\n"
                 "(Paper Fig. 10 — target: ≥ 0.98)")
    ax.legend()
    fig.tight_layout()
    savefig(fig, "fig10_fairness.pdf", outdir)


# ── figure 11: performance isolation ─────────────────────────────────────────

def plot_exp3(dat_path, outdir, n_srv=20):
    """
    Experiment 3: performance isolation.
    The .dat file has 6 columns: time gbps flows fairness svc1_gbps svc2_gbps.
    We plot Service 1 and Service 2 goodput separately (paper Fig 11).
    """
    arr = load_dat(dat_path)
    if arr is None:
        print(f"  [skip] {dat_path} is empty")
        return
    t     = arr[:, 0]
    gbps  = arr[:, 1]
    fair  = arr[:, 3]
    has_split = arr.shape[1] >= 6
    svc1  = arr[:, 4] if has_split else gbps
    svc2  = arr[:, 5] if has_split else np.zeros_like(gbps)

    fig, ax = plt.subplots(figsize=(6, 3.5))
    ax.plot(t, svc1, color="steelblue",  label="Service 1 (persistent, from t=0)")
    ax.plot(t, svc2, color="firebrick",  label="Service 2 (joins at t=30 s)", linestyle="--")
    ax.axvline(x=30.0, color="firebrick", linestyle=":", alpha=0.5)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Goodput (Gbps)")
    ax.set_title("Performance isolation: Service 1 vs Service 2\n"
                 "(Paper Fig. 11 — Service 1 should be unaffected by Service 2)")
    ax.legend()
    fig.tight_layout()
    savefig(fig, "fig11_isolation.pdf", outdir)


# ── figure 13: link failure convergence ───────────────────────────────────────

def plot_exp4(dat_path, outdir):
    arr = load_dat(dat_path)
    if arr is None:
        print(f"  [skip] {dat_path} is empty")
        return
    t, gbps, flows, fair = arr[:, 0], arr[:, 1], arr[:, 2], arr[:, 3]

    fig, ax = plt.subplots(figsize=(7, 3.5))
    ax.plot(t, gbps, color="steelblue", label="Aggregate goodput")

    # Annotate failure / restore events (matches experiment 4 schedule)
    # Int[0] fails t=20,25,30  restored t=100,105,110
    # Int[1] fails t=60,65,70  restored t=130,135,140
    fail_times    = [20, 25, 30, 60, 65, 70]
    restore_times = [100, 105, 110, 130, 135, 140]

    for ft in fail_times:
        ax.axvline(x=ft, color="firebrick", linestyle=":", linewidth=0.8, alpha=0.7)
    for rt in restore_times:
        ax.axvline(x=rt, color="forestgreen", linestyle=":", linewidth=0.8, alpha=0.7)

    fail_patch    = mpatches.Patch(color="firebrick",    alpha=0.7, label="Link failure")
    restore_patch = mpatches.Patch(color="forestgreen",  alpha=0.7, label="Link restore")
    gput_line, = ax.plot([], [], color="steelblue", label="Aggregate goodput")
    ax.legend(handles=[gput_line, fail_patch, restore_patch])

    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Aggregate goodput (Gbps)")
    ax.set_title("Convergence after link failures (graceful degradation)\n"
                 "(Paper Fig. 13 — reconvergence < 1 s)")
    fig.tight_layout()
    savefig(fig, "fig13_failure.pdf", outdir)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Plot VL2 NS-3 simulation results")
    parser.add_argument("--results", default=".", help="Directory with vl2_expN_goodput.dat files")
    parser.add_argument("--outdir",  default=None, help="Output directory for PDFs (default: same as results)")
    args = parser.parse_args()

    results_dir = args.results
    outdir      = args.outdir or results_dir

    os.makedirs(outdir, exist_ok=True)

    print(f"Reading from : {results_dir}")
    print(f"Writing to   : {outdir}")
    print()

    def dat(n):
        return os.path.join(results_dir, f"vl2_exp{n}_goodput.dat")

    if os.path.exists(dat(1)):
        print("Plotting Experiment 1 (all-to-all shuffle)...")
        plot_exp1(dat(1), outdir)
    else:
        print(f"[skip] {dat(1)} not found")

    if os.path.exists(dat(2)):
        print("Plotting Experiment 2 (VLB fairness)...")
        plot_exp2(dat(2), outdir)
    else:
        print(f"[skip] {dat(2)} not found")

    if os.path.exists(dat(3)):
        print("Plotting Experiment 3 (performance isolation)...")
        plot_exp3(dat(3), outdir)
    else:
        print(f"[skip] {dat(3)} not found")

    if os.path.exists(dat(4)):
        print("Plotting Experiment 4 (link failure)...")
        plot_exp4(dat(4), outdir)
    else:
        print(f"[skip] {dat(4)} not found")

    print("\nDone.")


if __name__ == "__main__":
    main()
