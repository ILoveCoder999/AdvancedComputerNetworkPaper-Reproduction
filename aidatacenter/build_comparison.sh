#!/usr/bin/env bash
# build_comparison.sh — 把跨拓扑对比的 3 个 sim + 共享头拷进 ns-3 并编译。
# 用法: ./build_comparison.sh /path/to/ns-3-dev
#
# 做法：放到 ns-3 的 scratch/ 顶层。每个 *-sim.cc 自动成为 `./ns3 run <name>` 目标；
#       引号包含("cmp-common.h")会从 .cc 所在的 scratch 目录解析到同放的头文件，
#       因此无需写 CMakeLists（最稳，跨 ns-3 版本通用）。
set -euo pipefail
NS3="${1:-}"
[[ -z "$NS3" || ! -d "$NS3" ]] && { echo "usage: $0 /path/to/ns-3-dev"; exit 1; }
[[ -x "$NS3/ns3" ]] || { echo "error: $NS3/ns3 not found (是 ns-3 源码树吗?)"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"

SIMS=(leaf-spine-sim mesh-sim mesh3d-sim mesh4dw6-sim dragonfly-ib-sim rail-ib-sim fat_tree_sim spectrum-x-sim rng-sim)
HDRS=(cmp-common.h nccl-collectives.h energy-model.h pfc-lossless.h mesh-route-header.h link-credit.h)

for s in "${SIMS[@]}"; do cp "$HERE/$s.cc" "$NS3/scratch/"; done
for h in "${HDRS[@]}"; do cp "$HERE/$h"   "$NS3/scratch/"; done
echo "copied ${#SIMS[@]} sims + ${#HDRS[@]} headers -> $NS3/scratch/"

cd "$NS3"
./ns3 build "${SIMS[@]}"

cat <<'EOF'

build OK. 跑同一套 all-reduce(九者用相同 arRanks / modelMB / linkRate → apples-to-apples):

  ./ns3 run "leaf-spine-sim   --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "fat_tree_sim     --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "spectrum-x-sim   --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "rail-ib-sim      --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "dragonfly-ib-sim --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "mesh-sim         --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "mesh3d-sim       --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
  ./ns3 run "mesh4dw6-sim     --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
  ./ns3 run "rng-sim          --topology=rng --pattern=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"

每个 sim 写出 cmp_<topo>.csv(到 ns-3 根目录)。然后出对比图:

  cp cmp_*.csv /path/to/aidatacenter/
  cd /path/to/aidatacenter && python3 compare_topologies.py
  #  → 打印 8 拓扑对比表 + 出 topo_comparison.png

注意:
  · fat_tree_sim 默认 K=8(N=128);rng-sim 默认 N=100;mesh3d 11³=1331;mesh4dw6 6⁴=1296。其余 N=289~1332。
    busbw 效率 / pJ-per-bit 规模稳健,可横比;JCT/busbw 绝对值受规模与线速影响。
  · mesh3d / mesh4dw6 默认队列 8p(为 relaycongest 突发实验设),对比时加 --queuePkts=256。
  · mesh 家族(mesh-sim/mesh3d/mesh4dw6)默认开 **逐跳链路信用**(--credit=1, 无损,
    dropped 应=0;信用窗=--creditPkts,默认=queuePkts)。要有损对照跑 --credit=0。
  · rng-sim 的 fig13 复现仍用 --pattern=clique/hubs/matching(见该文件头注释),互不影响。
详见 RUN_COMPARISON.md。
EOF
