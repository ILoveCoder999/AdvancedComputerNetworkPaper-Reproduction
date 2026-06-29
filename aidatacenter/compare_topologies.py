#!/usr/bin/env python3
"""
compare_topologies.py — 跨拓扑 AllReduce 对比图(leaf-spine / mesh / dragonfly)

数据来源：三个 *-sim.cc 在 `--scenario=nccl_ar` 下各写一份 cmp_<topo>.csv：
    ./ns3 run "leaf-spine-sim --scenario=nccl_ar --arRanks=32 --modelMB=8"
    ./ns3 run "mesh-sim       --scenario=nccl_ar --arRanks=32 --modelMB=8"
    ./ns3 run "dragonfly-ib-sim --scenario=nccl_ar --arRanks=32 --modelMB=8"
三者用 **同一组 (arRanks, M)** 跑同一套 NCCL Ring AllReduce → apples-to-apples。

本脚本读 cmp_*.csv，打印对比表，并出一张多面板对比图：
  · JCT (越低越好)          — AI 数据中心头号指标(整步=最慢一条流)
  · busbw (Gbps) + 效率      — NCCL 口径总线带宽 / 线速;规模稳健的横比核心
  · 每交付比特能耗 (pJ/bit)  — 网络能效(越低越好)
  · 每瓦吞吐 (Gbps/W)        — 能效(越高越好)
  · 对分带宽 (Gbps)          — 配了多少跨网容量
  · 平均网络功耗 (W)

用法:
  python3 compare_topologies.py                 # 读当前目录 cmp_*.csv，出 topo_comparison.png
  python3 compare_topologies.py --dir . --out topo_comparison.png
  python3 compare_topologies.py --files cmp_leaf_spine.csv cmp_mesh2d.csv cmp_dragonfly.csv
"""
import argparse
import csv
import glob
import os
import sys

# 列名 → (类型)。cmp-common.h::WriteCmpCsv 的表头。
FLOAT_COLS = ("lineRate_Gbps", "jct_s", "tput_Gbps", "algbw_Gbps", "busbw_Gbps",
              "busbw_eff", "total_energy_J", "avg_power_W", "pJ_per_bit",
              "gbps_per_W", "bisection_Gbps", "pfc_paused_s", "pfc_peak_KB")
INT_COLS = ("arRanks", "modelBytes", "sent", "delivered", "dropped", "pfc_drops")

# 拓扑显示名 + 固定配色(顺序固定，图例稳定)。8 种拓扑,按 fabric 家族排序。
TOPO_ORDER = ["leaf_spine", "fat_tree", "spectrum_x", "rail",
              "dragonfly", "rng", "mesh2d", "mesh3d", "mesh4d"]
TOPO_LABEL = {"leaf_spine": "Leaf-Spine\n(RoCE+PFC)",
              "fat_tree":   "Fat-Tree\n(3-tier RoCE)",
              "spectrum_x": "Spectrum-X\n(RoCE/SHIELD)",
              "rail":       "Rail-IB\n(R∥ rings)",
              "dragonfly":  "Dragonfly\n(IB credit)",
              "rng":        "RNG flat\n(RRG)",
              "mesh2d":     "2D Mesh\n(switchless)",
              "mesh3d":     "3D Mesh\n(switchless)",
              "mesh4d":     "4D Mesh\n(switchless)",
              "rng_fattree": "RNG-FatTree\n(ref)"}
TOPO_COLOR = {"leaf_spine": "#4e79a7", "fat_tree": "#a0cbe8",
              "spectrum_x": "#59a14f", "rail": "#8cd17d",
              "dragonfly":  "#b07aa1", "rng": "#e15759",
              "mesh2d":     "#f28e2b", "mesh3d": "#9c755f",
              "mesh4d":     "#76b7b2",
              "rng_fattree": "#bab0ac"}


def load_row(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return None
    r = rows[-1]  # 取最后一行(最近一次运行)
    for k in FLOAT_COLS:
        v = r.get(k, "")
        r[k] = float(v) if v not in (None, "") else 0.0
    for k in INT_COLS:
        v = r.get(k, "")
        r[k] = int(float(v)) if v not in (None, "") else 0
    return r


def collect(files):
    data = {}
    for p in files:
        r = load_row(p)
        if r is None:
            print(f"[warn] {p} 为空，跳过", file=sys.stderr)
            continue
        topo = r.get("topology", os.path.basename(p))
        data[topo] = r
    return data


def fmt_w(w):
    return f"{w/1000:.1f} kW" if abs(w) >= 1000 else f"{w:.0f} W"


def fmt_t(s):
    if s <= 0:
        return "n/a"
    if s < 1e-3:
        return f"{s*1e6:.1f} µs"
    if s < 1:
        return f"{s*1e3:.2f} ms"
    return f"{s:.3f} s"


def print_table(data):
    order = [t for t in TOPO_ORDER if t in data] + \
            [t for t in data if t not in TOPO_ORDER]
    if not order:
        print("没有可用数据。", file=sys.stderr)
        return
    # 一致性检查:arRanks / modelBytes 应一致才算 apples-to-apples
    arr = {data[t]["arRanks"] for t in order}
    mbs = {data[t]["modelBytes"] for t in order}
    print("\n========= 跨拓扑 AllReduce 对比 =========")
    if len(arr) == 1 and len(mbs) == 1:
        M = next(iter(mbs))
        print(f"工作负载一致 ✓  arRanks={next(iter(arr))}  M={M/2**20:.0f} MiB/rank "
              f"(每条 all-reduce 全网搬运 2(P-1)·M)")
    else:
        print(f"[警告] 工作负载不一致! arRanks={arr} modelBytes={mbs} "
              f"→ 对比非 apples-to-apples，请用相同 --arRanks/--modelMB 重跑。")
    hdr = f"{'指标':<26}" + "".join(f"{t:>16}" for t in order)
    print("-" * len(hdr))
    print(hdr)
    print("-" * len(hdr))

    def row(label, fn):
        print(f"{label:<26}" + "".join(f"{fn(data[t]):>16}" for t in order))

    row("线速 line rate (Gbps)", lambda r: f"{r['lineRate_Gbps']:.0f}")
    row("JCT (越低越好)", lambda r: fmt_t(r["jct_s"]))
    row("busbw (Gbps)", lambda r: f"{r['busbw_Gbps']:.1f}")
    row("busbw 效率 = busbw/线速", lambda r: f"{r['busbw_eff']*100:.1f}%")
    row("交付/丢包", lambda r: f"{r['delivered']}/{r['dropped']}")
    row("pJ/bit (越低越好)", lambda r: f"{r['pJ_per_bit']:.2f}")
    row("Gbps/W (越高越好)", lambda r: f"{r['gbps_per_W']:.4f}")
    row("对分带宽 (Gbps)", lambda r: f"{r['bisection_Gbps']:.0f}")
    row("平均网络功耗", lambda r: fmt_w(r["avg_power_W"]))
    # PFC 仅 RoCE fabric 有(-1=N/A)
    row("PFC PAUSE 累计 (s)",
        lambda r: "n/a" if r["pfc_paused_s"] < 0 else f"{r['pfc_paused_s']:.4f}")
    row("PFC 丢包(应=0)",
        lambda r: "n/a" if r["pfc_paused_s"] < 0 else f"{r['pfc_drops']}")
    print("-" * len(hdr))


def _setup_cjk(plt):
    """选一个系统里有的中文字体，避免标题/标签出现豆腐块。"""
    import matplotlib.font_manager as fm
    prefer = ["Noto Sans CJK SC", "Noto Sans CJK JP", "Noto Sans CJK TC",
              "Source Han Sans SC", "WenQuanYi Zen Hei", "WenQuanYi Micro Hei",
              "SimHei", "Microsoft YaHei", "PingFang SC", "Hiragino Sans GB",
              "Heiti SC", "Songti SC", "STHeiti", "Arial Unicode MS"]
    avail = {f.name for f in fm.fontManager.ttflist}
    # 同时保留多个 CJK 候选作回退，但**不混入 DejaVu**——matplotlib 的逐字回退
    # 一旦列表里有 DejaVu，会对 CJK 字形误报 missing。只放 CJK 字体即可。
    cjk_in_order = [n for n in prefer if n in avail]
    for name in prefer:
        if name in avail:
            plt.rcParams["font.family"] = "sans-serif"
            plt.rcParams["font.sans-serif"] = cjk_in_order
            plt.rcParams["axes.unicode_minus"] = False
            return name
    print("[warn] 未找到中文字体，图中中文可能显示为方块。", file=sys.stderr)
    return None


def make_plot(data, out):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"[warn] 无 matplotlib，跳过出图: {e}", file=sys.stderr)
        return False
    _setup_cjk(plt)

    order = [t for t in TOPO_ORDER if t in data] + \
            [t for t in data if t not in TOPO_ORDER]
    labels = [TOPO_LABEL.get(t, t) for t in order]
    colors = [TOPO_COLOR.get(t, "#888888") for t in order]
    x = range(len(order))

    # (标题, 取值函数, 单位, 方向)  方向: 'lo'=越低越好 'hi'=越高越好
    panels = [
        ("JCT  (越低越好)", lambda r: r["jct_s"] * 1e3, "ms", "lo"),
        ("busbw  (越高越好)", lambda r: r["busbw_Gbps"], "Gbps", "hi"),
        ("busbw 效率", lambda r: r["busbw_eff"] * 100, "% of line", "hi"),
        ("网络能效  (越低越好)", lambda r: r["pJ_per_bit"], "pJ/bit", "lo"),
        ("每瓦吞吐  (越高越好)", lambda r: r["gbps_per_W"], "Gbps/W", "hi"),
        ("对分带宽", lambda r: r["bisection_Gbps"] / 1000.0, "Tbps", "hi"),
    ]

    fig, axes = plt.subplots(2, 3, figsize=(18, 9))
    arr = next(iter({data[t]["arRanks"] for t in order}))
    mb = next(iter({data[t]["modelBytes"] for t in order})) / 2**20
    fig.suptitle(f"跨拓扑 NCCL Ring AllReduce 对比  "
                 f"(arRanks={arr}, M={mb:.0f} MiB/rank)  — 同负载, apples-to-apples",
                 fontsize=13, fontweight="bold")

    for ax, (title, fn, unit, direction) in zip(axes.flat, panels):
        vals = [fn(data[t]) for t in order]
        bars = ax.bar(x, vals, color=colors, edgecolor="black", linewidth=0.6)
        ax.set_title(title, fontsize=11)
        ax.set_ylabel(unit, fontsize=9)
        ax.set_xticks(list(x))
        ax.set_xticklabels(labels, fontsize=7)
        ax.grid(axis="y", alpha=0.3)
        # 标"赢家":越低越好取 min，越高越好取 max
        if any(v > 0 for v in vals):
            best = min(range(len(vals)), key=lambda i: vals[i]) if direction == "lo" \
                else max(range(len(vals)), key=lambda i: vals[i])
            bars[best].set_hatch("//")
        vmax = max(vals) if vals else 1
        for i, v in enumerate(vals):
            ax.text(i, v + vmax * 0.01, f"{v:.2f}" if v < 100 else f"{v:.0f}",
                    ha="center", va="bottom", fontsize=6.5, rotation=0)
        ax.set_ylim(0, vmax * 1.18 if vmax > 0 else 1)

    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.text(0.5, 0.005, "斜纹 = 该指标最优。busbw 效率 / pJ-per-bit 为规模稳健指标；"
             "JCT/busbw 绝对值随线速与规模变化。", ha="center", fontsize=8, style="italic")
    fig.savefig(out, dpi=130)
    print(f"\n出图 → {out}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".", help="cmp_*.csv 所在目录")
    ap.add_argument("--files", nargs="*", help="显式指定 cmp_*.csv 文件")
    ap.add_argument("--out", default="topo_comparison.png")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()

    files = args.files if args.files else sorted(glob.glob(os.path.join(args.dir, "cmp_*.csv")))
    if not files:
        print(f"未找到 cmp_*.csv(目录 {args.dir})。先在 ns-3 里跑三个 sim 的 "
              f"--scenario=nccl_ar。", file=sys.stderr)
        sys.exit(1)
    print("读取:", ", ".join(files))
    data = collect(files)
    if not data:
        sys.exit(1)
    print_table(data)
    if not args.no_plot:
        make_plot(data, args.out)


if __name__ == "__main__":
    main()
