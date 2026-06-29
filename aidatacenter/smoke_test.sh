#!/usr/bin/env bash
# smoke_test.sh — 极小规模端到端冒烟,几分钟跑完,确认"编译 + nccl_ar + 逐跳信用 +
#   能耗/cmp 输出 + 两张对比图"整条管线通,再去跑 batch_test_all.sh 真实规模。
#
#   用法:  NS3_DIR=/path/to/ns-3-dev ./smoke_test.sh
#   LAYOUT=subdir(默认):aidatacenter/ 子目录 + --no-build;LAYOUT=flat 用平铺。
#   9 个 sim 规模全部运行时可调,默认即小规模(几十~百节点),一次全跑完。
set -uo pipefail

NS3_DIR="${NS3_DIR:-/path/to/ns-3-dev}"
SMOKE_FULL="${SMOKE_FULL:-0}"
LAYOUT="${LAYOUT:-subdir}"
AIDC="$(cd "$(dirname "$0")" && pwd)"   # 本脚本所在目录(自动定位,在哪跑都行)
OUT="$AIDC/smoke_out"
AR=8; MB=1; LR="400Gbps"; Q=64        # 极小负载:8 rank、1 MiB
PASS=0; FAIL=0; FAILED=""

if [ "$LAYOUT" = "subdir" ]; then PFX="aidatacenter/"; NB="--no-build"; else PFX=""; NB=""; fi

if [ ! -x "$NS3_DIR/ns3" ]; then echo "错误:未找到 $NS3_DIR/ns3,请设 NS3_DIR"; exit 1; fi

echo "========================================================="
echo " 冒烟测试(极小规模, LAYOUT=$LAYOUT):arRanks=$AR modelMB=$MB linkRate=$LR"
echo "========================================================="
echo ">> [1/4] 编译..."
if [ "$LAYOUT" = "flat" ]; then
    bash "$AIDC/build_comparison.sh" "$NS3_DIR"
elif [ "$AIDC" = "$NS3_DIR/scratch/aidatacenter" ]; then
    echo "  (脚本已在 scratch/aidatacenter,就地 ./ns3 build)"
    ( cd "$NS3_DIR" && ./ns3 build )
else
    bash "$AIDC/setup_aidatacenter.sh" "$NS3_DIR"
fi

rm -rf "$OUT"; mkdir -p "$OUT/credit_on" "$OUT/credit_off"
cd "$NS3_DIR"

# 校验某个 cmp_<topo>.csv:存在?并打印 sent/delivered/dropped;lossless=1 时要求 dropped==0
check_csv () { # $1=csv路径  $2=标签  $3=需无损(1/0)
  python3 - "$1" "$2" "${3:-0}" <<'PY'
import csv,sys
path,tag,lossless=sys.argv[1],sys.argv[2],sys.argv[3]
try:
    r=list(csv.DictReader(open(path)))[-1]
except Exception as e:
    print(f"  [FAIL] {tag}: 没产出 {path} ({e})"); sys.exit(2)
sent,deliv,drop=int(float(r['sent'])),int(float(r['delivered'])),int(float(r['dropped']))
ok = (deliv>0) and (lossless!='1' or drop==0)
print(f"  [{'PASS' if ok else 'FAIL'}] {tag}: sent={sent} delivered={deliv} dropped={drop}"
      + ("  (信用无损,应=0)" if lossless=='1' else ""))
sys.exit(0 if ok else 2)
PY
}
run () { # $1=标签 $2="progname args"(不含前缀) $3=产出csv $4=收集目录 $5=需无损
  echo " - $1"
  local log="$4/${1// /_}.log"
  if ! ./ns3 run "${PFX}$2" $NB >"$log" 2>&1; then
    echo "  [FAIL] $1: ns3 run 出错 ↓(完整见 $log)"
    tail -n 8 "$log" | sed 's/^/      /'
    FAIL=$((FAIL+1)); FAILED="$FAILED $1"; return
  fi
  mv -f "$3" "$4/" 2>/dev/null
  if check_csv "$4/$(basename "$3")" "$1" "$5"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); FAILED="$FAILED $1"; fi
}

echo ">> [2/4] 跑小规模(信用开,无损)..."
run "leaf_spine N=16" "leaf-spine-sim --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --K=8 --nLeaf=4"                       cmp_leaf_spine.csv "$OUT" 0
run "fat_tree   N=16" "fat_tree_sim   --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --K=4"                                  cmp_fat_tree.csv  "$OUT" 0
run "mesh2d     N=36" "mesh-sim       --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --side=6 --queuePkts=$Q --credit=1"     cmp_mesh2d.csv "$OUT" 1
run "mesh3d     N=64" "mesh3d-sim     --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --side=4 --queuePkts=$Q --credit=1"     cmp_mesh3d.csv "$OUT" 1
run "mesh4d     N=81" "mesh4dw6-sim   --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --side=3 --queuePkts=$Q --credit=1"     cmp_mesh4d.csv "$OUT" 1
run "rng        N=64" "rng-sim --topology=rng --pattern=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --queuePkts=$Q --N=64 --degree=8"  cmp_rng.csv       "$OUT" 0
run "spectrum_x N=64" "spectrum-x-sim   --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --N=64"          cmp_spectrum_x.csv "$OUT" 0
run "rail       N=32" "rail-ib-sim      --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --nPerRail=8"   cmp_rail.csv       "$OUT" 0
run "dragonfly  N=20" "dragonfly-ib-sim --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --a=2 --p=2"    cmp_dragonfly.csv  "$OUT" 0
# 注:9 个 sim 现在全部规模运行时可调,默认即小规模冒烟;不再需要 SMOKE_FULL。

echo ">> [3/4] 信用消融(mesh 各跑 credit=1/0)..."
cp -f "$OUT/cmp_mesh2d.csv" "$OUT/cmp_mesh3d.csv" "$OUT/cmp_mesh4d.csv" "$OUT/credit_on/" 2>/dev/null
for sim_side in "mesh-sim 6" "mesh3d-sim 4" "mesh4dw6-sim 3"; do
  set -- $sim_side
  ./ns3 run "${PFX}$1 --scenario=nccl_ar --arRanks=$AR --modelMB=$MB --linkRate=$LR --side=$2 --queuePkts=$Q --credit=0" $NB >/dev/null 2>&1 || true
done
mv -f cmp_mesh2d.csv cmp_mesh3d.csv cmp_mesh4d.csv "$OUT/credit_off/" 2>/dev/null

echo ">> [4/4] 出图(验证两个绘图脚本)..."
python3 "$AIDC/compare_topologies.py" --dir "$OUT" --out "$OUT/topo_comparison.png" || echo "  [warn] 主对比图失败"
python3 "$AIDC/compare_credit.py" --on "$OUT/credit_on" --off "$OUT/credit_off" --out "$OUT/credit_ablation.png" || echo "  [warn] 消融图失败"

echo "========================================================="
echo " 冒烟结果:PASS=$PASS  FAIL=$FAIL"
[ -n "$FAILED" ] && echo " 失败项:$FAILED"
echo " 输出在:$OUT(含 cmp_*.csv / topo_comparison.png / credit_ablation.png)"
[ "$FAIL" -eq 0 ] && echo " ✅ 管线通,可以去跑 ./batch_test_all.sh(真实规模)" \
                  || echo " ❌ 有失败项,先按上面 [FAIL] 排查再放大规模"
echo "========================================================="
[ "$FAIL" -eq 0 ]
