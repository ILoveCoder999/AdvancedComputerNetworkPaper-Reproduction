/*
 * link_credit_test.cc — 用纯 C++ 离散事件驱动 LinkCredit 引擎,验证逐跳信用流控的
 * 关键不变量。无 ns-3。编译: g++ -std=c++14 -O2 link_credit_test.cc -o lct && ./lct
 *
 * 场景(中继汇聚,正是 mesh 上 all-reduce 的拥塞本质):
 *   三个源 0,1,2 各发 K 个包给宿 9,路由都经共享链路 5→6→9。
 *   信用窗 buf 很小 → 共享链路 5→6 必然反压,层层回退到三个源。
 * 断言:全部交付、零信用为负、排空后信用全恢复(守恒)、峰值在飞≤窗、确有排队(反压)。
 */
#include <cassert>
#include <cstdio>
#include <map>
#include <queue>
#include <vector>
#include "link-credit.h"

struct Ev {
  double t;
  int type;        // 0=ARRIVE(node,pkt) ; 1=RETURN(node,arg=fromNbr)
  uint16_t node;
  uint64_t pkt;
  uint16_t arg;
};
struct EvCmp { bool operator() (const Ev& a, const Ev& b) const { return a.t > b.t; } };

int main ()
{
  const int    NNODES = 10;
  const uint32_t BUF   = 2;        // 故意很小,逼出反压
  const int    K       = 10;       // 每源包数
  const double LINK    = 0.10;     // 传输+传播延迟(任意单位)
  const double CRED    = 0.05;     // 信用回程延迟

  std::vector<LinkCredit<uint64_t>> lc (NNODES);
  std::map<uint64_t, std::vector<uint16_t>> route;
  std::priority_queue<Ev, std::vector<Ev>, EvCmp> pq;

  double now = 0.0;
  uint64_t delivered = 0, originated = 0;
  uint64_t maxQueued = 0;

  auto idxOf = [&] (uint64_t pkt, uint16_t node) -> int {
    const auto& r = route[pkt];
    for (size_t i = 0; i < r.size (); ++i) if (r[i] == node) return (int) i;
    return -1;
  };
  auto trackQ = [&] () {
    uint64_t q = 0; for (auto& c : lc) q += c.Queued ();
    if (q > maxQueued) maxQueued = q;
  };

  for (int v = 0; v < NNODES; ++v)
    {
      lc[v].Configure (BUF, true);
      lc[v].SetTx ([&, v] (uint16_t nbr, const uint64_t& pkt) {
        // 真正"发往下游 nbr":在 now+LINK 时刻到达 nbr
        pq.push (Ev{now + LINK, 0, nbr, pkt, 0});
      });
      lc[v].SetReturn ([&, v] (uint16_t upstream) {
        // 包离开本节点 v → 通知上游:它到 v 的链路回 1 信用(now+CRED 到达上游)
        pq.push (Ev{now + CRED, 1, upstream, 0, (uint16_t) v});
      });
    }

  // 三源各发 K 个包 → 宿 9,共享 5→6→9
  uint64_t pid = 1;
  uint16_t srcs[3] = {0, 1, 2};
  for (int s = 0; s < 3; ++s)
    for (int k = 0; k < K; ++k)
      {
        uint64_t id = pid++;
        route[id] = { srcs[s], 5, 6, 9 };
        ++originated;
        lc[srcs[s]].Submit (5, id, LinkCredit<uint64_t>::NO_UP);  // 源:无上游
        trackQ ();
      }

  // 事件循环
  while (!pq.empty ())
    {
      Ev e = pq.top (); pq.pop ();
      now = e.t;
      if (e.type == 0)               // ARRIVE at e.node
        {
          int idx = idxOf (e.pkt, e.node);
          assert (idx >= 0);
          const auto& r = route[e.pkt];
          if (idx == (int) r.size () - 1)        // 最终交付
            {
              ++delivered;
              uint16_t up = r[idx - 1];           // 给上游回信用(交付即腾出槽)
              pq.push (Ev{now + CRED, 1, up, 0, e.node});
            }
          else                                    // 中继转发
            {
              uint16_t up = (idx == 0) ? LinkCredit<uint64_t>::NO_UP : r[idx - 1];
              lc[e.node].Submit (r[idx + 1], e.pkt, up);
            }
          trackQ ();
        }
      else                           // RETURN: e.node 收到 "→e.arg 链路回 1 信用"
        {
          lc[e.node].OnReturn (e.arg);
          trackQ ();
        }
    }

  // ── 不变量断言 ──
  bool anyNeg = false, allRestored = true, peakOk = true;
  for (auto& c : lc)
    {
      if (c.AnyNegative ()) anyNeg = true;
      if (!c.AllRestored ()) allRestored = false;
      // 峰值在飞每条链路应 ≤ BUF
      for (uint16_t nbr = 0; nbr < NNODES; ++nbr)
        if (c.Peak (nbr) > BUF) peakOk = false;
    }

  printf ("originated=%llu delivered=%llu  maxQueued=%llu (反压证据,应>0)\n",
          (unsigned long long) originated, (unsigned long long) delivered,
          (unsigned long long) maxQueued);
  printf ("creditNeverNegative=%s  allCreditRestored=%s  peak<=BUF=%s\n",
          anyNeg ? "NO" : "YES", allRestored ? "YES" : "NO", peakOk ? "YES" : "NO");
  // 节点5(汇聚点)峰值在飞应正好用满窗 BUF
  printf ("node5 peak in-flight on 5->6 = %u (窗=%u)\n", lc[5].Peak (6), BUF);

  assert (delivered == originated);   // 无丢、全交付(liveness)
  assert (!anyNeg);                   // 信用永不为负
  assert (allRestored);               // 排空后信用守恒
  assert (peakOk);                    // 窗约束:在飞≤BUF
  assert (maxQueued > 0);             // 确实发生了反压排队
  assert (lc[5].Peak (6) == BUF);     // 共享链路被打满到窗上限
  printf ("ALL LINK-CREDIT INVARIANTS PASSED\n");
  return 0;
}
