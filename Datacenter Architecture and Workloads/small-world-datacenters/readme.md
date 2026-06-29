======================================================================
  Small-World Datacenters — SOCC'11 复现项目
  论文：Shin, Wong, Sirer, Cheriton — ACM SOCC 2011
======================================================================

一、项目简介
----------------------------------------------------------------------
本项目使用 Python + NS3 复现论文 "Small-World Datacenters" 的核心实验。
论文提出将小世界网络理论应用于数据中心拓扑设计，在保持每节点度数为6
的前提下，通过添加 Kleinberg 随机链接大幅缩短平均路径长度，从而降低
延迟、提升带宽、增强容错性。

复现范围：
  - Fig. 3  Dijkstra 最短路径长度对比
  - Fig. 4  贪心路由路径长度对比
  - Fig. 5  端到端包传输延迟（NS3 仿真）
  - Fig. 6  最大聚合带宽（NS3 仿真）
  - Fig. 11 节点故障容错性分析


二、文件说明
----------------------------------------------------------------------
swdc_topology.py        拓扑构建 + 图算法（Fig. 3、4、11）
swdc-simulation.cc      NS3 C++ 仿真主程序（Fig. 5、6）
CMakeLists.txt          NS3 CMake 编译配置
plot_results.py         读取 CSV，生成所有图表
README.txt              本文件
README.md               Markdown 格式说明文档

results/                实验结果目录（运行后自动生成）
  fig3_dijkstra_path_length.csv
  fig4_greedy_path_length.csv
  fig11_fault_tolerance.csv
  swdc_ns3_results.csv
  swdc_fault_results.csv
  figures/              最终图表（PDF + PNG）


三、拓扑说明
----------------------------------------------------------------------
本项目实现了以下 5 种拓扑，均满足 degree-6 约束（每节点6条链路）：

  SW-Ring        环形 + 4条 Kleinberg 随机链接
  SW-2DTorus     32×16 二维环面 + 2条 Kleinberg 随机链接
  SW-3DHexTorus  8×8×8 三维环面 + 1条 Kleinberg 随机链接
  CamCube        8×8×8 三维环面，无随机链接（基准对比）
  CDC            传统三层树形数据中心（ToR → Agg → Core）

Kleinberg 随机链接的选取概率正比于节点间距离的 -d 次方（d 为拓扑维数），
保证了小世界网络的"可导航性"，使贪心路由接近最优路径。


四、环境依赖
----------------------------------------------------------------------
Python 部分：
  pip install networkx matplotlib numpy

NS3 部分：
  需要 NS3 3.36 或更高版本（CMake 构建系统）
  sudo apt install cmake g++ python3 ninja-build

  克隆 NS3：
    git clone https://gitlab.com/nsnam/ns-3-dev.git ~/ns-3-dev
    cd ~/ns-3-dev
    ./ns3 configure --enable-examples
    ./ns3 build


五、安装步骤
----------------------------------------------------------------------
1. 将仿真文件放入 NS3 scratch 目录：

   mkdir -p ~/ns-3-dev/scratch/small-world-datacenters
   cp swdc-simulation.cc CMakeLists.txt \
      ~/ns-3-dev/scratch/small-world-datacenters/

2. 编译：

   cd ~/ns-3-dev
   ./ns3 build scratch/small-world-datacenters

   编译成功后可见：
   [100%] Linking CXX executable small-world-datacenters/ns3-dev-swdc-simulation-debug


六、运行步骤
----------------------------------------------------------------------
Step 1  Python 图分析（Fig. 3、4、11）约 5-10 分钟

  mkdir -p ~/ns-3-dev/results
  python3 swdc_topology.py ~/ns-3-dev/results


Step 2  NS3 延迟/带宽仿真（Fig. 5、6）约 10-20 小时

  cd ~/ns-3-dev

  nohup bash -c '
  cd ~/ns-3-dev
  RESULTS=~/ns-3-dev/results
  for TOPO in SW-Ring SW-2DTorus SW-3DHexTorus CamCube; do
    for PKT in 64 1024; do
      for TRAFFIC in uniform local mapreduce; do
        NS_LOG="SwdcSimulation=info" ./ns3 run "swdc-simulation \
          --topo=$TOPO --traffic=$TRAFFIC \
          --pktSize=$PKT --nNodes=512 --simTime=1.0 --nPkts=500 \
          --outFile=$RESULTS/swdc_ns3_results.csv"
      done
    done
  done
  ' > ~/ns-3-dev/run.log 2>&1 &

  tail -f ~/ns-3-dev/run.log


Step 3  CDC 三种配置（接 Step 2）

  cd ~/ns-3-dev

  nohup bash -c '
  cd ~/ns-3-dev
  RESULTS=~/ns-3-dev/results
  for CONFIG in "2 5" "1 7" "1 5"; do
    TOR=$(echo $CONFIG | awk "{print \$1}")
    AGG=$(echo $CONFIG | awk "{print \$2}")
    for PKT in 64 1024; do
      for TRAFFIC in uniform local mapreduce; do
        NS_LOG="SwdcSimulation=info" ./ns3 run "swdc-simulation \
          --topo=CDC --traffic=$TRAFFIC \
          --pktSize=$PKT --nNodes=512 --simTime=1.0 --nPkts=500 \
          --torOversub=$TOR --aggOversub=$AGG \
          --outFile=$RESULTS/swdc_ns3_results.csv"
      done
    done
  done
  ' > ~/ns-3-dev/cdc.log 2>&1 &

  tail -f ~/ns-3-dev/cdc.log


Step 4  容错测试（Fig. 11 NS3版）约 8-16 小时

  cd ~/ns-3-dev

  nohup bash -c '
  cd ~/ns-3-dev
  RESULTS=~/ns-3-dev/results
  for TOPO in SW-Ring SW-2DTorus SW-3DHexTorus CamCube; do
    for FRAC in 0.0 0.1 0.2 0.3 0.4 0.5; do
      NS_LOG="SwdcSimulation=info" ./ns3 run "swdc-simulation \
        --topo=$TOPO --traffic=uniform \
        --pktSize=1024 --nNodes=512 --simTime=0.5 --nPkts=200 \
        --faultTest=true --faultFrac=$FRAC \
        --outFile=$RESULTS/swdc_fault_results.csv"
    done
  done
  ' > ~/ns-3-dev/fault.log 2>&1 &

  tail -f ~/ns-3-dev/fault.log


Step 5  画图（所有数据收集完成后）

  python3 plot_results.py ~/ns-3-dev/results
  # 图片在 ~/ns-3-dev/results/figures/ 里


七、快速验证（64节点，约 1-3 分钟）
----------------------------------------------------------------------
如需快速验证代码逻辑是否正确，可用小规模测试：

  NS_LOG="SwdcSimulation=info" ./ns3 run "swdc-simulation \
    --topo=SW-3DHexTorus --traffic=uniform \
    --pktSize=1024 --nNodes=64 --nPkts=100 \
    --simTime=0.5 --outFile=results/test_small.csv"

  正常输出：Delivery ratio: 100%


八、参数说明
----------------------------------------------------------------------
--topo        拓扑类型：SW-Ring / SW-2DTorus / SW-3DHexTorus / CamCube / CDC
--traffic     流量模型：uniform / local / mapreduce
--pktSize     包大小（字节），论文测试 64 和 1024
--nNodes      节点数，SW-3DHexTorus/CamCube 需为完全立方数（如 512=8³）
--simTime     仿真时长（秒），默认 0.5
--nPkts       每个发送端发送的包数，默认 500
--seed        随机种子，默认 42
--faultTest   是否启用容错测试，默认 false
--faultFrac   故障节点比例（0～0.5），默认 0.0
--outFile     CSV 输出文件路径
--torOversub  CDC ToR 超订比（仅 CDC 拓扑使用）
--aggOversub  CDC Agg 超订比（仅 CDC 拓扑使用）


九、耗时估计
----------------------------------------------------------------------
步骤                          耗时
Python 图分析                 5～10 分钟
NS3 SW+CamCube（24组）        10～20 小时
NS3 CDC（18组）               8～15 小时
NS3 容错测试（24组）          8～16 小时
画图                          < 1 分钟

建议使用 nohup 后台运行，过夜完成。


十、已知问题与修复记录
----------------------------------------------------------------------
[已修复] Negative delay 崩溃
  原因：trafficStart=0.05，sink 启动时间 0.05-0.1=-0.05 为负数
  修复：将 trafficStart 改为 0.15

[已修复] Delivery ratio 0%
  原因：buildStaticRouting 中下一跳地址填的是自己的 IP 而非邻居 IP
  修复：将 ifaceAddr[src][cur] 改为 ifaceAddr[cur][src]

[已修复] 容错测试卡死
  原因：故障节点的链路被删除但图结构保留，BFS 经过故障节点导致路由死循环
  修复：BFS 时排除故障节点；流量只在活跃节点之间生成


十一、与论文的符合程度说明
----------------------------------------------------------------------
Fig. 3/4（路径长度）：高度符合
  SW-Ring~3.3, SW-2DTorus~3.9, CamCube~6.0，与论文数值吻合。

Fig. 5（延迟）：排序趋势符合，绝对值不同
  论文使用真实 NetFPGA 硬件（延迟 10-50µs），NS3 仿真因 debug build
  和 burst 流量造成排队，延迟在 1000-80000µs 量级，相差约两个数量级。
  但 SW-Ring < SW-2DTorus < CamCube < CDC 的排序完全一致。

Fig. 6（带宽）：趋势符合
  论文通过增大发送速率找饱和点，我们发固定数量包，SW 拓扑带宽高于
  CDC 的结论一致，但具体曲线形状不同。

Fig. 11（容错）：高度符合
  50% 节点故障时所有 SW 拓扑保持 >96% 连通性，与论文结论一致。


十二、引用
----------------------------------------------------------------------
@inproceedings{shin2011small,
  title     = {Small-World Datacenters},
  author    = {Shin, Jae-Hyun and Wong, Bernard and
               Sirer, Emin G{\"u}n and Cheriton, David R.},
  booktitle = {Proceedings of the 2nd ACM Symposium on Cloud Computing},
  year      = {2011}
}

======================================================================
