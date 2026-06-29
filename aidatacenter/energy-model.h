/*
 * energy-model.h — 统一数据中心网络功耗 / 能耗模型（所有拓扑共用）
 * ============================================================================
 * 目标：给 fat-tree / leaf-spine / spectrum-x / rail-ib / dragonfly /
 *       mesh2d / mesh3d / mesh4d 以及"无交换机直连"架构提供 **同一套** 功耗参数
 *       与同一个计算公式，从而让吞吐 vs 能耗对比是 apples-to-apples 的。
 *
 * 设计原则（来自顶会实测结论，见文末引用）：
 *  1) 网络器件 **不是** 能量正比的（non-energy-proportional）。交换机空载功耗
 *     约为满载的 ~70%（idle:active ≈ 3:1），光模块/激光器基本恒定常开。
 *     => 模型以"静态功耗 × 时间"为主，"动态 per-bit"为辅。
 *  2) 把功耗拆到可计数的物理单元：
 *        设备基座（switch 机箱 / NIC / FPGA） + 每端口 SerDes/PHY +
 *        每端口介质（光模块 or DAC铜） + 交换 ASIC 每端口 + 动态 per-bit。
 *     每个拓扑只需要把自己的"清单(inventory)"填进来即可，公式完全一致。
 *  3) 无交换机直连架构的关键公平点：省掉了 switch 机箱/ASIC 功耗，但
 *     **FPGA 网卡承担了转发**，且直连链路数暴增 → 端口/光模块/FPGA 功耗上升。
 *     本模型把这部分如实计入 nFpga 与端口数,不让"去交换机"白占便宜。
 *
 * 用法（在任意 *-sim.cc 的 main() 末尾，统计完吞吐后加 ~6 行）：
 *     #include "energy-model.h"
 *     ...
 *     EnergyModel em;                       // 默认参数(可 CommandLine 覆盖)
 *     em.inv = EnergyInventory::FatTree(N, nLeaf, nSpine, nCore, ...);  // 或手填
 *     em.SetBits(g_total_bits);             // 全网链路发送总比特(MacTx 累计)
 *     em.SetDuration(rxDurSec);             // 稳态收包时长(秒)
 *     em.SetTraffic(delivered, pktBytes);   // 用于"每交付比特能耗"指标
 *     em.WriteCsv("energy_unified.csv", "fat_tree", scenario);
 *     em.PrintSummary("fat_tree", scenario);
 *
 * 纯 C++（不依赖 ns-3），可单独编译做单元测试。
 * ============================================================================
 */
#ifndef DCN_ENERGY_MODEL_H
#define DCN_ENERGY_MODEL_H

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

// ───────────────────────── 功耗参数表（唯一真值来源） ─────────────────────────
// 所有拓扑共用这一份。改这里 = 改全部对比。每个数字都给了出处与可调区间。
struct EnergyParams
{
  // —— 参考速率：SerDes / ASIC / NIC 的功耗大致随聚合端口速率线性 —— //
  double refRateGbps   = 400.0;   // 参数标定基准速率

  // —— 设备基座功耗（常开，单位 W）—— //
  // 交换机机箱固定开销：风扇 + 控制 CPU + PSU 损耗 + ASIC 静态漏电。
  // ElasticTree(NSDI'10) 实测：交换机空载≈满载70%，主体是这块固定开销。
  double switchChassisW   = 250.0;  // 每台交换机/路由器固定开销 [150~400]
  // 交换 ASIC 每 400G 端口的报文处理硅功耗（含片上交换/缓冲/查表）。
  // 由 51.2T 级 ASIC(~500W)/128 端口 ≈ 4W/400G 端口反推。
  double asicPerPortW     = 4.0;    // 仅交换机侧端口计 [2~6]
  // 主机 NIC/HCA 基座（ConnectX-7 / 400G RoCE 或 IB NDR HCA）。
  double nicBaseW         = 20.0;   // 每块 NIC/HCA [15~25]；BlueField-3 DPU≈75~150
  // FPGA 网卡基座（无交换机架构中承担转发，Alveo/U系列量级）。
  double fpgaBaseW        = 50.0;   // 每块 FPGA [40~75]

  // —— 每端口（每条链路两端各算一个端口）—— //
  // SerDes / PHY 功耗。Abts(ISCA'10)：SerDes 占 ASIC 功耗比重持续上升(>30%)。
  double serdesPerPortW   = 1.5;    // 每端口 @refRate [1.0~2.5]

  // —— 介质：光模块 vs DAC 铜缆（每端口一个）—— //
  double xcvr400gW        = 10.0;   // 400G 光模块 [8~12]
  double xcvr800gW        = 16.0;   // 800G 光模块 [14~20]
  double dacW             = 0.3;    // 无源 DAC≈0；有源 AEC≈0.5~1.5 [0~1.5]

  // —— 动态项：随实际转发比特计（交换/SerDes 翻转的活动功耗）—— //
  // 网络非能量正比 → 动态占比小；前面板可插光最高~20pJ/bit，交换活动取保守值。
  double dynamicPjPerBit  = 3.0;    // 每"全网链路发送比特"的动态能耗 [1~20]

  // 速率换算：把 400G 光模块功耗外推到任意速率（线性近似）
  double XcvrW (double rateGbps) const
  {
    if (rateGbps <= 400.0) return xcvr400gW * (rateGbps / 400.0);
    if (rateGbps <= 800.0) return xcvr400gW + (xcvr800gW - xcvr400gW) * ((rateGbps - 400.0) / 400.0);
    return xcvr800gW * (rateGbps / 800.0);
  }
  double RateScale (double rateGbps) const { return rateGbps / refRateGbps; }
};

// ───────────────────────── 拓扑清单（每个拓扑填这个） ─────────────────────────
// 约定：一条链路有两个"端口(port)"=两个端点。SerDes 与 介质 按端口计；
// ASIC 每端口仅算交换机侧端口；设备基座按设备数计。
struct EnergyInventory
{
  uint64_t nSwitch       = 0;   // 交换机/路由器台数（无交换机架构=0）
  uint64_t nSwitchPorts  = 0;   // 交换机侧端口总数（=交换机端所有链路端点）
  uint64_t nHostPorts    = 0;   // 主机侧端口总数（=所有 NIC/FPGA 链路端点）
  uint64_t nNic          = 0;   // 主机 NIC/HCA 总数
  uint64_t nFpga         = 0;   // FPGA 网卡总数（传统架构=0；无交换机架构>0）
  uint64_t nOpticalPorts = 0;   // 全部端口中走光模块的数量（其余按 DAC 计）
  double   portRateGbps  = 400.0;
  uint64_t nGpu          = 0;   // 用于"每 GPU 能耗"归一化（=GPU 数）

  // —— 公平对比(apples-to-apples)字段 —— //
  // 网络对分带宽(provisioned bisection bandwidth, Gbps)。由各拓扑构造器按
  // 标准公式算出，体现"这套网到底配了多少跨网容量"。可手动覆盖。
  double   bisectionGbps = 0.0;

  uint64_t TotalPorts () const { return nSwitchPorts + nHostPorts; }
  // 线缆条数 = 端口数/2（每条链路两端口）
  double   NCables ()    const { return TotalPorts () / 2.0; }

  // —— 便捷构造器：常见拓扑一行生成清单 —— //
  // 三层胖树：server-ToR(L1=N) / ToR-Aggr(L2) / Aggr-Core(L3)，全光。
  static EnergyInventory FatTree (uint64_t N, uint64_t nTor, uint64_t nAggr,
                                  uint64_t nCore, uint64_t L2, uint64_t L3,
                                  double rate = 400.0, uint64_t gpuPerSrv = 1)
  {
    EnergyInventory v;
    uint64_t L1 = N;                       // server↔ToR
    v.nSwitch      = nTor + nAggr + nCore;
    v.nHostPorts   = L1;                   // 每服务器一个 NIC 端口
    v.nSwitchPorts = L1 + 2*L2 + 2*L3;     // ToR侧L1 + (ToR/Aggr)各端 + (Aggr/Core)各端
    v.nNic         = N;
    v.nFpga        = 0;
    v.nOpticalPorts= v.TotalPorts();       // 默认全光；机架内可改 DAC
    v.portRateGbps = rate;
    v.nGpu         = N * gpuPerSrv;
    v.bisectionGbps= N * rate / 2.0;       // 非阻塞 3 层 Clos：满对分
    return v;
  }
  // 两层 Leaf-Spine：server-Leaf(N) / Leaf-Spine(L2)。
  static EnergyInventory LeafSpine (uint64_t N, uint64_t nLeaf, uint64_t nSpine,
                                    uint64_t L2, double rate = 400.0,
                                    uint64_t gpuPerSrv = 1)
  {
    EnergyInventory v;
    uint64_t L1 = N;
    v.nSwitch      = nLeaf + nSpine;
    v.nHostPorts   = L1;
    v.nSwitchPorts = L1 + 2*L2;
    v.nNic         = N;
    v.nOpticalPorts= v.TotalPorts();
    v.portRateGbps = rate;
    v.nGpu         = N * gpuPerSrv;
    v.bisectionGbps= L2 * rate / 2.0;      // 对分受 spine 链路数限：L2/2 条跨网
    return v;
  }
  // Rail-Optimized：R 条独立 Leaf-Spine，每服务器 R 块 HCA。
  // 传入单条 rail 的 (N_rail, nLeaf, nSpine, L2_rail)。
  static EnergyInventory Rail (uint64_t R, uint64_t N_rail, uint64_t nLeaf,
                               uint64_t nSpine, uint64_t L2_rail,
                               double rate = 400.0)
  {
    EnergyInventory v;
    uint64_t L1 = N_rail;
    v.nSwitch      = R * (nLeaf + nSpine);
    v.nHostPorts   = R * L1;               // 每 rail 每端点一个 HCA 端口
    v.nSwitchPorts = R * (L1 + 2*L2_rail);
    v.nNic         = R * N_rail;           // R 块 HCA × 端点数
    v.nOpticalPorts= v.TotalPorts();
    v.portRateGbps = rate;
    v.nGpu         = R * N_rail;           // 每服务器 R 个 GPU(各占一条 rail) → 总 GPU
    v.bisectionGbps= R * (L2_rail * rate / 2.0); // R 条 rail 各自对分之和
    return v;
  }
  // Dragonfly：server-router(L1=N) + intra-group(Lintra) + inter-group/global(Lglobal)。
  // global 链路是长距，默认走光；intra/host 可选 DAC。
  static EnergyInventory Dragonfly (uint64_t N, uint64_t nRouter, uint64_t Lintra,
                                    uint64_t Lglobal, double rate = 400.0,
                                    uint64_t gpuPerSrv = 1)
  {
    EnergyInventory v;
    uint64_t L1 = N;
    v.nSwitch      = nRouter;
    v.nHostPorts   = L1;
    v.nSwitchPorts = L1 + 2*Lintra + 2*Lglobal;
    v.nNic         = N;
    v.nOpticalPorts= v.TotalPorts();       // 保守全光；可把 host/intra 改 DAC
    v.portRateGbps = rate;
    v.nGpu         = N * gpuPerSrv;
    v.bisectionGbps= Lglobal * rate / 2.0; // 受全局链路数限（dragonfly 对分估计）
    return v;
  }
  // 无交换机直连（mesh/torus/直连）：只有主机-主机链路。
  //   nNodes      = 节点数（=GPU 数，或服务器数，自行定义）
  //   nLinks      = 直连链路总条数（无向，每条 2 个端口）
  //   fpgaPerNode = 每节点 FPGA 网卡数（你的方案：每 GPU 4 块）
  //   opticalFrac = 走光的端口占比（短直连可用 DAC → 设 <1）
  //   bisectionLinks = 横切对分面的链路条数（决定对分带宽）。
  //     d 维 torus 边长 k：bisectionLinks = 2·k^(d-1)（双向环绕）；mesh = k^(d-1)。
  //     3D torus(11^3)→2·121=242；不确定就传 0，事后手填 inv.bisectionGbps。
  static EnergyInventory Switchless (uint64_t nNodes, uint64_t nLinks,
                                     uint64_t fpgaPerNode, double rate = 400.0,
                                     double opticalFrac = 1.0,
                                     uint64_t bisectionLinks = 0)
  {
    EnergyInventory v;
    v.nSwitch      = 0;
    v.nSwitchPorts = 0;
    v.nHostPorts   = 2 * nLinks;           // 直连链路两端都是主机(FPGA)端口
    v.nNic         = 0;                    // NIC 功能并入 FPGA
    v.nFpga        = nNodes * fpgaPerNode;
    v.nOpticalPorts= (uint64_t)(opticalFrac * v.nHostPorts);
    v.portRateGbps = rate;
    v.nGpu         = nNodes;
    v.bisectionGbps= bisectionLinks * rate; // 横切链路条数 × 单链路速率
    return v;
  }
};

// ───────────────────────────── 计算引擎 ─────────────────────────────
class EnergyModel
{
public:
  EnergyParams    p;
  EnergyInventory inv;

  void SetBits (uint64_t totalLinkBits) { m_bits = totalLinkBits; }
  void SetDuration (double seconds)     { m_durSec = seconds; }
  void SetTraffic (uint64_t deliveredPkts, uint32_t pktBytes)
  { m_deliveredBits = (double) deliveredPkts * pktBytes * 8.0; }
  // 实测稳态吞吐(Gbps)。各 sim 已算出(如 tputGbps)，传进来做归一化对比。
  void SetThroughput (double gbps) { m_tputGbps = gbps; }

  // —— 静态功耗 (W) —— 常开器件（占主体，体现网络非能量正比）
  double StaticPowerW () const
  {
    double rs = p.RateScale (inv.portRateGbps);
    uint64_t total   = inv.TotalPorts ();
    uint64_t optical = inv.nOpticalPorts;
    uint64_t dac     = (total > optical) ? (total - optical) : 0;

    double pSwitch  = inv.nSwitch       * p.switchChassisW
                    + inv.nSwitchPorts  * p.asicPerPortW * rs;
    double pSerdes  = total             * p.serdesPerPortW * rs;
    double pOptics  = optical           * p.XcvrW (inv.portRateGbps);
    double pDac     = dac               * p.dacW;
    double pNic     = inv.nNic          * p.nicBaseW * rs;
    double pFpga    = inv.nFpga         * p.fpgaBaseW;
    return pSwitch + pSerdes + pOptics + pDac + pNic + pFpga;
  }

  // —— 动态能耗 (J) —— 随实际转发比特
  double DynamicEnergyJ () const { return m_bits * p.dynamicPjPerBit * 1e-12; }
  double StaticEnergyJ  () const { return StaticPowerW () * m_durSec; }
  double TotalEnergyJ   () const { return StaticEnergyJ () + DynamicEnergyJ (); }
  double AvgPowerW      () const { return m_durSec > 0 ? TotalEnergyJ () / m_durSec : StaticPowerW (); }

  // —— 关键对比指标 —— //
  // 每"交付比特"的网络能耗 (pJ/bit) —— 网络能效，越低越好
  double EnergyPerDeliveredBitPj () const
  { return m_deliveredBits > 0 ? TotalEnergyJ () / m_deliveredBits * 1e12 : 0.0; }
  // 每 GPU 平均网络功耗 (W)
  double PowerPerGpuW () const { return inv.nGpu ? AvgPowerW () / inv.nGpu : 0.0; }
  double StaticFraction () const
  { double t = TotalEnergyJ (); return t > 0 ? StaticEnergyJ () / t : 1.0; }

  // —— 公平对比(apples-to-apples)指标 —— //
  // 每 GPU 配了多少：对分带宽 / 端口 / 线缆 / 光模块。把"堆 NIC"显形。
  double BisectionPerGpuGbps () const { return inv.nGpu ? inv.bisectionGbps / inv.nGpu : 0.0; }
  double PortsPerGpu ()        const { return inv.nGpu ? (double) inv.TotalPorts () / inv.nGpu : 0.0; }
  double CablesPerGpu ()       const { return inv.nGpu ? inv.NCables () / inv.nGpu : 0.0; }
  double XcvrPerGpu ()         const { return inv.nGpu ? (double) inv.nOpticalPorts / inv.nGpu : 0.0; }
  // 配单位对分带宽要花多少瓦：横向比"性价比"的关键
  double WattsPerBisectionGbps () const
  { return inv.bisectionGbps > 0 ? AvgPowerW () / inv.bisectionGbps : 0.0; }
  // 实测吞吐 ÷ 配置对分带宽 = 对分利用率（>1 说明流量局部性好/未走满对分）
  double BisectionUtil () const
  { return inv.bisectionGbps > 0 ? m_tputGbps / inv.bisectionGbps : 0.0; }
  // 每瓦吞吐 (Gbps/W) —— 真正的能效，越高越好
  double TputPerWatt () const { return AvgPowerW () > 0 ? m_tputGbps / AvgPowerW () : 0.0; }

  void PrintSummary (const std::string &topo, const std::string &scen) const
  {
    std::printf ("\n──────── ENERGY [%s / %s] ────────\n", topo.c_str (), scen.c_str ());
    std::printf ("  Inventory : switch=%llu swPorts=%llu hostPorts=%llu nic=%llu fpga=%llu optical=%llu gpu=%llu @%.0fG\n",
                 (unsigned long long) inv.nSwitch, (unsigned long long) inv.nSwitchPorts,
                 (unsigned long long) inv.nHostPorts, (unsigned long long) inv.nNic,
                 (unsigned long long) inv.nFpga, (unsigned long long) inv.nOpticalPorts,
                 (unsigned long long) inv.nGpu, inv.portRateGbps);
    std::printf ("  Static P  : %.1f W   (switch+optics+serdes+nic+fpga, always-on)\n", StaticPowerW ());
    std::printf ("  Dyn  E    : %.3f J   over %.4f s\n", DynamicEnergyJ (), m_durSec);
    std::printf ("  Avg  P    : %.1f W   |  per-GPU %.1f W\n", AvgPowerW (), PowerPerGpuW ());
    std::printf ("  Total E   : %.3f J   (static %.0f%%)\n", TotalEnergyJ (), 100.0 * StaticFraction ());
    std::printf ("  Net eff   : %.2f pJ/bit delivered\n", EnergyPerDeliveredBitPj ());
    std::printf ("  — FAIRNESS (apples-to-apples) —\n");
    std::printf ("    Bisection : %.0f Gbps total | %.1f Gbps/GPU\n",
                 inv.bisectionGbps, BisectionPerGpuGbps ());
    std::printf ("    Per GPU   : ports=%.1f cables=%.1f xcvr=%.1f\n",
                 PortsPerGpu (), CablesPerGpu (), XcvrPerGpu ());
    std::printf ("    Cost/BW   : %.3f W per Gbps-bisection   <-- 性价比横向对比\n",
                 WattsPerBisectionGbps ());
    if (m_tputGbps > 0)
      std::printf ("    Achieved  : tput=%.0f Gbps | bisecUtil=%.2f | %.4f Gbps/W\n",
                   m_tputGbps, BisectionUtil (), TputPerWatt ());
    std::printf ("──────────────────────────────────────\n");
  }

  // 追加写入统一 CSV，所有拓扑写同一个文件，方便一张表/一张图对比
  void WriteCsv (const std::string &path, const std::string &topo,
                 const std::string &scen) const
  {
    bool exists = false;
    { std::ifstream f (path); exists = f.good (); }
    std::ofstream csv (path, std::ios::app);
    if (!exists)
      csv << "topology,scenario,nSwitch,nSwitchPorts,nHostPorts,nNic,nFpga,"
             "nOpticalPorts,nGpu,rateGbps,durSec,linkBits,deliveredBits,"
             "staticW,avgW,totalJ,staticFrac,perGpuW,pJ_per_delivered_bit,"
             "bisectionGbps,bisecPerGpuGbps,portsPerGpu,cablesPerGpu,xcvrPerGpu,"
             "wPerBisecGbps,tputGbps,bisecUtil,gbpsPerW\n";
    csv << topo << "," << scen << ","
        << inv.nSwitch << "," << inv.nSwitchPorts << "," << inv.nHostPorts << ","
        << inv.nNic << "," << inv.nFpga << "," << inv.nOpticalPorts << ","
        << inv.nGpu << "," << inv.portRateGbps << "," << m_durSec << ","
        << m_bits << "," << (uint64_t) m_deliveredBits << ","
        << StaticPowerW () << "," << AvgPowerW () << "," << TotalEnergyJ () << ","
        << StaticFraction () << "," << PowerPerGpuW () << ","
        << EnergyPerDeliveredBitPj () << ","
        << inv.bisectionGbps << "," << BisectionPerGpuGbps () << ","
        << PortsPerGpu () << "," << CablesPerGpu () << "," << XcvrPerGpu () << ","
        << WattsPerBisectionGbps () << "," << m_tputGbps << ","
        << BisectionUtil () << "," << TputPerWatt () << "\n";
  }

private:
  uint64_t m_bits {0};
  double   m_durSec {0.0};
  double   m_deliveredBits {0.0};
  double   m_tputGbps {0.0};
};

/*
 * 参数出处（顶会 / 厂商实测）
 * ───────────────────────────────────────────────────────────────────────────
 * [1] Abts et al., "Energy Proportional Datacenter Networks", ISCA 2010.
 *     —— 网络非能量正比；SerDes 占交换 ASIC 功耗比重持续上升(>30%)。
 * [2] Heller et al., "ElasticTree: Saving Energy in Data Center Networks",
 *     NSDI 2010. —— 交换机空载≈满载70%，idle:active≈3:1（=> 静态主导）。
 * [3] 400G/800G 可插光模块功耗：400G≈8~12W，800G≈14~20W；无源 DAC≈0W
 *     （厂商/行业数据，OSFP/QSFP-DD）。CPO 可降到<5pJ/bit（未来）。
 * [4] NVIDIA Spectrum-4 = 51.2Tbps / 128×400G；较上代功耗-40%（厂商）。
 *     ConnectX-7 400G NIC ~20W；BlueField-3 DPU ~75~150W（厂商量级）。
 * 上述为默认值；命令行可整体覆盖以做敏感性分析(sensitivity sweep)。
 */
#endif // DCN_ENERGY_MODEL_H
