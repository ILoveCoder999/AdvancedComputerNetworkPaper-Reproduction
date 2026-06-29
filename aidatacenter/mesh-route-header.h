/*
 * mesh-route-header.h — carries the DOR (dimension-order) path for the 2D mesh.
 * Path is at most 3 node-ids: [src, (optional intersection), dst].
 * Each host reads the header, forwards to next hop. 0 or 1 relay hops.
 */
#ifndef MESH_ROUTE_HEADER_H
#define MESH_ROUTE_HEADER_H

#include "ns3/header.h"
#include <vector>
#include <cstdint>

namespace ns3 {

class MeshRouteHeader : public Header
{
public:
  MeshRouteHeader () = default;

  void SetPath (const std::vector<uint16_t> &p) { m_path = p; m_cursor = 1; }
  const std::vector<uint16_t> & GetPath () const { return m_path; }
  uint16_t GetCursor () const { return m_cursor; }
  void SetCursor (uint16_t c) { m_cursor = c; }
  // FIX: 原来 m_path.size()-2 < 0 对无符号类型永远为 false，导致 size=0/1 时下溢
  uint16_t RelayHops () const { return m_path.size () >= 2 ? (uint16_t)(m_path.size () - 2) : 0; }

  static TypeId GetTypeId ()
  {
    static TypeId tid = TypeId ("ns3::MeshRouteHeader")
      .SetParent<Header> ().AddConstructor<MeshRouteHeader> ();
    return tid;
  }
  TypeId GetInstanceTypeId () const override { return GetTypeId (); }

  uint32_t GetSerializedSize () const override
  { return 2 + 2 + 2 * m_path.size (); }

  void Serialize (Buffer::Iterator start) const override
  {
    start.WriteHtonU16 (m_cursor);
    start.WriteHtonU16 ((uint16_t) m_path.size ());
    for (uint16_t id : m_path) start.WriteHtonU16 (id);
  }

  uint32_t Deserialize (Buffer::Iterator start) override
  {
    m_cursor = start.ReadNtohU16 ();
    uint16_t n = start.ReadNtohU16 ();
    m_path.clear (); m_path.reserve (n);
    for (uint16_t i = 0; i < n; ++i) m_path.push_back (start.ReadNtohU16 ());
    return GetSerializedSize ();
  }

  void Print (std::ostream &os) const override
  {
    os << "MRH cur=" << m_cursor << " path=[";
    for (size_t i = 0; i < m_path.size (); ++i)
      os << m_path[i] << (i + 1 < m_path.size () ? "," : "");
    os << "]";
  }

private:
  std::vector<uint16_t> m_path;
  uint16_t m_cursor {1};
};

} // namespace ns3
#endif
