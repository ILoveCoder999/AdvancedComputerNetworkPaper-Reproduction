// =============================================================================
// tree-baseline.cc
//   论文 Table 2 第一列 "Tree" 的基线: 传统分层树, 上行 3.6:1 过载。
//   k=4 时: 16 主机, 4 个 pod 交换机(各接 4 主机), 1 个核心交换机。
//   主机链路 96Mbps; pod->core 上行限速到 (hostsPerPod*96)/3.6 = 106.67Mbps。
//   用 ns-3 全局路由(单路径), 过载比体现在上行链路上。
//
//   与 fattree 程序共用同样的主机编号/流量模式/统计口径, 便于并排对比。
//   运行: ./ns3 run "fattree-tree --pattern=random --simTime=10"
// =============================================================================
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/applications-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/flow-monitor-module.h"

#include <algorithm>
#include <random>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("FatTreeBaselineTree");

int
main (int argc, char *argv[])
{
  uint32_t k = 4;
  double simTime = 10.0;
  std::string linkRate = "96Mbps";
  std::string pattern = "random";
  uint32_t strideVal = 1;
  uint32_t seed = 1;
  double oversub = 3.6;          // 论文的上行过载比
  std::string transport = "udp"; // 传输层: udp | tcp
  double headroom = 1.08;        // 物理链路相对应用速率的线速余量

  CommandLine cmd;
  cmd.AddValue ("k", "Fat-tree degree k (必须为偶数)", k);
  cmd.AddValue ("simTime", "Simulation time in seconds", simTime);
  cmd.AddValue ("linkRate", "Host link data rate", linkRate);
  cmd.AddValue ("pattern", "Traffic pattern: random | stride | sameid", pattern);
  cmd.AddValue ("stride", "Stride distance i (for --pattern=stride)", strideVal);
  cmd.AddValue ("seed", "RNG seed (for --pattern=random)", seed);
  cmd.AddValue ("oversub", "Uplink oversubscription ratio (pod->core)", oversub);
  cmd.AddValue ("transport", "Transport layer: udp | tcp", transport);
  cmd.AddValue ("headroom", "Link wire-rate over app-rate factor", headroom);
  cmd.Parse (argc, argv);

  // TCP 用满 MTU 的 MSS(默认 536 太小) + 大缓冲, 否则吞吐被包头拖垮
  Config::SetDefault ("ns3::TcpSocket::SegmentSize", UintegerValue (1448));
  Config::SetDefault ("ns3::TcpSocket::SndBufSize", UintegerValue (4 * 1024 * 1024));
  Config::SetDefault ("ns3::TcpSocket::RcvBufSize", UintegerValue (4 * 1024 * 1024));

  NS_ABORT_MSG_IF (k % 2 != 0 || k < 2, "k 必须为 >=2 的偶数");
  Time::SetResolution (Time::NS);

  uint32_t numHosts = k * k * k / 4;
  uint32_t numPods = k;                       // 每个 pod 一台 pod 交换机
  uint32_t hostsPerPod = numHosts / numPods;  // = k^2/4 (与 fattree 的 pod 分组一致)

  // 应用速率(=理想口径)与各链路的物理线速(含 headroom 余量)
  uint64_t appBps = DataRate (linkRate).GetBitRate ();
  uint64_t hostWire = (uint64_t) (appBps * headroom);                         // 主机接入链路线速
  uint64_t uplinkApp = (uint64_t) (hostsPerPod * (double) appBps / oversub);  // 上行应用容量
  uint64_t uplinkWire = (uint64_t) (uplinkApp * headroom);                    // 上行链路线速

  NodeContainer hosts, podSw, core;
  hosts.Create (numHosts);
  podSw.Create (numPods);
  core.Create (1);

  InternetStackHelper internet;
  internet.InstallAll ();

  PointToPointHelper p2pHost, p2pUp;
  p2pHost.SetDeviceAttribute ("DataRate", DataRateValue (DataRate (hostWire)));
  p2pHost.SetChannelAttribute ("Delay", StringValue ("10us"));
  p2pUp.SetDeviceAttribute ("DataRate", DataRateValue (DataRate (uplinkWire)));
  p2pUp.SetChannelAttribute ("Delay", StringValue ("10us"));

  Ipv4AddressHelper addr;
  addr.SetBase ("10.0.0.0", "255.255.255.252");

  std::vector<Ipv4Address> hostAddr (numHosts);

  // 主机 <-> pod 交换机 (每主机一条 96Mbps 独享链路)
  for (uint32_t i = 0; i < numHosts; ++i)
    {
      uint32_t pod = i / hostsPerPod;
      NetDeviceContainer d = p2pHost.Install (hosts.Get (i), podSw.Get (pod));
      Ipv4InterfaceContainer ic = addr.Assign (d);
      hostAddr[i] = ic.GetAddress (0); // 主机侧地址
      addr.NewNetwork ();
    }

  // pod 交换机 <-> 核心 (过载上行)
  for (uint32_t p = 0; p < numPods; ++p)
    {
      NetDeviceContainer d = p2pUp.Install (podSw.Get (p), core.Get (0));
      addr.Assign (d);
      addr.NewNetwork ();
    }

  // 传统树用单路径全局路由
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  // 通信置换 (与 fattree 程序保持一致)
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
      for (uint32_t i = 0; i < numHosts; ++i)
        {
          if (target[i] == i)
            {
              uint32_t j = (i + 1) % numHosts;
              std::swap (target[i], target[j]);
            }
        }
    }

  // 流量: 每台主机一条流 (UDP 恒定 96Mbps 或 TCP 贪婪发送)
  uint16_t port = 9000;
  std::string sockFactory = (transport == "tcp") ? "ns3::TcpSocketFactory" : "ns3::UdpSocketFactory";
  ApplicationContainer sinkApps, srcApps;
  for (uint32_t i = 0; i < numHosts; ++i)
    {
      PacketSinkHelper sink (sockFactory, InetSocketAddress (Ipv4Address::GetAny (), port));
      sinkApps.Add (sink.Install (hosts.Get (i)));
    }
  for (uint32_t i = 0; i < numHosts; ++i)
    {
      InetSocketAddress dst (hostAddr[target[i]], port);
      // UDP / TCP 共用 OnOff, 均按"恒定 96Mbps"限速发送(只换 socket 工厂)
      OnOffHelper onoff (sockFactory, dst);
      onoff.SetAttribute ("OnTime", StringValue ("ns3::ConstantRandomVariable[Constant=1]"));
      onoff.SetAttribute ("OffTime", StringValue ("ns3::ConstantRandomVariable[Constant=0]"));
      onoff.SetAttribute ("DataRate", DataRateValue (DataRate (linkRate)));
      onoff.SetAttribute ("PacketSize", UintegerValue (1448));
      srcApps.Add (onoff.Install (hosts.Get (i)));
    }
  sinkApps.Start (Seconds (0.0));
  sinkApps.Stop (Seconds (simTime));
  srcApps.Start (Seconds (1.0));
  srcApps.Stop (Seconds (simTime));

  Simulator::Stop (Seconds (simTime));
  Simulator::Run ();

  // 直接累加各接收端收到的应用字节(比 FlowMonitor 快, sink 不含 TCP 反向 ACK)
  double totalRxBit = 0.0;
  for (uint32_t i = 0; i < sinkApps.GetN (); ++i)
    {
      Ptr<PacketSink> sink = DynamicCast<PacketSink> (sinkApps.Get (i));
      totalRxBit += sink->GetTotalRx () * 8.0;
    }
  double duration = simTime - 1.0;
  double effective = totalRxBit / duration;
  double ideal = numHosts * (double) appBps;
  double pct = (effective / ideal) * 100.0;

  std::string label = (pattern == "stride") ? ("Stride(" + std::to_string (strideVal) + ")")
                      : (pattern == "sameid") ? "Same-ID" : "Random";

  std::cout << "==== Traditional Tree (k=" << k << ", transport=" << transport
            << ", oversub=" << oversub << ", pattern=" << label << ") ====" << std::endl;
  std::cout << "主机数: " << numHosts << "，上行应用容量: " << uplinkApp / 1e6
            << " Mbps，接收端数: " << sinkApps.GetN () << std::endl;
  std::cout << "达到理想带宽的百分比: " << pct << " %" << std::endl;

  std::cout << "RESULT,tree," << pattern << ","
            << (pattern == "stride" ? (int) strideVal : -1) << "," << seed << "," << pct
            << std::endl;

  Simulator::Destroy ();
  return 0;
}
