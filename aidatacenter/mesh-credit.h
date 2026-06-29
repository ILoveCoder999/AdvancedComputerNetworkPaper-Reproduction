/*
 * mesh-credit.h — per-destination credit-based flow control to make the 2D mesh
 * lossless under incast. Receiver advertises buffer-depth credits; a sender must
 * hold a credit before transmitting, else it queues. Credits return as the
 * receiver drains packets. Identical mechanism to the OCS-sim version, scoped
 * here to mesh hosts.
 */
#ifndef MESH_CREDIT_H
#define MESH_CREDIT_H

#include "ns3/core-module.h"
#include <map>
#include <deque>
#include <functional>

namespace ns3 {

class MeshCredit : public Object
{
public:
  static TypeId GetTypeId ()
  {
    static TypeId tid = TypeId ("ns3::MeshCredit")
      .SetParent<Object> ().AddConstructor<MeshCredit> ();
    return tid;
  }

  void Configure (uint32_t bufferPkts, Time drainPerPkt, bool enabled)
  { m_buffer = bufferPkts; m_drain = drainPerPkt; m_enabled = enabled; }

  bool Enabled () const { return m_enabled; }

  void InitDest (uint32_t dst)
  { if (!m_credit.count (dst)) m_credit[dst] = m_buffer; }

  // returns true if allowed to send now (credit consumed); false -> must queue
  bool TryConsume (uint32_t dst)
  {
    if (!m_enabled) return true;        // no flow control: always send
    InitDest (dst);
    if (m_credit[dst] > 0) { m_credit[dst]--; return true; }
    return false;
  }

  // called at receiver on arrival: occupancy++ and schedule drain -> credit back
  // returns false if buffer overflowed (only possible when disabled) -> drop
  bool OnArrival (uint32_t dst)
  {
    InitDest (dst);
    m_occ[dst]++;
    if (m_occ[dst] > m_peak[dst]) m_peak[dst] = m_occ[dst];
    if (!m_enabled && m_occ[dst] > m_buffer)
      { m_occ[dst]--; m_drop[dst]++; return false; }   // lossy overflow
    Simulator::Schedule (m_drain, &MeshCredit::Release, this, dst);
    return true;
  }

  void SetOnRelease (std::function<void(uint32_t)> cb) { m_onRelease = cb; }
  uint32_t Peak (uint32_t dst) { return m_peak[dst]; }
  uint32_t Drops (uint32_t dst) { return m_drop[dst]; }

private:
  void Release (uint32_t dst)
  {
    if (m_occ[dst] > 0) m_occ[dst]--;
    if (m_enabled) { m_credit[dst]++; if (m_onRelease) m_onRelease (dst); }
  }

  uint32_t m_buffer {64};
  Time m_drain {NanoSeconds (82)};
  bool m_enabled {true};
  std::map<uint32_t,int> m_credit;
  std::map<uint32_t,uint32_t> m_occ, m_peak, m_drop;
  std::function<void(uint32_t)> m_onRelease;
};

} // namespace ns3
#endif
