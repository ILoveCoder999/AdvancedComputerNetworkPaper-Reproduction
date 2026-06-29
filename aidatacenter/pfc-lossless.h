/*
 * pfc-lossless.h — 优先流控(PFC)无损机制，替"深 DropTail 近似无损"
 * ============================================================================
 * 为什么需要：RoCEv2 的无损靠 PFC(IEEE 802.1Qbb) 实现——入口队列占用超过阈值就向
 * 上游发 PAUSE 帧，让上游停发，从而**真正零丢包**，而不是把队列调很深"近似"。
 * 深 DropTail 的问题：
 *   · 掩盖丢包/排队信号(你 fat-tree queue=1024 远超 BDP)；
 *   · 不产生 PFC 的副作用——HOL blocking、拥塞扩散、PFC 死锁(CBD)、PAUSE 风暴
 *     ——而这些恰是 TON/SIGCOMM 审稿人最较真的真实行为(Guo SIGCOMM'16)。
 *
 * 本头实现一个可挂到 ns-3 交换机入口侧的 PFC 状态机(也可纯 C++ 单测)：
 *   · 每(入口端口, 优先级) 维护占用计数 occ；
 *   · occ ≥ X_off → 发 PAUSE(上游停)；occ ≤ X_on → 发 RESUME(上游续)；
 *   · headroom = 1 个 BDP：PAUSE 在途期间上游还能发的量，必须有 headroom 才零丢；
 *     X_off = bufferBytes - headroomBytes，X_on = X_off - hysteresis。
 *   · 暴露 PAUSE 时长统计 → 可观测 HOL blocking / PAUSE 风暴。
 *
 * 死锁(CBD)检测请配合 routing_deadlock.py：PFC 无损 + 路由有环 = 真死锁。
 * ns-3 接线见 CC_FIDELITY.md。纯 C++14。
 * ============================================================================
 */
#ifndef PFC_LOSSLESS_H
#define PFC_LOSSLESS_H

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>

namespace pfc {

// 由 BDP 推荐 PFC 阈值(字节)。lineRateBps、rttSec 给该规模实测值。
struct PfcConfig {
  uint64_t bufferBytes   = 0;     // 该(端口,优先级)可用缓冲
  uint64_t headroomBytes = 0;     // = 1 BDP (PAUSE 在途余量)，零丢的必要条件
  uint64_t xoff = 0, xon = 0;     // 触发/解除阈值
  static PfcConfig FromBDP (double lineRateBps, double rttSec,
                            double bufferBDPs = 3.0, double hystFrac = 0.1)
  {
    PfcConfig c;
    uint64_t bdp = (uint64_t) (lineRateBps * rttSec / 8.0);   // 字节
    c.headroomBytes = bdp;                                    // 1 BDP headroom
    c.bufferBytes   = (uint64_t) (bufferBDPs * bdp);
    c.xoff = (c.bufferBytes > c.headroomBytes) ? c.bufferBytes - c.headroomBytes : 0;
    c.xon  = (c.xoff > (uint64_t)(hystFrac * bdp)) ? c.xoff - (uint64_t)(hystFrac * bdp) : 0;
    return c;
  }
};

// 一个(端口×优先级)的 PFC 状态机。
class PfcPort {
public:
  void Configure (const PfcConfig& c) { m_cfg = c; }

  // 入队 bytes；返回 false 表示**溢出丢包**(正确 PFC 下不应发生→可断言无损)。
  bool OnEnqueue (uint64_t bytes, double nowSec) {
    m_occ += bytes;
    if (m_occ > m_peak) m_peak = m_occ;
    if (!m_paused && m_occ >= m_cfg.xoff) { m_paused = true; m_pauseStart = nowSec;
      if (m_sendPause) m_sendPause (true); }
    if (m_occ > m_cfg.bufferBytes) { m_drops++; m_occ -= bytes; return false; } // 不该发生
    return true;
  }
  // 出队(转发) bytes
  void OnDequeue (uint64_t bytes, double nowSec) {
    m_occ = (m_occ > bytes) ? m_occ - bytes : 0;
    if (m_paused && m_occ <= m_cfg.xon) { m_paused = false;
      m_pausedSec += (nowSec - m_pauseStart);
      if (m_sendPause) m_sendPause (false); }
  }
  void SetSendPause (std::function<void(bool)> cb) { m_sendPause = std::move (cb); }

  bool     Paused ()   const { return m_paused; }
  uint64_t Occ ()      const { return m_occ; }
  uint64_t Peak ()     const { return m_peak; }
  uint64_t Drops ()    const { return m_drops; }   // 应恒为 0
  double   PausedSec () const { return m_pausedSec; }

private:
  PfcConfig m_cfg;
  uint64_t  m_occ {0}, m_peak {0}, m_drops {0};
  bool      m_paused {false};
  double    m_pauseStart {0}, m_pausedSec {0};
  std::function<void(bool)> m_sendPause;   // 接 ns-3：向上游端口发 PAUSE/RESUME
};

// 多端口/多优先级容器
class PfcSwitch {
public:
  void Configure (uint32_t port, uint8_t prio, const PfcConfig& c)
  { m_ports[key (port, prio)].Configure (c); }
  PfcPort& Port (uint32_t port, uint8_t prio) { return m_ports[key (port, prio)]; }

  uint64_t TotalDrops () const {
    uint64_t d = 0; for (auto& kv : m_ports) d += kv.second.Drops (); return d;
  }
  // —— 聚合统计(跨所有入口端口)，供 *-sim.cc 在 main() 末尾汇报 —— //
  uint64_t MaxPeak () const {                       // 全网最深入口占用(字节)
    uint64_t m = 0; for (auto& kv : m_ports) m = std::max (m, kv.second.Peak ()); return m;
  }
  double TotalPausedSec () const {                  // 所有端口 PAUSE 时长之和(HOL/风暴信号)
    double s = 0; for (auto& kv : m_ports) s += kv.second.PausedSec (); return s;
  }
  uint32_t PausedPorts () const {                   // 曾触发过 PAUSE 的端口数
    uint32_t n = 0; for (auto& kv : m_ports) if (kv.second.PausedSec () > 0) ++n; return n;
  }
  size_t NumPorts () const { return m_ports.size (); }
private:
  static uint64_t key (uint32_t port, uint8_t prio)
  { return ((uint64_t) port << 8) | prio; }
  std::map<uint64_t, PfcPort> m_ports;
};

} // namespace pfc
#endif // PFC_LOSSLESS_H
