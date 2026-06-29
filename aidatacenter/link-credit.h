/*
 * link-credit.h — 逐跳(hop-by-hop)链路级信用流控引擎(纯 C++14,无 ns-3 依赖,可单测)
 * ============================================================================
 * 为什么是逐跳而不是端到端:
 *   mesh-credit.h 是 *端到端 per-destination* 信用——对 incast(多源→一宿)有效,
 *   但对 NCCL Ring AllReduce 无效:环 all-reduce 中每个宿只有一个源(置换模式),
 *   宿端永不拥塞;真正拥塞发生在 **被多条环流共享的中继链路** 上。只有 *逐跳链路*
 *   信用能对中继链路反压 → 这才是直连网格(IB/credit)真正的无损机制,也才让 JCT
 *   反映真实拥塞,可与 leaf-spine 的 PFC 等价对比。
 *
 * 机制(标准 credit-based link-level flow control):
 *   · 每条有向链路 self→nbr 有信用 = 下游为该链路预留的缓冲槽数(creditPkts)。
 *   · 节点要在 self→nbr 上发包,必须先持有该链路 1 个信用(消耗);否则入队等待。
 *   · 当下游 nbr 把该包再转发出去(或最终交付)时,nbr 的那个槽释放 → 回一个信用
 *     给 self(本引擎通过 ret 回调通知上层"给上游回信用")。
 *   · DOR(维序路由)的信道依赖图无环 → 逐跳信用 *无死锁*(标准结论)。
 *
 * 本引擎只管 *信用记账 + 排队 + 触发实际发送/回信用* 的纯逻辑;真正的"发包"和
 * "把信用送回上游"由上层通过两个回调实现(ns-3 里=UDP 发送 / Simulator::Schedule)。
 * 因此引擎本身不依赖 ns-3,可用普通 g++ 跑离散事件单测(见 link_credit_test.cc)。
 *
 * 用法(每个转发节点持有一个 LinkCredit<PktHandle>):
 *   lc.Configure(creditPkts, enabled);
 *   lc.SetTx ([](uint16_t nbr, const Pkt& p){ ...真正发到 nbr... });
 *   lc.SetReturn([](uint16_t upstream){ ...通知 upstream:它到本节点的链路回了 1 信用... });
 *   lc.Submit(nextHop, pkt, upstream);   // upstream=NO_UP 表示本节点是源(无上游可回)
 *   lc.OnReturn(nbr);                    // 收到"self→nbr 链路回 1 信用"
 * ============================================================================
 */
#ifndef LINK_CREDIT_H
#define LINK_CREDIT_H

#include <cstdint>
#include <deque>
#include <functional>
#include <map>

template <class Pkt>
class LinkCredit
{
public:
  // 节点 id 用 uint16_t(与 mesh-route-header 一致);0xFFFF 作"无上游"哨兵。
  static const uint16_t NO_UP = 0xFFFF;

  void Configure (uint32_t bufPkts, bool enabled)
  { m_buf = bufPkts ? bufPkts : 1; m_on = enabled; }

  bool Enabled () const { return m_on; }

  void SetTx     (std::function<void(uint16_t, const Pkt&)> f) { m_tx = std::move (f); }
  void SetReturn (std::function<void(uint16_t)> f)             { m_ret = std::move (f); }

  // 提交一个待发往 nbr 的包。upstream = 该包离开本节点后应回信用的上游(源包填 NO_UP)。
  void Submit (uint16_t nbr, const Pkt& pkt, uint16_t upstream)
  {
    if (!m_on)
      {                                   // 关流控:直发,并立即把上游信用补回(保持计数自洽)
        if (m_tx) m_tx (nbr, pkt);
        if (upstream != NO_UP && m_ret) m_ret (upstream);
        return;
      }
    Init (nbr);
    m_q[nbr].push_back (QE{pkt, upstream});
    Drain (nbr);
  }

  // 收到"本节点→nbr 这条链路回了 1 个信用"(下游腾出了一个槽)。
  void OnReturn (uint16_t nbr)
  {
    if (!m_on) return;
    Init (nbr);
    ++m_credit[nbr];
    ++m_returns;
    Drain (nbr);
  }

  // —— 统计 / 自检访问器 —— //
  uint32_t Peak (uint16_t nbr) const
  { auto it = m_peak.find (nbr); return it == m_peak.end () ? 0 : it->second; }
  uint64_t Queued () const
  { uint64_t s = 0; for (const auto& kv : m_q) s += kv.second.size (); return s; }
  uint64_t Sent () const { return m_sent; }          // 实际经本引擎发出的包数
  uint64_t Returns () const { return m_returns; }
  // 当前任一链路信用是否为负(应永不发生)
  bool AnyNegative () const
  { for (const auto& kv : m_credit) if (kv.second < 0) return true; return false; }
  // 所有已初始化链路的信用是否都恢复到满(排空后应成立 → 信用守恒)
  bool AllRestored () const
  { for (const auto& kv : m_credit) if (kv.second != (int) m_buf) return false; return true; }

private:
  struct QE { Pkt pkt; uint16_t up; };

  void Init (uint16_t nbr)
  { if (!m_credit.count (nbr)) m_credit[nbr] = (int) m_buf; }

  void Drain (uint16_t nbr)
  {
    auto& q = m_q[nbr];
    while (!q.empty () && m_credit[nbr] > 0)
      {
        QE e = q.front (); q.pop_front ();
        --m_credit[nbr];
        uint32_t inflight = m_buf - (uint32_t) m_credit[nbr];
        if (inflight > m_peak[nbr]) m_peak[nbr] = inflight;
        ++m_sent;
        if (m_tx) m_tx (nbr, e.pkt);              // 真正把包发往下游 nbr
        if (e.up != NO_UP && m_ret) m_ret (e.up); // 包已离开本节点 → 给上游回 1 信用
      }
  }

  uint32_t m_buf {256};
  bool     m_on  {true};
  std::map<uint16_t, int>                 m_credit;  // self→nbr 剩余信用
  std::map<uint16_t, std::deque<QE>>      m_q;       // self→nbr 等待队列
  std::map<uint16_t, uint32_t>            m_peak;    // self→nbr 峰值在飞
  uint64_t m_sent {0};
  uint64_t m_returns {0};
  std::function<void(uint16_t, const Pkt&)> m_tx;
  std::function<void(uint16_t)>             m_ret;
};

#endif // LINK_CREDIT_H
