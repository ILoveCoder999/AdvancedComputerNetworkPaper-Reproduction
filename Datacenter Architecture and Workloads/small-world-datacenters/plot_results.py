"""
plot_results.py
===============
Reads CSV outputs from swdc_topology.py and swdc-simulation.cc
and reproduces Figs. 3, 4, 5, 6, 11-12 from SWDC SOCC'11.

Usage:
    python3 plot_results.py [results_dir]
"""

import os
import sys
import csv
import collections
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

RESULTS_DIR = sys.argv[1] if len(sys.argv) > 1 else "."
FIG_DIR = os.path.join(RESULTS_DIR, "figures")
os.makedirs(FIG_DIR, exist_ok=True)

# Color/style map matching the paper's legend order
TOPO_ORDER = ["SW-Ring", "SW-2DTorus", "SW-3DHexTorus", "CamCube",
              "CDC(2,5,1)", "CDC(1,7,1)", "CDC(1,5,1)"]
COLORS = {
    "SW-Ring":        "#1f77b4",
    "SW-2DTorus":     "#ff7f0e",
    "SW-3DHexTorus":  "#2ca02c",
    "CamCube":        "#d62728",
    "CDC(2,5,1)":     "#9467bd",
    "CDC(1,7,1)":     "#8c564b",
    "CDC(1,5,1)":     "#7f7f7f",
}
HATCHES = {
    "SW-Ring":        "",
    "SW-2DTorus":     "//",
    "SW-3DHexTorus":  "xx",
    "CamCube":        "\\\\",
    "CDC(2,5,1)":     "--",
    "CDC(1,7,1)":     "..",
    "CDC(1,5,1)":     "oo",
}


def load_csv(path):
    if not os.path.exists(path):
        print(f"  [WARN] File not found: {path}")
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def bar_chart(ax, labels, values, title, ylabel, colors=None, hatches=None):
    x = np.arange(len(labels))
    bars = ax.bar(x, values, width=0.55, color=[colors.get(l, "steelblue") for l in labels],
                  edgecolor="black", linewidth=0.7,
                  hatch=[hatches.get(l, "") for l in labels] if hatches else None)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=20, ha="right", fontsize=7)
    ax.set_ylabel(ylabel, fontsize=8)
    ax.set_title(title, fontsize=9)
    ax.yaxis.grid(True, linestyle="--", alpha=0.5)
    ax.set_axisbelow(True)


# ─────────────────────────────────────────────
# Fig. 3 — Dijkstra path length
# ─────────────────────────────────────────────

def plot_fig3():
    rows = load_csv(os.path.join(RESULTS_DIR, "fig3_dijkstra_path_length.csv"))
    if not rows:
        return
    labels = [r["topology"] for r in rows]
    values = [float(r["avg_path_length"]) for r in rows]

    fig, ax = plt.subplots(figsize=(5, 3.5))
    bar_chart(ax, labels, values,
              "Fig. 3 — Dijkstra Average Path Length\n(N=512, degree-6)",
              "Average Path Length (hops)",
              COLORS, HATCHES)
    fig.tight_layout()
    out = os.path.join(FIG_DIR, "fig3_dijkstra_path_length.pdf")
    fig.savefig(out, bbox_inches="tight")
    fig.savefig(out.replace(".pdf", ".png"), dpi=150, bbox_inches="tight")
    print(f"  Saved {out}")
    plt.close(fig)


# ─────────────────────────────────────────────
# Fig. 4 — Greedy path length
# ─────────────────────────────────────────────

def plot_fig4():
    rows = load_csv(os.path.join(RESULTS_DIR, "fig4_greedy_path_length.csv"))
    if not rows:
        return
    labels = [r["topology"] for r in rows]
    values = [float(r["avg_path_length"]) for r in rows]

    fig, ax = plt.subplots(figsize=(5, 3.5))
    bar_chart(ax, labels, values,
              "Fig. 4 — Greedy Routing Average Path Length\n(N=512, k=3 hops, degree-6)",
              "Average Path Length (hops)",
              COLORS, HATCHES)
    fig.tight_layout()
    out = os.path.join(FIG_DIR, "fig4_greedy_path_length.pdf")
    fig.savefig(out, bbox_inches="tight")
    fig.savefig(out.replace(".pdf", ".png"), dpi=150, bbox_inches="tight")
    print(f"  Saved {out}")
    plt.close(fig)


# ─────────────────────────────────────────────
# Fig. 5 — Packet delivery latency
# ─────────────────────────────────────────────

def plot_fig5(rows):
    """rows from swdc_ns3_results.csv"""
    if not rows:
        return

    traffics = ["uniform", "local", "mapreduce"]
    pkt_sizes = [64, 1024]

    fig, axes = plt.subplots(2, 3, figsize=(12, 7))
    fig.suptitle("Fig. 5 — Packet Delivery Latency (N=512)", fontsize=11)

    for pi, pkt in enumerate(pkt_sizes):
        for ti, traffic in enumerate(traffics):
            ax = axes[pi][ti]
            filtered = [r for r in rows
                        if int(r["pkt_size_B"]) == pkt
                        and r["traffic"] == traffic]

            # Build label → latency mapping
            vals = {}
            for r in filtered:
                topo = r["topology"]
                lat  = float(r["avg_latency_us"])
                # Aggregate CDC variants
                if topo == "CDC":
                    tor = r.get("tor_oversub", "?")
                    agg = r.get("agg_oversub", "?")
                    topo = f"CDC({tor},{agg},1)"
                if topo not in vals or vals[topo] < lat:
                    vals[topo] = lat

            labels = [t for t in TOPO_ORDER if t in vals]
            values = [vals[t] for t in labels]

            if not labels:
                ax.text(0.5, 0.5, "no data", ha="center", va="center",
                        transform=ax.transAxes, fontsize=8, color="gray")
            else:
                bar_chart(ax, labels, values,
                          f"{traffic.capitalize()} ({pkt}B)",
                          "Latency (µs)" if ti == 0 else "",
                          COLORS, HATCHES)
            ax.set_xlabel("")

    fig.tight_layout()
    out = os.path.join(FIG_DIR, "fig5_latency.pdf")
    fig.savefig(out, bbox_inches="tight")
    fig.savefig(out.replace(".pdf", ".png"), dpi=150, bbox_inches="tight")
    print(f"  Saved {out}")
    plt.close(fig)


# ─────────────────────────────────────────────
# Fig. 6 — Maximum aggregate bandwidth
# ─────────────────────────────────────────────

def plot_fig6():
    rows = load_csv(os.path.join(RESULTS_DIR, "swdc_bw_results.csv"))
    if not rows:
        return

    traffics = ["uniform", "local", "mapreduce"]
    pkt_sizes = [64, 1024]

    fig, axes = plt.subplots(2, 3, figsize=(12, 7))
    fig.suptitle("Fig. 6 — Maximum Aggregate Bandwidth (N=512)", fontsize=11)

    for pi, pkt in enumerate(pkt_sizes):
        for ti, traffic in enumerate(traffics):
            ax = axes[pi][ti]
            filtered = [r for r in rows
                        if int(r["pkt_size_B"]) == pkt
                        and r["traffic"] == traffic]

            vals = {}
            for r in filtered:
                topo = r["topology"]
                bw   = float(r["agg_bw_Gbps"])
                if topo == "CDC":
                    tor = r.get("tor_oversub", "?")
                    agg = r.get("agg_oversub", "?")
                    topo = f"CDC({tor},{agg},1)"
                if topo not in vals or vals[topo] < bw:
                    vals[topo] = bw

            labels = [t for t in TOPO_ORDER if t in vals]
            values = [vals[t] for t in labels]

            if not labels:
                ax.text(0.5, 0.5, "no data", ha="center", va="center",
                        transform=ax.transAxes, fontsize=8, color="gray")
            else:
                bar_chart(ax, labels, values,
                          f"{traffic.capitalize()} ({pkt}B)",
                          "Bandwidth (Gbps)" if ti == 0 else "",
                          COLORS, HATCHES)

    fig.tight_layout()
    out = os.path.join(FIG_DIR, "fig6_bandwidth.pdf")
    fig.savefig(out, bbox_inches="tight")
    fig.savefig(out.replace(".pdf", ".png"), dpi=150, bbox_inches="tight")
    print(f"  Saved {out}")
    plt.close(fig)


# ─────────────────────────────────────────────
# Fig. 11-12 — Fault tolerance
# ─────────────────────────────────────────────

def plot_fig11():
    """Python graph-theoretic fault tolerance"""
    rows = load_csv(os.path.join(RESULTS_DIR, "fig11_fault_tolerance.csv"))
    if not rows:
        return

    data = collections.defaultdict(dict)
    for r in rows:
        topo = r["topology"]
        frac = float(r["failure_fraction"])
        rate = float(r["survival_rate"])
        data[topo][frac] = rate

    fig, ax = plt.subplots(figsize=(6, 4))
    for topo in ["SW-Ring", "SW-2DTorus", "SW-3DHexTorus", "CamCube"]:
        if topo not in data:
            continue
        fracs = sorted(data[topo].keys())
        rates = [data[topo][f] * 100 for f in fracs]
        ax.plot(fracs, rates, label=topo, color=COLORS.get(topo, "k"),
                marker="o", markersize=4, linewidth=1.5)

    ax.set_xlabel("Node Failure Fraction")
    ax.set_ylabel("Flow Survival Rate (%)")
    ax.set_title("Fig. 11-12 — Fault Tolerance\n(N=512, graph-theoretic connectivity)")
    ax.xaxis.set_major_formatter(ticker.PercentFormatter(xmax=1.0))
    ax.set_xlim(0, 0.52)
    ax.set_ylim(0, 105)
    ax.legend(fontsize=8)
    ax.grid(True, linestyle="--", alpha=0.5)
    fig.tight_layout()
    out = os.path.join(FIG_DIR, "fig11_fault_tolerance.pdf")
    fig.savefig(out, bbox_inches="tight")
    fig.savefig(out.replace(".pdf", ".png"), dpi=150, bbox_inches="tight")
    print(f"  Saved {out}")
    plt.close(fig)


def plot_fig11_ns3():
    """NS3-based fault tolerance (delivery ratio vs failure fraction)"""
    rows = load_csv(os.path.join(RESULTS_DIR, "swdc_fault_results.csv"))
    if not rows:
        return

    data = collections.defaultdict(dict)
    for r in rows:
        topo = r["topology"]
        frac = float(r["fault_frac"])
        rate = float(r["delivery_ratio"])
        data[topo][frac] = rate

    fig, ax = plt.subplots(figsize=(6, 4))
    for topo in ["SW-Ring", "SW-2DTorus", "SW-3DHexTorus", "CamCube"]:
        if topo not in data:
            continue
        fracs = sorted(data[topo].keys())
        rates = [data[topo][f] * 100 for f in fracs]
        ax.plot(fracs, rates, label=topo, color=COLORS.get(topo, "k"),
                marker="s", markersize=4, linewidth=1.5, linestyle="--")

    ax.set_xlabel("Node Failure Fraction")
    ax.set_ylabel("Packet Delivery Ratio (%)")
    ax.set_title("Fig. 11 (NS3) — Packet Delivery Under Failures\n(N=512, uniform traffic, 1KB)")
    ax.xaxis.set_major_formatter(ticker.PercentFormatter(xmax=1.0))
    ax.set_xlim(-0.02, 0.55)
    ax.set_ylim(0, 105)
    ax.legend(fontsize=8)
    ax.grid(True, linestyle="--", alpha=0.5)
    fig.tight_layout()
    out = os.path.join(FIG_DIR, "fig11_fault_ns3.pdf")
    fig.savefig(out, bbox_inches="tight")
    fig.savefig(out.replace(".pdf", ".png"), dpi=150, bbox_inches="tight")
    print(f"  Saved {out}")
    plt.close(fig)


# ─────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────

if __name__ == "__main__":
    print(f"Plotting results from: {RESULTS_DIR}")
    print(f"Saving figures to:     {FIG_DIR}")
    print()

    # Graph analysis plots
    print("Fig. 3 — Dijkstra path length")
    plot_fig3()

    print("Fig. 4 — Greedy path length")
    plot_fig4()

    # NS3 simulation plots
    latency_rows = load_csv(os.path.join(RESULTS_DIR, "swdc_ns3_results.csv"))
    print("Fig. 5 — Latency")
    plot_fig5(latency_rows)

    print("Fig. 6 — Bandwidth")
    plot_fig6()

    print("Fig. 11 — Fault tolerance (graph-theoretic)")
    plot_fig11()

    print("Fig. 11 — Fault tolerance (NS3)")
    plot_fig11_ns3()

    print()
    print(f"All figures saved as PDF + PNG in {FIG_DIR}")
