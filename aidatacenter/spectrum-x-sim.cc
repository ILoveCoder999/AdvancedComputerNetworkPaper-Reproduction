/*
 * spectrum-x-sim.cc — NVIDIA Spectrum-X RoCEv2 数据中心网络仿真
 * ──────────────────────────────────────────────────────────────
 *   · 单一统一 RoCEv2 以太网 Fabric;Spectrum-4(K=64×400G)交换机;ConnectX-7 NIC
 *   · 路由:Adaptive Routing ≈ RandomEcmpRouting
 *   · 拥塞控制:NVIDIA SHIELD ≈ 激进 ECN(MinTh=8p, MaxTh=24p)
 *   · 2-tier Leaf-Spine:serversPerLeaf=K/2=32, nSpine=K/2=32, nLeaf=ceil(N/32)
 *
 * 规模运行时可调:--N(端点/服务器数;默认 1332 对齐 mesh3d 1331)。冒烟用小 --N。
 * 注意:N 较大时 ns-3 PopulateRoutingTables(全局路由+ECMP)较慢(~分钟级,非卡死)。
 *
 * 流量场景(--scenario=): uniform | incast | allreduce | bisection | nccl_ar
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

// —— 接入：NCCL 流量层 + 统一能耗模型(单一统一 RoCEv2 Leaf-Spine fabric) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("SpectrumXSim");

// ─── 拓扑常量(N 运行时可调;N_LEAF 在 main 解析 --N 后重算)────────────────────
static uint32_t       N              = 1332;         // 服务器数(--N 可调)
static const uint32_t K_SWITCH       = 64;           // Spectrum-4 端口数
static const uint32_t SRV_PER_LEAF   = K_SWITCH / 2; // = 32
static uint32_t       N_LEAF         = (N + SRV_PER_LEAF - 1) / SRV_PER_LEAF; // ceil(N/32)
static const uint32_t N_SPINE        = K_SWITCH / 2; // = 32

// ─── 全局统计 ──────────────────────────────────────────────────────────────
struct HostProbe { double sentNs; double recvNs; };
static std::map<uint64_t, HostProbe> g_inflight;
static std::vector<HostProbe>        g_done;
static uint64_t                  g_sent      = 0;
static double                    g_first_recv = -1.0;
static double                    g_last_recv  = -1.0;
static uint64_t                  g_total_bits = 0;
static double                    g_inject_start = -1.0;  // all-reduce 注入起点(算 JCT)

// ─── 应用层 ─────────────────────────────────────────────────────────────────
class SxHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("SxHost")
      .SetParent<Application> ()
      .AddConstructor<SxHost> ();
    return t;
  }

  void Setup (uint32_t id, Ptr<Socket> rx, std::map<uint32_t, Address> addr)
  { m_id = id; m_rx = rx; m_addr = std::move (addr); }

  void Send (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    if (m_id == dst) return;
    Ptr<Packet> pkt = Create<Packet> (bytes);
    uint8_t b[12];
    std::memcpy (b,     &id,  8);
    std::memcpy (b + 8, &dst, 4);
    pkt->AddAtEnd (Create<Packet> (b, 12));

    HostProbe pr; pr.sentNs = Simulator::Now ().GetNanoSeconds (); pr.recvNs = 0;
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
  { m_rx->SetRecvCallback (MakeCallback (&SxHost::OnRecv, this)); }
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
        if (g_first_recv < 0) g_first_recv = nowNs;
        g_last_recv = nowNs;

        auto it = g_inflight.find (id);
        if (it != g_inflight.end ())
          { it->second.recvNs = nowNs; g_done.push_back (it->second); g_inflight.erase (it); }
      }
  }

  uint32_t m_id {0};
  Ptr<Socket> m_rx, m_tx;
  std::map<uint32_t, Address> m_addr;
};

// ─── main ──────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string scenario     = "uniform";
  uint32_t    uniformFlows = 200000;
  uint32_t    incastFanin  = 64;
  uint32_t    pktBytes     = 1024;
  uint32_t    queuePkts    = 128;    // RoCEv2：ECN 触发前缓冲深度
  std::string linkRate     = "400Gbps";
  std::string linkDelay    = "200ns"; // 以太网延迟（比 IB 略高）
  uint32_t    ecnMinTh     = 8;       // SHIELD 激进 ECN(比标准 DCQCN 小)
  uint32_t    ecnMaxTh     = 24;
  // —— 跨拓扑对比 all-reduce(nccl_ar) —— //
  uint32_t    arRanks      = 32;
  uint32_t    modelMB      = 8;
  double      opticalFrac  = 1.0;
  double      simStop      = 3.0;

  CommandLine cmd;
  cmd.AddValue ("scenario",     "uniform|incast|allreduce|bisection|nccl_ar", scenario);
  cmd.AddValue ("N",            "服务器数(端点);冒烟用小值,默认 1332",   N);
  cmd.AddValue ("arRanks",      "nccl_ar: all-reduce rank count", arRanks);
  cmd.AddValue ("modelMB",      "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("opticalFrac",  "fraction of ports on optics (rest DAC)", opticalFrac);
  cmd.AddValue ("simStop",      "simulator stop time (s)", simStop);
  cmd.AddValue ("uniformFlows", "number of random flows",   uniformFlows);
  cmd.AddValue ("incastFanin",  "incast fan-in",            incastFanin);
  cmd.AddValue ("pktBytes",     "payload bytes",            pktBytes);
  cmd.AddValue ("queuePkts",    "queue depth (packets)",    queuePkts);
  cmd.AddValue ("linkRate",     "link rate",                linkRate);
  cmd.AddValue ("linkDelay",    "link delay",               linkDelay);
  cmd.AddValue ("ecnMinTh",     "RED ECN MinTh (pkts)",     ecnMinTh);
  cmd.AddValue ("ecnMaxTh",     "RED ECN MaxTh (pkts)",     ecnMaxTh);
  cmd.Parse (argc, argv);

  // 解析 --N 后重算 Leaf 数(Spine 数由 K 决定,固定 32)
  if (N < 1) N = 1;
  N_LEAF = (N + SRV_PER_LEAF - 1) / SRV_PER_LEAF;

  // Adaptive Routing 近似：RandomEcmpRouting
  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                      BooleanValue (true));

  std::cout << "=== NVIDIA Spectrum-X RoCEv2 ===\n"
            << "  N=" << N << " servers  Topology: " << N_LEAF
            << " Leaf × " << N_SPINE << " Spine (K=" << K_SWITCH << ")\n"
            << "  Oversubscription: " << (double)N_LEAF/N_SPINE << "x\n"
            << "  Link: " << linkRate << " / " << linkDelay << "\n"
            << "  CC: SHIELD (ECN MinTh=" << ecnMinTh << "p MaxTh=" << ecnMaxTh << "p)"
            << "  AR: RandomECMP\n";
  if (N >= 512)
    std::cout << "  [note] N 较大,PopulateRoutingTables(全局路由)可能要几分钟,非卡死。\n";
  std::cout << "\n";

  // ── 节点 ──
  NodeContainer servers; servers.Create (N);
  NodeContainer leaves;  leaves.Create (N_LEAF);
  NodeContainer spines;  spines.Create (N_SPINE);

  InternetStackHelper inet;
  inet.Install (servers);
  inet.Install (leaves);
  inet.Install (spines);

  // ── 链路模板 ──
  std::ostringstream qs; qs << queuePkts << "p";
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute  ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay",    StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue<Packet>",
                "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  // NVIDIA SHIELD 激进 ECN
  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::RedQueueDisc",
                        "MinTh",   DoubleValue (ecnMinTh),
                        "MaxTh",   DoubleValue (ecnMaxTh),
                        "UseEcn",  BooleanValue (true),
                        "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  Ipv4AddressHelper ip;
  std::map<uint32_t, Address>     addr;
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

  // ── 边缘层：Server → Leaf ──
  for (uint32_t s = 0; s < N; ++s)
    {
      uint32_t leaf_id = s / SRV_PER_LEAF;
      Ipv4Address sip;
      mkLink (servers.Get (s), leaves.Get (leaf_id), &sip);
      serverIp[s] = sip;
    }

  // ── 汇聚层：Leaf ↔ Spine（全互联）──
  for (uint32_t l = 0; l < N_LEAF; ++l)
    for (uint32_t sp = 0; sp < N_SPINE; ++sp)
      mkLink (leaves.Get (l), spines.Get (sp));

  std::cout << "  Links: " << N << " srv-leaf + "
            << N_LEAF*N_SPINE << " leaf-spine = " << subnet << " total\n";

  std::cout << "  Populating routing tables (" << N+N_LEAF+N_SPINE << " nodes)...\n";
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  for (uint32_t i = 0; i < N; ++i)
    addr[i] = InetSocketAddress (serverIp[i], PORT);

  std::vector<Ptr<SxHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      Ptr<Socket> rx = Socket::CreateSocket (servers.Get (i),
                          TypeId::LookupByName ("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny (), PORT));
      Ptr<SxHost> a = CreateObject<SxHost> ();
      a->Setup (i, rx, addr);
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
      for (uint32_t s = 0; s < N; ++s)
        {
          uint32_t dst = (s + 1) % N;
          for (uint32_t m = 0; m < 64; ++m)
            Simulator::Schedule (Seconds (base + m * serSec),
                                 &SxHost::Send, apps[s], dst, pktBytes, pid++);
        }
      std::cout << "  Scenario: allreduce ring (N=" << N << ")\n";
    }
  else if (scenario == "incast")
    {
      uint32_t dst  = 0;
      double   base = 1.0;
      uint32_t cnt  = 0;
      for (uint32_t s = 1; s < N && cnt < incastFanin; ++s, ++cnt)
        for (uint32_t m = 0; m < 64; ++m)
          {
            double when = base + (cnt * 64 + m) * serSec;
            Simulator::Schedule (Seconds (when), &SxHost::Send, apps[s],
                                 dst, pktBytes, pid++);
          }
      std::cout << "  Scenario: incast fan-in="
                << std::min (incastFanin, N-1) << " -> server 0\n";
    }
  else if (scenario == "bisection")
    {
      double   base = 1.0;
      uint32_t cnt  = 0;
      for (uint32_t s = 0; s < N / 2; ++s)
        for (uint32_t m = 0; m < 64; ++m, ++cnt)
          {
            uint32_t dst = N / 2 + s;
            Simulator::Schedule (Seconds (base + cnt * serSec),
                                 &SxHost::Send, apps[s], dst, pktBytes, pid++);
          }
      std::cout << "  Scenario: bisection (first half -> second half)\n";
    }
  else if (scenario == "nccl_ar")
    {
      // 跨拓扑对比工作负载：真 NCCL Ring AllReduce(arRanks 个 rank 跨网铺开)。
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      std::vector<uint32_t> ranks;
      std::vector<nccl::CommOp> ops = cmp::BuildComparisonAllReduce (N, arRanks, M, &ranks);
      std::vector<double> nextT (N, 1.0);
      uint64_t injected = 0;
      for (const auto &o : ops)
        {
          if (o.onNvlink) continue;
          uint32_t nPk = (uint32_t) nccl::ToPackets (o.bytes, pktBytes);
          for (uint32_t k = 0; k < nPk; ++k)
            {
              Simulator::Schedule (Seconds (nextT[o.src]), &SxHost::Send,
                                   apps[o.src], o.dst, pktBytes, pid++);
              nextT[o.src] += serSec; ++injected;
            }
        }
      g_inject_start = 1.0;
      std::cout << "  Scenario: nccl_ar Ring AllReduce  arRanks=" << ranks.size ()
                << "  M=" << modelMB << "MiB  ops=" << ops.size ()
                << "  pkts=" << injected << "\n";
    }
  else  // uniform
    {
      double when = 1.0;
      for (uint32_t k = 0; k < uniformFlows; ++k)
        {
          uint32_t s = rng->GetInteger (0, N - 1);
          uint32_t d = rng->GetInteger (0, N - 1);
          if (s == d) continue;
          Simulator::Schedule (Seconds (when), &SxHost::Send, apps[s],
                               d, pktBytes, pid++);
          when += 5e-7;
        }
      std::cout << "  Scenario: uniform random (" << uniformFlows << " flows)\n";
    }

  double simDuration = simStop;
  Simulator::Stop (Seconds (simDuration));
  std::cout << "  Running simulation...\n\n";
  Simulator::Run ();
  Simulator::Destroy ();

  uint64_t delivered = g_done.size ();
  uint64_t dropped   = (g_sent >= delivered) ? (g_sent - delivered) : 0;
  double rxDurSec = (g_last_recv > g_first_recv && g_first_recv > 0)
                    ? (g_last_recv - g_first_recv) / 1e9 : 0.0;
  double tputGbps = rxDurSec > 0
                    ? (delivered * pktBytes * 8.0) / (rxDurSec * 1e9) : 0.0;

  std::vector<double> lats;
  lats.reserve (g_done.size ());
  for (auto &p : g_done)
    if (p.recvNs > 0) lats.push_back (p.recvNs - p.sentNs);
  std::sort (lats.begin (), lats.end ());
  double meanUs = 0, p99Us = 0;
  if (!lats.empty ())
    {
      double sum = 0; for (double v : lats) sum += v;
      meanUs = sum / lats.size () / 1000.0;
      p99Us  = lats[lats.size () * 99 / 100] / 1000.0;
    }

  std::cout << "=== Results: scenario=" << scenario << "  N=" << N << " ===\n";
  std::cout << "sent=" << g_sent << "  delivered=" << delivered
            << "  dropped=" << dropped
            << " (" << (g_sent ? 100.0*dropped/g_sent : 0.0) << "%)\n";
  std::cout << "  Throughput=" << tputGbps << " Gbps  Duration=" << rxDurSec << " s\n";
  std::cout << "  Latency mean=" << meanUs << " µs  p99=" << p99Us << " µs\n";

  std::ofstream csv ("spectrum_x_result.csv");
  csv << "scenario,N,N_leaf,N_spine,K,ecn_min,ecn_max,"
         "sent,delivered,dropped,throughput_Gbps,mean_us,p99_us\n";
  csv << scenario << "," << N << "," << N_LEAF << "," << N_SPINE << ","
      << K_SWITCH << "," << ecnMinTh << "," << ecnMaxTh << ","
      << g_sent << "," << delivered << "," << dropped << ","
      << tputGbps << "," << meanUs << "," << p99Us << "\n";
  std::cout << "wrote spectrum_x_result.csv\n";

  // ═══════════ 统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════════
  double jct = (g_inject_start > 0 && g_last_recv > 0)
               ? (g_last_recv / 1e9 - g_inject_start) : rxDurSec;
  {
    EnergyModel em;
    em.inv = EnergyInventory::LeafSpine (N, N_LEAF, N_SPINE, (uint64_t) N_LEAF * N_SPINE,
                                         (double) dr.GetBitRate () / 1e9, /*gpuPerSrv=*/1);
    if (opticalFrac < 1.0)
      em.inv.nOpticalPorts = (uint64_t) (opticalFrac * em.inv.TotalPorts ());
    em.SetBits (g_total_bits);
    em.SetDuration (jct > 0 ? jct : simDuration);
    em.SetTraffic (delivered, pktBytes);
    em.SetThroughput (tputGbps);
    em.WriteCsv ("energy_unified.csv", "spectrum_x", scenario);
    em.PrintSummary ("spectrum_x", scenario);

    if (scenario == "nccl_ar")
      {
        uint64_t M = (uint64_t) modelMB * 1024 * 1024;
        uint32_t P = (uint32_t) cmp::StridedRanks (N, arRanks).size ();
        double lineRateGbps = (double) dr.GetBitRate () / 1e9;
        cmp::CmpRow row;
        row.topo = "spectrum_x"; row.arRanks = P; row.modelBytes = M;
        row.lineRateGbps = lineRateGbps; row.jctSec = jct;
        row.sent = g_sent; row.delivered = delivered; row.dropped = dropped;
        row.tputGbps = tputGbps;
        row.algbwGbps = cmp::AlgBwGbps (M, jct);
        row.busbwGbps = cmp::BusBwGbps (M, jct, P);
        row.busbwEff  = lineRateGbps > 0 ? row.busbwGbps / lineRateGbps : 0;
        row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
        row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
        row.bisectionGbps = em.inv.bisectionGbps;
        cmp::WriteCmpCsv ("cmp_spectrum_x.csv", row);
        std::cout << "wrote cmp_spectrum_x.csv  (busbw=" << row.busbwGbps
                  << " Gbps, eff=" << row.busbwEff << ")\n";
      }
  }
  return 0;
}
