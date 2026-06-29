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
// —— 接入：NCCL 流量层 + 统一能耗模型(3D mesh = 无交换机直连 → Switchless) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("Mesh3dSim");

// 每轴节点数 M(mesh 维度,非模型大小):默认 11 → N=11³=1331;--side 可调小做冒烟。
static uint32_t M = 11;
static uint32_t NID (uint32_t x, uint32_t y, uint32_t z) { return (x*M + y)*M + z; }
static void XYZ (uint32_t i, uint32_t &x, uint32_t &y, uint32_t &z)
{ z = i % M; i /= M; y = i % M; x = i / M; }

static std::vector<uint16_t> Dor3 (uint32_t s, uint32_t d)
{
  uint32_t sx, sy, sz, dx, dy, dz;
  XYZ (s, sx, sy, sz);
  XYZ (d, dx, dy, dz);
  std::vector<uint16_t> path;
  path.push_back ((uint16_t) s);
  uint32_t cx = sx, cy = sy, cz = sz;
  if (cx != dx) { cx = dx; path.push_back ((uint16_t) NID (cx, cy, cz)); }
  if (cy != dy) { cy = dy; path.push_back ((uint16_t) NID (cx, cy, cz)); }
  if (cz != dz) { cz = dz; path.push_back ((uint16_t) NID (cx, cy, cz)); }
  return path;
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

static double g_first_recv_time = -1.0;
static double g_last_recv_time = -1.0;
// FIX: 改由 MacTx trace 写入，与 fat-tree/RRG 统计口径一致
static uint64_t g_total_bits_transmitted = 0;
static double   g_inject_start = -1.0;   // all-reduce 注入起点(算 JCT)

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
    if (m_id == dst) return;                 // src==dst guard，防 path[1] 越界
    std::vector<uint16_t> path = Dor3 (m_id, dst);
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
            double nowNs = Simulator::Now ().GetNanoSeconds ();
            if (g_first_recv_time < 0) g_first_recv_time = nowNs;
            g_last_recv_time = nowNs;

            std::map<uint64_t, MeshProbe>::iterator it = g_inflight.find (id);
            if (it != g_inflight.end ())
              {
                it->second.recvNs = nowNs;
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
            m_lc.Submit (next_hop, pkt, upstream);   // 经信用引擎(departs 时回上游信用)
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
    // 比特由 MacTx trace 统计
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
  std::string scenario = "uniform";
  bool schedule = true;
  uint32_t pktBytes = 1024;
  uint32_t queuePkts = 8;
  uint32_t uniformFlows = 200000;
  std::string linkRate = "100Gbps";
  std::string linkDelay = "200ns";
  // —— 跨拓扑对比 all-reduce(nccl_ar) + 能耗清单参数 —— //
  uint32_t arRanks      = 32;      // all-reduce 逻辑 rank 数(跨网铺开)
  uint32_t modelMB      = 8;       // 每 rank 梯度/模型大小 M (MiB)
  uint32_t fpgaPerNode  = 8;       // switchless 每节点转发引擎数(能耗敏感)
  double   opticalFrac  = 1.0;     // 走光端口占比
  double   simStop      = -1.0;    // 仿真停止时刻(s); -1=自动(nccl_ar→25, 其它→3)
  // —— 逐跳信用流控 —— //
  bool     credit       = true;    // 1=逐跳链路信用(无损); 0=有损直发(对照)
  uint32_t creditPkts   = 0;       // 每链路信用窗(包); 0=取 queuePkts

  CommandLine cmd;
  cmd.AddValue ("scenario", "uniform|relaycongest|nccl_ar", scenario);
  cmd.AddValue ("schedule", "1=paced (lossless), 0=burst (drops)", schedule);
  cmd.AddValue ("pktBytes", "payload bytes", pktBytes);
  cmd.AddValue ("queuePkts", "finite queue depth (packets)", queuePkts);
  cmd.AddValue ("uniformFlows", "number of random flows", uniformFlows);
  cmd.AddValue ("linkRate", "link rate", linkRate);
  cmd.AddValue ("linkDelay", "link delay", linkDelay);
  cmd.AddValue ("arRanks",  "nccl_ar: all-reduce rank count", arRanks);
  cmd.AddValue ("modelMB",  "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("fpgaPerNode", "switchless forwarding engines per node", fpgaPerNode);
  cmd.AddValue ("opticalFrac", "fraction of ports on optics (rest DAC)", opticalFrac);
  cmd.AddValue ("simStop",  "simulator stop time (s); -1=auto", simStop);
  cmd.AddValue ("credit", "1=hop-by-hop link credit (lossless), 0=lossy", credit);
  cmd.AddValue ("creditPkts", "per-link credit window (pkts); 0=use queuePkts", creditPkts);
  cmd.AddValue ("side", "每轴节点数 M; N=side^3 (默认11=1331;冒烟用小值)", M);
  cmd.Parse (argc, argv);

  DataRate dr (linkRate);
  double lineRateBps = (double) dr.GetBitRate ();

  // 信用窗 = creditPkts(默认取 queuePkts);链路/队列盘深度取 max(queuePkts, 信用窗)。
  uint32_t cpk    = creditPkts ? creditPkts : queuePkts;
  uint32_t qDepth = std::max (queuePkts, cpk);
  g_creditDelay   = Time (linkDelay);

  uint32_t N = M * M * M;
  std::cout << "building " << M << "x" << M << "x" << M << " = " << N
            << " host 3D mesh ...\n";

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
                        "MaxSize", StringValue (qs.str ()));

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
      // FIX: MacTx trace 统计链路层比特数
      dev.Get (0)->TraceConnectWithoutContext ("MacTx",
        MakeCallback (+[](Ptr<const Packet> p){ g_total_bits_transmitted += p->GetSize () * 8; }));
      dev.Get (1)->TraceConnectWithoutContext ("MacTx",
        MakeCallback (+[](Ptr<const Packet> p){ g_total_bits_transmitted += p->GetSize () * 8; }));
      uint32_t base = subnet * 4;
      std::ostringstream b;
      b << "10." << ((base >> 16) & 0xff) << "." << ((base >> 8) & 0xff)
        << "." << (base & 0xff);
      ip.SetBase (b.str ().c_str (), "255.255.255.252");
      Ipv4InterfaceContainer ic = ip.Assign (dev);
      if (!firstIp.count (u)) firstIp[u] = ic.GetAddress (0);
      if (!firstIp.count (v)) firstIp[v] = ic.GetAddress (1);
      ++subnet;
    };

  for (uint32_t x = 0; x < M; ++x)
    for (uint32_t y = 0; y < M; ++y)
      for (uint32_t z = 0; z < M; ++z)
        {
          uint32_t u = NID (x, y, z);
          for (uint32_t x2 = 0; x2 < M; ++x2)
            if (x2 != x) addEdge (u, NID (x2, y, z));
          for (uint32_t y2 = 0; y2 < M; ++y2)
            if (y2 != y) addEdge (u, NID (x, y2, z));
          for (uint32_t z2 = 0; z2 < M; ++z2)
            if (z2 != z) addEdge (u, NID (x, y, z2));
        }
  uint64_t nLinks = subnet;
  std::cout << "  links built: " << nLinks << "  (credit=" << credit
            << " window=" << cpk << "p, queue=" << qDepth << "p)\n";

  std::cout << "  populating routing tables (may take a minute) ...\n";
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
  // FIX: 用实际线速换算序列化时间(原硬编码 100e9)
  double serSec = (pktBytes * 8.0) / lineRateBps;

  if (scenario == "relaycongest")
    {
      uint32_t dy = 5, dz = 5, DX = 8;
      uint32_t D = NID (DX, dy, dz);
      double base = 1.0;
      uint32_t flow = 0;
      for (uint32_t x = 0; x < M; ++x)
        {
          if (x == DX) continue;
          uint32_t s = NID (x, dy, dz);
          for (uint32_t m = 0; m < 256; ++m)
            {
              double when = schedule ? base + (flow * 256 + m) * serSec
                                     : base + (m * 1e-9);
              Simulator::Schedule (Seconds (when), &MeshHost::Send, apps[s],
                                   D, pktBytes, id++);
            }
          ++flow;
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
              nextT[o.src] += serSec; ++injected;
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
      for (uint32_t k = 0; k < uniformFlows; ++k)
        {
          uint32_t s = rng->GetInteger (0, N - 1);
          uint32_t d = rng->GetInteger (0, N - 1);
          if (s == d) continue;
          Simulator::Schedule (Seconds (when), &MeshHost::Send, apps[s],
                               d, pktBytes, id++);
          when += 5e-7;
        }
    }

  // nccl_ar 默认跑久一点等 all-reduce 收完;其它场景保持原 3s
  double simDuration = (simStop > 0) ? simStop
                                     : (scenario == "nccl_ar" ? 25.0 : 3.0);
  Simulator::Stop (Seconds (simDuration));
  std::cout << "running simulation ...\n";
  Simulator::Run ();
  Simulator::Destroy ();

  uint64_t delivered = g_done.size ();
  uint64_t dropped = (g_sent >= delivered) ? (g_sent - delivered) : 0;

  double rxDurationSec = 0.0;
  double throughputGbps = 0.0;
  if (g_last_recv_time > g_first_recv_time && g_first_recv_time > 0)
    {
      rxDurationSec = (g_last_recv_time - g_first_recv_time) / 1e9;
      throughputGbps = (delivered * pktBytes * 8.0) / (rxDurationSec * 1e9);
    }

  double P_static_node = 2000.0;
  double P_full_load   = 8000.0;
  double P_compute     = P_full_load - P_static_node;
  double E_dynamic_bit = 10e-12;

  double staticEnergy = N * P_static_node * simDuration;
  double computeEnergy = 0.0;
  if (rxDurationSec > 0)
    computeEnergy = N * P_compute * rxDurationSec;
  double dynamicNetworkEnergy = g_total_bits_transmitted * E_dynamic_bit;
  double totalEnergy = staticEnergy + computeEnergy + dynamicNetworkEnergy;
  double avgPowerW   = totalEnergy / simDuration;

  std::map<uint16_t, std::vector<double>> byRelay;
  for (uint32_t i = 0; i < g_done.size (); ++i)
    if (g_done[i].recvNs > 0)
      byRelay[g_done[i].relays].push_back (g_done[i].recvNs - g_done[i].sentNs);

  std::cout << "\n=== 3D mesh " << M << "^3=" << N
            << " scenario=" << scenario << " credit=" << credit
            << " window=" << cpk << "p ===\n";
  std::cout << "sent=" << g_sent << " delivered=" << delivered
            << " dropped=" << dropped
            << " (" << (g_sent ? (100.0 * dropped / g_sent) : 0) << "%)"
            << (credit ? "  [credit 无损:dropped 应=0]" : "  [credit off:有损对照]") << "\n";

  std::cout << "-------------------------------------------\n";
  std::cout << "吞吐量 (Throughput)     : " << throughputGbps << " Gbps\n";
  std::cout << "接收有效时长 (Duration)  : " << rxDurationSec << " s\n";
  std::cout << "全网总能耗 (Total Energy): " << totalEnergy << " J  (本地粗模型, 仅参考)\n";
  std::cout << "  ├─ 服务器基础静态能耗(Static) : " << staticEnergy << " J (2000W 基础)\n";
  std::cout << "  ├─ GPU满载额外计算能耗(Compute): " << computeEnergy << " J (突发 8000W 增量)\n";
  std::cout << "  └─ 网卡/光模块动态传输(Network): " << dynamicNetworkEnergy << " J\n";
  std::cout << "全网平均总功耗 (Avg Power): " << avgPowerW << " W\n";
  std::cout << "-------------------------------------------\n";

  std::cout << "relay  count   mean_us   p99_us   max_us\n";
  for (std::map<uint16_t, std::vector<double>>::iterator kv = byRelay.begin ();
       kv != byRelay.end (); ++kv)
    {
      std::vector<double> v = kv->second;
      std::sort (v.begin (), v.end ());
      double sum = 0;
      for (uint32_t i = 0; i < v.size (); ++i) sum += v[i];
      double mean = sum / v.size ();
      double p99 = v[std::min ((size_t) (v.size () - 1), v.size () * 99 / 100)];
      double max_val = v.back ();
      printf ("%3u   %6zu  %8.3f  %8.3f  %8.3f\n", kv->first, v.size (),
              mean / 1000, p99 / 1000, max_val / 1000);
    }

  std::ofstream csv ("mesh3d_result.csv");
  csv << "scenario,credit,sent,delivered,dropped,throughput_Gbps,total_energy_J,avg_power_W\n";
  csv << scenario << "," << credit << ","
      << g_sent << "," << delivered << "," << dropped << ","
      << throughputGbps << "," << totalEnergy << "," << avgPowerW << "\n";
  std::cout << "wrote mesh3d_result.csv\n";

  // ═══════════ 统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════════
  // 注意:本文件全局 M 是 mesh 维度(=11),模型大小用 Mbytes,勿混。
  double jct = (g_inject_start > 0 && g_last_recv_time > 0)
               ? (g_last_recv_time / 1e9 - g_inject_start) : rxDurationSec;
  double lineRateGbps = lineRateBps / 1e9;
  uint32_t bisLinks = M * M * ((M / 2) * (M - M / 2));   // 沿 x 轴平衡割:121·(5·6)=3630

  EnergyModel em;
  // 3D mesh = 无交换机直连:Switchless 清单(端口/光模块/FPGA 如实计入)。
  em.inv = EnergyInventory::Switchless (N, nLinks, fpgaPerNode,
                                        lineRateGbps, opticalFrac, bisLinks);
  em.SetBits (g_total_bits_transmitted);
  em.SetDuration (jct > 0 ? jct : simDuration);
  em.SetTraffic (delivered, pktBytes);
  em.SetThroughput (throughputGbps);
  em.WriteCsv ("energy_unified.csv", "mesh3d", scenario);
  em.PrintSummary ("mesh3d", scenario);

  if (scenario == "nccl_ar")
    {
      uint64_t Mbytes = (uint64_t) modelMB * 1024 * 1024;
      uint32_t Pranks = (uint32_t) cmp::StridedRanks (N, arRanks).size ();
      cmp::CmpRow row;
      row.topo = "mesh3d"; row.arRanks = Pranks; row.modelBytes = Mbytes;
      row.lineRateGbps = lineRateGbps; row.jctSec = jct;
      row.sent = g_sent; row.delivered = delivered; row.dropped = dropped;
      row.tputGbps = throughputGbps;
      row.algbwGbps = cmp::AlgBwGbps (Mbytes, jct);
      row.busbwGbps = cmp::BusBwGbps (Mbytes, jct, Pranks);
      row.busbwEff  = lineRateGbps > 0 ? row.busbwGbps / lineRateGbps : 0;
      row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
      row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
      row.bisectionGbps = em.inv.bisectionGbps;
      row.pfcPausedSec = -1; row.pfcPeakKB = -1; row.pfcDrops = 0;   // 信用 fabric 非 PFC
      cmp::WriteCmpCsv ("cmp_mesh3d.csv", row);
      std::cout << "wrote cmp_mesh3d.csv  (busbw=" << row.busbwGbps
                << " Gbps, eff=" << row.busbwEff << ")\n";
    }
  return 0;
}
