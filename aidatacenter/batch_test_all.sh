#!/usr/bin/env bash
# 批量运行 AI 数据中心所有拓扑的 AllReduce 仿真并生成对比图。
# 支持两种布局(LAYOUT 环境变量):
#   subdir(默认):文件在 scratch/aidatacenter/ 子目录(setup_aidatacenter.sh + CMakeLists),
#                 运行名带 aidatacenter/ 前缀,且因 ns3 子目录自动重建目标名 bug 一律加 --no-build。
#   flat        :文件平铺到 scratch/ 根(build_comparison.sh),运行名裸名、无 --no-build。
set -euo pipefail

# 1. 配置参数 (环境变量可覆盖)
NS3_DIR="${NS3_DIR:-/path/to/ns-3-dev}"  # 你的 ns-3 路径
AR_RANKS="${AR_RANKS:-32}"               # 逻辑 Rank 数
MODEL_MB="${MODEL_MB:-8}"                # 梯度模型大小 (MiB)
LINK_RATE="${LINK_RATE:-400Gbps}"        # 统一线速 400G
CREDIT_ABLATION="${CREDIT_ABLATION:-1}"  # 1=额外跑 mesh 信用消融(无损 vs 有损)
LAYOUT="${LAYOUT:-subdir}"               # subdir | flat
AIDC="$(cd "$(dirname "$0")" && pwd)"    # 本脚本所在目录(自动定位,在哪跑都行)
OUT_DIR="$AIDC/results_batch"

# 按布局确定运行名前缀 PFX 与是否 --no-build(NB)
if [ "$LAYOUT" = "subdir" ]; then PFX="aidatacenter/"; NB="--no-build"; else PFX=""; NB=""; fi

echo "========================================================="
echo " 批量测试 AI 数据中心架构  (LAYOUT=$LAYOUT)"
echo " 参数: arRanks=${AR_RANKS}, modelMB=${MODEL_MB}, linkRate=${LINK_RATE}"
echo "========================================================="

if [ ! -d "$NS3_DIR" ] || [ ! -x "$NS3_DIR/ns3" ]; then
    echo "错误: 未找到 ns-3($NS3_DIR/ns3)。请设 NS3_DIR。"; exit 1
fi

# 运行一个 sim:加前缀 + (子目录布局下)--no-build。cmp_*.csv 会写到 ns-3 根目录。
runsim () { echo " - $1"; ./ns3 run "${PFX}$2" $NB; }

echo ">> [1/4] 编译..."
if [ "$LAYOUT" = "flat" ]; then
    bash "$AIDC/build_comparison.sh" "$NS3_DIR"
elif [ "$AIDC" = "$NS3_DIR/scratch/aidatacenter" ]; then
    echo "  (脚本已在 scratch/aidatacenter,就地 ./ns3 build)"
    ( cd "$NS3_DIR" && ./ns3 build )
else
    bash "$AIDC/setup_aidatacenter.sh" "$NS3_DIR"   # 从外部源目录拷入 + build
fi

echo ">> [2/4] 批量运行 9 种网络拓扑 (统一规模 N≈1296,可能几分钟)..."
cd "$NS3_DIR"
BASE_ARGS="--scenario=nccl_ar --arRanks=${AR_RANKS} --modelMB=${MODEL_MB} --linkRate=${LINK_RATE}"

runsim "Leaf-Spine (RoCEv2+PFC, --K=72 --nLeaf=36 → N=1296)" \
       "leaf-spine-sim ${BASE_ARGS} --K=72 --nLeaf=36"
runsim "Fat-Tree (3-tier RoCEv2, K=16 → N=1024)" \
       "fat_tree_sim ${BASE_ARGS}"
runsim "Spectrum-X (RoCEv2/SHIELD, N=1332)" \
       "spectrum-x-sim ${BASE_ARGS}"
runsim "Rail-IB (多轨并行 IB, N=1332)" \
       "rail-ib-sim ${BASE_ARGS}"
runsim "Dragonfly (IB credit, N=1332)" \
       "dragonfly-ib-sim ${BASE_ARGS}"
runsim "2D-Mesh (A=36 → N=1296, 最重)" \
       "mesh-sim ${BASE_ARGS} --queuePkts=256"
runsim "3D-Mesh (11³=1331)" \
       "mesh3d-sim ${BASE_ARGS} --queuePkts=256"
runsim "4D-Mesh (6⁴=1296)" \
       "mesh4dw6-sim ${BASE_ARGS} --queuePkts=256"
# rng 现已可统一规模:--N=1296 --degree=16(随机正则图,d=16);pattern 而非 scenario。
runsim "RNG (扁平随机正则图, --N=1296 d=16 → 统一规模)" \
       "rng-sim --topology=rng --pattern=nccl_ar --arRanks=${AR_RANKS} --modelMB=${MODEL_MB} --linkRate=${LINK_RATE} --queuePkts=256 --N=1296 --degree=16"

echo ">> [3/4] 收集 CSV 并生成主对比图(9 拓扑)..."
mkdir -p "$OUT_DIR"
mv cmp_*.csv "$OUT_DIR/"
mv energy_unified.csv "$OUT_DIR/" 2>/dev/null || true
python3 "$AIDC/compare_topologies.py" --dir "$OUT_DIR" --out "$OUT_DIR/topo_comparison.png"

# 信用消融:mesh 家族 无损(credit=1) vs 有损(credit=0),同参数各跑一遍 → credit_on/off
if [ "$CREDIT_ABLATION" = "1" ]; then
    echo ">> [4/4] 信用消融:mesh-sim/mesh3d/mesh4dw6 各跑 credit=1/0..."
    mkdir -p "$OUT_DIR/credit_on" "$OUT_DIR/credit_off"
    cd "$NS3_DIR"
    MESH_ARGS="--scenario=nccl_ar --arRanks=${AR_RANKS} --modelMB=${MODEL_MB} --linkRate=${LINK_RATE} --queuePkts=256"
    for mode in 1 0; do
        if [ "$mode" = "1" ]; then sub="credit_on"; else sub="credit_off"; fi
        runsim "credit=$mode mesh-sim"     "mesh-sim     ${MESH_ARGS} --credit=${mode}"
        runsim "credit=$mode mesh3d-sim"   "mesh3d-sim   ${MESH_ARGS} --credit=${mode}"
        runsim "credit=$mode mesh4dw6-sim" "mesh4dw6-sim ${MESH_ARGS} --credit=${mode}"
        mv cmp_mesh2d.csv cmp_mesh3d.csv cmp_mesh4d.csv "$OUT_DIR/$sub/"
    done
    python3 "$AIDC/compare_credit.py" --on "$OUT_DIR/credit_on" \
            --off "$OUT_DIR/credit_off" --out "$OUT_DIR/credit_ablation.png"
fi

echo "========================================================="
echo " 批量测试完成！"
echo " 主对比图 (9 拓扑): $OUT_DIR/topo_comparison.png"
[ "$CREDIT_ABLATION" = "1" ] && echo " 信用消融图 (mesh 无损 vs 有损): $OUT_DIR/credit_ablation.png"
echo " 全部 CSV / 图表在: $OUT_DIR"
echo "========================================================="
