#!/usr/bin/env bash
# setup_aidatacenter.sh — CMake 子目录方式:把本套文件装进 ns-3 的
#   scratch/aidatacenter/ 并编译。与 build_comparison.sh(平铺到 scratch/ 根)二选一。
#   子目录方式更干净(自带 CMakeLists、运行名带 aidatacenter/ 前缀、不污染 scratch 根)。
#
#   用法: ./setup_aidatacenter.sh /path/to/ns-3-dev
set -euo pipefail
NS3="${1:-}"
[[ -n "$NS3" && -x "$NS3/ns3" ]] || { echo "用法: $0 /path/to/ns-3-dev"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$NS3/scratch/aidatacenter"

# 9 个对比仿真(ns-3,会被 CMake 各编成 aidatacenter/<name>)
SIMS=(leaf-spine-sim fat_tree_sim spectrum-x-sim rail-ib-sim dragonfly-ib-sim
      mesh-sim mesh3d-sim mesh4dw6-sim rng-sim)
# 共享头(只需在场,CMake 不列;引号 include 从同目录解析)
HDRS=(cmp-common.h nccl-collectives.h energy-model.h pfc-lossless.h
      mesh-route-header.h mesh-credit.h link-credit.h)
# 纯 C++ 自检(CMakeLists 已 exclude,放进去方便在该目录直接 g++)
TESTS=(test_cmp_common.cc cmp_pipeline_check.cc link_credit_test.cc mesh4d_pipeline_check.cc)

mkdir -p "$DEST"
miss=0
for f in "${SIMS[@]}"; do
  if [[ -f "$HERE/$f.cc" ]]; then cp "$HERE/$f.cc" "$DEST/"; else echo "[缺] $f.cc"; miss=1; fi
done
for h in "${HDRS[@]}"; do
  if [[ -f "$HERE/$h" ]]; then cp "$HERE/$h" "$DEST/"; else echo "[缺] $h"; miss=1; fi
done
for t in "${TESTS[@]}"; do [[ -f "$HERE/$t" ]] && cp "$HERE/$t" "$DEST/" || true; done
cp "$HERE/CMakeLists.txt" "$DEST/"
[[ $miss -eq 0 ]] || { echo "有文件缺失(见上),补齐后重跑。"; exit 1; }
echo "已拷入 $DEST  (${#SIMS[@]} sims + ${#HDRS[@]} headers + CMakeLists)"

cd "$NS3"
./ns3 configure >/dev/null 2>&1 || true   # 新增子目录后首次需重配;失败不致命(build 会再配)
./ns3 build

cat <<'EOF'

build OK。子目录方式运行(注意前缀 aidatacenter/,统一规模 N≈1296 为默认):

  ./ns3 run "aidatacenter/leaf-spine-sim   --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --K=72 --nLeaf=36"
  ./ns3 run "aidatacenter/fat_tree_sim     --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "aidatacenter/spectrum-x-sim   --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "aidatacenter/rail-ib-sim      --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "aidatacenter/dragonfly-ib-sim --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
  ./ns3 run "aidatacenter/mesh-sim         --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
  ./ns3 run "aidatacenter/mesh3d-sim       --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
  ./ns3 run "aidatacenter/mesh4dw6-sim     --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
  ./ns3 run "aidatacenter/rng-sim --topology=rng --pattern=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"

先冒烟(小规模):给 mesh 加 --side=3~6、fat_tree --K=4、leaf-spine --K=8 --nLeaf=4 即可。

纯 C++ 自检(进 scratch/aidatacenter 用 g++,CMake 不编它们):
  cd scratch/aidatacenter
  g++ -std=c++14 -O2 link_credit_test.cc     -o /tmp/lct && /tmp/lct
  g++ -std=c++14 -O2 mesh4d_pipeline_check.cc -o /tmp/chk && /tmp/chk .

cmp_*.csv / energy_unified.csv 会写在 ns-3 根目录;出图同前:
  python3 compare_topologies.py --dir /path/to/ns-3-dev
EOF
