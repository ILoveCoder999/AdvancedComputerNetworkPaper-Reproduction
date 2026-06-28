#ifndef FAT_TREE_CENTRAL_SCHEDULER_H
#define FAT_TREE_CENTRAL_SCHEDULER_H

#include "ns3/object.h"
#include "ns3/ipv4-address.h"

#include <map>
#include <set>
#include <vector>

namespace ns3 {

/**
 * 论文 3.7 节的中央流调度器 (Flow Scheduling)，全局视角版本。
 *
 * 用法: 建网后把所有流登记 (RegisterFlow), 再调用 Compute() 一次性算出
 * 无冲突的核心分配; 运行时边缘/汇聚交换机用 GetCoreForFlow() 查表。
 *
 * 关键链路 (a = core/(k/2)):
 *   type0  edge(srcPod,srcEdge) -> agg(srcPod,a)     上行
 *   type1  agg(srcPod,a)        -> core c            上行
 *   type2  core c               -> agg(dstPod,a)     下行
 *   type3  agg(dstPod,a)        -> edge(dstPod,dstEdge) 下行
 *
 * Compute() 先把"同 Pod 流"按两层路由会走的链路占住(它们走默认路由, 不可调度),
 * 再对跨 Pod 流做贪心分配; 贪心放不下时做一次"单跳腾挪"(把某条已放置流换到
 * 别的核心, 给当前流让路), 尽量逼近重排无阻塞的理想分配。放不下则回退两层路由。
 */
class CentralScheduler : public Object
{
public:
  static TypeId GetTypeId (void);
  CentralScheduler ();

  void Setup (uint32_t k);
  /// 登记一条流 (建网后、Compute 前调用)
  void RegisterFlow (Ipv4Address src, Ipv4Address dst);
  /// 全局计算无冲突核心分配
  void Compute (void);
  /// 查表: 返回该流应走的核心编号, -1 表示回退两层路由
  int32_t GetCoreForFlow (Ipv4Address src, Ipv4Address dst);

private:
  struct Flow
  {
    uint32_t sPod, sEdge, dPod, dEdge, destId;
    uint64_t key;
    bool inter;
  };

  void LinksFor (const Flow &f, uint32_t c, uint64_t out[4]) const;
  bool Free (const Flow &f, uint32_t c) const;
  void DoReserve (const Flow &f, uint32_t c);
  void Unreserve (const Flow &f);
  bool TryPlace (const Flow &f);
  bool TryPlaceWithEviction (const Flow &f);
  static uint64_t LinkKey (uint32_t type, uint32_t w, uint32_t x, uint32_t y);

  uint32_t m_k;
  uint32_t m_half;
  uint32_t m_numCore;

  std::vector<Flow> m_flows;
  std::map<uint64_t, Flow> m_flowByKey;
  std::map<uint64_t, int32_t> m_flowCore;     //!< 最终结果: flow -> core (-1=回退)
  std::map<uint64_t, int32_t> m_assignedCore; //!< 已放置 flow -> core (计算中间态)
  std::set<uint64_t> m_reserved;              //!< 已占用链路
  std::map<uint64_t, uint64_t> m_linkOwner;   //!< 链路 -> 占用它的(可腾挪)跨 Pod 流
};

} // namespace ns3

#endif // FAT_TREE_CENTRAL_SCHEDULER_H
