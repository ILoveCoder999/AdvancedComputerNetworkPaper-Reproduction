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

#include "mesh-route-header.h"
#include "link-credit.h"       // 逐跳链路级信用流控引擎(纯 C++ 模板)
// —— 接入：NCCL 流量层 + 统一能耗模型(mesh = 无交换机直连 → Switchless 清单) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("MeshSim");

// 每轴节点数 A:默认 36 → N=A²=1296(统一规模);可用 --side 调小做冒烟(如 --side=6→36)。
// 注意:2D 每轴全连在 A=36 下度数 =2(A-1)=70、链路 ≈45360,是全家族**最重**的一个
//       (建表/路由耗时与内存最大;这本身也说明 2D 全连不扩展)。冒烟务必用小 --side。
static uint32_t A = 36;
static uint32_t NID (uint32_t r, uint32_t c) { return r * A + c; }
static void RC (uint32_t id, uint32_t &r, uint32_t &c) { r = id / A; c = id % A; }

static std::vector<uint16_t> Dor (uint32_t s, uint32_t d)
{
  uint32_t rs, cs, rd, cd;
  RC (s, rs, cs);
  RC (d, rd, cd);
  if (s == d) return { (uint16_t) s };
  if (rs == rd || cs == cd) return { (uint16_t) s, (uint16_t) d };
  return { (uint16_t) s, (uint16_t) NID (rs, cd), (uint16_t) d };
}

struct MeshProbe
{
  double sentNs;
  double recvNs;
  uint16_t relays;
};
static std::map<uint64_t, MeshProbe> g_inflight;
static std::vector<MeshProbe> g_done;
static uint64_t g_sent = 0;
static uint64_t g_total_bits = 0;     // 全网链路发送总比特(MacTx 累计)，供能耗模型
static double   g_inject_start = -1.0; // all-reduce 注入起点(算 JCT)

// —— 信用回程需要按 nodeId 找到对端 MeshHost 并按链路时延投递 —— //
class MeshHost;
static std::vector<Ptr<MeshHost>>* g_apps = nullptr;
static Time g_creditDelay;

class MeshHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("MeshHost")
      .SetParent<Application> ()
      .AddConstructor<MeshHost> ();
    return t;
  }

  void Setup (uint32_t id, Ptr<Socket> rx, std::map<uint32_t, Address> addr,
              bool creditOn, uint32_t creditPkts)
  {
    m_id = id;
    m_rx = rx;
    m_addr = std::move (addr);
    m_lc.Configure (creditPkts, creditOn);
    m_lc.SetTx ([this](uint16_t nbr, const Ptr<Packet>& p){ SendTo (nbr, p); });
    m_lc.SetReturn ([this](uint16_t up){ ScheduleCreditTo (up); });
  }

  // 源发起：经信用引擎提交到第一跳(path[1])。
  void Send (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    if (m_id == dst) return;                 // src==dst guard
    std::vector<uint16_t> path = Dor (m_id, dst);
    MeshRouteHeader h;
    h.SetPath (path);
    Ptr<Packet> pkt = Create<Packet> (bytes);
    pkt->AddHeader (h);
    AppendId (pkt, id, dst);
    uint16_t relays = (path.size () >= 2) ? (uint16_t) (path.size () - 2) : 0;
    MeshProbe pr;
    pr.sentNs = Simulator::Now ().GetNanoSeconds ();   // 注入时刻(含信用等待 → JCT 反映反压)
    pr.recvNs = 0;
    pr.relays = relays;
    g_inflight[id] = pr;
    g_sent++;
    m_lc.Submit (path[1], pkt, LinkCredit<Ptr<Packet>>::NO_UP);  // 源无上游
  }

  void OnLinkCredit (uint16_t fromNbr) { m_lc.OnReturn (fromNbr); }

private:
  void StartApplication () override
  {
    m_rx->SetRecvCallback (MakeCallback (&MeshHost::OnRecv, this));
  }
  void StopApplication () override {}

  static void AppendId (Ptr<Packet> p, uint64_t id, uint32_t dst)
  {
    uint8_t b[12];
    std::memcpy (b, &id, 8);
    std::memcpy (b + 8, &dst, 4);
    p->AddAtEnd (Create<Packet> (b, 12));
  }

  void ScheduleCreditTo (uint16_t up)
  {
    if (up == LinkCredit<Ptr<Packet>>::NO_UP) return;
    if (!g_apps || up >= g_apps->size ()) return;
    Simulator::Schedule (g_creditDelay, &MeshHost::OnLinkCredit, (*g_apps)[up], m_id);
  }

  // FIX: 统一为 scan 式逐跳转发(与 mesh3d/mesh4d 一致),并接入逐跳信用。
  void OnRecv (Ptr<Socket> s)
  {
    Ptr<Packet> pkt;
    while ((pkt = s->Recv ()))
      {
        uint64_t id = 0;
        uint32_t fdst = 0;
        if (pkt->GetSize () >= 12)
          {
            uint8_t b[12];
            Ptr<Packet> t = pkt->CreateFragment (pkt->GetSize () - 12, 12);
            t->CopyData (b, 12);
            std::memcpy (&id, b, 8);
            std::memcpy (&fdst, b + 8, 4);
            pkt->RemoveAtEnd (12);
          }
        MeshRouteHeader h;
        pkt->RemoveHeader (h);
        const std::vector<uint16_t> &path = h.GetPath ();

        size_t myPos = path.size ();
        for (size_t i = 0; i < path.size (); ++i)
          if (path[i] == m_id) { myPos = i; break; }

        if (m_id == path.back ())          // 最终交付
          {
            std::map<uint64_t, MeshProbe>::iterator it = g_inflight.find (id);
            if (it != g_inflight.end ())
              {
                it->second.recvNs = Simulator::Now ().GetNanoSeconds ();
                g_done.push_back (it->second);
                g_inflight.erase (it);
              }
            if (myPos >= 1) ScheduleCreditTo (path[myPos - 1]);  // 交付即腾槽 → 回上游信用
            continue;
          }

        if (myPos != path.size () && myPos + 1 < path.size ())   // 中继转发
          {
            uint16_t next_hop = path[myPos + 1];
            h.SetCursor ((uint16_t) (myPos + 1));
            pkt->AddHeader (h);
            AppendId (pkt, id, fdst);
            uint16_t upstream = (myPos >= 1) ? path[myPos - 1]
                                             : LinkCredit<Ptr<Packet>>::NO_UP;
            m_lc.Submit (next_hop, pkt, upstream);
          }
      }
  }

  void SendTo (uint16_t nid, Ptr<Packet> pkt)
  {
    std::map<uint32_t, Address>::iterator it = m_addr.find (nid);
    if (it == m_addr.end ()) return;
    if (!m_tx)
      m_tx = Socket::CreateSocket (GetNode (),
               TypeId::LookupByName ("ns3::UdpSocketFactory"));
    m_tx->SendTo (pkt, 0, it->second);
  }

  uint32_t m_id {0};
  Ptr<Socket> m_rx;
  Ptr<Socket> m_tx;
  std::map<uint32_t, Address> m_addr;
  LinkCredit<Ptr<Packet>> m_lc;       // 逐跳链路信用
};

int main (int argc, char *argv[])
{
  std::string scenario = "incast";
  bool schedule = true;
  uint32_t fanin = 64;
  uint32_t pktBytes = 1024;
  uint32_t queuePkts = 64;
  std::string linkRate = "100Gbps";
  std::string linkDelay = "200ns";
  // —— 跨拓扑对比 all-reduce(nccl_ar) + 能耗清单参数 —— //
  uint32_t arRanks      = 32;      // all-reduce 逻辑 rank 数(跨网铺开)
  uint32_t modelMB      = 8;       // 每 rank 梯度/模型大小 M (MiB)
  uint32_t fpgaPerNode  = 8;       // 每 host 转发引擎数(switchless 能耗关键敏感项)
  double   opticalFrac  = 1.0;     // 走光端口占比(短直连可设<1 走 DAC 降耗)
  double   simStop      = 25.0;    // 仿真停止时刻(s)
  // —— 逐跳信用流控 —— //
  bool     credit       = true;    // 1=逐跳链路信用(无损); 0=有损直发(对照)
  uint32_t creditPkts   = 0;       // 每链路信用窗(包); 0=取 queuePkts

  CommandLine cmd;
  cmd.AddValue ("scenario", "uniform|incast|hotspot|relaycongest|nccl_ar", scenario);
  cmd.AddValue ("arRanks",  "nccl_ar: all-reduce rank count", arRanks);
  cmd.AddValue ("modelMB",  "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("fpgaPerNode", "switchless forwarding engines per host", fpgaPerNode);
  cmd.AddValue ("opticalFrac", "fraction of ports on optics (rest DAC)", opticalFrac);
  cmd.AddValue ("simStop",  "simulator stop time (s)", simStop);
  cmd.AddValue ("schedule", "1=paced (lossless), 0=burst (drops)", schedule);
  cmd.AddValue ("fanin", "incast fan-in", fanin);
  cmd.AddValue ("pktBytes", "payload bytes", pktBytes);
  cmd.AddValue ("queuePkts", "finite link queue depth (packets)", queuePkts);
  cmd.AddValue ("linkRate", "link rate", linkRate);
  cmd.AddValue ("linkDelay", "link delay", linkDelay);
  cmd.AddValue ("credit", "1=hop-by-hop link credit (lossless), 0=lossy", credit);
  cmd.AddValue ("creditPkts", "per-link credit window (pkts); 0=use queuePkts", creditPkts);
  cmd.AddValue ("side", "每轴节点数 A; N=side^2 (默认36=1296;冒烟用小值)", A);
  cmd.Parse (argc, argv);

  DataRate dr (linkRate);
  double lineRateBps = (double) dr.GetBitRate ();

  // 信用窗 = creditPkts(默认取 queuePkts);链路/队列盘深度取 max(queuePkts, 信用窗)。
  uint32_t cpk    = creditPkts ? creditPkts : queuePkts;
  uint32_t qDepth = std::max (queuePkts, cpk);
  g_creditDelay   = Time (linkDelay);

  uint32_t N = A * A;
  NodeContainer hosts;
  hosts.Create (N);
  InternetStackHelper inet;
  inet.Install (hosts);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay", StringValue (linkDelay));
  std::ostringstream qs;
  qs << qDepth << "p";
  p2p.SetQueue ("ns3::DropTailQueue<Packet>",
                "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::FifoQueueDisc",
                        "MaxSize", StringValue (qs.str ()));   // FIX: 由 8p 改为随 qDepth

  Ipv4AddressHelper ip;
  std::map<uint32_t, Address> addr;
  std::map<uint32_t, Ipv4Address> firstIp;
  const uint16_t PORT = 9000;
  uint32_t subnet = 0;

  std::function<void(uint32_t, uint32_t)> addEdge =
    [&] (uint32_t u, uint32_t v)
    {
      if (v <= u) return;
      NetDeviceContainer dev = p2p.Install (hosts.Get (u), hosts.Get (v));
      tch.Install (dev);
      dev.Get (0)->TraceConnectWithoutContext ("MacTx",
        MakeCallback (+[](Ptr<const Packet> p){ g_total_bits += p->GetSize () * 8; }));
      dev.Get (1)->TraceConnectWithoutContext ("MacTx",
        MakeCallback (+[](Ptr<const Packet> p){ g_total_bits += p->GetSize () * 8; }));
      std::ostringstream b;
      b << "10." << ((subnet >> 8) & 0xff) << "." << (subnet & 0xff) << ".0";
      ip.SetBase (b.str ().c_str (), "255.255.255.252");
      Ipv4InterfaceContainer ic = ip.Assign (dev);
      if (!firstIp.count (u)) firstIp[u] = ic.GetAddress (0);
      if (!firstIp.count (v)) firstIp[v] = ic.GetAddress (1);
      ++subnet;
    };

  for (uint32_t r = 0; r < A; ++r)
    for (uint32_t c = 0; c < A; ++c)
      {
        uint32_t u = NID (r, c);
        for (uint32_t c2 = 0; c2 < A; ++c2)
          if (c2 != c) addEdge (u, NID (r, c2));
        for (uint32_t r2 = 0; r2 < A; ++r2)
          if (r2 != r) addEdge (u, NID (r2, c));
      }
  uint64_t nLinks = subnet;

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  for (uint32_t i = 0; i < N; ++i)
    addr[i] = InetSocketAddress (firstIp[i], PORT);

  std::vector<Ptr<MeshHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      Ptr<Socket> rx = Socket::CreateSocket (hosts.Get (i),
                          TypeId::LookupByName ("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny (), PORT));
      Ptr<MeshHost> a = CreateObject<MeshHost> ();
      a->Setup (i, rx, addr, credit, cpk);
      hosts.Get (i)->AddApplication (a);
      a->SetStartTime (Seconds (0.0));
      a->SetStopTime (Seconds (30.0));
      apps[i] = a;
    }
  g_apps = &apps;

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  uint64_t id = 1;
  double serSec = (pktBytes * 8.0) / lineRateBps;   // FIX: 用实际线速(原硬编码 100e9)

  if (scenario == "incast")
    {
      uint32_t dst = NID (A / 2, A / 2);
      double base = 1.0;
      for (uint32_t k = 0; k < fanin; ++k)
        {
          uint32_t s = (dst + 1 + k) % N;
          for (uint32_t m = 0; m < 64; ++m)
            {
              double when = schedule ? base + (k * 64 + m) * serSec
                                     : base + (m * 1e-9);
              Simulator::Schedule (Seconds (when), &MeshHost::Send, apps[s],
                                   dst, pktBytes, id++);
            }
        }
    }
  else if (scenario == "relaycongest")
    {
      uint32_t rd = 8, cd = 8, RX = 3;
      uint32_t D = NID (rd, cd);
      double base = 1.0;
      for (uint32_t c = 0; c < A; ++c)
        {
          if (c == cd) continue;
          uint32_t s = NID (RX, c);
          for (uint32_t m = 0; m < 256; ++m)
            {
              double when = schedule ? base + (c * 256 + m) * serSec
                                     : base + (m * 1e-9);
              Simulator::Schedule (Seconds (when), &MeshHost::Send, apps[s],
                                   D, pktBytes, id++);
            }
        }
    }
  else if (scenario == "hotspot")
    {
      std::vector<uint32_t> hot = { NID (0,0), NID (0,1), NID (1,0), NID (1,1) };
      double base = 1.0; uint32_t cnt = 0;
      for (uint32_t s = 0; s < N; ++s)
        for (uint32_t j = 0; j < hot.size (); ++j)
          if (s != hot[j])
            {
              double when = schedule ? base + (cnt++) * serSec : base + 1e-9;
              Simulator::Schedule (Seconds (when), &MeshHost::Send, apps[s],
                                   hot[j], pktBytes, id++);
            }
    }
  else if (scenario == "nccl_ar")
    {
      // 跨拓扑对比工作负载：真 NCCL Ring AllReduce(arRanks 个 rank 跨网铺开)。
      // 源按线速起拍注入,网内拥塞由逐跳信用反压(无损)。
      uint64_t Mbytes = (uint64_t) modelMB * 1024 * 1024;
      std::vector<uint32_t> ranks;
      std::vector<nccl::CommOp> ops = cmp::BuildComparisonAllReduce (N, arRanks, Mbytes, &ranks);
      std::vector<double> nextT (N, 1.0);
      uint64_t injected = 0;
      for (const auto &o : ops)
        {
          if (o.onNvlink) continue;
          uint32_t nPk = (uint32_t) nccl::ToPackets (o.bytes, pktBytes);
          for (uint32_t k = 0; k < nPk; ++k)
            {
              Simulator::Schedule (Seconds (nextT[o.src]), &MeshHost::Send,
                                   apps[o.src], o.dst, pktBytes, id++);
              nextT[o.src] += serSec;
              ++injected;
            }
        }
      g_inject_start = 1.0;
      std::cout << "  Scenario: nccl_ar Ring AllReduce  arRanks=" << ranks.size ()
                << "  M=" << modelMB << "MiB  ops=" << ops.size ()
                << "  pkts=" << injected << "\n";
    }
  else
    {
      double when = 1.0;
      for (uint32_t k = 0; k < 2000; ++k)
        {
          uint32_t s = rng->GetInteger (0, N - 1);
          uint32_t d = rng->GetInteger (0, N - 1);
          if (s == d) continue;
          Simulator::Schedule (Seconds (when), &MeshHost::Send, apps[s],
                               d, pktBytes, id++);
          when += 1e-6;
        }
    }

  Simulator::Stop (Seconds (simStop));
  std::cout << "running simulation ... (credit=" << credit
            << " window=" << cpk << "p, queue=" << qDepth << "p, links=" << nLinks << ")\n";
  Simulator::Run ();
  Simulator::Destroy ();

  uint64_t delivered = g_done.size ();
  uint64_t dropped = (g_sent >= delivered) ? (g_sent - delivered) : 0;

  std::map<uint16_t, std::vector<double>> byRelay;
  for (uint32_t i = 0; i < g_done.size (); ++i)
    if (g_done[i].recvNs > 0)
      byRelay[g_done[i].relays].push_back (g_done[i].recvNs - g_done[i].sentNs);

  std::cout << "\n=== scenario=" << scenario << " credit=" << credit
            << " fanin=" << fanin << " window=" << cpk << "p ===\n";
  std::cout << "sent=" << g_sent << " delivered=" << delivered
            << " dropped=" << dropped
            << " (" << (g_sent ? (100.0 * dropped / g_sent) : 0) << "%)"
            << (credit ? "  [credit 无损:dropped 应=0]" : "  [credit off:有损对照]") << "\n";
  std::cout << "relay  count   mean_us   p99_us\n";
  for (std::map<uint16_t, std::vector<double>>::iterator kv = byRelay.begin ();
       kv != byRelay.end (); ++kv)
    {
      std::vector<double> v = kv->second;
      std::sort (v.begin (), v.end ());
      double sum = 0;
      for (uint32_t i = 0; i < v.size (); ++i) sum += v[i];
      double mean = sum / v.size ();
      double p99 = v[std::min ((size_t) (v.size () - 1), v.size () * 99 / 100)];
      printf ("%3u   %6zu  %8.3f  %8.3f\n", kv->first, v.size (),
              mean / 1000, p99 / 1000);
    }

  std::ofstream csv ("mesh_result.csv");
  csv << "scenario,credit,fanin,sent,delivered,dropped\n";
  csv << scenario << "," << credit << "," << fanin << ","
      << g_sent << "," << delivered << "," << dropped << "\n";
  std::cout << "wrote mesh_result.csv\n";

  // ═══════════ 统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════════
  // 收包时段 + JCT(从 g_done 取 first/last 交付时刻)。
  double firstRecvNs = -1, lastRecvNs = -1;
  for (const auto &p : g_done)
    if (p.recvNs > 0)
      {
        if (firstRecvNs < 0 || p.recvNs < firstRecvNs) firstRecvNs = p.recvNs;
        if (p.recvNs > lastRecvNs) lastRecvNs = p.recvNs;
      }
  double rxDurSec = (lastRecvNs > firstRecvNs && firstRecvNs > 0)
                    ? (lastRecvNs - firstRecvNs) / 1e9 : 0.0;
  double jct = (g_inject_start > 0 && lastRecvNs > 0)
               ? (lastRecvNs / 1e9 - g_inject_start) : rxDurSec;
  double tputGbps = rxDurSec > 0
                    ? (delivered * pktBytes * 8.0) / (rxDurSec * 1e9) : 0.0;

  double lineRateGbps = lineRateBps / 1e9;
  uint32_t bisLinks = A * ((A / 2) * (A - A / 2)); // 平衡列切割估计:A=17 → 17·(8·9)=1224

  EnergyModel em;
  // mesh = 无交换机直连：Switchless 清单(端口/光模块/FPGA 如实计入,不让"去交换机"白占便宜)。
  em.inv = EnergyInventory::Switchless (N, nLinks, fpgaPerNode,
                                        lineRateGbps, opticalFrac, bisLinks);
  em.SetBits (g_total_bits);
  em.SetDuration (jct > 0 ? jct : simStop);
  em.SetTraffic (delivered, pktBytes);
  em.SetThroughput (tputGbps);
  em.WriteCsv ("energy_unified.csv", "mesh2d", scenario);
  em.PrintSummary ("mesh2d", scenario);

  if (scenario == "nccl_ar")
    {
      uint64_t Mbytes = (uint64_t) modelMB * 1024 * 1024;
      uint32_t Pranks = (uint32_t) cmp::StridedRanks (N, arRanks).size ();
      cmp::CmpRow row;
      row.topo = "mesh2d"; row.arRanks = Pranks; row.modelBytes = Mbytes;
      row.lineRateGbps = lineRateGbps; row.jctSec = jct;
      row.sent = g_sent; row.delivered = delivered; row.dropped = dropped;
      row.tputGbps = tputGbps;
      row.algbwGbps = cmp::AlgBwGbps (Mbytes, jct);
      row.busbwGbps = cmp::BusBwGbps (Mbytes, jct, Pranks);
      row.busbwEff  = lineRateGbps > 0 ? row.busbwGbps / lineRateGbps : 0;
      row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
      row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
      row.bisectionGbps = em.inv.bisectionGbps;
      // mesh 用逐跳信用/DOR,非 PFC fabric → PFC 字段保持 N/A(-1)
      row.pfcPausedSec = -1; row.pfcPeakKB = -1; row.pfcDrops = 0;
      cmp::WriteCmpCsv ("cmp_mesh2d.csv", row);
      std::cout << "wrote cmp_mesh2d.csv  (busbw=" << row.busbwGbps
                << " Gbps, eff=" << row.busbwEff << ")\n";
    }
  return 0;
}
