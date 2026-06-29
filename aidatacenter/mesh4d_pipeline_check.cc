/*
 * mesh4d_pipeline_check.cc — 验证 mesh4dw6-sim.cc 中织入的纯 C++ 部分(不需 ns-3)。
 * 复刻 sim 末尾对 cmp::/EnergyModel 的调用,确认 4D-mesh 的 inventory / busbw /
 * cmp 行都产出合理数字,并写出 cmp_mesh4d.csv 供 compare_topologies.py 读取。
 *   编译: g++ -std=c++14 -O2 -I<headers> mesh4d_pipeline_check.cc -o chk
 */
#include <cassert>
#include <cstdio>
#include <vector>
#include "cmp-common.h"     // 含 nccl-collectives.h
#include "energy-model.h"

int main ()
{
  const uint32_t W = 6;             // d=4 维(每轴 W=6 节点)
  const uint32_t N = W * W * W * W;                 // 1296
  const uint32_t arRanks = 32;
  const uint64_t M = (uint64_t) 8 * 1024 * 1024;    // 8 MiB
  const double   rate = 400.0;                      // Gbps
  const uint32_t fpgaPerNode = 8;

  // —— 与 sim 完全一致的派生量 —— //
  uint64_t nLinks = (uint64_t) N * 4 * (W - 1) / 2;            // 每轴 K_W 完全图 = 12960
  uint64_t bisectionLinks = (uint64_t)(W/2)*(W/2) * (uint64_t)(W*W*W); // (W/2)^2·W^(d-1)=1944

  // 1) rank 跨网铺开
  std::vector<uint32_t> ranks;
  std::vector<nccl::CommOp> ops = cmp::BuildComparisonAllReduce (N, arRanks, M, &ranks);
  uint32_t P = (uint32_t) ranks.size ();
  printf ("ranks=%u (first=%u stride=%u last=%u)\n",
          P, ranks.front (), ranks.size () > 1 ? ranks[1] - ranks[0] : 0, ranks.back ());
  printf ("ring ops=%zu  (expect 2(P-1)P=%u)\n", ops.size (), 2 * (P - 1) * P);

  // 2) 字节量校验:每 rank 发送 == 2(P-1)/P·M
  auto perRank = nccl::PerRankSent (ops);
  uint64_t expPer = cmp::RingPerRankBytes (M, P);
  uint64_t gotPer = perRank.empty () ? 0 : perRank.begin ()->second;
  printf ("per-rank bytes got=%llu expect=%llu  %s\n",
          (unsigned long long) gotPer, (unsigned long long) expPer,
          gotPer == expPer ? "OK" : "MISMATCH");

  // 3) 能耗 inventory(Switchless)
  EnergyModel em;
  em.inv = EnergyInventory::Switchless (N, nLinks, fpgaPerNode, rate, 1.0, bisectionLinks);
  printf ("inv: fpga=%llu hostPorts=%llu(=2*links) gpu=%llu bisection=%.0f Gbps (=%.0f links*%.0f)\n",
          (unsigned long long) em.inv.nFpga, (unsigned long long) em.inv.nHostPorts,
          (unsigned long long) em.inv.nGpu, em.inv.bisectionGbps,
          (double) bisectionLinks, rate);

  // 4) 用一个代表性 jct 串起 busbw / 能耗 / cmp 行(数值仅占位,验证管线)
  double jct = 1.2e-3;                 // 占位完成时间
  uint64_t delivered = nccl::ToPackets (cmp::RingTotalBytes (M, P), 1024);
  em.SetBits ((uint64_t) delivered * 1024 * 8);
  em.SetDuration (jct);
  em.SetTraffic (delivered, 1024);
  double busbw = cmp::BusBwGbps (M, jct, P);
  em.SetThroughput (cmp::AlgBwGbps (M, jct));
  printf ("busbw=%.1f Gbps  eff=%.1f%%  staticP=%.0f W  pJ/bit=%.2f  Gbps/W=%.4f\n",
          busbw, 100.0 * busbw / rate, em.StaticPowerW (),
          em.EnergyPerDeliveredBitPj (), em.TputPerWatt ());

  cmp::CmpRow row;
  row.topo = "mesh4d"; row.arRanks = P; row.modelBytes = M;
  row.lineRateGbps = rate; row.jctSec = jct;
  row.delivered = delivered;
  row.tputGbps = cmp::AlgBwGbps (M, jct);
  row.algbwGbps = cmp::AlgBwGbps (M, jct);
  row.busbwGbps = busbw; row.busbwEff = busbw / rate;
  row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
  row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
  row.bisectionGbps = em.inv.bisectionGbps;
  row.pfcPausedSec = -1; row.pfcPeakKB = -1; row.pfcDrops = 0;
  cmp::WriteCmpCsv ("cmp_mesh4d.csv", row);
  printf ("wrote cmp_mesh4d.csv\n");

  // 断言关键不变量
  assert (P == 32);
  assert (ops.size () == 2u * (P - 1) * P);
  assert (gotPer == expPer);
  assert (em.inv.nFpga == (uint64_t) N * fpgaPerNode);
  assert (em.inv.nHostPorts == 2 * nLinks);
  assert (em.inv.bisectionGbps == bisectionLinks * rate);
  printf ("ALL ASSERTIONS PASSED\n");
  return 0;
}
