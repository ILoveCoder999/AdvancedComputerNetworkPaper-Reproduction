#!/usr/bin/env python3
"""
compare_credit.py — mesh 家族 逐跳信用消融:无损(--credit=1) vs 有损(--credit=0)

数据来源:同一组 (arRanks, M, linkRate, queuePkts) 下,把每个 mesh sim 各跑两遍,
把 cmp_mesh*.csv 分别收进两个目录:
    credit_on/   <- mesh-sim/mesh3d-sim/mesh4dw6-sim  --credit=1  (逐跳信用,无损)
    credit_off/  <- 同上                              --credit=0  (有损直发,对照)

本脚本读这两个目录,打印对照表,并出一张图:
  · JCT   (credit=1 vs 0)        — all-reduce 完成时间
  · busbw (credit=1 vs 0)        — NCCL 总线带宽
  · 丢包率 dropped/sent (%)      — **信用的核心收益**:credit=1 应≈0;credit=0>0
                                    (有损 → all-reduce 结果不完整/错误,JCT 仅对已交付子集)

用法:
  python3 compare_credit.py --on credit_on --off credit_off --out credit_ablation.png
"""
import argparse
import csv
import glob
import os
import sys

FLOAT_COLS = ("lineRate_Gbps", "jct_s", "tput_Gbps", "busbw_Gbps", "busbw_eff",
              "pJ_per_bit", "gbps_per_W")
INT_COLS = ("arRanks", "modelBytes", "sent", "delivered", "dropped")

TOPO_ORDER = ["mesh2d", "mesh3d", "mesh4d"]
TOPO_LABEL = {"mesh2d": "2D Mesh", "mesh3d": "3D Mesh", "mesh4d": "4D Mesh"}
C_ON  = "#4e79a7"   # 无损(credit=1)
C_OFF = "#e15759"   # 有损(credit=0)


def load_dir(d):
    data = {}
    for p in sorted(glob.glob(os.path.join(d, "cmp_*.csv"))):
        with open(p) as f:
            rows = list(csv.DictReader(f))
        if not rows:
            continue
        r = rows[-1]
        rec = {}
        for k in FLOAT_COLS:
            v = r.get(k, "")
            rec[k] = float(v) if v not in (None, "") else 0.0
        for k in INT_COLS:
            v = r.get(k, "")
            rec[k] = int(float(v)) if v not in (None, "") else 0
        data[r.get("topology", os.path.basename(p))] = rec
    return data


def fmt_t(s):
    if s <= 0:
        return "n/a"
    if s < 1e-3:
        return f"{s*1e6:.1f}us"
    if s < 1:
        return f"{s*1e3:.2f}ms"
    return f"{s:.3f}s"


def drop_pct(r):
    return 100.0 * r["dropped"] / r["sent"] if r["sent"] else 0.0


def print_table(on, off, topos):
    print("\n===== mesh 家族 逐跳信用消融:无损(credit=1) vs 有损(credit=0) =====")
    arr = {on[t]["arRanks"] for t in topos} | {off[t]["arRanks"] for t in topos}
    mbs = {on[t]["modelBytes"] for t in topos} | {off[t]["modelBytes"] for t in topos}
    if len(arr) == 1 and len(mbs) == 1:
        print(f"工作负载一致 ✓ arRanks={next(iter(arr))} "
              f"M={next(iter(mbs))/2**20:.0f}MiB/rank")
    else:
        print(f"[警告] on/off 工作负载不一致 arRanks={arr} modelBytes={mbs} → 重跑对齐")
    hdr = f"{'指标':<22}" + "".join(f"{TOPO_LABEL.get(t,t):>22}" for t in topos)
    print("-" * len(hdr)); print(hdr); print("-" * len(hdr))

    def row(label, fn):
        print(f"{label:<22}" + "".join(f"{fn(t):>22}" for t in topos))

    row("JCT  credit=1",  lambda t: fmt_t(on[t]["jct_s"]))
    row("JCT  credit=0",  lambda t: fmt_t(off[t]["jct_s"]))
    row("JCT  Δ(off→on)", lambda t: (f"{(off[t]['jct_s']/on[t]['jct_s']-1)*100:+.1f}%"
                                      if on[t]["jct_s"] > 0 else "n/a"))
    row("busbw=1 (Gbps)", lambda t: f"{on[t]['busbw_Gbps']:.1f}")
    row("busbw=0 (Gbps)", lambda t: f"{off[t]['busbw_Gbps']:.1f}")
    row("丢包% credit=1", lambda t: f"{drop_pct(on[t]):.2f}%")
    row("丢包% credit=0", lambda t: f"{drop_pct(off[t]):.2f}%")
    row("交付/发 credit=1", lambda t: f"{on[t]['delivered']}/{on[t]['sent']}")
    row("交付/发 credit=0", lambda t: f"{off[t]['delivered']}/{off[t]['sent']}")
    print("-" * len(hdr))
    print("注:credit=0 丢包 → all-reduce 不完整(结果错误);其 JCT 仅对已交付子集,"
          "与 credit=1 不可直接当‘更快’解读。信用的收益首先是 **无损/正确**。")


def setup_cjk(plt):
    import matplotlib.font_manager as fm
    prefer = ["Noto Sans CJK SC", "Noto Sans CJK JP", "Source Han Sans SC",
              "WenQuanYi Zen Hei", "WenQuanYi Micro Hei", "SimHei",
              "Microsoft YaHei", "PingFang SC", "Hiragino Sans GB", "Arial Unicode MS"]
    avail = {f.name for f in fm.fontManager.ttflist}
    cjk = [n for n in prefer if n in avail]
    if cjk:
        plt.rcParams["font.family"] = "sans-serif"
        plt.rcParams["font.sans-serif"] = cjk
    plt.rcParams["axes.unicode_minus"] = False


def make_plot(on, off, topos, out):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:
        print(f"[warn] 无 matplotlib,跳过出图: {e}", file=sys.stderr)
        return False
    setup_cjk(plt)
    import numpy as np

    labels = [TOPO_LABEL.get(t, t) for t in topos]
    x = np.arange(len(topos)); w = 0.38

    panels = [
        ("JCT  (越低越好)", "ms", lambda r: r["jct_s"] * 1e3),
        ("busbw  (越高越好)", "Gbps", lambda r: r["busbw_Gbps"]),
        ("丢包率 dropped/sent  (信用核心收益)", "%", drop_pct),
    ]
    fig, axes = plt.subplots(1, 3, figsize=(16, 5))
    arr = next(iter({on[t]["arRanks"] for t in topos}))
    mb = next(iter({on[t]["modelBytes"] for t in topos})) / 2**20
    fig.suptitle(f"mesh 家族 逐跳信用消融:无损(credit=1) vs 有损(credit=0)  "
                 f"(arRanks={arr}, M={mb:.0f} MiB/rank)", fontsize=13, fontweight="bold")

    for ax, (title, unit, fn) in zip(axes, panels):
        von = [fn(on[t]) for t in topos]
        vof = [fn(off[t]) for t in topos]
        b1 = ax.bar(x - w/2, von, w, label="credit=1 (无损)", color=C_ON,
                    edgecolor="black", linewidth=0.5)
        b2 = ax.bar(x + w/2, vof, w, label="credit=0 (有损)", color=C_OFF,
                    edgecolor="black", linewidth=0.5)
        ax.set_title(title, fontsize=11)
        ax.set_ylabel(unit, fontsize=9)
        ax.set_xticks(x); ax.set_xticklabels(labels, fontsize=9)
        ax.grid(axis="y", alpha=0.3)
        ax.legend(fontsize=8)
        vmax = max(von + vof + [1e-9])
        for bars in (b1, b2):
            for bar in bars:
                h = bar.get_height()
                ax.text(bar.get_x() + bar.get_width()/2, h + vmax*0.01,
                        f"{h:.2f}" if h < 100 else f"{h:.0f}",
                        ha="center", va="bottom", fontsize=7)
        ax.set_ylim(0, vmax * 1.18)

    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.text(0.5, 0.005, "credit=1 逐跳链路信用 → 零丢包(无损,all-reduce 正确);"
             "credit=0 有损直发 → 丢包,结果不完整。JCT/busbw 仅对已交付包统计。",
             ha="center", fontsize=8, style="italic")
    fig.savefig(out, dpi=130)
    print(f"\n出图 → {out}")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--on", default="credit_on", help="credit=1 的 cmp_*.csv 目录")
    ap.add_argument("--off", default="credit_off", help="credit=0 的 cmp_*.csv 目录")
    ap.add_argument("--out", default="credit_ablation.png")
    ap.add_argument("--no-plot", action="store_true")
    args = ap.parse_args()

    on, off = load_dir(args.on), load_dir(args.off)
    topos = [t for t in TOPO_ORDER if t in on and t in off]
    if not topos:
        print(f"[err] {args.on} / {args.off} 里没有成对的 cmp_mesh*.csv。"
              f"先跑 batch_test_all.sh 的信用消融段。", file=sys.stderr)
        sys.exit(1)
    print_table(on, off, topos)
    if not args.no_plot:
        make_plot(on, off, topos, args.out)


if __name__ == "__main__":
    main()
