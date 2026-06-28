#ifndef FAT_TREE_ROUTING_H
#define FAT_TREE_ROUTING_H

#include "ns3/ipv4-routing-protocol.h"
#include "ns3/ipv4-address.h"
#include "ns3/ipv4.h"
#include "ns3/output-stream-wrapper.h"
#include "ns3/nstime.h"
#include "ns3/event-id.h"

#include <map>

namespace ns3 {

class CentralScheduler; // 前向声明 (见 fat-tree-central-scheduler.h)

/**
 * 论文 "A Scalable, Commodity Data Center Network Architecture" 中的
 * 多路径路由实现，安装在交换机节点上。
 *
 *   type: 1 = Edge(边缘)   2 = Aggregation(汇聚)   3 = Core(核心)
 *
 * 支持三种上行分流方式 (通过 SetMode 选择)：
 *   mode 0 = 两层路由表 (two-level table)，按目的主机 ID 做确定性散列；
 *   mode 1 = 流分类 (flow classification)，动态把流分配到最空闲的等价上行口，
 *            并周期性把大流从拥挤口挪到空闲口 (论文 3.6 节)；
 *   mode 2 = 流调度 (flow scheduling)，询问中央调度器为每条流指定无冲突核心
 *            (论文 3.7 节)；找不到则回退到两层表。
 *
 * 下行/核心层的判定始终是确定性的，只有"跨 Pod 上行"这一步会受 mode 影响。
 */
class FatTreeRouting : public Ipv4RoutingProtocol
{
public:
  static TypeId GetTypeId (void);
  FatTreeRouting ();
  ~FatTreeRouting () override;

  /// 配置该交换机在胖树中的坐标 (建拓扑时调用)
  void SetSwitchType (uint8_t type, uint8_t pod, uint8_t switchId, uint8_t k);
  /// 把"论文逻辑端口号"绑定到 ns-3 的接口索引 (建拓扑时调用)
  void AddPortMapping (uint32_t logicalPort, uint32_t interfaceIndex);
  /// 选择上行分流方式: 0=two-level, 1=flow-classification, 2=flow-scheduling
  void SetMode (uint8_t mode);
  /// 注入中央调度器 (仅 mode 2 使用)
  void SetScheduler (Ptr<CentralScheduler> scheduler);
  /// 启动周期性流重排 (仅 mode 1 下的边缘/汇聚交换机有意义)
  void StartClassifier (Time interval);
  /// 设置"大流"判定: L4 目的端口==该值才走分类/调度; 0=关闭(所有流都算大流, UDP 单向时用)
  void SetElephantPort (uint16_t port);

  // ---------------- Ipv4RoutingProtocol 必须实现的接口 ----------------
  Ptr<Ipv4Route> RouteOutput (Ptr<Packet> p, const Ipv4Header &header,
                              Ptr<NetDevice> oif, Socket::SocketErrno &sockerr) override;
  bool RouteInput (Ptr<const Packet> p, const Ipv4Header &header, Ptr<const NetDevice> idev,
                   const UnicastForwardCallback &ucb, const MulticastForwardCallback &mcb,
                   const LocalDeliverCallback &lcb, const ErrorCallback &ecb) override;
  void NotifyInterfaceUp (uint32_t interface) override;
  void NotifyInterfaceDown (uint32_t interface) override;
  void NotifyAddAddress (uint32_t interface, Ipv4InterfaceAddress address) override;
  void NotifyRemoveAddress (uint32_t interface, Ipv4InterfaceAddress address) override;
  void SetIpv4 (Ptr<Ipv4> ipv4) override;
  void PrintRoutingTable (Ptr<OutputStreamWrapper> stream,
                          Time::Unit unit = Time::S) const override;

private:
  /// 每条流的记录: 本周期累计字节数 + 当前绑定的上行逻辑端口
  struct FlowRec
  {
    uint64_t bytes;
    uint32_t port;
  };

  /// 是否属于"跨 Pod 上行"决策 (只有这一步会用到 classify / schedule)
  bool IsUpward (uint8_t destPod, uint8_t destSwitch) const;
  /// 是否为需特殊分流的"大流"(按 L4 目的端口判定, 用于 TCP 下排除反向 ACK 流)
  bool IsElephant (Ptr<const Packet> p, const Ipv4Header &header) const;
  /// 两层路由表的确定性判定 (核心/下行/本地/静态上行)
  bool ComputeOutPort (Ipv4Address dest, uint32_t &logicalPort) const;
  /// classify 模式: 给定流, 返回(并维护)其上行逻辑端口
  uint32_t ClassifyUpPort (uint64_t flowKey, uint32_t pktSize);
  /// classify 模式: 周期性把大流从最忙上行口挪到最闲上行口
  void Rebalance ();
  /// 根据逻辑端口构造一条 ns-3 路由表项
  Ptr<Ipv4Route> BuildRoute (Ipv4Address dest, uint32_t logicalPort) const;

  uint8_t m_type;     //!< 1=Edge 2=Aggregation 3=Core
  uint8_t m_pod;      //!< 所属 Pod 编号
  uint8_t m_switchId; //!< Pod 内编号
  uint8_t m_k;        //!< 胖树端口数 k
  uint8_t m_mode;          //!< 0=two-level, 1=classification, 2=scheduling
  uint16_t m_elephantPort; //!< 大流判定端口 (0=关闭)
  Ptr<Ipv4> m_ipv4;        //!< 所在节点的 Ipv4 协议栈
  Ptr<CentralScheduler> m_scheduler; //!< 中央调度器(mode 2)

  std::map<uint32_t, uint32_t> m_portToIf; //!< 逻辑端口 -> ns-3 接口索引

  // ---- flow classification 状态 ----
  Time m_interval;                         //!< 流重排周期
  EventId m_rebalanceEvent;                //!< 周期性事件句柄
  std::map<uint64_t, FlowRec> m_flowTable; //!< 流(src,dst) -> 记录
  std::map<uint32_t, uint64_t> m_portLoad; //!< 上行逻辑端口 -> 本周期字节数
};

} // namespace ns3

#endif // FAT_TREE_ROUTING_H
