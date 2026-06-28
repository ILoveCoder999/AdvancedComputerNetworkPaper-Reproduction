#include "fat-tree-routing.h"
#include "fat-tree-central-scheduler.h"

#include "ns3/ipv4-route.h"
#include "ns3/log.h"
#include "ns3/socket.h"
#include "ns3/simulator.h"
#include "ns3/tcp-header.h"
#include "ns3/udp-header.h"

#include <limits>

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("FatTreeRouting");
NS_OBJECT_ENSURE_REGISTERED (FatTreeRouting);

TypeId
FatTreeRouting::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::FatTreeRouting")
                          .SetParent<Ipv4RoutingProtocol> ()
                          .SetGroupName ("Internet")
                          .AddConstructor<FatTreeRouting> ();
  return tid;
}

FatTreeRouting::FatTreeRouting ()
    : m_type (0), m_pod (0), m_switchId (0), m_k (4), m_mode (0), m_elephantPort (0)
{
}

FatTreeRouting::~FatTreeRouting ()
{
}

void
FatTreeRouting::SetSwitchType (uint8_t type, uint8_t pod, uint8_t switchId, uint8_t k)
{
  m_type = type;
  m_pod = pod;
  m_switchId = switchId;
  m_k = k;
}

void
FatTreeRouting::AddPortMapping (uint32_t logicalPort, uint32_t interfaceIndex)
{
  m_portToIf[logicalPort] = interfaceIndex;
}

void
FatTreeRouting::SetMode (uint8_t mode)
{
  m_mode = mode;
}

void
FatTreeRouting::SetScheduler (Ptr<CentralScheduler> scheduler)
{
  m_scheduler = scheduler;
}

void
FatTreeRouting::SetElephantPort (uint16_t port)
{
  m_elephantPort = port;
}

bool
FatTreeRouting::IsElephant (Ptr<const Packet> p, const Ipv4Header &header) const
{
  if (m_elephantPort == 0)
    {
      return true; // 未启用阈值(如 UDP 单向): 所有流都按大流处理
    }
  uint8_t proto = header.GetProtocol ();
  uint16_t dport = 0;
  if (proto == 6) // TCP
    {
      TcpHeader h;
      p->PeekHeader (h);
      dport = h.GetDestinationPort ();
    }
  else if (proto == 17) // UDP
    {
      UdpHeader h;
      p->PeekHeader (h);
      dport = h.GetDestinationPort ();
    }
  // 只有发往数据端口的(正向数据流)才算大流; 反向 ACK(目的为临时端口)走两层路由
  return dport == m_elephantPort;
}

void
FatTreeRouting::StartClassifier (Time interval)
{
  if (m_mode != 1)
    {
      return; // 仅流分类模式需要
    }
  if (m_type != 1 && m_type != 2)
    {
      return; // 仅边缘/汇聚交换机做上行分流
    }
  m_interval = interval;
  m_rebalanceEvent = Simulator::Schedule (interval, &FatTreeRouting::Rebalance, this);
}

bool
FatTreeRouting::IsUpward (uint8_t destPod, uint8_t destSwitch) const
{
  if (m_type == 2) // 汇聚: 跨 Pod 才上行
    {
      return destPod != m_pod;
    }
  if (m_type == 1) // 边缘: 非本地子网才上行
    {
      return !(destPod == m_pod && destSwitch == m_switchId);
    }
  return false; // 核心层不上行
}

bool
FatTreeRouting::ComputeOutPort (Ipv4Address dest, uint32_t &logicalPort) const
{
  uint32_t ip = dest.Get ();
  uint8_t destPod = (ip >> 16) & 0xFF;
  uint8_t destSwitch = (ip >> 8) & 0xFF;
  uint8_t destId = ip & 0xFF;
  uint8_t half = m_k / 2;

  if (m_type == 3) // Core: 终止前缀，直接转给目的 Pod
    {
      logicalPort = destPod;
    }
  else if (m_type == 2) // Aggregation
    {
      if (destPod == m_pod)
        {
          logicalPort = destSwitch; // 同 Pod: 下行到对应边缘交换机
        }
      else
        {
          // 跨 Pod: 二级后缀查找，上行到核心 (论文 Algorithm 1)
          logicalPort = ((destId - 2 + m_switchId) % half) + half;
        }
    }
  else if (m_type == 1) // Edge
    {
      if (destPod == m_pod && destSwitch == m_switchId)
        {
          logicalPort = destId - 2; // 本地子网: 下行到主机
        }
      else
        {
          // 出向: (目的主机ID + 本交换机位置) 后缀散列向上 (Algorithm 1，边缘层同样加位置偏移)
          logicalPort = ((destId - 2 + m_switchId) % half) + half;
        }
    }
  else
    {
      return false; // 未配置类型
    }
  return true;
}

uint32_t
FatTreeRouting::ClassifyUpPort (uint64_t flowKey, uint32_t pktSize)
{
  uint8_t half = m_k / 2;

  std::map<uint64_t, FlowRec>::iterator it = m_flowTable.find (flowKey);
  if (it != m_flowTable.end ())
    {
      // 已有流: 保持原端口(避免乱序)，累加负载
      it->second.bytes += pktSize;
      m_portLoad[it->second.port] += pktSize;
      return it->second.port;
    }

  // 新流: 分配到当前最空闲的上行端口 [half, k-1]
  uint32_t best = half;
  uint64_t bestLoad = std::numeric_limits<uint64_t>::max ();
  for (uint32_t pt = half; pt < m_k; ++pt)
    {
      uint64_t load = m_portLoad.count (pt) ? m_portLoad[pt] : 0;
      if (load < bestLoad)
        {
          bestLoad = load;
          best = pt;
        }
    }

  FlowRec rec;
  rec.bytes = pktSize;
  rec.port = best;
  m_flowTable[flowKey] = rec;
  m_portLoad[best] += pktSize;
  return best;
}

void
FatTreeRouting::Rebalance ()
{
  uint8_t half = m_k / 2;

  // 论文 3.6 节: 尝试把大流从最忙上行口挪到最闲上行口，抹平差距
  for (uint32_t pass = 0; pass < m_k; ++pass)
    {
      uint32_t pMax = half, pMin = half;
      uint64_t lMax = 0, lMin = std::numeric_limits<uint64_t>::max ();
      for (uint32_t pt = half; pt < m_k; ++pt)
        {
          uint64_t load = m_portLoad.count (pt) ? m_portLoad[pt] : 0;
          if (load >= lMax)
            {
              lMax = load;
              pMax = pt;
            }
          if (load < lMin)
            {
              lMin = load;
              pMin = pt;
            }
        }
      if (lMax <= lMin)
        {
          break; // 已经平衡
        }
      uint64_t diff = lMax - lMin;

      // 在最忙口上找一条"小于差距"的最大流来搬动(搬完不会反超)
      uint64_t bestKey = 0;
      uint64_t bestSize = 0;
      bool found = false;
      for (std::map<uint64_t, FlowRec>::iterator it = m_flowTable.begin ();
           it != m_flowTable.end (); ++it)
        {
          if (it->second.port == pMax && it->second.bytes > 0 && it->second.bytes < diff &&
              it->second.bytes > bestSize)
            {
              bestSize = it->second.bytes;
              bestKey = it->first;
              found = true;
            }
        }
      if (!found)
        {
          break;
        }
      m_flowTable[bestKey].port = pMin;
      m_portLoad[pMax] -= bestSize;
      m_portLoad[pMin] += bestSize;
    }

  // 重置本周期计数(端口绑定保持粘性)，并安排下一周期
  for (std::map<uint64_t, FlowRec>::iterator it = m_flowTable.begin (); it != m_flowTable.end (); ++it)
    {
      it->second.bytes = 0;
    }
  for (std::map<uint32_t, uint64_t>::iterator it = m_portLoad.begin (); it != m_portLoad.end (); ++it)
    {
      it->second = 0;
    }
  m_rebalanceEvent = Simulator::Schedule (m_interval, &FatTreeRouting::Rebalance, this);
}

Ptr<Ipv4Route>
FatTreeRouting::BuildRoute (Ipv4Address dest, uint32_t logicalPort) const
{
  std::map<uint32_t, uint32_t>::const_iterator it = m_portToIf.find (logicalPort);
  if (it == m_portToIf.end ())
    {
      return Ptr<Ipv4Route> (); // 该逻辑端口未接线
    }
  uint32_t iface = it->second;

  Ptr<Ipv4Route> route = Create<Ipv4Route> ();
  route->SetDestination (dest);
  route->SetGateway (dest); // 点对点链路无需 ARP，网关填目的即可
  route->SetSource (m_ipv4->GetAddress (iface, 0).GetLocal ());
  route->SetOutputDevice (m_ipv4->GetNetDevice (iface));
  return route;
}

Ptr<Ipv4Route>
FatTreeRouting::RouteOutput (Ptr<Packet>, const Ipv4Header &header,
                             Ptr<NetDevice>, Socket::SocketErrno &sockerr)
{
  // 交换机自身基本不发起流量，这里用确定性两层表即可
  Ipv4Address dest = header.GetDestination ();
  uint32_t logicalPort;
  if (!ComputeOutPort (dest, logicalPort))
    {
      sockerr = Socket::ERROR_NOROUTETOHOST;
      return Ptr<Ipv4Route> ();
    }
  Ptr<Ipv4Route> route = BuildRoute (dest, logicalPort);
  if (!route)
    {
      sockerr = Socket::ERROR_NOROUTETOHOST;
      return Ptr<Ipv4Route> ();
    }
  sockerr = Socket::ERROR_NOTERROR;
  return route;
}

bool
FatTreeRouting::RouteInput (Ptr<const Packet> p, const Ipv4Header &header,
                            Ptr<const NetDevice> idev, const UnicastForwardCallback &ucb,
                            const MulticastForwardCallback &, const LocalDeliverCallback &lcb,
                            const ErrorCallback &ecb)
{
  NS_LOG_FUNCTION (this << header.GetDestination ());
  NS_ASSERT (m_ipv4);

  Ipv4Address dest = header.GetDestination ();
  uint32_t iif = m_ipv4->GetInterfaceForDevice (idev);

  // 若目的地址恰好是本机 (交换机一般不会)，本地交付
  for (uint32_t i = 0; i < m_ipv4->GetNInterfaces (); ++i)
    {
      for (uint32_t j = 0; j < m_ipv4->GetNAddresses (i); ++j)
        {
          if (m_ipv4->GetAddress (i, j).GetLocal () == dest)
            {
              lcb (p, header, iif);
              return true;
            }
        }
    }

  uint32_t ip = dest.Get ();
  uint8_t destPod = (ip >> 16) & 0xFF;
  uint8_t destSwitch = (ip >> 8) & 0xFF;

  uint32_t logicalPort = 0;
  bool resolved = false;

  if (IsUpward (destPod, destSwitch) && IsElephant (p, header))
    {
      uint8_t half = m_k / 2;
      if (m_mode == 2 && m_scheduler) // flow scheduling
        {
          int32_t core = m_scheduler->GetCoreForFlow (header.GetSource (), dest);
          if (core >= 0)
            {
              uint32_t cc = static_cast<uint32_t> (core);
              if (m_type == 1)
                {
                  logicalPort = half + (cc / half); // 边缘 -> 指定汇聚 (a = c/half)
                }
              else
                {
                  logicalPort = half + (cc % half); // 汇聚 -> 指定核心 (u = c%half)
                }
              resolved = true;
            }
        }
      else if (m_mode == 1) // flow classification
        {
          uint64_t flowKey = (static_cast<uint64_t> (header.GetSource ().Get ()) << 32) | ip;
          logicalPort = ClassifyUpPort (flowKey, p->GetSize ());
          resolved = true;
        }
    }

  if (!resolved && !ComputeOutPort (dest, logicalPort))
    {
      ecb (p, header, Socket::ERROR_NOROUTETOHOST);
      return false;
    }

  Ptr<Ipv4Route> route = BuildRoute (dest, logicalPort);
  if (!route)
    {
      ecb (p, header, Socket::ERROR_NOROUTETOHOST);
      return false;
    }
  ucb (route, p, header);
  return true;
}

void
FatTreeRouting::NotifyInterfaceUp (uint32_t)
{
}

void
FatTreeRouting::NotifyInterfaceDown (uint32_t)
{
}

void
FatTreeRouting::NotifyAddAddress (uint32_t, Ipv4InterfaceAddress)
{
}

void
FatTreeRouting::NotifyRemoveAddress (uint32_t, Ipv4InterfaceAddress)
{
}

void
FatTreeRouting::SetIpv4 (Ptr<Ipv4> ipv4)
{
  m_ipv4 = ipv4;
}

void
FatTreeRouting::PrintRoutingTable (Ptr<OutputStreamWrapper> stream, Time::Unit) const
{
  std::ostream *os = stream->GetStream ();
  *os << "[FatTreeRouting] type=" << (uint32_t) m_type << " pod=" << (uint32_t) m_pod
      << " switchId=" << (uint32_t) m_switchId << " k=" << (uint32_t) m_k
      << " mode=" << (uint32_t) m_mode << "\n";
}

} // namespace ns3
