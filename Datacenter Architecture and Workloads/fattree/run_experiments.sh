#!/usr/bin/env bash
# =============================================================================
# run_experiments.sh
#   复现论文 "A Scalable, Commodity Data Center Network Architecture" Table 2,
#   四种方法并排对比:
#     Tree(传统过载树) / Two-Level / Flow Classification / Flow Scheduling
#
# 用法 (在 ns-3 根目录下执行):
#   bash scratch/fattree/run_experiments.sh
#
# 可用环境变量:
#   K=4          胖树端口数 (偶数; sameid 最坏情况仅支持 k=4)
#   SIMTIME=10   每次仿真时长(秒)；想更接近论文可设 60
#   RUNS=3       Random 测试取平均的随机种子个数
#
# 注意: 一共要跑很多次仿真(约 6 行 × 4 方法, Random 还要 ×RUNS),
#       debug 版可能要十几到几十分钟。想快可减小 SIMTIME / RUNS,
#       或先 ./ns3 configure -d optimized 编优化版。
# =============================================================================
set -u

K=${K:-4}
SIMTIME=${SIMTIME:-10}
RUNS=${RUNS:-3}
TRANSPORT=${TRANSPORT:-tcp}   # 论文用 TCP; 想用 UDP 跑设 TRANSPORT=udp

if [ ! -x ./ns3 ]; then
  echo "错误: 请在 ns-3 根目录(含 ./ns3 脚本)下运行本脚本。" >&2
  exit 1
fi

# 跑一次仿真, 抽取 RESULT 行最后一列(百分比)
#   $1 = 可执行名(fattree | fattree-tree)
#   $2 = 额外参数(如 --routing=schedule, 树基线传 "")
#   $3 = 显示名(用于判断 Random 是否取平均)
#   其余 = 传给仿真的模式参数
one_run () {
  local exe="$1" extra="$2" name="$3"; shift 3
  local args="$*"
  if [ "$name" = "Random" ]; then
    local tot=0 n=0 p s
    for s in $(seq 1 "$RUNS"); do
      p=$(./ns3 run "$exe $extra --transport=$TRANSPORT --seed=$s $args --k=$K --simTime=$SIMTIME" \
            2>/dev/null | awk -F, '/^RESULT,/{print $NF}')
      [ -n "$p" ] && tot=$(awk -v a="$tot" -v b="$p" 'BEGIN{print a+b}')
      n=$((n + 1))
    done
    awk -v t="$tot" -v n="$n" 'BEGIN{printf "%.1f", t/n}'
  else
    ./ns3 run "$exe $extra --transport=$TRANSPORT $args --k=$K --simTime=$SIMTIME" 2>/dev/null \
      | awk -F, '/^RESULT,/{print $NF}'
  fi
}

# 用法: run_row 显示名 "论文Tree/TL/CL/SC" <仿真模式参数...>
run_row () {
  local name="$1" paper="$2"; shift 2
  local tr tl cl sc
  tr=$(one_run fattree-tree ""                  "$name" "$@")
  tl=$(one_run fattree      "--routing=twolevel" "$name" "$@")
  cl=$(one_run fattree      "--routing=classify" "$name" "$@")
  sc=$(one_run fattree      "--routing=schedule" "$name" "$@")
  printf "%-11s %9s %9s %9s %9s   %s\n" \
    "$name" "${tr:-FAIL}" "${tl:-FAIL}" "${cl:-FAIL}" "${sc:-FAIL}" "$paper"
}

echo "k=$K, transport=$TRANSPORT, simTime=${SIMTIME}s, Random 平均次数=${RUNS}"
echo "-------------------------------------------------------------------------------"
printf "%-11s %9s %9s %9s %9s   %s\n" \
  "Pattern" "Tree" "TwoLevel" "Classify" "Schedule" "论文 Tree/TL/CL/SC"
echo "-------------------------------------------------------------------------------"

run_row "Stride(1)" "100/100/100/100"   --pattern=stride --stride=1
run_row "Stride(2)" "78.1/100/100/99.5" --pattern=stride --stride=2
run_row "Stride(4)" "27.9/100/100/100"  --pattern=stride --stride=4
run_row "Stride(8)" "28.0/100/100/99.9" --pattern=stride --stride=8
run_row "Random"    "53.4/75/76.3/93.5" --pattern=random
run_row "Same-ID"   "27.8/38.5/75.4/87.4" --pattern=sameid

echo "-------------------------------------------------------------------------------"
echo "说明:"
echo " - 数值=达到理想双边带宽(k=4 时 1.536Gbps)的百分比, 越高越好。"
echo " - 趋势应为: 跨 Pod 模式下 Tree 最差(~28%); 胖树三法里"
echo "   Schedule >= Classify >= TwoLevel, 最坏情况(Same-ID)差距最明显。"
echo " - 仿真器/UDP 与论文(Click/TCP)不同, 绝对值会有出入, 看趋势与大小关系。"
