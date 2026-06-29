/*
 * cmp-common.h — 跨拓扑 AllReduce 对比的共享工具（leaf-spine / mesh / dragonfly 共用）
 * ============================================================================
 * 目的：让三个 *-sim.cc 用 **完全相同** 的 AllReduce 工作负载做对比，
 *       从而对比是 apples-to-apples 的——同样的逻辑 rank 数(arRanks)、同样的
 *       模型/梯度大小(M)、同样的 NCCL Ring 算法；只有"拓扑 + 织入方式"不同。
 *
 * 为什么要 arRanks 而不是直接用各拓扑的全部节点 N：
 *   完整 ring all-reduce 是 2(P-1) 步，每步每 rank 传 M/P。若 P=512、M=256MB，
 *   ns-3 报文级仿真要跑数亿个包，根本跑不完。SCALE_DOWN.md / scale_planner.py
 *   的方法论正是：用小规模、带宽受限(bandwidth-bound)的代表性运行去推大规模。
 *   故对比 all-reduce 固定一个可跑完的 arRanks(默认 32) + 足够大的 M(带宽受限)，
 *   三种拓扑都用同一组参数 → 公平。报告的首要指标(busbw 效率、pJ/bit、Gbps/W)
 *   是 **规模稳健** 的:同一 (arRanks,M) 下横比拓扑成立。
 *
 * 把 arRanks 个逻辑 rank **跨整张网均匀铺开**(StridedRanks)，让 all-reduce 真的
 *   穿越 fabric(而不是恰好全落在一个 leaf / 一个 group 上 → 不经骨干，对比失真)。
 *
 * 纯 C++14、不依赖 ns-3，可独立编译做单元测试(见 test_cmp_common.cc)。
 * ============================================================================
 */
#ifndef CMP_COMMON_H
#define CMP_COMMON_H

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "nccl-collectives.h"

namespace cmp {

// ── 把 arRanks 个逻辑 rank 均匀铺到 N 个物理节点上(跨网铺开) ──────────────────
//   rank i → 物理节点 (i*N/arRanks)。stride = N/arRanks。
//   例：N=512, arRanks=32 → stride 16 → 节点 0,16,32,...,496 (16 个 leaf 各 2 个)。
inline std::vector<uint32_t> StridedRanks (uint32_t N, uint32_t arRanks)
{
  std::vector<uint32_t> r;
  if (N == 0 || arRanks == 0) return r;
  if (arRanks > N) arRanks = N;
  for (uint32_t i = 0; i < arRanks; ++i)
    r.push_back ((uint32_t) ((uint64_t) i * N / arRanks));
  return r;
}

// ── Ring all-reduce 的解析量(校验 / busbw 计算用) ───────────────────────────
//   每 rank 总发送 = 2(P-1)/P · M；全网总发送 = 2(P-1)·M。
inline uint64_t RingPerRankBytes (uint64_t M, uint32_t P)
{ return (P >= 2) ? (uint64_t) (2.0 * (P - 1) / (double) P * M) : 0; }
inline uint64_t RingTotalBytes (uint64_t M, uint32_t P)
{ return (P >= 2) ? (uint64_t) 2 * (P - 1) * M : 0; }

// ── NCCL 口径带宽(Gbps)，jct = 整条 all-reduce 完成时间(秒) ──────────────────
//   algbw = M / t ；busbw = algbw · 2(P-1)/P (ring 的"总线带宽"，可与线速直接比)。
inline double AlgBwGbps (uint64_t M, double jctSec)
{ return jctSec > 0 ? (double) M * 8.0 / jctSec / 1e9 : 0.0; }
inline double BusBwGbps (uint64_t M, double jctSec, uint32_t P)
{ return (P >= 2) ? AlgBwGbps (M, jctSec) * 2.0 * (P - 1) / (double) P : 0.0; }

// ── 一行对比结果(每个拓扑各写一份 cmp_<topo>.csv，compare_topologies.py 读取) ──
struct CmpRow {
  std::string topo;
  uint32_t    arRanks    = 0;
  uint64_t    modelBytes = 0;        // M (每 rank 梯度量)
  double      lineRateGbps = 0;
  double      jctSec     = 0;        // all-reduce 完成时间(注入→最后交付)
  uint64_t    sent = 0, delivered = 0, dropped = 0;
  double      tputGbps  = 0;         // 原始交付吞吐(delivered·pkt/jct)
  double      algbwGbps = 0;         // = M/t
  double      busbwGbps = 0;         // = algbw·2(P-1)/P
  double      busbwEff  = 0;         // = busbw / lineRate ∈ (0,1]，规模稳健对比的核心
  // 能耗(取自统一 EnergyModel)
  double      totalEnergyJ = 0, avgPowerW = 0, pjPerBit = 0, gbpsPerW = 0;
  double      bisectionGbps = 0;
  // 仅 RoCE/PFC fabric 有意义；其它(IB credit / mesh)填 -1 表示 N/A
  double      pfcPausedSec = -1, pfcPeakKB = -1;
  uint64_t    pfcDrops = 0;          // 正确 PFC 下应恒为 0
};

inline void WriteCmpCsv (const std::string& path, const CmpRow& r)
{
  std::ofstream f (path);
  f << "topology,arRanks,modelBytes,lineRate_Gbps,jct_s,sent,delivered,dropped,"
       "tput_Gbps,algbw_Gbps,busbw_Gbps,busbw_eff,"
       "total_energy_J,avg_power_W,pJ_per_bit,gbps_per_W,bisection_Gbps,"
       "pfc_paused_s,pfc_peak_KB,pfc_drops\n";
  f << r.topo << "," << r.arRanks << "," << r.modelBytes << "," << r.lineRateGbps << ","
    << r.jctSec << "," << r.sent << "," << r.delivered << "," << r.dropped << ","
    << r.tputGbps << "," << r.algbwGbps << "," << r.busbwGbps << "," << r.busbwEff << ","
    << r.totalEnergyJ << "," << r.avgPowerW << "," << r.pjPerBit << "," << r.gbpsPerW << ","
    << r.bisectionGbps << "," << r.pfcPausedSec << "," << r.pfcPeakKB << "," << r.pfcDrops << "\n";
}

// ── 生成对比用 Ring all-reduce 计划：rank 跨网铺开后回放 ─────────────────────
//   返回 CommOp 列表(src/dst 已是物理节点 id)。chunk = M/P 每步。
inline std::vector<nccl::CommOp>
BuildComparisonAllReduce (uint32_t N, uint32_t arRanks, uint64_t M,
                          std::vector<uint32_t>* ranksOut = nullptr)
{
  std::vector<uint32_t> ranks = StridedRanks (N, arRanks);
  if (ranksOut) *ranksOut = ranks;
  std::vector<nccl::CommOp> ops;
  nccl::RingAllReduce (ranks, M, ops);   // src/dst = ranks[i] = 物理节点 id
  return ops;
}

} // namespace cmp
#endif // CMP_COMMON_H
