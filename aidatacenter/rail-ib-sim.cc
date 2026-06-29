/*
 * rail-ib-sim.cc — Rail-Optimized InfiniBand NDR 仿真
 * ────────────────────────────────────────────────────────────
 * 每服务器 R 块 HCA → R 条完全独立的 IB NDR Rail(每条 = 2-tier Leaf-Spine Clos)。
 * 跨 Rail(同服务器不同 GPU)走 NVLink(不过网络)。AllReduce 分 R 条并行环。
 * 默认 R=4, N_PER_RAIL=333 → N=1332(对齐 mesh3d 1331)。
 *
 * 规模运行时可调:--nPerRail(每条 rail 端点数;冒烟用小值,如 8 → N=32)。
 *   R 保持编译期常量 4(g_first_recv[R] 等定长数组依赖它);N、N_LEAF 解析后重算。
 *   注意:N 较大时 ns-3 PopulateRoutingTables(全局路由)较慢(分钟级,非卡死)。
 *
 * IB 无损:深队列(queuePkts=256)近似 credit。流量场景同其它 sim,另加 nccl_ar。
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"

// —— 接入：NCCL 流量层 + 统一能耗模型(Rail = R 条独立 Leaf-Spine,IB credit) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("RailIbSim");

// ─── 拓扑常量(R 编译期固定;N_PER_RAIL 运行时 --nPerRail,N/N_LEAF 解析后重算)──
static const uint32_t R            = 4;    // Rail 数(= 每服务器 GPU/HCA 数;定长数组依赖,保持 const)
static uint32_t       N_PER_RAIL   = 333;  // 每条 Rail 端点数(--nPerRail)
static uint32_t       N            = R * N_PER_RAIL;  // 总 GPU 节点 = 1332
static const uint32_t K_RAIL       = 32;
static const uint32_t SRV_PER_LEAF = K_RAIL / 2;   // = 16
static uint32_t       N_LEAF       = (N_PER_RAIL + SRV_PER_LEAF - 1) / SRV_PER_LEAF; // ceil
static const uint32_t N_SPINE      = K_RAIL / 2;   // = 16

// ─── 全局统计(跨所有 Rail 聚合)──────────────────────────────────────────────
struct HostProbe { double sentNs; double recvNs; uint32_t rail; };
static std::map<uint64_t, HostProbe> g_inflight;
static std::vector<HostProbe>        g_done;
static uint64_t                  g_sent      = 0;
static double g_first_recv[R], g_last_recv[R];     // R 为 const → 定长数组合法
static uint64_t g_total_bits = 0;
static double   g_inject_start = -1.0;

// ─── 应用层 ─────────────────────────────────────────────────────────────────
class RailHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("RailHost")
      .SetParent<Application> ()
      .AddConstructor<RailHost> ();
    return t;
  }

  void Setup (uint32_t id, uint32_t rail, Ptr<Socket> rx,
              std::map<uint32_t, Address> addr)
  { m_id = id; m_rail = rail; m_rx = rx; m_addr = std::move (addr); }

  void Send (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    if (m_id == dst) return;
    Ptr<Packet> pkt = Create<Packet> (bytes);
    uint8_t b[12];
    std::memcpy (b,     &id,  8);
    std::memcpy (b + 8, &dst, 4);
    pkt->AddAtEnd (Create<Packet> (b, 12));

    HostProbe pr; pr.sentNs = Simulator::Now ().GetNanoSeconds ();
    pr.recvNs = 0; pr.rail = m_rail;
    g_inflight[id] = pr;
    g_sent++;

    auto it = m_addr.find (dst);
    if (it == m_addr.end ()) return;
    if (!m_tx)
      m_tx = Socket::CreateSocket (GetNode (),
               TypeId::LookupByName ("ns3::UdpSocketFactory"));
    m_tx->SendTo (pkt, 0, it->second);
  }

private:
  void StartApplication () override
  { m_rx->SetRecvCallback (MakeCallback (&RailHost::OnRecv, this)); }
  void StopApplication () override {}

  void OnRecv (Ptr<Socket> s)
  {
    Ptr<Packet> pkt;
    while ((pkt = s->Recv ()))
      {
        if (pkt->GetSize () < 12) continue;
        uint8_t b[12];
        pkt->CreateFragment (pkt->GetSize () - 12, 12)->CopyData (b, 12);
        uint64_t id; std::memcpy (&id, b, 8);

        double nowNs = Simulator::Now ().GetNanoSeconds ();
        if (g_first_recv[m_rail] < 0) g_first_recv[m_rail] = nowNs;
        g_last_recv[m_rail] = nowNs;

        auto it = g_inflight.find (id);
        if (it != g_inflight.end ())
          { it->second.recvNs = nowNs; g_done.push_back (it->second); g_inflight.erase (it); }
      }
  }

  uint32_t m_id {0}, m_rail {0};
  Ptr<Socket> m_rx, m_tx;
  std::map<uint32_t, Address> m_addr;
};

// ─── main ──────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string scenario     = "allreduce";
  uint32_t    uniformFlows = 200000;
  uint32_t    incastFanin  = 64;
  uint32_t    pktBytes     = 1024;
  uint32_t    queuePkts    = 256;
  std::string linkRate     = "400Gbps";
  std::string linkDelay    = "100ns";
  // —— 跨拓扑对比 all-reduce(nccl_ar) —— //
  uint32_t    arRanks      = 32;
  uint32_t    modelMB      = 8;
  double      opticalFrac  = 1.0;
  double      simStop      = 3.0;

  CommandLine cmd;
  cmd.AddValue ("scenario",     "uniform|incast|allreduce|bisection|nccl_ar", scenario);
  cmd.AddValue ("nPerRail",     "每条 rail 端点数(冒烟用小值,如 8 → N=32);默认 333", N_PER_RAIL);
  cmd.AddValue ("arRanks",      "nccl_ar: total all-reduce ranks (split across R rails)", arRanks);
  cmd.AddValue ("modelMB",      "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("opticalFrac",  "fraction of ports on optics (rest DAC)", opticalFrac);
  cmd.AddValue ("simStop",      "simulator stop time (s)", simStop);
  cmd.AddValue ("uniformFlows", "random flows per rail",    uniformFlows);
  cmd.AddValue ("incastFanin",  "incast fan-in per rail",   incastFanin);
  cmd.AddValue ("pktBytes",     "payload bytes",            pktBytes);
  cmd.AddValue ("queuePkts",    "IB credit buffer depth",   queuePkts);
  cmd.AddValue ("linkRate",     "link rate",                linkRate);
  cmd.AddValue ("linkDelay",    "link delay",               linkDelay);
  cmd.Parse (argc, argv);

  // 解析 --nPerRail 后重算 N 与 N_LEAF(R 固定 4)
  if (N_PER_RAIL < 2) N_PER_RAIL = 2;
  N      = R * N_PER_RAIL;
  N_LEAF = (N_PER_RAIL + SRV_PER_LEAF - 1) / SRV_PER_LEAF;

  for (uint32_t r = 0; r < R; ++r)
    { g_first_recv[r] = -1.0; g_last_recv[r] = -1.0; }

  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                      BooleanValue (true));

  std::cout << "=== Rail-Optimized InfiniBand NDR ===\n"
            << "  R=" << R << " rails  N_per_rail=" << N_PER_RAIL
            << "  Total N=" << N << "\n"
            << "  Per-rail topology: K=" << K_RAIL
            << "  nLeaf=" << N_LEAF << "  nSpine=" << N_SPINE
            << "  oversubscription=" << (double)N_LEAF/N_SPINE << "x\n"
            << "  Link: " << linkRate << " / " << linkDelay
            << "  (IB credit-based lossless, deep queue=" << queuePkts << "p)\n";
  if (N >= 512)
    std::cout << "  [note] N 较大,PopulateRoutingTables(全局路由)可能要几分钟,非卡死。\n";
  std::cout << "\n";

  NodeContainer servers; servers.Create (N);
  NodeContainer leaves[R], spines[R];
  for (uint32_t r = 0; r < R; ++r)
    { leaves[r].Create (N_LEAF); spines[r].Create (N_SPINE); }

  InternetStackHelper inet;
  inet.Install (servers);
  for (uint32_t r = 0; r < R; ++r)
    { inet.Install (leaves[r]); inet.Install (spines[r]); }

  std::ostringstream qs; qs << queuePkts << "p";
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute  ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay",    StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue<Packet>",
                "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::FifoQueueDisc",
                        "MaxSize", StringValue (qs.str ()));

  Ipv4AddressHelper ip;
  std::map<uint32_t, Address> addr;
  std::map<uint32_t, Ipv4Address> serverIp;
  uint32_t subnet = 0;
  const uint16_t PORT = 9000;

  auto mkLink = [&] (Ptr<Node> u, Ptr<Node> v, Ipv4Address *ipU = nullptr)
  {
    NetDeviceContainer dev = p2p.Install (u, v);
    tch.Install (dev);
    dev.Get (0)->TraceConnectWithoutContext ("MacTx",
      MakeCallback (+[](Ptr<const Packet> p){ g_total_bits += p->GetSize () * 8; }));
    dev.Get (1)->TraceConnectWithoutContext ("MacTx",
      MakeCallback (+[](Ptr<const Packet> p){ g_total_bits += p->GetSize () * 8; }));
    uint32_t base = subnet * 4;
    std::ostringstream b;
    b << "10." << ((base >> 16) & 0xff) << "."
               << ((base >>  8) & 0xff) << "."
               << ( base        & 0xff);
    ip.SetBase (b.str ().c_str (), "255.255.255.252");
    Ipv4InterfaceContainer ic = ip.Assign (dev);
    if (ipU) *ipU = ic.GetAddress (0);
    ++subnet;
    return ic;
  };

  for (uint32_t r = 0; r < R; ++r)
    {
      uint32_t srv_base = r * N_PER_RAIL;
      for (uint32_t s = 0; s < N_PER_RAIL; ++s)
        {
          uint32_t  leaf_id = s / SRV_PER_LEAF;
          uint32_t  sid     = srv_base + s;
          Ipv4Address sip;
          mkLink (servers.Get (sid), leaves[r].Get (leaf_id), &sip);
          serverIp[sid] = sip;
        }
      for (uint32_t l = 0; l < N_LEAF; ++l)
        for (uint32_t sp = 0; sp < N_SPINE; ++sp)
          mkLink (leaves[r].Get (l), spines[r].Get (sp));
    }

  std::cout << "  Links built: " << subnet << " total  ("
            << R << " rails × " << subnet/R << " per rail)\n";

  std::cout << "  Populating routing tables (" << N + R*(N_LEAF+N_SPINE) << " nodes)...\n";
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  for (uint32_t i = 0; i < N; ++i)
    addr[i] = InetSocketAddress (serverIp[i], PORT);

  std::vector<Ptr<RailHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      uint32_t rail = i / N_PER_RAIL;
      Ptr<Socket> rx = Socket::CreateSocket (servers.Get (i),
                          TypeId::LookupByName ("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny (), PORT));
      Ptr<RailHost> a = CreateObject<RailHost> ();
      a->Setup (i, rail, rx, addr);
      servers.Get (i)->AddApplication (a);
      a->SetStartTime (Seconds (0.0));
      a->SetStopTime  (Seconds (30.0));
      apps[i] = a;
    }

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  uint64_t pid = 1;
  DataRate dr (linkRate);
  double serSec = (pktBytes * 8.0) / (double) dr.GetBitRate ();

  if (scenario == "allreduce")
    {
      double base = 1.0;
      for (uint32_t r = 0; r < R; ++r)
        {
          uint32_t base_id = r * N_PER_RAIL;
          for (uint32_t s = 0; s < N_PER_RAIL; ++s)
            {
              uint32_t src = base_id + s;
              uint32_t dst = base_id + (s + 1) % N_PER_RAIL;
              for (uint32_t m = 0; m < 64; ++m)
                Simulator::Schedule (Seconds (base + m * serSec),
                                     &RailHost::Send, apps[src], dst, pktBytes, pid++);
            }
        }
      std::cout << "  Scenario: allreduce ring × " << R << " parallel rails"
                << " (N_per_ring=" << N_PER_RAIL << ")\n";
    }
  else if (scenario == "incast")
    {
      double base = 1.0;
      for (uint32_t r = 0; r < R; ++r)
        {
          uint32_t base_id = r * N_PER_RAIL;
          uint32_t dst     = base_id;
          uint32_t cnt = 0;
          for (uint32_t s = 1; s < N_PER_RAIL && cnt < incastFanin; ++s, ++cnt)
            for (uint32_t m = 0; m < 64; ++m)
              {
                double when = base + (r * incastFanin * 64 + cnt * 64 + m) * serSec;
                Simulator::Schedule (Seconds (when), &RailHost::Send,
                                     apps[base_id + s], dst, pktBytes, pid++);
              }
        }
      std::cout << "  Scenario: incast fan-in=" << std::min(incastFanin, N_PER_RAIL-1)
                << " per rail × " << R << " rails\n";
    }
  else if (scenario == "bisection")
    {
      double base = 1.0;
      for (uint32_t r = 0; r < R; ++r)
        {
          uint32_t base_id = r * N_PER_RAIL;
          uint32_t cnt = 0;
          for (uint32_t s = 0; s < N_PER_RAIL / 2; ++s)
            for (uint32_t m = 0; m < 64; ++m, ++cnt)
              {
                uint32_t src = base_id + s;
                uint32_t dst = base_id + N_PER_RAIL / 2 + s;
                Simulator::Schedule (Seconds (base + cnt * serSec),
                                     &RailHost::Send, apps[src], dst, pktBytes, pid++);
              }
        }
      std::cout << "  Scenario: bisection (within each rail) × " << R << " rails\n";
    }
  else if (scenario == "nccl_ar")
    {
      // Rail-optimized all-reduce：R 条并行环,每条在一条 rail 内、各 arRanks/R 个 rank。
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      uint32_t perRail = arRanks / R; if (perRail < 2) perRail = 2;
      std::vector<double> nextT (N, 1.0);
      uint64_t injected = 0;
      for (uint32_t r = 0; r < R; ++r)
        {
          std::vector<uint32_t> sub = cmp::StridedRanks (N_PER_RAIL, perRail);
          std::vector<uint32_t> ranks;
          for (uint32_t idx : sub) ranks.push_back (r * N_PER_RAIL + idx);
          std::vector<nccl::CommOp> ops;
          nccl::RingAllReduce (ranks, M, ops);
          for (const auto &o : ops)
            {
              if (o.onNvlink) continue;
              uint32_t nPk = (uint32_t) nccl::ToPackets (o.bytes, pktBytes);
              for (uint32_t k = 0; k < nPk; ++k)
                {
                  Simulator::Schedule (Seconds (nextT[o.src]), &RailHost::Send,
                                       apps[o.src], o.dst, pktBytes, pid++);
                  nextT[o.src] += serSec; ++injected;
                }
            }
        }
      g_inject_start = 1.0;
      std::cout << "  Scenario: nccl_ar  " << R << " parallel rings × " << perRail
                << " ranks  M=" << modelMB << "MiB  pkts=" << injected << "\n";
    }
  else  // uniform
    {
      double when = 1.0;
      uint32_t flows_per_rail = uniformFlows / R;
      for (uint32_t r = 0; r < R; ++r)
        {
          uint32_t base_id = r * N_PER_RAIL;
          for (uint32_t k = 0; k < flows_per_rail; ++k)
            {
              uint32_t s = base_id + rng->GetInteger (0, N_PER_RAIL - 1);
              uint32_t d = base_id + rng->GetInteger (0, N_PER_RAIL - 1);
              if (s == d) continue;
              Simulator::Schedule (Seconds (when), &RailHost::Send, apps[s], d, pktBytes, pid++);
              when += 5e-7;
            }
        }
      std::cout << "  Scenario: uniform random × " << R << " rails\n";
    }

  double simDuration = simStop;
  Simulator::Stop (Seconds (simDuration));
  std::cout << "  Running simulation...\n\n";
  Simulator::Run ();
  Simulator::Destroy ();

  uint64_t delivered = g_done.size ();
  uint64_t dropped   = (g_sent >= delivered) ? (g_sent - delivered) : 0;

  double railTput[R] = {};
  for (uint32_t r = 0; r < R; ++r)
    if (g_last_recv[r] > g_first_recv[r] && g_first_recv[r] > 0)
      {
        double dur = (g_last_recv[r] - g_first_recv[r]) / 1e9;
        uint64_t rail_deliv = 0;
        for (auto &p : g_done) if (p.rail == r && p.recvNs > 0) ++rail_deliv;
        railTput[r] = (rail_deliv * pktBytes * 8.0) / (dur * 1e9);
      }
  double totalTput = 0;
  for (uint32_t r = 0; r < R; ++r) totalTput += railTput[r];

  std::vector<double> lats;
  for (auto &p : g_done) if (p.recvNs > 0) lats.push_back (p.recvNs - p.sentNs);
  std::sort (lats.begin (), lats.end ());
  double meanUs = 0, p99Us = 0;
  if (!lats.empty ())
    {
      double sum = 0; for (double v : lats) sum += v;
      meanUs = sum / lats.size () / 1000.0;
      p99Us  = lats[lats.size () * 99 / 100] / 1000.0;
    }

  uint32_t nSwitches = R * (N_LEAF + N_SPINE);
  std::cout << "=== Results: scenario=" << scenario << "  R=" << R << "  N=" << N << " ===\n";
  std::cout << "sent=" << g_sent << "  delivered=" << delivered
            << "  dropped=" << dropped
            << " (" << (g_sent ? 100.0*dropped/g_sent : 0.0) << "%)\n";
  for (uint32_t r = 0; r < R; ++r) printf("  Rail %u: %.3f Gbps\n", r, railTput[r]);
  std::cout << "  全系统聚合吞吐: " << totalTput << " Gbps\n";
  std::cout << "  Latency mean=" << meanUs << " µs  p99=" << p99Us << " µs\n";

  std::ofstream csv ("rail_ib_result.csv");
  csv << "scenario,R,N_per_rail,N_total,switches,total_throughput_Gbps,mean_us,p99_us,"
         "sent,delivered,dropped\n";
  csv << scenario << "," << R << "," << N_PER_RAIL << "," << N << ","
      << nSwitches << "," << totalTput << "," << meanUs << "," << p99Us << ","
      << g_sent << "," << delivered << "," << dropped << "\n";
  std::cout << "wrote rail_ib_result.csv\n";

  // ═══════════ 统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════════
  // JCT = 最慢一条 rail 的完成时间(R 条并行)。
  double jctRail = 0;
  for (uint32_t r = 0; r < R; ++r)
    if (g_last_recv[r] > 0 && g_inject_start > 0)
      jctRail = std::max (jctRail, g_last_recv[r] / 1e9 - g_inject_start);
  double lineRateGbps = (double) dr.GetBitRate () / 1e9;
  uint64_t L2rail = (uint64_t) N_LEAF * N_SPINE;

  EnergyModel em;
  em.inv = EnergyInventory::Rail (R, N_PER_RAIL, N_LEAF, N_SPINE, L2rail, lineRateGbps);
  if (opticalFrac < 1.0)
    em.inv.nOpticalPorts = (uint64_t) (opticalFrac * em.inv.TotalPorts ());
  em.SetBits (g_total_bits);
  em.SetDuration (jctRail > 0 ? jctRail : simDuration);
  em.SetTraffic (delivered, pktBytes);
  em.SetThroughput (totalTput);
  em.WriteCsv ("energy_unified.csv", "rail", scenario);
  em.PrintSummary ("rail", scenario);

  if (scenario == "nccl_ar")
    {
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      uint32_t perRail = arRanks / R; if (perRail < 2) perRail = 2;
      uint32_t P = perRail * R;
      cmp::CmpRow row;
      row.topo = "rail"; row.arRanks = P; row.modelBytes = M;
      row.lineRateGbps = lineRateGbps; row.jctSec = jctRail;
      row.sent = g_sent; row.delivered = delivered; row.dropped = dropped;
      row.tputGbps = totalTput;
      row.algbwGbps = cmp::AlgBwGbps (M, jctRail);
      row.busbwGbps = cmp::BusBwGbps (M, jctRail, P);
      row.busbwEff  = lineRateGbps > 0 ? row.busbwGbps / lineRateGbps : 0;
      row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
      row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
      row.bisectionGbps = em.inv.bisectionGbps;
      row.pfcPausedSec = -1; row.pfcPeakKB = -1; row.pfcDrops = 0;   // IB credit,非 PFC
      cmp::WriteCmpCsv ("cmp_rail.csv", row);
      std::cout << "wrote cmp_rail.csv  (busbw=" << row.busbwGbps
                << " Gbps, eff=" << row.busbwEff << ", " << R << " parallel rings)\n";
    }
  return 0;
}
