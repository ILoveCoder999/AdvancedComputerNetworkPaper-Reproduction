#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/flow-monitor-module.h"

#include "fat-tree-routing.h"           // 我们自定义的双层路由
#include "fat-tree-central-scheduler.h" // 中央流调度器 (mode 2)

#include <algorithm>
#include <random>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("FatTreeSimulation");

// 由四个字节构造一个 IPv4 地址，例如 MakeIp(10,pod,sw,id)
static Ipv4Address
MakeIp (uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
  uint32_t v = (a << 24) | (b << 16) | (c << 8) | d;
  return Ipv4Address (v);
}

// 给某节点的某网卡配置 IP，返回它在该节点上的接口索引
static uint32_t
AssignAddr (Ptr<Node> node, Ptr<NetDevice> dev, Ipv4Address addr, Ipv4Mask mask)
{
  Ptr<Ipv4> ipv4 = node->GetObject<Ipv4> ();
  int32_t iff = ipv4->GetInterfaceForDevice (dev);
  if (iff < 0)
    {
      iff = ipv4->AddInterface (dev);
    }
  ipv4->AddAddress (iff, Ipv4InterfaceAddress (addr, mask));
  ipv4->SetMetric (iff, 1);
  ipv4->SetUp (iff);
  return (uint32_t) iff;
}

int
main (int argc, char *argv[])
{
  uint32_t k = 4;            // k 叉胖树 (论文默认 k=4)
  double simTime = 10.0;     // 仿真时长(秒)；论文用 60s，调试时可调小
  std::string linkRate = "96Mbps"; // 论文 5.1 节限速
  std::string pattern = "random";  // 通信模式: random | stride
  uint32_t strideVal = 1;          // stride 模式的步长 i
  uint32_t seed = 1;               // random 模式的随机种子(便于复现)
  std::string routing = "twolevel"; // 上行分流: twolevel | classify | schedule
  double classifyInterval = 1.0;    // 流重排周期(秒)，仅 classify 模式
  std::string transport = "udp";    // 传输层: udp | tcp
  double headroom = 1.08;           // 物理链路相对应用速率的线速余量(给包头留空间)

  CommandLine cmd;
  cmd.AddValue ("k", "Fat-tree degree k (必须为偶数)", k);
  cmd.AddValue ("simTime", "Simulation time in seconds", simTime);
  cmd.AddValue ("linkRate", "Per-link data rate", linkRate);
  cmd.AddValue ("pattern", "Traffic pattern: random | stride", pattern);
  cmd.AddValue ("stride", "Stride distance i (for --pattern=stride)", strideVal);
  cmd.AddValue ("seed", "RNG seed (for --pattern=random)", seed);
  cmd.AddValue ("routing", "Upward routing: twolevel | classify | schedule", routing);
  cmd.AddValue ("classifyInterval", "Flow-classifier rebalance period (s)", classifyInterval);
  cmd.AddValue ("transport", "Transport layer: udp | tcp", transport);
  cmd.AddValue ("headroom", "Link wire-rate over app-rate factor (for header overhead)", headroom);
  cmd.Parse (argc, argv);

  uint64_t appBps = DataRate (linkRate).GetBitRate (); // 应用层目标速率(=理想口径)

  // TCP 关键默认值: ns-3 默认 MSS 仅 536 字节, 包头开销过大会把吞吐(尤其短路径)拖垮;
  // 设成贴满 MTU 的 1448, 并调大收发缓冲, 让 TCP 能跑满链路。(对 UDP 无影响)
  Config::SetDefault ("ns3::TcpSocket::SegmentSize", UintegerValue (1448));
  Config::SetDefault ("ns3::TcpSocket::SndBufSize", UintegerValue (4 * 1024 * 1024));
  Config::SetDefault ("ns3::TcpSocket::RcvBufSize", UintegerValue (4 * 1024 * 1024));

  NS_ABORT_MSG_IF (k % 2 != 0 || k < 2, "k 必须为 >=2 的偶数");
  uint32_t half = k / 2;
  uint8_t mode = 0; // FatTreeRouting 上行分流模式: 0=two-level
  if (routing == "classify")
    {
      mode = 1;
    }
  else if (routing == "schedule")
    {
      mode = 2;
    }

  Time::SetResolution (Time::NS);

  uint32_t numCore = half * half;
  uint32_t numAgg = k * half;
  uint32_t numEdge = k * half;
  uint32_t numHosts = k * half * half; // = k^3 / 4

  NodeContainer coreNodes, aggNodes, edgeNodes, hostNodes;
  coreNodes.Create (numCore);
  aggNodes.Create (numAgg);
  edgeNodes.Create (numEdge);
  hostNodes.Create (numHosts);

  // 1) 全部节点安装协议栈
  InternetStackHelper internet;
  internet.InstallAll ();

  // 流调度模式: 创建一个全局中央调度器
  Ptr<CentralScheduler> scheduler;
  if (mode == 2)
    {
      scheduler = CreateObject<CentralScheduler> ();
      scheduler->Setup (k);
    }

  // 2) 在交换机上换装自定义的 FatTreeRouting，并配置其胖树坐标
  std::vector<Ptr<FatTreeRouting>> coreRt (numCore), aggRt (numAgg), edgeRt (numEdge);

  for (uint32_t c = 0; c < numCore; ++c)
    {
      Ptr<Ipv4> ipv4 = coreNodes.Get (c)->GetObject<Ipv4> ();
      ipv4->SetAttribute ("IpForward", BooleanValue (true));
      Ptr<FatTreeRouting> rt = CreateObject<FatTreeRouting> ();
      rt->SetSwitchType (3, 0, c, k); // Core
      rt->SetMode (mode);
      if (scheduler)
        {
          rt->SetScheduler (scheduler);
        }
      ipv4->SetRoutingProtocol (rt);
      coreRt[c] = rt;
    }
  for (uint32_t a = 0; a < numAgg; ++a)
    {
      Ptr<Ipv4> ipv4 = aggNodes.Get (a)->GetObject<Ipv4> ();
      ipv4->SetAttribute ("IpForward", BooleanValue (true));
      Ptr<FatTreeRouting> rt = CreateObject<FatTreeRouting> ();
      rt->SetSwitchType (2, a / half, a % half, k); // Aggregation
      rt->SetMode (mode);
      if (scheduler)
        {
          rt->SetScheduler (scheduler);
        }
      ipv4->SetRoutingProtocol (rt);
      aggRt[a] = rt;
    }
  for (uint32_t e = 0; e < numEdge; ++e)
    {
      Ptr<Ipv4> ipv4 = edgeNodes.Get (e)->GetObject<Ipv4> ();
      ipv4->SetAttribute ("IpForward", BooleanValue (true));
      Ptr<FatTreeRouting> rt = CreateObject<FatTreeRouting> ();
      rt->SetSwitchType (1, e / half, e % half, k); // Edge
      rt->SetMode (mode);
      if (scheduler)
        {
          rt->SetScheduler (scheduler);
        }
      ipv4->SetRoutingProtocol (rt);
      edgeRt[e] = rt;
    }

  // 3) 物理链路参数
  PointToPointHelper p2p;
  // 物理线速 = 应用速率 × headroom, 给 TCP/IP 包头留空间, 避免恰好打满时自挤丢包
  p2p.SetDeviceAttribute ("DataRate", DataRateValue (DataRate ((uint64_t) (appBps * headroom))));
  p2p.SetChannelAttribute ("Delay", StringValue ("10us"));

  Ipv4Mask mask24 ("255.255.255.0");
  Ipv4Mask mask30 ("255.255.255.252");

  // 交换机之间的基础设施链路用 172.16.0.0/30 逐条编址 (不参与路由判定)
  const uint32_t infraBase = (172u << 24) | (16u << 16); // 172.16.0.0
  uint32_t infraLink = 0;

  std::vector<Ipv4Address> hostGw (numHosts); // 每台主机的网关(边缘交换机侧 IP)
  std::vector<uint32_t> hostIf (numHosts);    // 每台主机的接口索引

  // ---- 主机 <-> 边缘 (每台主机一条独享链路) ----
  for (uint32_t pod = 0; pod < k; ++pod)
    {
      for (uint32_t e = 0; e < half; ++e)
        {
          uint32_t edgeIdx = pod * half + e;
          for (uint32_t h = 0; h < half; ++h)
            {
              uint32_t hostIdx = pod * half * half + e * half + h;
              NetDeviceContainer dev =
                  p2p.Install (edgeNodes.Get (edgeIdx), hostNodes.Get (hostIdx));

              Ipv4Address hostAddr = MakeIp (10, pod, e, h + 2);       // 10.pod.edge.id
              Ipv4Address edgeAddr = MakeIp (10, pod, e, h + 2 + 128); // 同子网内的网关地址

              uint32_t edgeIfIdx = AssignAddr (edgeNodes.Get (edgeIdx), dev.Get (0), edgeAddr, mask24);
              uint32_t hIfIdx = AssignAddr (hostNodes.Get (hostIdx), dev.Get (1), hostAddr, mask24);

              edgeRt[edgeIdx]->AddPortMapping (h, edgeIfIdx); // 边缘下行端口 = h

              hostGw[hostIdx] = edgeAddr;
              hostIf[hostIdx] = hIfIdx;
            }
        }
    }

  // ---- 边缘 <-> 汇聚 (同 Pod 内全连接) ----
  for (uint32_t pod = 0; pod < k; ++pod)
    {
      for (uint32_t e = 0; e < half; ++e)
        {
          uint32_t edgeIdx = pod * half + e;
          for (uint32_t v = 0; v < half; ++v)
            {
              uint32_t aggIdx = pod * half + v;
              NetDeviceContainer dev =
                  p2p.Install (edgeNodes.Get (edgeIdx), aggNodes.Get (aggIdx));

              Ipv4Address a0 (infraBase + infraLink * 4 + 1);
              Ipv4Address a1 (infraBase + infraLink * 4 + 2);
              ++infraLink;

              uint32_t edgeIfIdx = AssignAddr (edgeNodes.Get (edgeIdx), dev.Get (0), a0, mask30);
              uint32_t aggIfIdx = AssignAddr (aggNodes.Get (aggIdx), dev.Get (1), a1, mask30);

              edgeRt[edgeIdx]->AddPortMapping (half + v, edgeIfIdx); // 边缘上行端口 half+v
              aggRt[aggIdx]->AddPortMapping (e, aggIfIdx);           // 汇聚下行端口 e
            }
        }
    }

  // ---- 汇聚 <-> 核心 ----
  for (uint32_t pod = 0; pod < k; ++pod)
    {
      for (uint32_t a = 0; a < half; ++a)
        {
          uint32_t aggIdx = pod * half + a;
          for (uint32_t u = 0; u < half; ++u)
            {
              uint32_t coreIdx = a * half + u;
              NetDeviceContainer dev =
                  p2p.Install (aggNodes.Get (aggIdx), coreNodes.Get (coreIdx));

              Ipv4Address a0 (infraBase + infraLink * 4 + 1);
              Ipv4Address a1 (infraBase + infraLink * 4 + 2);
              ++infraLink;

              uint32_t aggIfIdx = AssignAddr (aggNodes.Get (aggIdx), dev.Get (0), a0, mask30);
              uint32_t coreIfIdx = AssignAddr (coreNodes.Get (coreIdx), dev.Get (1), a1, mask30);

              aggRt[aggIdx]->AddPortMapping (half + u, aggIfIdx); // 汇聚上行端口 half+u
              coreRt[coreIdx]->AddPortMapping (pod, coreIfIdx);   // 核心端口 = pod
            }
        }
    }

  // 4) 主机默认路由 -> 指向所属边缘交换机
  Ipv4StaticRoutingHelper staticHelper;
  for (uint32_t i = 0; i < numHosts; ++i)
    {
      Ptr<Ipv4> ipv4 = hostNodes.Get (i)->GetObject<Ipv4> ();
      Ptr<Ipv4StaticRouting> sr = staticHelper.GetStaticRouting (ipv4);
      sr->SetDefaultRoute (hostGw[i], hostIf[i]);
    }

  // 4b) classify 模式: 启动边缘/汇聚交换机的周期性流重排
  if (mode == 1)
    {
      for (uint32_t e = 0; e < numEdge; ++e)
        {
          edgeRt[e]->StartClassifier (Seconds (classifyInterval));
        }
      for (uint32_t a = 0; a < numAgg; ++a)
        {
          aggRt[a]->StartClassifier (Seconds (classifyInterval));
        }
    }

  // 4c) TCP 下启用"大流"判定: 仅发往数据端口的正向流进分类/调度,
  //     反向 ACK(目的为临时端口)回退两层路由 (论文只调度大象流)
  if (transport == "tcp" && mode != 0)
    {
      for (uint32_t e = 0; e < numEdge; ++e)
        {
          edgeRt[e]->SetElephantPort (9000);
        }
      for (uint32_t a = 0; a < numAgg; ++a)
        {
          aggRt[a]->SetElephantPort (9000);
        }
    }

  // 5) 生成通信置换 (1-to-1)，对应论文 5.2 节的 benchmark
  //    - stride : 主机 x 发给 (x + i) mod N            (论文 Stride(i))
  //    - random : 随机双射、无自环                       (论文 Random)
  std::vector<uint32_t> target (numHosts);
  if (pattern == "stride")
    {
      for (uint32_t i = 0; i < numHosts; ++i)
        {
          target[i] = (i + strideVal) % numHosts;
        }
    }
  else if (pattern == "sameid")
    {
      // 论文 "Same-ID Outgoing" 最坏情况 (内置 k=4 的双射置换):
      // 同一子网两台主机发往"主机ID相同"的目的, 迫使两层路由把它们散列到同一上行口
      // (静态时约 2:1 拥塞)。已离线验证: 是 0..15 的排列, 且全部跨 Pod。
      NS_ABORT_MSG_IF (k != 4, "sameid 最坏情况目前只内置了 k=4 的置换");
      const uint32_t perm[16] = {4, 6, 8, 10, 0, 2, 12, 14, 13, 15, 1, 3, 5, 7, 9, 11};
      for (uint32_t i = 0; i < numHosts; ++i)
        {
          target[i] = perm[i];
        }
    }
  else // random
    {
      for (uint32_t i = 0; i < numHosts; ++i)
        {
          target[i] = i;
        }
      std::mt19937 rng (seed);
      std::shuffle (target.begin (), target.end (), rng);
      for (uint32_t i = 0; i < numHosts; ++i) // 消除自环(发给自己)
        {
          if (target[i] == i)
            {
              uint32_t j = (i + 1) % numHosts;
              std::swap (target[i], target[j]);
            }
        }
    }

  // 5b) schedule 模式: 把全部流登记给中央调度器, 离线算出无冲突核心分配
  if (mode == 2 && scheduler)
    {
      for (uint32_t i = 0; i < numHosts; ++i)
        {
          Ipv4Address s = hostNodes.Get (i)->GetObject<Ipv4> ()->GetAddress (hostIf[i], 0).GetLocal ();
          uint32_t t = target[i];
          Ipv4Address d =
              hostNodes.Get (t)->GetObject<Ipv4> ()->GetAddress (hostIf[t], 0).GetLocal ();
          scheduler->RegisterFlow (s, d);
        }
      scheduler->Compute ();
    }

  // 6) 安装流量: 每台主机一条流 (UDP 恒定 96Mbps, 或 TCP 贪婪发送)
  uint16_t port = 9000;
  std::string sockFactory = (transport == "tcp") ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";
  ApplicationContainer sinkApps, srcApps;

  for (uint32_t i = 0; i < numHosts; ++i)
    {
      PacketSinkHelper sink (sockFactory, InetSocketAddress (Ipv4Address::GetAny (), port));
      sinkApps.Add (sink.Install (hostNodes.Get (i)));
    }

  for (uint32_t i = 0; i < numHosts; ++i)
    {
      uint32_t t = target[i];
      Ptr<Ipv4> tIpv4 = hostNodes.Get (t)->GetObject<Ipv4> ();
      Ipv4Address targetAddr = tIpv4->GetAddress (hostIf[t], 0).GetLocal ();
      InetSocketAddress dst (targetAddr, port);

      // UDP / TCP 共用 OnOff, 都按论文口径"恒定 96Mbps"限速发送(只换 socket 工厂)。
      // 用限速而非贪婪 BulkSend, 避免短路径上贪婪 TCP 的锯齿丢包把吞吐拖垮。
      OnOffHelper onoff (sockFactory, dst);
      onoff.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1]"));
      onoff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
      onoff.SetAttribute ("DataRate", DataRateValue (DataRate (linkRate)));
      onoff.SetAttribute ("PacketSize", UintegerValue (1448));
      srcApps.Add (onoff.Install (hostNodes.Get (i)));
    }

  sinkApps.Start (Seconds (0.0));
  sinkApps.Stop (Seconds (simTime));
  srcApps.Start (Seconds (1.0)); // 1s 时整体起量
  srcApps.Stop (Seconds (simTime));

  // 7) 运行
  Simulator::Stop (Seconds (simTime));
  Simulator::Run ();

  // 8) 统计有效双边带宽: 直接累加各接收端(PacketSink)收到的应用字节。
  //    比 FlowMonitor 快很多; sink 只收正向数据, 天然不含 TCP 反向 ACK。
  double totalRxBit = 0.0;
  for (uint32_t i = 0; i < sinkApps.GetN (); ++i)
    {
      Ptr<PacketSink> sink = DynamicCast<PacketSink> (sinkApps.Get (i));
      totalRxBit += sink->GetTotalRx () * 8.0;
    }

  double duration = simTime - 1.0;  // 扣除 1s 启动延迟
  double effective = totalRxBit / duration;
  double ideal = numHosts * (double) appBps; // 每主机应用速率 (k=4, 96Mbps 时为 1.536 Gbps)
  double pct = (effective / ideal) * 100.0;

  std::string label = (pattern == "stride") ? ("Stride(" + std::to_string (strideVal) + ")") : "Random";

  std::cout << "==== Fat-Tree (k=" << k << ", transport=" << transport << ", routing=" << routing
            << ", pattern=" << label << ") ====" << std::endl;
  std::cout << "主机数: " << numHosts << "，接收端数: " << sinkApps.GetN () << std::endl;
  std::cout << "有效双边带宽: " << effective / 1e9 << " Gbps" << std::endl;
  std::cout << "达到理想带宽的百分比: " << pct << " %" << std::endl;

  // 便于脚本解析的机器可读行(百分比恒为最后一列): RESULT,routing,pattern,stride,seed,percent
  std::cout << "RESULT," << routing << "," << pattern << ","
            << (pattern == "stride" ? (int) strideVal : -1) << "," << seed << "," << pct
            << std::endl;

  Simulator::Destroy ();
  return 0;
}
