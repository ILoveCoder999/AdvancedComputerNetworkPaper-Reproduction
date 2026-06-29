/*
 * fat-tree-sim-all-scenarios.cc — RoCEv2 完整实现（全场景集成版）
 * ─────────────────────────────────────────────────────────────────────────
 * 拓扑：三层无收敛胖树（3-Tier Clos Fat-Tree）
 * Server → ToR (Leaf) → Aggregation → Core (Spine)
 * K=8：8 pods × 4 ToR × 4 servers = 128 服务器（非阻塞）
 * 注：规模由常量 K 决定，N = (K/2)^2 × K = K^3/4。放大改 K 即可（K=16→1024，K=20→2000）。
 *
 * 流量场景（--scenario=）
 * 1. 传统网络场景: uniform / incast / allreduce / bisection
 * 2. SIGCOMM 2026 RNG 论文场景: clique / hubs / matching （配合参数 --f= 激活）
 *
 * 评估机制：
 * 全部走 RoCEv2 / DCQCN 拥塞控制，利用稳定吞吐供需比计算过订阅比（Oversubscription Ratio）：
 * Oversub = Demand (当前业务无阻塞理论带宽需求) / Throughput (全网实际接收吞吐)
 */

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <cmath>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/socket.h"

// —— 接入：NCCL 流量层 + 统一能耗模型(3-tier Clos Fat-Tree,RoCE) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("FatTreeRocev2All");

static const uint16_t DATA_PORT = 9000;
static const uint16_t CNP_PORT  = 9001;

// ─── 拓扑常量(规模运行时可调:--K)─────────────────────────────────────────
// 默认 K=16 → N=K^3/4=1024(统一规模;K³/4 量化命不中 1296,fat-tree 唯一规模离群)。
// --K 运行时改(如 --K=4→N=16 冒烟);派生量在 main 解析 --K 后重算(见下方 RECALC)。
static uint32_t K               = 16;
static uint32_t total_pods      = K;
static uint32_t nCore           = (K/2) * (K/2);
static uint32_t servers_per_tor = K/2;
static uint32_t tors_per_pod    = K/2;
static uint32_t aggrs_per_pod   = K/2;
static uint32_t N               = total_pods * tors_per_pod * servers_per_tor;

// ─── 全局统计 ──────────────────────────────────────────────────────────────
struct FlowProbe { double sentNs; double recvNs; };
static std::map<uint64_t, FlowProbe> g_inflight;
static std::vector<FlowProbe>        g_done;
static uint64_t                  g_sent      = 0;
static double                    g_first_recv = -1.0;
static double                    g_last_recv  = -1.0;
static uint64_t                  g_total_bits = 0;
static uint64_t                  g_cnp_sent   = 0;
static uint64_t                  g_cnp_recv   = 0;
static uint64_t                  g_ce_pkts    = 0;
static double                    g_inject_start = -1.0;  // all-reduce 注入起点(算 JCT)

// ─── DCQCN 参数 ────────────────────────────────────────────────────────────
namespace Dcqcn {
  static constexpr double   G          = 1.0/16.0;
  static constexpr double   RAI        = 40e6;
  static constexpr uint64_t RT_US      = 55;
  static constexpr double   MIN_RATE   = 1e9;  // 修复：降低到1Gbps保底速率
  static constexpr uint64_t CNP_GAP_US = 50;
}

// ─── RoCEv2 Host ──────────────────────────────────────────────────────────
class RoceHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("RoceHost")
      .SetParent<Application> ()
      .AddConstructor<RoceHost> ();
    return t;
  }

  void Setup (uint32_t id, Ptr<Socket> dataRx, Ptr<Socket> cnpRx,
              std::map<uint32_t, Address> addr, double lineRateBps)
  {
    m_id       = id;
    m_dataRx   = dataRx;
    m_cnpRx    = cnpRx;
    m_addr     = std::move (addr);
    m_lineRate = lineRateBps;
  }

  
  void EnqueueAndSend (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    if (m_id == dst) return;
    m_txQueue[dst].push_back ({dst, bytes, id});
    if (!m_pacing[dst])
      { m_pacing[dst] = true; Simulator::ScheduleNow (&RoceHost::DoSendPkt, this, dst); }
  }

private:
  struct Pending { uint32_t dst; uint32_t bytes; uint64_t id; };

  struct DcqcnState
  {
    double  rate    = 0;
    double  alpha   = 1.0;
    bool    cnpFlag = false;
    EventId rtTimer;
  };

  void StartApplication () override
  {
    m_dataRx->SetIpRecvTos (true);
    m_dataRx->SetRecvCallback (MakeCallback (&RoceHost::OnDataRecv, this));
    m_cnpRx->SetRecvCallback  (MakeCallback (&RoceHost::OnCnpRecv,  this));
  }
  void StopApplication () override {}

  void DoSendPkt (uint32_t dst)
  {
    auto &q = m_txQueue[dst];
    if (q.empty ()) { m_pacing[dst] = false; return; }
    Pending p = q.front (); q.pop_front ();
    ActuallySendData (p.dst, p.bytes, p.id);
    double rate   = GetRate (dst);
    double gapSec = ((p.bytes + 12) * 8.0) / rate;
    Simulator::Schedule (Seconds (gapSec), &RoceHost::DoSendPkt, this, dst);
  }

  double GetRate (uint32_t dst)
  {
    auto it = m_dcqcn.find (dst);
    return (it == m_dcqcn.end () || it->second.rate <= 0) ? m_lineRate : it->second.rate;
  }

  void ActuallySendData (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    Ptr<Packet> pkt = Create<Packet> (bytes);
    uint8_t b[12];
    std::memcpy (b,     &id,  8);
    std::memcpy (b + 8, &dst, 4);
    pkt->AddAtEnd (Create<Packet> (b, 12));

    FlowProbe pr; pr.sentNs = Simulator::Now ().GetNanoSeconds (); pr.recvNs = 0;
    g_inflight[id] = pr;
    g_sent++;

    auto it = m_addr.find (dst);
    if (it == m_addr.end ()) return;
    if (!m_dataTx)
      {
        m_dataTx = Socket::CreateSocket (GetNode (), TypeId::LookupByName ("ns3::UdpSocketFactory"));
        m_dataTx->SetIpTos (0x02);   // ECT(0)
      }
    m_dataTx->SendTo (pkt, 0, it->second);
  }

  void OnDataRecv (Ptr<Socket> s)
  {
    Address from; Ptr<Packet> pkt;
    while ((pkt = s->RecvFrom (from)))
      {
        SocketIpTosTag tosTag;
        bool hasTos = pkt->RemovePacketTag (tosTag);
        bool isCE   = hasTos && ((tosTag.GetTos () & 0x03) == 0x03);

        if (pkt->GetSize () < 12) continue;
        uint8_t b[12]; pkt->CreateFragment (pkt->GetSize () - 12, 12)->CopyData (b, 12);
        uint64_t id; std::memcpy (&id, b, 8);

        double nowNs = Simulator::Now ().GetNanoSeconds ();
        if (g_first_recv < 0) g_first_recv = nowNs;
        g_last_recv = nowNs;

        auto it = g_inflight.find (id);
        if (it != g_inflight.end ())
          { it->second.recvNs = nowNs; g_done.push_back (it->second); g_inflight.erase (it); }

        if (isCE)
          {
            ++g_ce_pkts;
            InetSocketAddress inet = InetSocketAddress::ConvertFrom (from);
            Ipv4Address srcIp = inet.GetIpv4 ();
            Time now = Simulator::Now ();
            auto &lastT = m_lastCnpTime[srcIp];
            if (lastT.IsZero () || now - lastT >= MicroSeconds (Dcqcn::CNP_GAP_US))
              { SendCnp (srcIp); lastT = now; }
          }
      }
  }

  void SendCnp (Ipv4Address targetIp)
  {
    if (!m_cnpTx) m_cnpTx = Socket::CreateSocket (GetNode (), TypeId::LookupByName ("ns3::UdpSocketFactory"));
    uint8_t b[4]; std::memcpy (b, &m_id, 4);
    Ptr<Packet> cnp = Create<Packet> (b, 4);
    m_cnpTx->SendTo (cnp, 0, InetSocketAddress (targetIp, CNP_PORT));
    ++g_cnp_sent;
  }

  void OnCnpRecv (Ptr<Socket> s)
  {
    Ptr<Packet> pkt;
    while ((pkt = s->Recv ()))
      {
        if (pkt->GetSize () < 4) continue;
        uint8_t b[4]; pkt->CopyData (b, 4);
        uint32_t receiverId; std::memcpy (&receiverId, b, 4);
        ++g_cnp_recv;
        ApplyDcqcnDecrease (receiverId);
      }
  }

  void ApplyDcqcnDecrease (uint32_t dst)
  {
    auto &st = m_dcqcn[dst];
    if (st.rate <= 0) st.rate = m_lineRate;
    st.alpha  = (1.0 - Dcqcn::G) * st.alpha + Dcqcn::G;
    st.rate  *= (1.0 - st.alpha / 2.0);
    st.rate   = std::max (st.rate, Dcqcn::MIN_RATE);
    st.cnpFlag = true;
    st.rtTimer.Cancel ();
    st.rtTimer = Simulator::Schedule (MicroSeconds (Dcqcn::RT_US), &RoceHost::DcqcnRateTimer, this, dst);
  }

  void DcqcnRateTimer (uint32_t dst)
  {
    auto &st = m_dcqcn[dst];
    st.alpha = (1.0 - Dcqcn::G) * st.alpha;
    if (st.cnpFlag) st.cnpFlag = false;
    else st.rate = std::min (st.rate + Dcqcn::RAI, m_lineRate);
    if (st.rate < m_lineRate * 0.999)
      st.rtTimer = Simulator::Schedule (MicroSeconds (Dcqcn::RT_US), &RoceHost::DcqcnRateTimer, this, dst);
  }

  uint32_t m_id {0};
  double   m_lineRate {100e9};
  Ptr<Socket> m_dataRx, m_cnpRx, m_dataTx, m_cnpTx;
  std::map<uint32_t, Address>             m_addr;
  std::map<uint32_t, std::deque<Pending>> m_txQueue;
  std::map<uint32_t, bool>                m_pacing;
  std::map<uint32_t, DcqcnState>          m_dcqcn;
  std::map<Ipv4Address, Time>             m_lastCnpTime;
};

// ─── main ──────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string scenario     = "uniform"; // uniform|incast|allreduce|bisection|clique|hubs|matching
  double      f            = 0.3;       // 针对论文场景的活跃度参数 [0.0, 1.0]
  uint32_t    uniformFlows = 100000;    // uniform 的背景流数量
  uint32_t    incastFanin  = 64;        //  incast 的扇入度
  uint32_t    pktBytes     = 1024;
  uint32_t    queuePkts    = 1024;      // 修复：增加到1024，避免队列溢出
  uint32_t    ecnMinTh     = 100;       // 修复：调整ECN阈值
  uint32_t    ecnMaxTh     = 300;       // 修复：调整ECN阈值
  std::string linkRate     = "100Gbps"; 
  std::string linkDelay    = "200ns";
  uint32_t    pktsPerFlow  = 3000;      // 拥塞平稳所需的每条流持续发包数
  // —— 跨拓扑对比 all-reduce(nccl_ar) —— //
  uint32_t    arRanks      = 32;        // all-reduce 逻辑 rank 数(跨网铺开)
  uint32_t    modelMB      = 8;         // 每 rank 梯度/模型大小 M (MiB)
  double      simStop      = 10.0;      // 仿真停止时刻(s)

  CommandLine cmd;
  cmd.AddValue ("scenario",     "uniform|incast|allreduce|bisection|clique|hubs|matching|nccl_ar", scenario);
  cmd.AddValue ("arRanks",      "nccl_ar: all-reduce rank count", arRanks);
  cmd.AddValue ("modelMB",      "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("simStop",      "simulator stop time (s)", simStop);
  cmd.AddValue ("f",            "active fraction [0,1] for clique/hubs/matching", f);
  cmd.AddValue ("uniformFlows", "number of random flows for uniform scenario", uniformFlows);
  cmd.AddValue ("incastFanin",  "incast fan-in limit",       incastFanin);
  cmd.AddValue ("pktsPerFlow",  "packets to send per flow",  pktsPerFlow);
  cmd.AddValue ("pktBytes",     "payload bytes",             pktBytes);
  cmd.AddValue ("queuePkts",    "physical queue depth",      queuePkts);
  cmd.AddValue ("ecnMinTh",     "RED ECN MinTh (pkts)",      ecnMinTh);
  cmd.AddValue ("ecnMaxTh",     "RED ECN MaxTh (pkts)",      ecnMaxTh);
  cmd.AddValue ("linkRate",     "link rate",                 linkRate);
  cmd.AddValue ("linkDelay",    "link delay",                linkDelay);
  cmd.AddValue ("K",            "switch radix; N=K^3/4 (默认16=1024;冒烟用 --K=4→16)", K);
  cmd.Parse (argc, argv);

  // RECALC: 解析 --K 后重算所有派生规模量(K 为偶数;K=4→N=16, K=16→N=1024)
  total_pods      = K;
  nCore           = (K / 2) * (K / 2);
  servers_per_tor = K / 2;
  tors_per_pod    = K / 2;
  aggrs_per_pod   = K / 2;
  N               = total_pods * tors_per_pod * servers_per_tor;

  DataRate dr (linkRate);
  double lineRateBps = (double) dr.GetBitRate ();

  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting", BooleanValue (true));

  std::cout << "=== 3-Tier Fat-Tree + RoCEv2 (DCQCN), K=" << K << " ===\n"
            << "  Total Nodes: " << N << " servers\n"
            << "  Active Scenario: " << scenario << "\n"
            << "  Link: " << linkRate << " / " << linkDelay << "\n"
            << "  Lossless Queue: " << queuePkts << "p | ECN RED: " << ecnMinTh << "/" << ecnMaxTh << "p\n\n";

  // ── 拓扑节点创建 ──
  NodeContainer servers; servers.Create (N);
  NodeContainer tors;    tors.Create    (total_pods * tors_per_pod);
  NodeContainer aggrs;   aggrs.Create   (total_pods * aggrs_per_pod);
  NodeContainer cores;   cores.Create   (nCore);

  InternetStackHelper inet;
  inet.Install (servers); inet.Install (tors);
  inet.Install (aggrs);   inet.Install (cores);

  // ── 链路参数模板 ──
  std::ostringstream qs; qs << queuePkts << "p";
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute  ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay",    StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  // 修复：启用RED队列进行ECN标记
  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::RedQueueDisc",
                        "MinTh",   DoubleValue (ecnMinTh),
                        "MaxTh",   DoubleValue (ecnMaxTh),
                        "UseEcn",  BooleanValue (true),
                        "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  Ipv4AddressHelper ip;
  std::map<uint32_t, Ipv4Address> serverIp;
  uint32_t subnet = 0;

  // 修复：使用独立的IP计数器避免冲突
  static uint32_t g_ip_counter = 1;

  auto connect_nodes = [&] (Ptr<Node> u, Ptr<Node> v, Ipv4Address *ipU = nullptr)
  {
    NetDeviceContainer dev = p2p.Install (u, v);
    tch.Install (dev);
    
    // 为每个链路分配唯一的 /30 子网
    uint32_t network = g_ip_counter * 4;
    std::ostringstream subnetStr;
    subnetStr << "10." << ((network >> 16) & 0xFF) << "." << ((network >> 8) & 0xFF) << "." << (network & 0xFF);
    
    ip.SetBase (subnetStr.str ().c_str (), "255.255.255.252");
    Ipv4InterfaceContainer ic = ip.Assign (dev);
    
    // 添加流量追踪
    dev.Get (0)->TraceConnectWithoutContext ("MacTx", MakeCallback (+[](Ptr<const Packet> p){ 
      g_total_bits += p->GetSize ()*8; 
    }));
    dev.Get (1)->TraceConnectWithoutContext ("MacTx", MakeCallback (+[](Ptr<const Packet> p){ 
      g_total_bits += p->GetSize ()*8; 
    }));
    
    if (ipU) *ipU = ic.GetAddress (0);
    g_ip_counter++;
    ++subnet;
    return ic;
  };

  // ── 胖树 Clos 三层互联 ──
  uint32_t s_idx = 0;
  
  // 连接 servers 到 ToR
  for (uint32_t p = 0; p < total_pods; ++p)
    for (uint32_t t = 0; t < tors_per_pod; ++t)
      {
        uint32_t tor_id = p * tors_per_pod + t;
        for (uint32_t s = 0; s < servers_per_tor && s_idx < N; ++s, ++s_idx)
          { 
            Ipv4Address sip; 
            connect_nodes (servers.Get (s_idx), tors.Get (tor_id), &sip); 
            serverIp[s_idx] = sip; 
          }
      }
      
  // 连接 ToR 到 Aggregation
  for (uint32_t p = 0; p < total_pods; ++p)
    for (uint32_t t = 0; t < tors_per_pod; ++t)
      for (uint32_t a = 0; a < aggrs_per_pod; ++a)
        connect_nodes (tors.Get (p*tors_per_pod+t), aggrs.Get (p*aggrs_per_pod+a));
        
  // 连接 Aggregation 到 Core
  for (uint32_t p = 0; p < total_pods; ++p)
    for (uint32_t a = 0; a < aggrs_per_pod; ++a)
      for (uint32_t c = 0; c < K/2; ++c)
        connect_nodes (aggrs.Get (p*aggrs_per_pod+a), cores.Get (a*(K/2)+c));

  std::map<uint32_t, Address> addr;
  for (uint32_t i = 0; i < N; ++i) 
    addr[i] = InetSocketAddress (serverIp[i], DATA_PORT);

  // ── 安装端节点 Application ──
  std::vector<Ptr<RoceHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      Ptr<Socket> dataRx = Socket::CreateSocket (servers.Get (i), TypeId::LookupByName ("ns3::UdpSocketFactory"));
      dataRx->Bind (InetSocketAddress (Ipv4Address::GetAny (), DATA_PORT));
      Ptr<Socket> cnpRx = Socket::CreateSocket (servers.Get (i), TypeId::LookupByName ("ns3::UdpSocketFactory"));
      cnpRx->Bind (InetSocketAddress (Ipv4Address::GetAny (), CNP_PORT));

      Ptr<RoceHost> a = CreateObject<RoceHost> ();
      a->Setup (i, dataRx, cnpRx, addr, lineRateBps);
      servers.Get (i)->AddApplication (a);
      a->SetStartTime (Seconds (0.0)); 
      a->SetStopTime (Seconds (30.0));
      apps[i] = a;
    }

  // 全局路由计算
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // ── 调度流量（全场景集成与时域平滑优化版）──
  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  uint64_t pid = 1;
  double totalDemandGbps = 0.0;

  // 为论文场景（clique/hubs/matching）动态计算和筛选活跃节点
  uint32_t nActive = std::max (2u, (uint32_t)std::ceil (f * N));
  std::vector<uint32_t> perm (N);
  for (uint32_t i = 0; i < N; ++i) perm[i] = i;
  for (uint32_t i = N - 1; i > 0; --i) { uint32_t j = rng->GetInteger (0, i); std::swap (perm[i], perm[j]); }
  std::vector<uint32_t> active (perm.begin (), perm.begin() + nActive);

  if (scenario == "incast")
    {
      uint32_t dst = 0, cnt = 0;
      for (uint32_t s = 1; s < N && cnt < incastFanin; ++s, ++cnt)
        for (uint32_t m = 0; m < pktsPerFlow; ++m)
          Simulator::Schedule (Seconds (1.0), &RoceHost::EnqueueAndSend, apps[s], dst, pktBytes, pid++);
      totalDemandGbps = 1.0 * (lineRateBps / 1e9);
    }
  else if (scenario == "allreduce")
    {
      for (uint32_t s = 0; s < N; ++s)
        { uint32_t dst = (s+1)%N;
          for (uint32_t m = 0; m < pktsPerFlow; ++m)
            Simulator::Schedule (Seconds (1.0), &RoceHost::EnqueueAndSend, apps[s], dst, pktBytes, pid++); }
      totalDemandGbps = N * (lineRateBps / 1e9);
    }
  else if (scenario == "bisection")
    {
      for (uint32_t s = 0; s < N/2; ++s)
        for (uint32_t m = 0; m < pktsPerFlow; ++m)
          Simulator::Schedule (Seconds (1.0), &RoceHost::EnqueueAndSend, apps[s], N/2+s, pktBytes, pid++);
      totalDemandGbps = (N / 2) * (lineRateBps / 1e9);
    }
  else if (scenario == "clique")
    {
      // ─── clique(f): 全互联模式（自带平滑） ───
      double flow_delay_counter = 0.0;
      for (uint32_t s : active)
        for (uint32_t d : active)
          {
            if (s == d) continue;
            double startTime = 1.0 + flow_delay_counter;
            for (uint32_t m = 0; m < pktsPerFlow; ++m){
              Simulator::Schedule (Seconds (startTime), &RoceHost::EnqueueAndSend, apps[s], d, pktBytes, pid++);
            }
            flow_delay_counter += 10e-6; // 每条流交错 10 微秒
            if (flow_delay_counter > 0.1) flow_delay_counter = 0.0;
          }
      totalDemandGbps = nActive * (lineRateBps / 1e9);
    }
  else if (scenario == "hubs")
    {
      // ─── hubs(f): 参数服务器集中收敛模式（自带平滑） ───
      double flow_delay_counter = 0.0;
      for (uint32_t s = 0; s < N; ++s)
        for (uint32_t h : active)
          {
            if (s == h) continue;
            double startTime = 1.0 + flow_delay_counter;
            for (uint32_t m = 0; m < pktsPerFlow; ++m)
              Simulator::Schedule (Seconds (startTime), &RoceHost::EnqueueAndSend, apps[s], h, pktBytes, pid++);
            
            flow_delay_counter += 10e-6;
            if (flow_delay_counter > 0.1) flow_delay_counter = 0.0;
          }
      totalDemandGbps = nActive * (lineRateBps / 1e9);
    }
  else if (scenario == "matching")
    {
      // ─── matching(f): 随机配对双向互喷（自带平滑） ───
      double flow_delay_counter = 0.0;
      uint32_t nPairs = nActive / 2;
      for (uint32_t k = 0; k < nPairs; ++k)
        {
          uint32_t s = active[k * 2];
          uint32_t d = active[k * 2 + 1];
          
          double startTime = 1.0 + flow_delay_counter;
          
          for (uint32_t m = 0; m < pktsPerFlow; ++m)
            {
              Simulator::Schedule (Seconds (startTime), &RoceHost::EnqueueAndSend, apps[s], d, pktBytes, pid++);
              Simulator::Schedule (Seconds (startTime), &RoceHost::EnqueueAndSend, apps[d], s, pktBytes, pid++);
            }
          
          flow_delay_counter += 15e-6;
          if (flow_delay_counter > 0.1) flow_delay_counter = 0.0;
        }
      totalDemandGbps = (2 * nPairs) * (lineRateBps / 1e9);
    }
  else if (scenario == "nccl_ar")
    {
      // 跨拓扑对比工作负载：真 NCCL Ring AllReduce(arRanks 个 rank 跨网铺开)。
      // 开环全注入(t=1.0)，由 DCQCN 限速器自调速。
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      std::vector<uint32_t> ranks;
      std::vector<nccl::CommOp> ops = cmp::BuildComparisonAllReduce (N, arRanks, M, &ranks);
      uint64_t injected = 0;
      for (const auto &o : ops)
        {
          if (o.onNvlink) continue;
          uint32_t nPk = (uint32_t) nccl::ToPackets (o.bytes, pktBytes);
          for (uint32_t k = 0; k < nPk; ++k)
            { Simulator::Schedule (Seconds (1.0), &RoceHost::EnqueueAndSend,
                                   apps[o.src], o.dst, pktBytes, pid++); ++injected; }
        }
      g_inject_start = 1.0;
      totalDemandGbps = ranks.size () * (lineRateBps / 1e9);
      std::cout << "  Scenario: nccl_ar Ring AllReduce  arRanks=" << ranks.size ()
                << "  M=" << modelMB << "MiB  ops=" << ops.size ()
                << "  pkts=" << injected << "\n";
    }
  else // uniform background
    {
      double when = 1.0;
      for (uint32_t k = 0; k < uniformFlows; ++k)
        {
          uint32_t s = rng->GetInteger (0, N-1), d = rng->GetInteger (0, N-1);
          if (s == d) continue;
          Simulator::Schedule (Seconds (when), &RoceHost::EnqueueAndSend, apps[s], d, pktBytes, pid++);
          when += 5e-7;
        }
      totalDemandGbps = N * (lineRateBps / 1e9) * 0.8;
    }

  // ── 运行仿真 ──
  double simDuration = simStop;  // nccl_ar 可调小;其它场景默认 10s
  Simulator::Stop (Seconds (simDuration));
  std::cout << "  Running NS3 Simulation Framework...\n";
  Simulator::Run ();
  Simulator::Destroy ();

  // ── 数据统计与指标修正 ──
  uint64_t delivered = g_done.size ();
  uint64_t dropped   = (g_sent >= delivered) ? (g_sent - delivered) : 0;
  double rxDurSec    = (g_last_recv > g_first_recv && g_first_recv > 0) ? (g_last_recv - g_first_recv) / 1e9 : 0.0;
  
  // 1. 计算稳态总收包吞吐量 (Gbps)
  double tputGbps    = rxDurSec > 0 ? (delivered * pktBytes * 8.0) / (rxDurSec * 1e9) : 0.0;

  // 2. 核心数学修正：过订阅比计算
  double oversubRatio = 1.0;
  if (tputGbps > 0) { oversubRatio = totalDemandGbps / tputGbps; }
  if (oversubRatio < 1.0) oversubRatio = 1.0; // 边界饱和

  // 延迟指标计算
  std::vector<double> lats;
  lats.reserve (g_done.size ());
  for (auto &p : g_done) if (p.recvNs > 0) lats.push_back (p.recvNs - p.sentNs);
  std::sort (lats.begin (), lats.end ());
  double meanUs = 0, p99Us = 0;
  if (!lats.empty ()) {
    double sum = 0; for (double v : lats) sum += v;
    meanUs = sum / lats.size() / 1000.0;
    p99Us  = lats[lats.size() * 99 / 100] / 1000.0;
  }

  std::cout << "\n================= RESULTS =================" << "\n"
            << "  Scenario Mode      : " << scenario << (scenario == "clique" || scenario == "hubs" || scenario == "matching" ? " (f=" + std::to_string(f) + ")" : "") << "\n"
            << "  Sent / Delivered   : " << g_sent << " / " << delivered << " (Dropped: " << dropped << ")\n"
            << "  CNP Sent/Recv      : " << g_cnp_sent << " / " << g_cnp_recv << "\n"
            << "  CE Packets         : " << g_ce_pkts << "\n"
            << "  Theory Demand      : " << totalDemandGbps << " Gbps\n"
            << "  Realized Tput      : " << tputGbps << " Gbps\n"
            << "  Oversubscription   : " << oversubRatio << "   <-- (Figure 13 纵轴指标)\n"
            << "  Latency (Mean/p99) : " << meanUs << " µs / " << p99Us << " µs\n"
            << "===========================================\n\n";

  // 追加写入公共 CSV 文件便于批量画图
  std::ofstream csv ("fattree_all_scenarios_result.csv", std::ios::app);
  static bool headerWrote = false;
  if (!headerWrote) {
      csv << "scenario,f,demand_gbps,tput_gbps,oversub_ratio,mean_us,p99_us,cnp_sent,cnp_recv,ce_packets\n";
      headerWrote = true;
  }
  csv << scenario << "," << f << "," << totalDemandGbps << "," << tputGbps << "," << oversubRatio << "," << meanUs << "," << p99Us << "," << g_cnp_sent << "," << g_cnp_recv << "," << g_ce_pkts << "\n";

  // ═══════════ 统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════════
  double jct = (g_inject_start > 0 && g_last_recv > 0)
               ? (g_last_recv / 1e9 - g_inject_start) : rxDurSec;
  {
    uint64_t nTor  = (uint64_t) total_pods * tors_per_pod;        // 32
    uint64_t nAggr = (uint64_t) total_pods * aggrs_per_pod;       // 32
    uint64_t L2    = (uint64_t) total_pods * tors_per_pod * aggrs_per_pod; // ToR-Aggr=128
    uint64_t L3    = (uint64_t) total_pods * aggrs_per_pod * (K / 2);      // Aggr-Core=128
    EnergyModel em;
    em.inv = EnergyInventory::FatTree (N, nTor, nAggr, nCore, L2, L3,
                                       lineRateBps / 1e9, /*gpuPerSrv=*/1);
    em.SetBits (g_total_bits);
    em.SetDuration (jct > 0 ? jct : simDuration);
    em.SetTraffic (delivered, pktBytes);
    em.SetThroughput (tputGbps);
    em.WriteCsv ("energy_unified.csv", "fat_tree", scenario);
    em.PrintSummary ("fat_tree", scenario);

    if (scenario == "nccl_ar")
      {
        uint64_t M = (uint64_t) modelMB * 1024 * 1024;
        uint32_t P = (uint32_t) cmp::StridedRanks (N, arRanks).size ();
        cmp::CmpRow row;
        row.topo = "fat_tree"; row.arRanks = P; row.modelBytes = M;
        row.lineRateGbps = lineRateBps / 1e9; row.jctSec = jct;
        row.sent = g_sent; row.delivered = delivered; row.dropped = dropped;
        row.tputGbps = tputGbps;
        row.algbwGbps = cmp::AlgBwGbps (M, jct);
        row.busbwGbps = cmp::BusBwGbps (M, jct, P);
        row.busbwEff  = (lineRateBps > 0) ? row.busbwGbps / (lineRateBps / 1e9) : 0;
        row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
        row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
        row.bisectionGbps = em.inv.bisectionGbps;
        cmp::WriteCmpCsv ("cmp_fat_tree.csv", row);
        std::cout << "wrote cmp_fat_tree.csv  (busbw=" << row.busbwGbps
                  << " Gbps, eff=" << row.busbwEff << ")\n";
      }
  }

  return 0;
}