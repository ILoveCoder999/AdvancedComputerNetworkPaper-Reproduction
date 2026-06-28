#include "fat-tree-central-scheduler.h"

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (CentralScheduler);

TypeId
CentralScheduler::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::CentralScheduler")
                          .SetParent<Object> ()
                          .SetGroupName ("Internet")
                          .AddConstructor<CentralScheduler> ();
  return tid;
}

CentralScheduler::CentralScheduler ()
    : m_k (4), m_half (2), m_numCore (4)
{
}

void
CentralScheduler::Setup (uint32_t k)
{
  m_k = k;
  m_half = k / 2;
  m_numCore = m_half * m_half;
}

uint64_t
CentralScheduler::LinkKey (uint32_t type, uint32_t w, uint32_t x, uint32_t y)
{
  return (static_cast<uint64_t> (type) << 48) | (static_cast<uint64_t> (w) << 32) |
         (static_cast<uint64_t> (x) << 16) | static_cast<uint64_t> (y);
}

void
CentralScheduler::RegisterFlow (Ipv4Address src, Ipv4Address dst)
{
  uint32_t s = src.Get ();
  uint32_t d = dst.Get ();
  Flow f;
  f.sPod = (s >> 16) & 0xFF;
  f.sEdge = (s >> 8) & 0xFF;
  f.dPod = (d >> 16) & 0xFF;
  f.dEdge = (d >> 8) & 0xFF;
  f.destId = d & 0xFF;
  f.key = (static_cast<uint64_t> (s) << 32) | d;
  f.inter = (f.sPod != f.dPod);
  m_flows.push_back (f);
  m_flowByKey[f.key] = f;
}

void
CentralScheduler::LinksFor (const Flow &f, uint32_t c, uint64_t out[4]) const
{
  uint32_t a = c / m_half;
  out[0] = LinkKey (0, f.sPod, f.sEdge, a);
  out[1] = LinkKey (1, f.sPod, c, 0);
  out[2] = LinkKey (2, f.dPod, c, 0);
  out[3] = LinkKey (3, f.dPod, f.dEdge, a);
}

bool
CentralScheduler::Free (const Flow &f, uint32_t c) const
{
  uint64_t L[4];
  LinksFor (f, c, L);
  for (int i = 0; i < 4; ++i)
    {
      if (m_reserved.count (L[i]))
        {
          return false;
        }
    }
  return true;
}

void
CentralScheduler::DoReserve (const Flow &f, uint32_t c)
{
  uint64_t L[4];
  LinksFor (f, c, L);
  for (int i = 0; i < 4; ++i)
    {
      m_reserved.insert (L[i]);
      m_linkOwner[L[i]] = f.key;
    }
  m_assignedCore[f.key] = static_cast<int32_t> (c);
  m_flowCore[f.key] = static_cast<int32_t> (c);
}

void
CentralScheduler::Unreserve (const Flow &f)
{
  std::map<uint64_t, int32_t>::iterator it = m_assignedCore.find (f.key);
  if (it == m_assignedCore.end ())
    {
      return;
    }
  uint32_t c = static_cast<uint32_t> (it->second);
  uint64_t L[4];
  LinksFor (f, c, L);
  for (int i = 0; i < 4; ++i)
    {
      m_reserved.erase (L[i]);
      m_linkOwner.erase (L[i]);
    }
  m_assignedCore.erase (it);
  m_flowCore[f.key] = -1;
}

bool
CentralScheduler::TryPlace (const Flow &f)
{
  for (uint32_t c = 0; c < m_numCore; ++c)
    {
      if (Free (f, c))
        {
          DoReserve (f, c);
          return true;
        }
    }
  return false;
}

bool
CentralScheduler::TryPlaceWithEviction (const Flow &f)
{
  for (uint32_t c = 0; c < m_numCore; ++c)
    {
      uint64_t L[4];
      LinksFor (f, c, L);

      // 收集冲突链路的占用者; 若撞到"同 Pod 流"占的不可腾挪链路, 跳过此核心
      std::set<uint64_t> owners;
      bool intraBlocked = false;
      for (int i = 0; i < 4; ++i)
        {
          if (m_reserved.count (L[i]))
            {
              std::map<uint64_t, uint64_t>::iterator it = m_linkOwner.find (L[i]);
              if (it == m_linkOwner.end ())
                {
                  intraBlocked = true;
                  break;
                }
              owners.insert (it->second);
            }
        }
      if (intraBlocked || owners.size () != 1)
        {
          continue; // 只做单跳腾挪
        }

      uint64_t gKey = *owners.begin ();
      Flow g = m_flowByKey[gKey];
      int32_t gOrig = m_assignedCore[gKey];

      Unreserve (g);
      if (!Free (f, c))
        {
          DoReserve (g, static_cast<uint32_t> (gOrig)); // 安全兜底(理论不会发生)
          continue;
        }
      DoReserve (f, c);

      // 给被腾挪的流另找一个核心
      bool gok = false;
      for (uint32_t c2 = 0; c2 < m_numCore; ++c2)
        {
          if (Free (g, c2))
            {
              DoReserve (g, c2);
              gok = true;
              break;
            }
        }
      if (gok)
        {
          return true;
        }

      // 失败回滚: 撤掉 f, 把 g 放回原核心
      Unreserve (f);
      DoReserve (g, static_cast<uint32_t> (gOrig));
    }
  return false;
}

void
CentralScheduler::Compute (void)
{
  m_reserved.clear ();
  m_linkOwner.clear ();
  m_assignedCore.clear ();
  m_flowCore.clear ();

  // 1) 同 Pod 流走两层路由的固定路径, 先占住其 edge->agg / agg->edge 链路
  for (size_t i = 0; i < m_flows.size (); ++i)
    {
      const Flow &f = m_flows[i];
      if (!f.inter)
        {
          uint32_t a = (f.destId - 2 + f.sEdge) % m_half;
          m_reserved.insert (LinkKey (0, f.sPod, f.sEdge, a));
          m_reserved.insert (LinkKey (3, f.sPod, f.dEdge, a));
          m_flowCore[f.key] = -1;
        }
    }

  // 2) 跨 Pod 流: 贪心放置
  std::vector<size_t> cross;
  for (size_t i = 0; i < m_flows.size (); ++i)
    {
      if (m_flows[i].inter)
        {
          cross.push_back (i);
        }
    }
  std::vector<bool> placed (cross.size (), false);
  for (size_t i = 0; i < cross.size (); ++i)
    {
      if (TryPlace (m_flows[cross[i]]))
        {
          placed[i] = true;
        }
    }

  // 3) 放不下的做单跳腾挪修复; 仍失败则回退两层路由
  for (size_t i = 0; i < cross.size (); ++i)
    {
      if (!placed[i])
        {
          if (!TryPlaceWithEviction (m_flows[cross[i]]))
            {
              m_flowCore[m_flows[cross[i]].key] = -1;
            }
        }
    }
}

int32_t
CentralScheduler::GetCoreForFlow (Ipv4Address src, Ipv4Address dst)
{
  uint64_t key = (static_cast<uint64_t> (src.Get ()) << 32) | dst.Get ();
  std::map<uint64_t, int32_t>::iterator it = m_flowCore.find (key);
  return (it != m_flowCore.end ()) ? it->second : -1;
}

} // namespace ns3
