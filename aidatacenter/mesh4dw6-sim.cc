/*
 * mesh4dw6-sim.cc — 四维"每轴全连"直连网格 (Hamming graph H(4,W), W=6 → N=6^4=1296)
 * ─────────────────────────────────────────────────────────────────────────
 * 无交换机直连架构：每个节点沿 4 个维度各与同轴其余所有节点直连(每轴 K_W 完全图)。
 *   度数 = 4·(W-1) = 20；无向链路总数 = N·20/2 = 12960。
 * 路由：4D 维序路由 (DOR: X→Y→Z→W)，源路由头 MeshRouteHeader 逐跳转发。
 *
 * 流控：逐跳链路级信用(link-credit.h)。每条 self→nbr 链路有 creditPkts 个信用,
 *   发包前必须持信用,否则在该链路队列等待;下游把包再转发/交付后回信用。DOR 信道
 *   依赖无环 → 无死锁;包永不因缓冲溢出被丢(无损),JCT 真实反映中继链路反压,
 *   从而与 leaf-spine 的 PFC 等价可比。--credit=0 退回有损直发(对照)。
 *
 * 对比工作负载: --scenario=nccl_ar 跑真 NCCL Ring AllReduce(arRanks 跨网铺开),
 *   统一 energy-model.h 的 Switchless 能耗,写 cmp_mesh4d.csv / energy_unified.csv。
 * ─────────────────────────────────────────────────────────────────────────
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

#include "mesh-route-header.h"
#include "link-credit.h"       // 逐跳链路级信用流控引擎(纯 C++ 模板)
// —— 接入：NCCL 流量层 + 统一能耗模型(无交换机直连) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("Mesh4dSim");

// 每轴节点数 W：默认 6 → N=6^4=1296;可用 --side 运行时调小做冒烟(如 --side=3→81)。
static uint32_t W = 6;

static uint32_t NID (uint32_t x, uint32_t y, uint32_t z, uint32_t w)
{
  return (((x * W + y) * W + z) * W) + w;
}
static void XYZW (uint32_t i, uint32_t &x, uint32_t &y, uint32_t &z, uint32_t &w)
{
  w = i % W; i /= W;
  z = i % W; i /= W;
  y = i % W;
  x = i / W;
}

// 四维维度渐进路由算法 (4D DOR: X -> Y -> Z -> W)
static std::vector<uint16_t> Dor4 (uint32_t s, uint32_t d)
{
  uint32_t sx, sy, sz, sw, dx, dy, dz, dw;
  XYZW (s, sx, sy, sz, sw);
  XYZW (d, dx, dy, dz, dw);
  std::vector<uint16_t> path;
  path.push_back ((uint16_t) s);

  uint32_t cx = sx, cy = sy, cz = sz, cw = sw;
  if (cx != dx) { cx = dx; path.push_back ((uint16_t) NID (cx, cy, cz, cw)); }
  if (cy != dy) { cy = dy; path.push_back ((uint16_t) NID (cx, cy, cz, cw)); }
  if (cz != dz) { cz = dz; path.push_back ((uint16_t) NID (cx, cy, cz, cw)); }
  if (cw != dw) { cw = dw; path.push_back ((uint16_t) NID (cx, cy, cz, cw)); }
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
// all-reduce 注入起点(算 JCT)；非 nccl_ar 时为 -1
static double g_inject_start = -1.0;

// —— 信用回程需要按 nodeId 找到对端 MeshHost 并按链路时延投递 —— //
class MeshHost;
static std::vector<Ptr<MeshHost>>* g_apps = nullptr;   // main 中指向 apps
static Time g_creditDelay;                             // = 链路传播时延

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
    std::vector<uint16_t> path = Dor4 (m_id, dst);
    MeshRouteHeader h;
    h.SetPath (path);
    Ptr<Packet> pkt = Create<Packet> (bytes);
    pkt->AddHeader (h);
    AppendId (pkt, id, dst);
    uint16_t relays = (path.size () >= 2) ? (uint16_t) (path.size () - 2) : 0;
    MeshProbe pr;
    pr.sentNs = Simulator::Now ().GetNanoSeconds ();   // 注入时刻(含后续信用等待 → JCT 反映反压)
    pr.recvNs = 0;
    pr.relays = relays;
    g_inflight[id] = pr;
    g_sent++;
    m_lc.Submit (path[1], pkt, LinkCredit<Ptr<Packet>>::NO_UP);  // 源无上游
  }

  // 收到"本节点→fromNbr 链路回 1 信用"
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

  // 给上游回信用:延迟一个链路时延后,调用上游的 OnLinkCredit(本节点 id)
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

        // 定位本节点在 path 中的位置(scan,与 mesh3d 口径一致)
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
  // —— 跨拓扑对比 all-reduce(nccl_ar) —— //
  uint32_t arRanks  = 32;     // all-reduce 逻辑 rank 数(跨网均匀铺开)
  uint32_t modelMB  = 8;      // 每 rank 梯度/模型大小 M (MiB)
  double   simStop  = -1.0;   // 仿真停止时刻(s); -1=自动(nccl_ar→25, 其它→1.5)
  // —— 无交换机能耗清单参数 —— //
  uint32_t fpgaPerNode = 8;   // 每节点 FPGA 转发引擎数(能耗敏感)
  double   opticalFrac = 1.0; // 走光端口占比(短直连可<1 走 DAC)
  // —— 逐跳信用流控 —— //
  bool     credit     = true; // 1=逐跳链路信用(无损); 0=有损直发(对照)
  uint32_t creditPkts = 0;    // 每链路信用窗(包); 0=取 queuePkts

  CommandLine cmd;
  cmd.AddValue ("scenario", "uniform|relaycongest|nccl_ar", scenario);
  cmd.AddValue ("schedule", "1=paced (lossless), 0=burst (drops)", schedule);
  cmd.AddValue ("pktBytes", "payload bytes", pktBytes);
  cmd.AddValue ("queuePkts", "finite queue depth (packets); nccl_ar 建议 256", queuePkts);
  cmd.AddValue ("uniformFlows", "number of random flows", uniformFlows);
  cmd.AddValue ("linkRate", "link rate", linkRate);
  cmd.AddValue ("linkDelay", "link delay", linkDelay);
  cmd.AddValue ("arRanks", "nccl_ar: all-reduce rank count", arRanks);
  cmd.AddValue ("modelMB", "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("simStop", "simulator stop time (s); -1=auto", simStop);
  cmd.AddValue ("fpgaPerNode", "switchless 每节点转发引擎数(能耗)", fpgaPerNode);
  cmd.AddValue ("opticalFrac", "走光端口占比 [0,1]", opticalFrac);
  cmd.AddValue ("credit", "1=hop-by-hop link credit (lossless), 0=lossy", credit);
  cmd.AddValue ("creditPkts", "per-link credit window (pkts); 0=use queuePkts", creditPkts);
  cmd.AddValue ("side", "每轴节点数 W; N=side^4 (默认6=1296;冒烟用小值)", W);
  cmd.Parse (argc, argv);

  DataRate dr (linkRate);
  double lineRateBps = (double) dr.GetBitRate ();

  // 信用窗 = creditPkts(默认取 queuePkts);链路/队列盘深度取 max(queuePkts, 信用窗)
  // 以保证"在飞包数(≤信用窗) ≤ 队列容量" → 数据永不被 DropTail 丢(无损靠信用)。
  uint32_t cpk    = creditPkts ? creditPkts : queuePkts;
  uint32_t qDepth = std::max (queuePkts, cpk);
  g_creditDelay   = Time (linkDelay);

  uint32_t N = W * W * W * W;
  std::cout << "building 4D mesh: " << W << "^4 = " << N << " hosts ...\n";

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

  for (uint32_t x = 0; x < W; ++x)
    for (uint32_t y = 0; y < W; ++y)
      for (uint32_t z = 0; z < W; ++z)
        for (uint32_t w = 0; w < W; ++w)
          {
            uint32_t u = NID (x, y, z, w);
            for (uint32_t x2 = 0; x2 < W; ++x2)
              if (x2 != x) addEdge (u, NID (x2, y, z, w));
            for (uint32_t y2 = 0; y2 < W; ++y2)
              if (y2 != y) addEdge (u, NID (x, y2, z, w));
            for (uint32_t z2 = 0; z2 < W; ++z2)
              if (z2 != z) addEdge (u, NID (x, y, z2, w));
            for (uint32_t w2 = 0; w2 < W; ++w2)
              if (w2 != w) addEdge (u, NID (x, y, z, w2));
          }
  uint64_t nLinks = subnet;     // 无向链路总条数(每轴 K_W 完全图; =N·4(W-1)/2)
  std::cout << "  4D links built: " << nLinks << "  (credit=" << credit
            << " window=" << cpk << "p, queue=" << qDepth << "p)\n";

  std::cout << "  populating routing tables (this WILL take a moment for 1296 nodes)... \n";
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
  g_apps = &apps;          // 供信用回程按 nodeId 投递

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  uint64_t id = 1;
  // FIX: 用实际线速换算序列化时间(原硬编码 100e9)
  double serSec = (pktBytes * 8.0) / lineRateBps;

  if (scenario == "nccl_ar")
    {
      // 跨拓扑对比工作负载：真 NCCL Ring AllReduce(arRanks 个 rank 跨网铺开)。
      // 源按线速起拍注入,网内拥塞由逐跳信用反压(无损)。
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      std::vector<uint32_t> ranks;
      std::vector<nccl::CommOp> ops = cmp::BuildComparisonAllReduce (N, arRanks, M, &ranks);
      std::map<uint32_t, double> nextT;     // 每源下一注入时刻
      uint64_t injected = 0;
      for (const auto &o : ops)
        {
          if (o.onNvlink) continue;
          uint32_t nPk = (uint32_t) nccl::ToPackets (o.bytes, pktBytes);
          for (uint32_t k = 0; k < nPk; ++k)
            {
              double &t = nextT[o.src];
              if (t < 1.0) t = 1.0;                    // 全体注入起点 = 1.0s
              Simulator::Schedule (Seconds (t), &MeshHost::Send, apps[o.src],
                                   o.dst, pktBytes, id++);
              t += serSec;
              ++injected;
            }
        }
      g_inject_start = 1.0;
      std::cout << "  scenario=nccl_ar Ring AllReduce  arRanks=" << ranks.size ()
                << "  M=" << modelMB << "MiB  ops=" << ops.size ()
                << "  pkts=" << injected << "\n";
    }
  else if (scenario == "relaycongest")
    {
      uint32_t dy = 3, dz = 3, dw = 3, DX = 4;
      uint32_t D = NID (DX, dy, dz, dw);
      double base = 1.0;
      uint32_t flow = 0;
      for (uint32_t x = 0; x < W; ++x)
        {
          if (x == DX) continue;
          uint32_t s = NID (x, dy, dz, dw);
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

  // nccl_ar 默认跑久一点等 all-reduce 收完;其它场景保持原 1.5s
  double simDuration = (simStop > 0) ? simStop
                                     : (scenario == "nccl_ar" ? 25.0 : 1.5);
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

  std::cout << "\n=== 4D mesh " << W << "^4=" << N
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

  std::ofstream csv ("mesh4d_result.csv");
  csv << "scenario,credit,sent,delivered,dropped,throughput_Gbps,total_energy_J,avg_power_W\n";
  csv << scenario << "," << credit << ","
      << g_sent << "," << delivered << "," << dropped << ","
      << throughputGbps << "," << totalEnergy << "," << avgPowerW << "\n";
  std::cout << "wrote mesh4d_result.csv\n";

  // ═══════════ 统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════════
  // 无交换机直连：FPGA 网卡承担转发；对分按"每轴 K_W 完全图"切：
  //   沿单轴 W/2 | W/2 切，每条轴线 (W/2)^2 条交叉边，共 W^(d-1) 条轴线。
  //   bisectionLinks = (W/2)^2 · W^(d-1)
  double jct = (g_inject_start > 0 && g_last_recv_time > 0)
               ? (g_last_recv_time / 1e9 - g_inject_start) : rxDurationSec;
  {
    uint64_t bisectionLinks = (uint64_t) (W / 2) * (W / 2)
                            * (uint64_t) (W * W * W);   // (W/2)^2 · W^(d-1)
    EnergyModel em;
    em.inv = EnergyInventory::Switchless (N, nLinks, fpgaPerNode,
                                          lineRateBps / 1e9, opticalFrac,
                                          bisectionLinks);
    em.SetBits (g_total_bits_transmitted);
    em.SetDuration (jct > 0 ? jct : (rxDurationSec > 0 ? rxDurationSec : simDuration));
    em.SetTraffic (delivered, pktBytes);
    em.SetThroughput (throughputGbps);
    em.WriteCsv ("energy_unified.csv", "mesh4d", scenario);
    em.PrintSummary ("mesh4d", scenario);

    if (scenario == "nccl_ar")
      {
        uint64_t M = (uint64_t) modelMB * 1024 * 1024;
        uint32_t P = (uint32_t) cmp::StridedRanks (N, arRanks).size ();
        cmp::CmpRow row;
        row.topo = "mesh4d"; row.arRanks = P; row.modelBytes = M;
        row.lineRateGbps = lineRateBps / 1e9; row.jctSec = jct;
        row.sent = g_sent; row.delivered = delivered; row.dropped = dropped;
        row.tputGbps = throughputGbps;
        row.algbwGbps = cmp::AlgBwGbps (M, jct);
        row.busbwGbps = cmp::BusBwGbps (M, jct, P);
        row.busbwEff  = (lineRateBps > 0) ? row.busbwGbps / (lineRateBps / 1e9) : 0;
        row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
        row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
        row.bisectionGbps = em.inv.bisectionGbps;
        // 无交换机直连用逐跳信用(非 PFC fabric)→ PFC 字段填 -1 表示 N/A
        row.pfcPausedSec = -1; row.pfcPeakKB = -1; row.pfcDrops = 0;
        cmp::WriteCmpCsv ("cmp_mesh4d.csv", row);
        std::cout << "wrote cmp_mesh4d.csv  (busbw=" << row.busbwGbps
                  << " Gbps, eff=" << row.busbwEff * 100 << "%)\n";
      }
  }

  return 0;
}
