/*
 * dragonfly-ib-sim.cc — Dragonfly 拓扑 + InfiniBand NDR 仿真
 * ──────────────────────────────────────
 * 平衡 Dragonfly(Kim et al. 2008):a=routers/group, p=servers/router, h=a 全局链路,
 *   g=a·h+1 groups, N=p·a·g。默认 a=p=6 → g=37, N=1332(对齐 mesh3d 1331)。
 *   规模运行时可调:--a / --p(冒烟用小值,如 --a=2 --p=2 → N=20)。
 *   注意:N 较大时 ns-3 PopulateRoutingTables(全局路由)较慢(分钟级,非卡死)。
 *
 * IB 近似:深队列(queuePkts=256)近似 credit 无损;RandomEcmpRouting 近似 UGAL。
 * 流量场景(--scenario=): uniform | incast | allreduce | bisection | nccl_ar
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"

// —— 接入：NCCL 流量层 + 统一能耗模型(Dragonfly = 交换式,IB credit 无损) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("DragonflyIbSim");

// ─── 拓扑常量(a/p 运行时可调;h,g,N 在 main 解析后重算)──────────────────────
static uint32_t A = 6;           // routers per group(--a)
static uint32_t P = 6;           // servers per router(--p)
static uint32_t H = A;           // global links per router (balanced: H=A)
static uint32_t G = A * H + 1;   // groups = a·h+1 = 37
static uint32_t N = P * A * G;   // total servers = 1332

// ─── 节点索引辅助(读全局 A/P,解析后有效)────────────────────────────────────
static uint32_t routerIdx (uint32_t g, uint32_t r) { return g * A + r; }
static uint32_t serverIdx (uint32_t g, uint32_t r, uint32_t s)
{ return (g * A + r) * P + s; }

// ─── 全局统计 ──────────────────────────────────────────────────────────────
struct HostProbe { double sentNs; double recvNs; };
static std::map<uint64_t, HostProbe> g_inflight;
static std::vector<HostProbe>        g_done;
static uint64_t                  g_sent      = 0;
static double                    g_first_recv = -1.0;
static double                    g_last_recv  = -1.0;
static uint64_t                  g_total_bits = 0;
static double                    g_inject_start = -1.0; // all-reduce 注入起点(算 JCT)

// ─── 应用层：IP/ECMP 路由 ───────────────────────────────────────────────────
class DfHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("DfHost")
      .SetParent<Application> ()
      .AddConstructor<DfHost> ();
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
  { m_rx->SetRecvCallback (MakeCallback (&DfHost::OnRecv, this)); }
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
  uint32_t    queuePkts    = 256;       // IB 无损:深队列近似 credit
  std::string linkRate     = "400Gbps";
  std::string linkDelay    = "100ns";
  // —— 跨拓扑对比 all-reduce(nccl_ar) + 能耗清单参数 —— //
  uint32_t    arRanks      = 32;
  uint32_t    modelMB      = 8;
  double      opticalFrac  = 1.0;
  double      simStop      = 3.0;

  CommandLine cmd;
  cmd.AddValue ("scenario",     "uniform|incast|allreduce|bisection|nccl_ar", scenario);
  cmd.AddValue ("a",            "routers per group(冒烟用小值,如 2);默认 6",  A);
  cmd.AddValue ("p",            "servers per router(冒烟用小值,如 2);默认 6", P);
  cmd.AddValue ("arRanks",      "nccl_ar: all-reduce rank count", arRanks);
  cmd.AddValue ("modelMB",      "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("opticalFrac",  "fraction of ports on optics (rest DAC)", opticalFrac);
  cmd.AddValue ("simStop",      "simulator stop time (s)", simStop);
  cmd.AddValue ("uniformFlows", "number of random flows",    uniformFlows);
  cmd.AddValue ("incastFanin",  "incast fan-in",             incastFanin);
  cmd.AddValue ("pktBytes",     "payload bytes",             pktBytes);
  cmd.AddValue ("queuePkts",    "IB credit buffer depth (packets)", queuePkts);
  cmd.AddValue ("linkRate",     "link rate",                 linkRate);
  cmd.AddValue ("linkDelay",    "link delay",                linkDelay);
  cmd.Parse (argc, argv);

  // 解析 --a/--p 后重算派生规模(平衡 dragonfly:H=A, G=A·H+1, N=P·A·G)
  if (A < 2) A = 2;
  if (P < 1) P = 1;
  H = A; G = A * H + 1; N = P * A * G;

  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                      BooleanValue (true));

  std::cout << "=== Dragonfly + InfiniBand NDR ===\n"
            << "  a=" << A << " p=" << P << " h=" << H
            << "  g=" << G << " groups  N=" << N << " servers\n"
            << "  Routers=" << A*G << "  Links: "
            << N << "(srv-rtr) + "
            << A*(A-1)/2*G << "(intra) + "
            << G*(G-1)/2 << "(inter) = "
            << N + A*(A-1)/2*G + G*(G-1)/2 << " total\n"
            << "  Link: " << linkRate << " / " << linkDelay
            << "  ECMP: on (approx UGAL)\n";
  if (N >= 512)
    std::cout << "  [note] N 较大,PopulateRoutingTables(全局路由)可能要几分钟,非卡死。\n";
  std::cout << "\n";

  NodeContainer servers; servers.Create (N);
  NodeContainer routers; routers.Create (A * G);

  InternetStackHelper inet;
  inet.Install (servers);
  inet.Install (routers);

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
  std::map<uint32_t, Ipv4Address> serverIp;
  uint32_t subnet = 0;

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

  // 1. Server ↔ Router
  for (uint32_t g = 0; g < G; ++g)
    for (uint32_t r = 0; r < A; ++r)
      for (uint32_t s = 0; s < P; ++s)
        {
          uint32_t sid = serverIdx (g, r, s);
          Ipv4Address sip;
          mkLink (servers.Get (sid), routers.Get (routerIdx (g, r)), &sip);
          serverIp[sid] = sip;
        }

  // 2. Intra-Group(group 内 router 全互联)
  for (uint32_t g = 0; g < G; ++g)
    for (uint32_t r1 = 0; r1 < A; ++r1)
      for (uint32_t r2 = r1 + 1; r2 < A; ++r2)
        mkLink (routers.Get (routerIdx (g, r1)),
                routers.Get (routerIdx (g, r2)));

  // 3. Inter-Group Global(平衡分配,每对 group 1 条)
  {
    std::set<std::pair<uint32_t,uint32_t>> linked;
    uint32_t interLinkCount = 0;
    for (uint32_t gi = 0; gi < G; ++gi)
      for (uint32_t gj = gi + 1; gj < G; ++gj)
        {
          auto key = std::make_pair (gi, gj);
          if (linked.count (key)) continue;
          linked.insert (key);
          uint32_t ri = (gj - gi - 1) % A;
          uint32_t rj = (G - (gj - gi) - 1) % A;
          mkLink (routers.Get (routerIdx (gi, ri)),
                  routers.Get (routerIdx (gj, rj)));
          ++interLinkCount;
        }
    std::cout << "  Inter-group links built: " << interLinkCount
              << "  (expect " << G*(G-1)/2 << ")\n";
  }

  std::cout << "  Populating routing tables (" << N + A*G << " nodes)...\n";
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  std::map<uint32_t, Address> addr;
  const uint16_t PORT = 9000;
  for (uint32_t i = 0; i < N; ++i)
    addr[i] = InetSocketAddress (serverIp[i], PORT);

  std::vector<Ptr<DfHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      Ptr<Socket> rx = Socket::CreateSocket (servers.Get (i),
                          TypeId::LookupByName ("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny (), PORT));
      Ptr<DfHost> a = CreateObject<DfHost> ();
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

  if (scenario == "incast")
    {
      uint32_t dst  = 0; double base = 1.0; uint32_t cnt = 0;
      for (uint32_t s = 1; s < N && cnt < incastFanin; ++s, ++cnt)
        for (uint32_t m = 0; m < 64; ++m)
          {
            double when = base + (cnt * 64 + m) * serSec;
            Simulator::Schedule (Seconds (when), &DfHost::Send, apps[s], dst, pktBytes, pid++);
          }
      std::cout << "  Scenario: incast fan-in=" << std::min (incastFanin, N-1) << " -> server 0\n";
    }
  else if (scenario == "allreduce")
    {
      double base = 1.0;
      for (uint32_t s = 0; s < N; ++s)
        {
          uint32_t dst = (s + 1) % N;
          for (uint32_t m = 0; m < 64; ++m)
            Simulator::Schedule (Seconds (base + m * serSec), &DfHost::Send, apps[s], dst, pktBytes, pid++);
        }
      std::cout << "  Scenario: allreduce ring (N=" << N << ")\n";
    }
  else if (scenario == "bisection")
    {
      double base = 1.0; uint32_t cnt = 0; uint32_t halfN = N / 2;
      for (uint32_t s = 0; s < halfN; ++s)
        for (uint32_t m = 0; m < 64; ++m, ++cnt)
          {
            uint32_t dst = halfN + s;
            Simulator::Schedule (Seconds (base + cnt * serSec), &DfHost::Send, apps[s], dst, pktBytes, pid++);
          }
      std::cout << "  Scenario: bisection (first " << G/2 << " groups -> last " << G - G/2 << " groups)\n";
    }
  else if (scenario == "nccl_ar")
    {
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
              Simulator::Schedule (Seconds (nextT[o.src]), &DfHost::Send,
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
          Simulator::Schedule (Seconds (when), &DfHost::Send, apps[s], d, pktBytes, pid++);
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
  double tputGbps = rxDurSec > 0 ? (delivered * pktBytes * 8.0) / (rxDurSec * 1e9) : 0.0;

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

  uint32_t nRouters = A * G;
  std::cout << "=== Results: scenario=" << scenario
            << "  a=" << A << " g=" << G << " N=" << N << " ===\n";
  std::cout << "sent=" << g_sent << "  delivered=" << delivered
            << "  dropped=" << dropped
            << " (" << (g_sent ? 100.0 * dropped / g_sent : 0.0) << "%)\n";
  std::cout << "  Throughput=" << tputGbps << " Gbps  Duration=" << rxDurSec << " s\n";
  std::cout << "  Latency mean=" << meanUs << " µs  p99=" << p99Us << " µs\n";

  std::ofstream csv ("dragonfly_ib_result.csv");
  csv << "scenario,a,p,h,g,N,routers,sent,delivered,dropped,throughput_Gbps,mean_us,p99_us\n";
  csv << scenario << "," << A << "," << P << "," << H << "," << G << "," << N
      << "," << nRouters << ","
      << g_sent << "," << delivered << "," << dropped << ","
      << tputGbps << "," << meanUs << "," << p99Us << "\n";
  std::cout << "wrote dragonfly_ib_result.csv\n";

  // ═══════════ 统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════════
  double jct = (g_inject_start > 0 && g_last_recv > 0)
               ? (g_last_recv / 1e9 - g_inject_start) : rxDurSec;
  double lineRateGbps = (double) dr.GetBitRate () / 1e9;
  uint64_t Lintra = (uint64_t) A * (A - 1) / 2 * G;
  uint64_t Lglobal = (uint64_t) G * (G - 1) / 2;

  EnergyModel em;
  em.inv = EnergyInventory::Dragonfly (N, nRouters, Lintra, Lglobal,
                                       lineRateGbps, /*gpuPerSrv=*/1);
  if (opticalFrac < 1.0)
    em.inv.nOpticalPorts = (uint64_t) (opticalFrac * em.inv.TotalPorts ());
  em.SetBits (g_total_bits);
  em.SetDuration (jct > 0 ? jct : simDuration);
  em.SetTraffic (delivered, pktBytes);
  em.SetThroughput (tputGbps);
  em.WriteCsv ("energy_unified.csv", "dragonfly", scenario);
  em.PrintSummary ("dragonfly", scenario);

  if (scenario == "nccl_ar")
    {
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      uint32_t Pranks = (uint32_t) cmp::StridedRanks (N, arRanks).size ();
      cmp::CmpRow row;
      row.topo = "dragonfly"; row.arRanks = Pranks; row.modelBytes = M;
      row.lineRateGbps = lineRateGbps; row.jctSec = jct;
      row.sent = g_sent; row.delivered = delivered; row.dropped = dropped;
      row.tputGbps = tputGbps;
      row.algbwGbps = cmp::AlgBwGbps (M, jct);
      row.busbwGbps = cmp::BusBwGbps (M, jct, Pranks);
      row.busbwEff  = lineRateGbps > 0 ? row.busbwGbps / lineRateGbps : 0;
      row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
      row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
      row.bisectionGbps = em.inv.bisectionGbps;
      row.pfcPausedSec = -1; row.pfcPeakKB = -1; row.pfcDrops = 0;   // IB credit,非 PFC
      cmp::WriteCmpCsv ("cmp_dragonfly.csv", row);
      std::cout << "wrote cmp_dragonfly.csv  (busbw=" << row.busbwGbps
                << " Gbps, eff=" << row.busbwEff << ")\n";
    }
  return 0;
}
