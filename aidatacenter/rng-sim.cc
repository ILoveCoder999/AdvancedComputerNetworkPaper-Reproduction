/*
 * rng-sim.cc
 *
 * 复现论文：RNG: Flat Datacenter Networks at Scale (SIGCOMM 2026)
 * ─────────────────────────────────────────────────────────────────
 * 目标：重现 Figure 13 — 过订阅比 vs 活跃比例（clique/hubs/matching）
 *       RNG vs Fat-Tree 相同规模下的吞吐对比
 *
 * 拓扑（论文 §9.3）
 *  RNG (--topology=rng)：随机正则图(RRG)，n 节点、d 度(默认 n=100,d=16;
 *       论文 n=1000,d=64 的缩小版)。规模运行时可调:--N / --degree。
 *  Fat-Tree (--topology=fattree)：n Leaf + d Spine,每 Leaf d 条上行(无阻塞)。
 *
 * 路由(§5)：两种拓扑都用 GlobalRouting + RandomEcmpRouting(近似 Spraypoint)。
 *
 * 流量(§9.3 / Figure 13)：clique(f) / hubs(f) / matching(f),t=1.0s 突发注入。
 *   另:--pattern=nccl_ar 走跨拓扑对比的真 NCCL Ring AllReduce(每源线速起拍)。
 *
 * 过订阅近似(图 13 纵轴)：oversub ≈ 1 / (delivered/sent)。
 *
 * 使用：
 *   ./rng-sim --topology=rng     --scanAll=1
 *   ./rng-sim --topology=fattree --scanAll=1
 *   ./rng-sim --topology=rng --pattern=nccl_ar --arRanks=32 --modelMB=8 --N=1296 --degree=16 --queuePkts=256
 *   单次:  ./rng-sim --topology=rng --pattern=clique --f=0.3
 *
 * !! 规模合法性:RRG 要求 degree < N 且 N*degree 为偶数。原版常量曾被设成
 *    N=32,d=32(d≥N 不可能)→ 拓扑生成进 fallback 后死循环。现默认 100/16(合法),
 *    并在 main 里钳制 d<N、保证 N*d 为偶,且 fallback 加"无进展即退出"防死循环。
 */

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include <deque>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/traffic-control-module.h"
#include "ns3/ipv4-global-routing-helper.h"

// —— 接入：NCCL 流量层 + 统一能耗模型(RNG = 扁平随机正则图,无交换机 → Switchless) ——
#include "cmp-common.h"        // 跨拓扑对比共享层(含 nccl-collectives.h)
#include "energy-model.h"      // 统一网络能耗(apples-to-apples)

using namespace ns3;
NS_LOG_COMPONENT_DEFINE ("RngSim");

// ─── 拓扑参数(运行时可调:--N / --degree;默认合法的 100/16)──────────────────
static uint32_t N_RNG    = 100;  // RNG 节点数(论文 n=1000 的缩小版)
static uint32_t D_RNG    = 16;   // RNG 节点度(必须 < N_RNG,且 N_RNG*D_RNG 为偶)
static uint32_t N_LEAF   = N_RNG;        // = N_RNG(main 解析后重算)
static uint32_t N_SPINE  = D_RNG;        // Spine 数 = Leaf 上行链路数 = D_RNG

// ─── 流量参数 ──────────────────────────────────────────────────────────────
static const uint32_t BURST_PKTS = 100;   // 每条流突发包数（小队列下快速产生拥塞）
static const uint32_t PKT_BYTES  = 1024;
static const uint16_t DATA_PORT  = 9000;

// ─── 全局统计 ──────────────────────────────────────────────────────────────
struct RngProbe { double sentNs; double recvNs; };
static std::map<uint64_t, RngProbe> g_inflight;
static std::vector<RngProbe>        g_done;
static uint64_t                     g_sent      = 0;
static double                       g_first_recv = -1.0;
static double                       g_last_recv  = -1.0;
static uint64_t                     g_total_bits = 0;
static double                       g_inject_start = -1.0;  // all-reduce 注入起点(算 JCT)

// ─── 通用 Host（IP 路由，不需要应用层转发）─────────────────────────────────
class NetHost : public Application
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId t = TypeId ("NetHost")
      .SetParent<Application> ()
      .AddConstructor<NetHost> ();
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

    RngProbe pr; pr.sentNs = Simulator::Now ().GetNanoSeconds (); pr.recvNs = 0;
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
  { m_rx->SetRecvCallback (MakeCallback (&NetHost::OnRecv, this)); }
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

// ─── 生成 RRG（configuration model）- 修复死循环问题 ──────────────────────
std::vector<std::pair<uint32_t,uint32_t>>
GenerateRRG (uint32_t n, uint32_t d, Ptr<UniformRandomVariable> rng)
{
  std::vector<std::pair<uint32_t,uint32_t>> edges;
  int attempts = 0;
  const int MAX_ATTEMPTS = 200;

  while (attempts < MAX_ATTEMPTS)
    {
      ++attempts;
      std::vector<uint32_t> stubs;
      stubs.reserve (n * d);
      for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < d; ++j)
          stubs.push_back (i);

      // Fisher-Yates 洗牌
      for (int64_t i = (int64_t)stubs.size()-1; i > 0; --i)
        {
          uint32_t j = rng->GetInteger(0, (uint32_t)i);
          std::swap(stubs[i], stubs[j]);
        }

      bool valid = true;
      std::set<std::pair<uint32_t,uint32_t>> seen;
      edges.clear ();

      for (size_t i = 0; i+1 < stubs.size(); i += 2)
        {
          uint32_t u = stubs[i], v = stubs[i+1];
          if (u == v) { valid = false; break; }
          auto key = std::make_pair (std::min(u,v), std::max(u,v));
          if (seen.count(key)) { valid = false; break; }
          seen.insert(key);
          edges.push_back ({u, v});
        }

      if (valid)
        {
          std::cout << "  RRG generated after " << attempts << " attempt(s)\n";
          return edges;
        }
    }

  // 备用方案：构建一个近似的 d-正则图（环 + 随机补边）
  std::cout << "  RRG generation failed after " << MAX_ATTEMPTS
            << " attempts, using fallback method\n";
  edges.clear();

  // 先构建一个环（度数2）
  for (uint32_t i = 0; i < n; ++i)
    edges.push_back ({i, (i+1) % n});

  std::vector<uint32_t> deg(n, 0);
  for (auto &e : edges) { deg[e.first]++; deg[e.second]++; }

  uint32_t max_extra_edges = 10000;
  uint32_t extra_edges = 0;

  while (extra_edges < max_extra_edges) {
    bool all_done = true;
    bool added    = false;          // FIX: 跟踪本轮是否真的加了边
    for (uint32_t i = 0; i < n; ++i) {
      if (deg[i] < d) {
        all_done = false;
        for (uint32_t j = 0; j < n; ++j) {
          if (i != j && deg[j] < d) {
            auto key = std::make_pair(std::min(i,j), std::max(i,j));
            bool already = false;
            for (auto &e : edges) {
              if ((e.first == key.first && e.second == key.second) ||
                  (e.first == key.second && e.second == key.first)) { already = true; break; }
            }
            if (!already) {
              edges.push_back({i, j}); deg[i]++; deg[j]++; extra_edges++; added = true; break;
            }
          }
        }
        break;
      }
    }
    if (all_done) break;
    if (!added) break;   // FIX: 图已饱和(再也加不动边)→ 退出,避免死循环(原 bug)
  }

  std::cout << "  Fallback graph generated with " << edges.size() << " edges\n";
  std::cout << "  Node degrees: min=" << *std::min_element(deg.begin(), deg.end())
            << " max=" << *std::max_element(deg.begin(), deg.end()) << "\n";
  return edges;
}

// ─── 运行单次仿真，返回统计数据 ───────────────────────────────────────────
struct SimResult
{
  uint64_t sent, delivered, dropped;
  double   deliveryRate;
  double   oversubApprox;
  double   throughputGbps;
  double   meanUs, p99Us;
};

SimResult RunSim (const std::string &topology,
                  const std::string &pattern,
                  double f,
                  uint32_t queuePkts,
                  const std::string &linkRate,
                  const std::string &linkDelay,
                  uint32_t arRanks = 32,
                  uint32_t modelMB = 8,
                  uint32_t fpgaPerNode = 8,
                  double   opticalFrac = 1.0)
{
  g_inflight.clear (); g_done.clear ();
  g_sent = 0; g_first_recv = -1.0; g_last_recv = -1.0; g_total_bits = 0;

  bool isRng = (topology == "rng");
  uint32_t N_ep = isRng ? N_RNG : N_LEAF;

  Config::SetDefault ("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                      BooleanValue (true));

  NodeContainer endpoints; endpoints.Create (N_ep);
  NodeContainer spines;
  if (!isRng) spines.Create (N_SPINE);

  InternetStackHelper inet;
  inet.Install (endpoints);
  if (!isRng) inet.Install (spines);

  std::ostringstream qs; qs << queuePkts << "p";
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute  ("DataRate", StringValue (linkRate));
  p2p.SetChannelAttribute ("Delay",    StringValue (linkDelay));
  p2p.SetQueue ("ns3::DropTailQueue<Packet>",
                "MaxSize", QueueSizeValue (QueueSize (qs.str ())));
  TrafficControlHelper tch;
  tch.SetRootQueueDisc ("ns3::FifoQueueDisc", "MaxSize", StringValue (qs.str ()));

  Ipv4AddressHelper ip;
  std::map<uint32_t, Ipv4Address> epIp;
  uint32_t subnet = 0;

  auto mkLink = [&] (Ptr<Node> u, Ptr<Node> v) -> std::pair<Ipv4Address,Ipv4Address>
  {
    NetDeviceContainer dev = p2p.Install (u, v);
    tch.Install (dev);
    dev.Get(0)->TraceConnectWithoutContext("MacTx",
      MakeCallback(+[](Ptr<const Packet> p){ g_total_bits += p->GetSize()*8; }));
    dev.Get(1)->TraceConnectWithoutContext("MacTx",
      MakeCallback(+[](Ptr<const Packet> p){ g_total_bits += p->GetSize()*8; }));
    uint32_t base = subnet * 4;
    std::ostringstream b;
    b << "10." << ((base>>16)&0xff) << "." << ((base>>8)&0xff) << "." << (base&0xff);
    ip.SetBase (b.str().c_str(), "255.255.255.252");
    Ipv4InterfaceContainer ic = ip.Assign (dev);
    ++subnet;
    return { ic.GetAddress(0), ic.GetAddress(1) };
  };

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();

  if (isRng)
    {
      auto edges = GenerateRRG (N_RNG, D_RNG, rng);
      for (auto &e : edges)
        {
          auto [ipU, ipV] = mkLink (endpoints.Get(e.first), endpoints.Get(e.second));
          if (!epIp.count(e.first))  epIp[e.first]  = ipU;
          if (!epIp.count(e.second)) epIp[e.second] = ipV;
        }
      std::cout << "  RNG topology: N=" << N_RNG << " D=" << D_RNG
                << " edges=" << edges.size() << "\n";
    }
  else
    {
      for (uint32_t l = 0; l < N_LEAF; ++l)
        for (uint32_t sp = 0; sp < N_SPINE; ++sp)
          {
            auto [ipL, ipSp] = mkLink (endpoints.Get(l), spines.Get(sp));
            if (!epIp.count(l)) epIp[l] = ipL;
          }
      std::cout << "  Fat-Tree: " << N_LEAF << " Leaf + " << N_SPINE
                << " Spine, " << (N_LEAF * N_SPINE) << " links\n";
    }

  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  std::map<uint32_t, Address> addr;
  for (uint32_t i = 0; i < N_ep; ++i)
    addr[i] = InetSocketAddress (epIp[i], DATA_PORT);

  std::vector<Ptr<NetHost>> apps (N_ep);
  for (uint32_t i = 0; i < N_ep; ++i)
    {
      Ptr<Socket> rx = Socket::CreateSocket (endpoints.Get(i),
                         TypeId::LookupByName("ns3::UdpSocketFactory"));
      rx->Bind (InetSocketAddress (Ipv4Address::GetAny(), DATA_PORT));
      Ptr<NetHost> a = CreateObject<NetHost>();
      a->Setup (i, rx, addr);
      endpoints.Get(i)->AddApplication (a);
      a->SetStartTime (Seconds(0.0));
      a->SetStopTime  (Seconds(30.0));
      apps[i] = a;
    }

  uint32_t nActive = std::max (2u, (uint32_t)std::ceil (f * N_ep));
  std::vector<uint32_t> perm (N_ep);
  for (uint32_t i = 0; i < N_ep; ++i) perm[i] = i;
  for (uint32_t i = N_ep-1; i > 0; --i)
    { uint32_t j = rng->GetInteger(0,i); std::swap(perm[i],perm[j]); }
  std::vector<uint32_t> active (perm.begin(), perm.begin() + nActive);

  uint64_t pid = 1;
  uint64_t nFlows = 0;
  const double T0 = 1.0;

  if (pattern == "clique")
    {
      for (uint32_t s : active)
        for (uint32_t d : active)
          if (s != d)
            {
              for (uint32_t m = 0; m < BURST_PKTS; ++m)
                Simulator::Schedule (Seconds(T0), &NetHost::Send, apps[s], d, PKT_BYTES, pid++);
              ++nFlows;
            }
    }
  else if (pattern == "hubs")
    {
      for (uint32_t s = 0; s < N_ep; ++s)
        for (uint32_t h : active)
          if ((uint32_t)s != h)
            {
              for (uint32_t m = 0; m < BURST_PKTS; ++m)
                Simulator::Schedule (Seconds(T0), &NetHost::Send, apps[s], h, PKT_BYTES, pid++);
              ++nFlows;
            }
    }
  else if (pattern == "nccl_ar")
    {
      // 跨拓扑对比工作负载：真 NCCL Ring AllReduce(arRanks 个 rank 跨网铺开)。
      // RNG flat 网络无 CC,按每源线速起拍注入(不同于论文 fig13 的瞬时突发)。
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      DataRate drr (linkRate);
      double serSec = (PKT_BYTES * 8.0) / (double) drr.GetBitRate ();
      std::vector<uint32_t> ranks = cmp::StridedRanks (N_ep, arRanks);
      std::vector<nccl::CommOp> ops;
      nccl::RingAllReduce (ranks, M, ops);
      std::vector<double> nextT (N_ep, T0);
      for (const auto &o : ops)
        {
          if (o.onNvlink) continue;
          uint32_t nPk = (uint32_t) nccl::ToPackets (o.bytes, PKT_BYTES);
          for (uint32_t k = 0; k < nPk; ++k)
            {
              Simulator::Schedule (Seconds (nextT[o.src]), &NetHost::Send,
                                   apps[o.src], o.dst, PKT_BYTES, pid++);
              nextT[o.src] += serSec;
            }
        }
      nFlows = ops.size ();
      g_inject_start = T0;
    }
  else // matching
    {
      uint32_t nPairs = nActive / 2;
      for (uint32_t k = 0; k < nPairs; ++k)
        {
          uint32_t s = active[k*2], d = active[k*2+1];
          for (uint32_t m = 0; m < BURST_PKTS; ++m)
            {
              Simulator::Schedule (Seconds(T0), &NetHost::Send, apps[s], d, PKT_BYTES, pid++);
              Simulator::Schedule (Seconds(T0), &NetHost::Send, apps[d], s, PKT_BYTES, pid++);
            }
          nFlows += 2;
        }
    }

  std::cout << "  pattern=" << pattern << " f=" << f
            << " nActive=" << nActive << " nFlows=" << nFlows
            << " nPkts=" << (pid-1) << "\n";

  Simulator::Stop (Seconds(3.0));
  Simulator::Run ();
  Simulator::Destroy ();

  SimResult r;
  r.sent      = g_sent;
  r.delivered = g_done.size();
  r.dropped   = (r.sent >= r.delivered) ? (r.sent - r.delivered) : 0;
  r.deliveryRate   = r.sent > 0 ? (double)r.delivered / r.sent : 0.0;
  r.oversubApprox  = r.deliveryRate > 0.001 ? 1.0 / r.deliveryRate : 99.0;

  double rxDur = (g_last_recv > g_first_recv && g_first_recv > 0)
                 ? (g_last_recv - g_first_recv) / 1e9 : 0.0;
  r.throughputGbps = rxDur > 0 ? (r.delivered * PKT_BYTES * 8.0) / (rxDur * 1e9) : 0.0;

  std::vector<double> lats;
  for (auto &p : g_done) if (p.recvNs > 0) lats.push_back (p.recvNs - p.sentNs);
  std::sort (lats.begin(), lats.end());
  r.meanUs = r.p99Us = 0.0;
  if (!lats.empty())
    {
      double sum=0; for (double v:lats) sum+=v;
      r.meanUs = sum/lats.size()/1000.0;
      r.p99Us  = lats[lats.size()*99/100]/1000.0;
    }

  // ═══════ nccl_ar：统一网络能耗 + 跨拓扑对比输出(energy-model.h / cmp-common.h) ═══════
  if (pattern == "nccl_ar")
    {
      double jct = (g_inject_start > 0 && g_last_recv > 0)
                   ? (g_last_recv / 1e9 - g_inject_start) : 0.0;
      DataRate drr (linkRate);
      double lineRateGbps = (double) drr.GetBitRate () / 1e9;
      uint64_t M = (uint64_t) modelMB * 1024 * 1024;
      uint32_t P = (uint32_t) cmp::StridedRanks (N_ep, arRanks).size ();

      EnergyModel em;
      if (isRng)
        {
          uint64_t nLinks  = subnet;                   // = RRG 边数
          uint32_t bisLinks = N_RNG * D_RNG / 4;       // 随机正则图(expander)平衡割估计
          em.inv = EnergyInventory::Switchless (N_ep, nLinks, fpgaPerNode,
                                                lineRateGbps, opticalFrac, bisLinks);
        }
      else
        {
          em.inv = EnergyInventory::LeafSpine (N_ep, N_LEAF, N_SPINE,
                                               (uint64_t) N_LEAF * N_SPINE, lineRateGbps, 1);
        }
      em.SetBits (g_total_bits);
      em.SetDuration (jct > 0 ? jct : 1.0);
      em.SetTraffic (r.delivered, PKT_BYTES);
      em.SetThroughput (r.throughputGbps);
      std::string topoTag = isRng ? "rng" : "rng_fattree";
      em.WriteCsv ("energy_unified.csv", topoTag, "nccl_ar");
      em.PrintSummary (topoTag, "nccl_ar");

      cmp::CmpRow row;
      row.topo = topoTag; row.arRanks = P; row.modelBytes = M;
      row.lineRateGbps = lineRateGbps; row.jctSec = jct;
      row.sent = r.sent; row.delivered = r.delivered; row.dropped = r.dropped;
      row.tputGbps = r.throughputGbps;
      row.algbwGbps = cmp::AlgBwGbps (M, jct);
      row.busbwGbps = cmp::BusBwGbps (M, jct, P);
      row.busbwEff  = lineRateGbps > 0 ? row.busbwGbps / lineRateGbps : 0;
      row.totalEnergyJ = em.TotalEnergyJ (); row.avgPowerW = em.AvgPowerW ();
      row.pjPerBit = em.EnergyPerDeliveredBitPj (); row.gbpsPerW = em.TputPerWatt ();
      row.bisectionGbps = em.inv.bisectionGbps;
      cmp::WriteCmpCsv ("cmp_" + topoTag + ".csv", row);
      std::cout << "wrote cmp_" << topoTag << ".csv  (busbw=" << row.busbwGbps
                << " Gbps, eff=" << row.busbwEff << ")\n";
    }
  return r;
}

// ─── main ──────────────────────────────────────────────────────────────────
int main (int argc, char *argv[])
{
  std::string topology  = "rng";     // "rng" | "fattree"
  std::string pattern   = "clique";  // "clique" | "hubs" | "matching" | "nccl_ar"
  double      f         = 0.3;
  uint32_t    queuePkts = 8;
  std::string linkRate  = "100Gbps";
  std::string linkDelay = "200ns";
  bool        scanAll   = false;
  // —— 跨拓扑对比 all-reduce(pattern=nccl_ar) —— //
  uint32_t    arRanks     = 32;
  uint32_t    modelMB     = 8;
  uint32_t    fpgaPerNode = 8;
  double      opticalFrac = 1.0;

  CommandLine cmd;
  cmd.AddValue ("topology",  "rng|fattree",              topology);
  cmd.AddValue ("pattern",   "clique|hubs|matching|nccl_ar", pattern);
  cmd.AddValue ("f",         "active fraction [0,1]",    f);
  cmd.AddValue ("queuePkts", "queue depth (packets)",    queuePkts);
  cmd.AddValue ("linkRate",  "link rate",                linkRate);
  cmd.AddValue ("linkDelay", "link delay",               linkDelay);
  cmd.AddValue ("scanAll",   "1=scan all f values → Figure 13 CSV", scanAll);
  cmd.AddValue ("N",         "RNG/Fat-Tree 端点数(节点数);冒烟用小值",       N_RNG);
  cmd.AddValue ("degree",    "RNG 度数 d(必须 < N,且 N*d 为偶)",            D_RNG);
  cmd.AddValue ("arRanks",   "nccl_ar: all-reduce rank count", arRanks);
  cmd.AddValue ("modelMB",   "nccl_ar: gradient size M per rank (MiB)", modelMB);
  cmd.AddValue ("fpgaPerNode", "nccl_ar: switchless forwarding engines per node", fpgaPerNode);
  cmd.AddValue ("opticalFrac", "nccl_ar: fraction of ports on optics", opticalFrac);
  cmd.Parse (argc, argv);

  // 规模合法性钳制:RRG 需 degree < N 且 N*degree 为偶,否则拓扑生成会进 fallback/死循环
  if (N_RNG < 2) N_RNG = 2;
  if (D_RNG >= N_RNG) { D_RNG = N_RNG - 1; std::cout << "[warn] degree>=N,钳到 " << D_RNG << "\n"; }
  if (((uint64_t) N_RNG * D_RNG) % 2 != 0) { if (D_RNG > 1) --D_RNG; else ++N_RNG;
    std::cout << "[warn] N*degree 为奇,调到 N=" << N_RNG << " d=" << D_RNG << "\n"; }
  N_LEAF = N_RNG; N_SPINE = D_RNG;

  std::cout << "=== RNG Paper Figure 13 Reproduction ===\n"
            << "  Topology : " << topology << " (N_ep="
            << (topology=="rng" ? N_RNG : N_LEAF) << ", d=" << D_RNG << ")\n"
            << "  Link     : " << linkRate << " / " << linkDelay << "\n"
            << "  Queue    : " << queuePkts << "p\n\n";

  // 跨拓扑对比快路径:pattern=nccl_ar → 只跑一次 all-reduce,写 cmp_<topo>.csv,跳过 fig13
  if (pattern == "nccl_ar")
    {
      std::cout << "  [nccl_ar] 跨拓扑对比 all-reduce  arRanks=" << arRanks
                << "  M=" << modelMB << "MiB  N=" << N_RNG
                << "  (queue 建议调大,如 --queuePkts=256)\n\n";
      RunSim (topology, "nccl_ar", f, queuePkts, linkRate, linkDelay,
              arRanks, modelMB, fpgaPerNode, opticalFrac);
      return 0;
    }

  std::vector<std::string> patterns;
  std::vector<double>      fracs;
  if (scanAll)
    {
      patterns = {"clique", "hubs", "matching"};
      fracs    = {0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0};
    }
  else { patterns = {pattern}; fracs = {f}; }

  std::string csvName = "rng_fig13_" + topology + ".csv";
  std::ofstream csv (csvName);
  csv << "topology,pattern,f,sent,delivered,drop_pct,"
         "delivery_rate,oversub_approx,throughput_Gbps,mean_us,p99_us\n";

  for (const std::string &pat : patterns)
    for (double fv : fracs)
      {
        std::cout << "─── " << topology << "  pattern=" << pat
                  << "  f=" << fv << " ───\n";
        SimResult r = RunSim (topology, pat, fv, queuePkts, linkRate, linkDelay);
        printf ("  sent=%-8lu  delivered=%-8lu  drop=%.1f%%\n",
                r.sent, r.delivered, r.sent ? 100.0*r.dropped/r.sent : 0.0);
        printf ("  delivery_rate=%.3f  oversub≈%.2f  tput=%.2f Gbps\n",
                r.deliveryRate, r.oversubApprox, r.throughputGbps);
        printf ("  latency mean=%.1fµs  p99=%.1fµs\n\n", r.meanUs, r.p99Us);
        csv << topology << "," << pat << "," << fv << ","
            << r.sent << "," << r.delivered << ","
            << (r.sent ? 100.0*r.dropped/r.sent : 0.0) << ","
            << r.deliveryRate << "," << r.oversubApprox << ","
            << r.throughputGbps << "," << r.meanUs << "," << r.p99Us << "\n";
        csv.flush ();
      }

  std::cout << "wrote " << csvName << "\n";
  return 0;
}
