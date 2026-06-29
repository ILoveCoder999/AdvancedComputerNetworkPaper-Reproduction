/*
 * leaf-spine-sim.cc — RoCEv2 完整实现
 * ─────────────────────────────────────────────────────────────────────────
 * 拓扑：两层无收敛胖树（Leaf-Spine / Two-tier Clos）
 *   Server → Leaf Switch → Spine Switch
 *   默认 K=64，nLeaf=16，nSpine=32，N=512 服务器（非阻塞）
 *
 * RoCEv2 三层机制
 * ────────────────
 *  1. 无损传输（Lossless）
 *       · 物理队列深度 256p（近似 PFC credit 缓冲）
 *       · RED + ECN：MinTh=20p/MaxTh=60p，超过阈值先标记 CE 而不丢包
 *         → 在真正丢包前就触发拥塞反馈，维持网络无损
 *
 *  2. ECN 显式拥塞通知
 *       · 发端调用 SetIpTos(ECT0=0x02)，标记所有数据包为 ECN-Capable
 *       · 交换机 RED 队列检测 ECT0 后将 ECN 字段改写为 CE（0x03）
 *       · 收端通过 SetIpRecvTos(true) + SocketIpTosTag 检测 CE 标记
 *       · 每条流（per src-IP）每 50 µs 最多发一个 CNP（CNP 限速）
 *
 *  3. DCQCN 拥塞控制（Zhu et al., SIGCOMM 2015）
 *       发端维护 per-dst 状态（rate, alpha）：
 *         · 收到 CNP → alpha ← (1-g)·alpha + g
 *                      rate  ← rate × (1 - alpha/2)   [乘性降速]
 *                      rate  ≥ MIN_RATE = 100 Mbps
 *         · RT 定时器（55 µs，无 CNP 时触发）→ 加性增速 RAI=40 Mbps
 *                      alpha ← (1-g)·alpha              [alpha 衰减]
 *                      rate  ← min(rate + RAI, line_rate)
 *       收端限速 CNP（每 src 每 50 µs 最多 1 个），防止 CNP 风暴
 *       发端 per-dst 限速器：
 *         · 包到来 → 入队，若空闲则立即启动发链
 *         · DoSendPkt：实际发包 → 按当前 rate 计算间隔 → 调度下一次发包
 *         · rate 降低 → 下一包间隔自动变大，即刻生效
 *
 * 流量场景（--scenario=）
 *   uniform   : 均匀随机
 *   incast    : 多对一，所有包立即入队，由 DCQCN 负责速率控制
 *   allreduce : 逻辑环 ring-allreduce
 *   bisection : 左半打右半
 *
 * 端口约定：
 *   DATA_PORT = 9000（数据）
 *   CNP_PORT  = 9001（Congestion Notification Packet）
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <deque>
#include <vector>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/socket.h"

// —— 接入：NCCL 流量层 + 统一能耗模型 + 真 PFC 无损(替深 DropTail 近似) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)
#include "pfc-lossless.h"      // IEEE 802.1Qbb PFC 状态机(挂交换机入口)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("LeafSpineRocev2");

static const uint16_t DATA_PORT = 9000;
static const uint16_t CNP_PORT  = 9001;

// ─── 全局统计 ──────────────────────────────────────────────────────────────
struct HostProbe { double sentNs; double recvNs; };
static std::map<uint64_t, HostProbe> g_inflight;
static std::vector<HostProbe>        g_done;
static uint64_t                  g_sent      = 0;
static double                    g_first_recv = -1.0;
static double                    g_last_recv  = -1.0;
static uint64_t                  g_total_bits = 0;
// DCQCN 诊断计数器
static uint64_t                  g_cnp_sent   = 0;   // 收端发出的 CNP 数
static uint64_t                  g_cnp_recv   = 0;   // 发端收到的 CNP 数
static uint64_t                  g_ce_pkts    = 0;   // 被标记 CE 的数据包数
static double                    g_inject_start = -1.0; // all-reduce 注入起点(算 JCT)

// ─── PFC 无损：真状态机挂在每个交换机入口队列盘(替"深 DropTail 近似无损") ──────
//  做法：把 ns-3 QueueDisc 的 Enqueue/Dequeue 真事件喂进 pfc::PfcPort，
//  得到真实的入口占用、PAUSE 时长(HOL/风暴信号)，并断言零丢(TotalDrops==0)。
//  这正是 pfc-lossless.h 的设计用途——可观测深 DropTail 掩盖掉的 PFC 副作用。
//  (进一步做"主动上游门控"见文末 TODO：把 SetSendPause 接到上游 NetDevice。)
static pfc::PfcSwitch            g_pfc;
static void PfcEnqCb (uint32_t port, Ptr<const QueueDiscItem> it)
{ g_pfc.Port (port, 0).OnEnqueue (it->GetSize (), Simulator::Now ().GetSeconds ()); }
static void PfcDeqCb (uint32_t port, Ptr<const QueueDiscItem> it)
{ g_pfc.Port (port, 0).OnDequeue (it->GetSize (), Simulator::Now ().GetSeconds ()); }

// ─── DCQCN 参数（源自原论文 Zhu et al. SIGCOMM 2015）──────────────────────
namespace Dcqcn {
  static constexpr double   G         = 1.0 / 16.0; // alpha 更新因子
  static constexpr double   RAI       = 40e6;        // 加性增速 40 Mbps/RT
  static constexpr uint64_t RT_US     = 55;          // Rate Timer 周期 µs
  static constexpr double   MIN_RATE  = 100e6;       // 最小发包速率 100 Mbps
  static constexpr uint64_t CNP_GAP_US = 50;         // 收端 CNP 限速间隔 µs
}

// ─── 应用层：RoCEv2 Host ──────────────────────────────────────────────────
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

  void Setup (uint32_t id,
              Ptr<Socket> dataRx,
              Ptr<Socket> cnpRx,
              std::map<uint32_t, Address> addr,
              double lineRateBps)
  {
    m_id        = id;
    m_dataRx    = dataRx;
    m_cnpRx     = cnpRx;
    m_addr      = std::move (addr);
    m_lineRate  = lineRateBps;
  }

  // 外部调用：将发包请求入队，由 DCQCN 限速器控制实际发送时机
  void EnqueueAndSend (uint32_t dst, uint32_t bytes, uint64_t id)
  {
    if (m_id == dst) return;
    m_txQueue[dst].push_back ({dst, bytes, id});
    if (!m_pacing[dst])
      {
        m_pacing[dst] = true;
        Simulator::ScheduleNow (&RoceHost::DoSendPkt, this, dst);
      }
  }

private:
  // ── 内部数据结构 ──────────────────────────────────────────────────────
  struct Pending { uint32_t dst; uint32_t bytes; uint64_t id; };

  // DCQCN per-dst 状态
  struct DcqcnState
  {
    double   rate    = 0;       // 发包速率（bps），0 表示未初始化→用 lineRate
    double   alpha   = 1.0;     // 拥塞因子 ∈ [0,1]
    bool     cnpFlag = false;   // 当前 RT 周期内是否收到 CNP
    EventId  rtTimer;           // Rate Timer 事件
  };

  // ── 生命周期 ──────────────────────────────────────────────────────────
  void StartApplication () override
  {
    // 数据接收：需要 TOS 回传（读取 ECN CE 位）
    m_dataRx->SetIpRecvTos (true);
    m_dataRx->SetRecvCallback (MakeCallback (&RoceHost::OnDataRecv, this));
    // CNP 接收
    m_cnpRx->SetRecvCallback (MakeCallback (&RoceHost::OnCnpRecv, this));
  }
  void StopApplication () override {}

  // ── 限速发包链 ────────────────────────────────────────────────────────
  void DoSendPkt (uint32_t dst)
  {
    auto &q = m_txQueue[dst];
    if (q.empty ()) { m_pacing[dst] = false; return; }

    Pending p = q.front (); q.pop_front ();
    ActuallySendData (p.dst, p.bytes, p.id);

    // 按当前速率计算下一包间隔
    double rate = GetRate (dst);
    double gapSec = ((p.bytes + 12) * 8.0) / rate;  // +12 = probe tail
    Simulator::Schedule (Seconds (gapSec), &RoceHost::DoSendPkt, this, dst);
  }

  double GetRate (uint32_t dst)
  {
    auto it = m_dcqcn.find (dst);
    if (it == m_dcqcn.end () || it->second.rate <= 0) return m_lineRate;
    return it->second.rate;
  }

  // ── 实际发包（设置 ECT(0) 让交换机可以打 CE 标记）────────────────────
  void ActuallySendData (uint32_t dst, uint32_t bytes, uint64_t id)
  {
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

    if (!m_dataTx)
      {
        m_dataTx = Socket::CreateSocket (GetNode (),
                     TypeId::LookupByName ("ns3::UdpSocketFactory"));
        // 【ECN 机制 1/2】ECT(0)=0x02：告知网络此流支持 ECN
        // RED 只对 ECT 包打 CE，否则直接丢。必须在首次发包前设置。
        m_dataTx->SetIpTos (0x02);
      }
    m_dataTx->SendTo (pkt, 0, it->second);
  }

  // ── 数据包接收：检测 CE，限速发 CNP ──────────────────────────────────
  void OnDataRecv (Ptr<Socket> s)
  {
    Address from;
    Ptr<Packet> pkt;
    while ((pkt = s->RecvFrom (from)))
      {
        // 【ECN 机制 2/2】读取 TOS 中的 ECN 字段
        SocketIpTosTag tosTag;
        bool hasTos = pkt->RemovePacketTag (tosTag);
        bool isCE   = hasTos && ((tosTag.GetTos () & 0x03) == 0x03);

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

        if (isCE)
          {
            ++g_ce_pkts;
            // 【DCQCN 机制 1/3】收端限速 CNP：每 src-IP 每 50 µs 最多 1 个
            InetSocketAddress inet = InetSocketAddress::ConvertFrom (from);
            Ipv4Address srcIp = inet.GetIpv4 ();
            Time now = Simulator::Now ();
            auto &lastT = m_lastCnpTime[srcIp];
            if (lastT.IsZero () || now - lastT >= MicroSeconds (Dcqcn::CNP_GAP_US))
              {
                SendCnp (srcIp);
                lastT = now;
              }
          }
      }
  }

  // ── 发 CNP（小控制包，携带本节点 ID，不设 ECT）────────────────────────
  void SendCnp (Ipv4Address targetIp)
  {
    if (!m_cnpTx)
      m_cnpTx = Socket::CreateSocket (GetNode (),
                  TypeId::LookupByName ("ns3::UdpSocketFactory"));
    // CNP 内容：4 bytes = 本节点 ID（收端）→ 发端据此知道需对哪个 dst 降速
    uint8_t b[4]; std::memcpy (b, &m_id, 4);
    Ptr<Packet> cnp = Create<Packet> (b, 4);
    m_cnpTx->SendTo (cnp, 0, InetSocketAddress (targetIp, CNP_PORT));
    ++g_cnp_sent;
  }

  // ── CNP 接收：DCQCN 乘性降速 ─────────────────────────────────────────
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

  // 【DCQCN 机制 2/3】乘性降速 + alpha 更新
  void ApplyDcqcnDecrease (uint32_t dst)
  {
    auto &st = m_dcqcn[dst];
    if (st.rate <= 0) st.rate = m_lineRate;

    // alpha 向 1 靠近（拥塞信号）
    st.alpha = (1.0 - Dcqcn::G) * st.alpha + Dcqcn::G;

    // 乘性降速
    st.rate *= (1.0 - st.alpha / 2.0);
    st.rate  = std::max (st.rate, Dcqcn::MIN_RATE);

    st.cnpFlag = true;

    // 取消旧定时器，重新计时（有 CNP → Fast Recovery 阶段）
    st.rtTimer.Cancel ();
    st.rtTimer = Simulator::Schedule (
      MicroSeconds (Dcqcn::RT_US),
      &RoceHost::DcqcnRateTimer, this, dst);
  }

  // 【DCQCN 机制 3/3】Rate Timer：加性增速 + alpha 衰减
  void DcqcnRateTimer (uint32_t dst)
  {
    auto &st = m_dcqcn[dst];

    // alpha 向 0 衰减（无拥塞信号时）
    st.alpha = (1.0 - Dcqcn::G) * st.alpha;

    if (st.cnpFlag)
      {
        // 本周期收到新 CNP：停在 Fast Recovery，等待下一个 timer
        st.cnpFlag = false;
      }
    else
      {
        // 加性增速
        st.rate = std::min (st.rate + Dcqcn::RAI, m_lineRate);
      }

    // 未恢复到线速则继续定时
    if (st.rate < m_lineRate * 0.999)
      st.rtTimer = Simulator::Schedule (
        MicroSeconds (Dcqcn::RT_US),
        &RoceHost::DcqcnRateTimer, this, dst);
  }

  // ── 成员变量 ──────────────────────────────────────────────────────────
  uint32_t m_id      {0};
  double   m_lineRate{400e9};

  Ptr<Socket> m_dataRx;   // 数据接收（PORT 9000）
  Ptr<Socket> m_cnpRx;    // CNP 接收（PORT 9001）
  Ptr<Socket> m_dataTx;   // 数据发送（懒初始化，设 ECT(0)）
  Ptr<Socket> m_cnpTx;    // CNP 发送（懒初始化，无 ECN）

  std::map<uint32_t, Address>              m_addr;
  std::map<uint32_t, std::deque<Pending>>  m_txQueue;
  std::map<uint32_t, bool>                 m_pacing;
  std::map<uint32_t, DcqcnState>           m_dcqcn;
  std::map<Ipv4Address, Time>              m_lastCnpTime; // CNP 限速时间戳
};

// ─── main ──────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string scenario     = "uniform";
  uint32_t    K            = 64;
  uint32_t    nLeaf        = 16;
  uint32_t    pktBytes     = 1024;
  uint32_t    queuePkts    = 256;   // 深物理队列：近似 PFC credit 缓冲
  uint32_t    ecnMinTh     = 20;    // RED ECN 标记开始阈值（包数）
  uint32_t    ecnMaxTh     = 60;    // RED ECN 标记最大阈值（包数）
  uint32_t    uniformFlows = 200000;
  uint32_t    incastFanin  = 64;
  std::string linkRate     = "400Gbps";
  std::string linkDelay    = "200ns";
  // —— 跨拓扑对比 all-reduce(nccl_ar) + PFC 参数 —— //
  uint32_t    arRanks      = 32;     // all-reduce 逻辑 rank 数(跨网铺开)
  uint32_t    modelMB      = 8;      // 每 rank 梯度/模型大小 M (MiB)
  bool        pfcOn        = true;   // 1=挂真 PFC 状态机到交换机入口并断言无损
                                     // (变量名避开 pfc:: 命名空间)
  double      rttUs        = 2.0;    // 估算 BDP 用的 RTT(µs)，定 PFC xoff/xon
  double      bufferBDPs   = 3.0;    // 入口缓冲 = 几个 BDP(headroom 固定 1 BDP)
  double      simStop      = 3.0;    // 仿真停止时刻(s)；大 M 时调大

  CommandLine cmd;
  cmd.AddValue ("scenario",     "uniform|incast|allreduce|bisection|nccl_ar", scenario);
  cmd.AddValue ("arRanks",      "nccl_ar: all-reduce rank count",     arRanks);
  cmd.AddValue ("modelMB",      "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("pfc",          "attach real PFC state machine (1/0)", pfcOn);
  cmd.AddValue ("rttUs",        "RTT (us) for PFC BDP sizing",        rttUs);
  cmd.AddValue ("bufferBDPs",   "ingress buffer in BDPs",             bufferBDPs);
  cmd.AddValue ("simStop",      "simulator stop time (s)",            simStop);
  cmd.AddValue ("K",            "switch radix",                       K);
  cmd.AddValue ("nLeaf",        "number of leaf switches",            nLeaf);
  cmd.AddValue ("pktBytes",     "payload bytes",                      pktBytes);
  cmd.AddValue ("queuePkts",    "physical queue depth (lossless buf)", queuePkts);
  cmd.AddValue ("ecnMinTh",     "RED ECN MinTh (pkts)",               ecnMinTh);
  cmd.AddValue ("ecnMaxTh",     "RED ECN MaxTh (pkts)",               ecnMaxTh);
  cmd.AddValue ("uniformFlows", "number of random flows",             uniformFlows);
  cmd.AddValue ("incastFanin",  "incast fan-in",                      incastFanin);
  cmd.AddValue ("linkRate",     "link rate",                          linkRate);
  cmd.AddValue ("linkDelay",    "link delay",                         linkDelay);
  cmd.Parse (argc, argv);

  uint32_t nSpine         = K / 2;
  uint32_t serversPerLeaf = K / 2;
  uint32_t N              = nLeaf * serversPerLeaf;

  DataRate dr (linkRate);
  double lineRateBps = (double) dr.GetBitRate ();

  if (nLeaf > K / 2)
    std::cerr << "[WARN] nLeaf=" << nLeaf << " > K/2=" << K/2
              << "，拓扑过订阅\n";

  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                      BooleanValue (true));

  std::cout << "=== Leaf-Spine + RoCEv2 (DCQCN) ===\n"
            << "  K=" << K << "  nLeaf=" << nLeaf << "  nSpine=" << nSpine
            << "  N=" << N << "\n"
            << "  Link: " << linkRate << " / " << linkDelay << "\n"
            << "  Lossless: DropTail " << queuePkts << "p (deep buffer)\n"
            << "  ECN: RED MinTh=" << ecnMinTh << "p MaxTh=" << ecnMaxTh << "p\n"
            << "  CC: DCQCN (g=" << Dcqcn::G << " RAI=" << Dcqcn::RAI/1e6
            << "Mbps RT=" << Dcqcn::RT_US << "µs)\n\n";

  // ── 节点 ──
  NodeContainer servers; servers.Create (N);
  NodeContainer leaves;  leaves.Create (nLeaf);
  NodeContainer spines;  spines.Create (nSpine);

  InternetStackHelper inet;
  inet.Install (servers); inet.Install (leaves); inet.Install (spines);

  // ── 链路模板 ──
  // 物理队列：深（256p）→ 近似 PFC 无损，保证 RED 标记前不丢包
  std::ostringstream qs; qs << queuePkts << "p";
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute  ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay",    StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue<Packet>",
                "MaxSize", QueueSizeValue (QueueSize (qs.str ())));

  // QDisc：RED + ECN（标记 CE 而不丢包，直到超过 MaxTh 才随机丢）
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

  // PFC 阈值由 BDP 推导(见 pfc-lossless.h)：headroom=1 BDP，缓冲=bufferBDPs 个 BDP。
  // 每个交换机入口端口(优先级 0)配同一档。
  pfc::PfcConfig pfcCfg = pfc::PfcConfig::FromBDP (lineRateBps, rttUs * 1e-6, bufferBDPs);
  uint32_t pfcPortCount = 0;

  // uSw/vSw 标记链路两端是否为交换机(只有交换机入口才挂 PFC 状态机)。
  auto mkLink = [&] (Ptr<Node> u, Ptr<Node> v, Ipv4Address *ipU = nullptr,
                     bool uSw = false, bool vSw = false)
  {
    NetDeviceContainer dev = p2p.Install (u, v);
    QueueDiscContainer qd = tch.Install (dev);
    dev.Get (0)->TraceConnectWithoutContext ("MacTx",
      MakeCallback (+[](Ptr<const Packet> p){ g_total_bits += p->GetSize ()*8; }));
    dev.Get (1)->TraceConnectWithoutContext ("MacTx",
      MakeCallback (+[](Ptr<const Packet> p){ g_total_bits += p->GetSize ()*8; }));
    // 真 PFC：把交换机入口队列盘的 Enqueue/Dequeue 真事件喂进 pfc::PfcPort。
    if (pfcOn)
      {
        auto hook = [&] (Ptr<QueueDisc> q)
        {
          uint32_t pid = pfcPortCount++;
          g_pfc.Configure (pid, 0, pfcCfg);
          q->TraceConnectWithoutContext ("Enqueue", MakeBoundCallback (&PfcEnqCb, pid));
          q->TraceConnectWithoutContext ("Dequeue", MakeBoundCallback (&PfcDeqCb, pid));
        };
        if (uSw && qd.GetN () > 0) hook (qd.Get (0));
        if (vSw && qd.GetN () > 1) hook (qd.Get (1));
      }
    uint32_t base = subnet * 4;
    std::ostringstream b;
    b << "10." << ((base>>16)&0xff) << "." << ((base>>8)&0xff) << "." << (base&0xff);
    ip.SetBase (b.str ().c_str (), "255.255.255.252");
    Ipv4InterfaceContainer ic = ip.Assign (dev);
    if (ipU) *ipU = ic.GetAddress (0);
    ++subnet;
  };

  // ── 边缘层 & 汇聚层 ──
  for (uint32_t l = 0; l < nLeaf; ++l)
    for (uint32_t s = 0; s < serversPerLeaf; ++s)
      {
        uint32_t sid = l * serversPerLeaf + s;
        Ipv4Address sip;
        mkLink (servers.Get (sid), leaves.Get (l), &sip,
                /*uSw=*/false, /*vSw=*/true);   // leaf 侧入口挂 PFC
        serverIp[sid] = sip;
      }
  for (uint32_t l = 0; l < nLeaf; ++l)
    for (uint32_t sp = 0; sp < nSpine; ++sp)
      mkLink (leaves.Get (l), spines.Get (sp), nullptr,
              /*uSw=*/true, /*vSw=*/true);       // leaf & spine 两侧入口都挂 PFC

  std::cout << "  Links: " << subnet << " total\n";

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  for (uint32_t i = 0; i < N; ++i)
    addr[i] = InetSocketAddress (serverIp[i], DATA_PORT);

  // ── 安装应用（每节点：1 数据 RX + 1 CNP RX）──
  std::vector<Ptr<RoceHost>> apps (N);
  for (uint32_t i = 0; i < N; ++i)
    {
      // 数据接收 socket
      Ptr<Socket> dataRx = Socket::CreateSocket (servers.Get (i),
                             TypeId::LookupByName ("ns3::UdpSocketFactory"));
      dataRx->Bind (InetSocketAddress (Ipv4Address::GetAny (), DATA_PORT));

      // CNP 接收 socket（发端监听此端口等待拥塞反馈）
      Ptr<Socket> cnpRx = Socket::CreateSocket (servers.Get (i),
                            TypeId::LookupByName ("ns3::UdpSocketFactory"));
      cnpRx->Bind (InetSocketAddress (Ipv4Address::GetAny (), CNP_PORT));

      Ptr<RoceHost> a = CreateObject<RoceHost> ();
      a->Setup (i, dataRx, cnpRx, addr, lineRateBps);
      servers.Get (i)->AddApplication (a);
      a->SetStartTime (Seconds (0.0));
      a->SetStopTime  (Seconds (30.0));
      apps[i] = a;
    }

  // ── 调度流量（全部立即入队，由 DCQCN 限速器控制实际发送时机）──
  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  uint64_t pid = 1;

  if (scenario == "incast")
    {
      // 全部在 t=1.0 入队，让 DCQCN 自然处理 incast 拥塞
      uint32_t dst = 0;
      for (uint32_t s = 1; s < N && (s-1) < incastFanin; ++s)
        for (uint32_t m = 0; m < 64; ++m)
          Simulator::Schedule (Seconds (1.0), &RoceHost::EnqueueAndSend,
                               apps[s], dst, pktBytes, pid++);
      std::cout << "  Scenario: incast fan-in="
                << std::min (incastFanin, N-1) << " -> server 0\n";
    }
  else if (scenario == "allreduce")
    {
      for (uint32_t s = 0; s < N; ++s)
        {
          uint32_t dst = (s + 1) % N;
          for (uint32_t m = 0; m < 64; ++m)
            Simulator::Schedule (Seconds (1.0), &RoceHost::EnqueueAndSend,
                                 apps[s], dst, pktBytes, pid++);
        }
      std::cout << "  Scenario: allreduce ring (N=" << N << ")\n";
    }
  else if (scenario == "bisection")
    {
      for (uint32_t s = 0; s < N / 2; ++s)
        for (uint32_t m = 0; m < 64; ++m)
          Simulator::Schedule (Seconds (1.0), &RoceHost::EnqueueAndSend,
                               apps[s], N/2 + s, pktBytes, pid++);
      std::cout << "  Scenario: bisection (left half -> right half)\n";
    }
  else if (scenario == "nccl_ar")
    {
      // 跨拓扑对比工作负载：真 NCCL Ring AllReduce(arRanks 个 rank 跨网铺开)。
      // 开环回放——每个 CommOp 拆成包，全部在 t=1.0 入队，由 DCQCN 自调速。
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      std::vector<uint32_t> ranks;
      std::vector<nccl::CommOp> ops = cmp::BuildComparisonAllReduce (N, arRanks, M, &ranks);
      uint64_t injected = 0;
      for (const auto &o : ops)
        {
          if (o.onNvlink) continue;
          uint32_t nPk = (uint32_t) nccl::ToPackets (o.bytes, pktBytes);
          for (uint32_t k = 0; k < nPk; ++k)
            {
              Simulator::Schedule (Seconds (1.0), &RoceHost::EnqueueAndSend,
                                   apps[o.src], o.dst, pktBytes, pid++);
              ++injected;
            }
        }
      g_inject_start = 1.0;
      std::cout << "  Scenario: nccl_ar Ring AllReduce  arRanks=" << ranks.size ()
                << "  M=" << modelMB << "MiB  ops=" << ops.size ()
                << "  pkts=" << injected
                << "  (PFC=" << (pfcOn ? "on" : "off") << ")\n";
    }
  else  // uniform
    {
      double when = 1.0;
      for (uint32_t k = 0; k < uniformFlows; ++k)
        {
          uint32_t s = rng->GetInteger (0, N-1);
          uint32_t d = rng->GetInteger (0, N-1);
          if (s == d) continue;
          Simulator::Schedule (Seconds (when), &RoceHost::EnqueueAndSend,
                               apps[s], d, pktBytes, pid++);
          when += 5e-7;
        }
      std::cout << "  Scenario: uniform (" << uniformFlows << " flows)\n";
    }

  // ── 运行 ──
  double simDuration = simStop;
  Simulator::Stop (Seconds (simDuration));
  std::cout << "  Running...\n\n";
  Simulator::Run ();
  Simulator::Destroy ();

  // ── 统计 ──
  uint64_t delivered = g_done.size ();
  uint64_t dropped   = (g_sent >= delivered) ? (g_sent - delivered) : 0;
  double rxDurSec    = (g_last_recv > g_first_recv && g_first_recv > 0)
                       ? (g_last_recv - g_first_recv) / 1e9 : 0.0;
  double tputGbps    = rxDurSec > 0
                       ? (delivered * pktBytes * 8.0) / (rxDurSec * 1e9) : 0.0;

  std::vector<double> lats;
  lats.reserve (g_done.size ());
  for (auto &p : g_done)
    if (p.recvNs > 0) lats.push_back (p.recvNs - p.sentNs);
  std::sort (lats.begin (), lats.end ());
  double meanUs=0, p50Us=0, p99Us=0, maxUs=0;
  if (!lats.empty ())
    {
      double sum=0; for (double v : lats) sum += v;
      meanUs = sum / lats.size () / 1000.0;
      p50Us  = lats[lats.size()*50/100] / 1000.0;
      p99Us  = lats[lats.size()*99/100] / 1000.0;
      maxUs  = lats.back () / 1000.0;
    }

  uint32_t nSwitches   = nLeaf + nSpine;
  double P_srv_static  = 2000.0, P_srv_full = 8000.0;
  double P_sw          = 300.0,  E_dyn_bit  = 10e-12;
  double staticEnergy  = (N * P_srv_static + nSwitches * P_sw) * simDuration;
  double computeEnergy = rxDurSec > 0 ? N * (P_srv_full-P_srv_static) * rxDurSec : 0;
  double netEnergy     = g_total_bits * E_dyn_bit;
  double totalEnergy   = staticEnergy + computeEnergy + netEnergy;

  std::cout << "=== Results: scenario=" << scenario
            << " K=" << K << " N=" << N << " ===\n";
  std::cout << "sent=" << g_sent << "  delivered=" << delivered
            << "  dropped=" << dropped
            << " (" << (g_sent ? 100.0*dropped/g_sent : 0.0) << "%)\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "吞吐量 (Throughput)  : " << tputGbps << " Gbps\n";
  std::cout << "Duration            : " << rxDurSec  << " s\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "延迟 (Latency)\n";
  std::cout << "  mean=" << meanUs << " µs  p50=" << p50Us
            << " µs  p99=" << p99Us << " µs  max=" << maxUs << " µs\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "RoCEv2 / DCQCN 诊断\n";
  std::cout << "  CE 标记包数  : " << g_ce_pkts << "\n";
  std::cout << "  CNP 发出     : " << g_cnp_sent << "\n";
  std::cout << "  CNP 收到     : " << g_cnp_recv << "\n";
  std::cout << "  CE 率        : "
            << (delivered > 0 ? 100.0*g_ce_pkts/delivered : 0.0) << "%\n";
  std::cout << "-------------------------------------------\n";
  std::cout << "全网总能耗 (Total Energy): " << totalEnergy << " J\n";
  std::cout << "  ├─ 服务器静态     : " << N*P_srv_static*simDuration
            << " J\n";
  std::cout << "  ├─ 服务器计算增量 : " << computeEnergy << " J\n";
  std::cout << "  ├─ 交换机静态     : " << nSwitches*P_sw*simDuration
            << " J  (" << nSwitches << " × 300W)\n";
  std::cout << "  └─ 链路动态       : " << netEnergy << " J\n";
  std::cout << "Avg Power           : " << totalEnergy/simDuration << " W\n";
  std::cout << "-------------------------------------------\n";

  std::ofstream csv ("leaf_spine_roce_result.csv");
  csv << "scenario,K,nLeaf,nSpine,N,ecn_min,ecn_max,"
         "sent,delivered,dropped,ce_pkts,cnp_sent,cnp_recv,"
         "throughput_Gbps,mean_us,p99_us,total_energy_J\n";
  csv << scenario << "," << K << "," << nLeaf << "," << nSpine << "," << N
      << "," << ecnMinTh << "," << ecnMaxTh << ","
      << g_sent << "," << delivered << "," << dropped << ","
      << g_ce_pkts << "," << g_cnp_sent << "," << g_cnp_recv << ","
      << tputGbps << "," << meanUs << "," << p99Us << ","
      << totalEnergy << "\n";
  std::cout << "wrote leaf_spine_roce_result.csv\n";

  // ═══════════ 统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════════
  // JCT = all-reduce 注入起点 → 最后一包交付；非 nccl_ar 时退回稳态收包时长。
  double jct = (g_inject_start > 0 && g_last_recv > 0)
               ? (g_last_recv / 1e9 - g_inject_start) : rxDurSec;
  {
    EnergyModel em;
    // 两层 Leaf-Spine 清单：N server、nLeaf、nSpine、leaf-spine 链路数 = nLeaf*nSpine。
    em.inv = EnergyInventory::LeafSpine (N, nLeaf, nSpine, (uint64_t) nLeaf * nSpine,
                                         lineRateBps / 1e9, /*gpuPerSrv=*/1);
    em.SetBits (g_total_bits);
    em.SetDuration (jct > 0 ? jct : simDuration);
    em.SetTraffic (delivered, pktBytes);
    em.SetThroughput (tputGbps);
    em.WriteCsv ("energy_unified.csv", "leaf_spine", scenario);
    em.PrintSummary ("leaf_spine", scenario);

    // PFC 汇报：真 PFC 下应零丢；PAUSE 时长/峰值占用是 HOL/拥塞扩散信号。
    if (pfcOn)
      {
        std::cout << "  — PFC (real state machine on switch ingress) —\n";
        std::cout << "    ingress ports : " << g_pfc.NumPorts ()
                  << "   drops=" << g_pfc.TotalDrops () << " (must be 0 = lossless)\n";
        std::cout << "    peak occ      : " << g_pfc.MaxPeak () / 1024.0 << " KB"
                  << "   pausedPorts=" << g_pfc.PausedPorts ()
                  << "   sumPausedSec=" << g_pfc.TotalPausedSec () << "\n";
      }

    if (scenario == "nccl_ar")
      {
        uint64_t M = (uint64_t) modelMB * 1024 * 1024;
        uint32_t Pranks = (uint32_t) cmp::StridedRanks (N, arRanks).size ();
        cmp::CmpRow row;
        row.topo = "leaf_spine"; row.arRanks = Pranks; row.modelBytes = M;
        row.lineRateGbps = lineRateBps / 1e9; row.jctSec = jct;
        row.sent = g_sent; row.delivered = delivered; row.dropped = dropped;
        row.tputGbps = tputGbps;
        row.algbwGbps = cmp::AlgBwGbps (M, jct);
        row.busbwGbps = cmp::BusBwGbps (M, jct, Pranks);
        row.busbwEff  = row.busbwGbps / (lineRateBps / 1e9);
        row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
        row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
        row.bisectionGbps = em.inv.bisectionGbps;
        if (pfcOn) { row.pfcPausedSec = g_pfc.TotalPausedSec ();
                     row.pfcPeakKB = g_pfc.MaxPeak () / 1024.0;
                     row.pfcDrops = g_pfc.TotalDrops (); }
        cmp::WriteCmpCsv ("cmp_leaf_spine.csv", row);
        std::cout << "wrote cmp_leaf_spine.csv  (busbw=" << row.busbwGbps
                  << " Gbps, eff=" << row.busbwEff << ")\n";
      }
  }
  return 0;
}
