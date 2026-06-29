/*
 * nccl-collectives.h — NCCL 风格集合通信流量生成层（替掉 s→s+1 朴素 allreduce）
 * ============================================================================
 * 让仿真流量"像 AI 数据中心"：真实训练的网络流量本质由 NCCL 的集合通信算法 +
 * 并行策略(DP/TP/PP/EP)决定，而不是随机打流。本层生成**模式正确、字节正确**的
 * 通信计划(schedule)，由各 *-sim.cc 回放(replay)。
 *
 * 生成的是一串 CommOp{src,dst,bytes,step,phase}：
 *   · 模式正确：谁跟谁通信，完全照 NCCL 算法（ring/tree/all-to-all）。
 *   · 字节正确：每步消息大小照算法（如 ring 每步 M/P），总量满足解析公式。
 *   · step 表达依赖：同 step 可并发，step 间是 barrier（下一步要等本步数据到）。
 *     回放时可：(a) 开环全注入(吞吐/能耗对比足够) 或 (b) 按 step 加 barrier(更真实的 FCT)。
 *
 * 覆盖：
 *   - RingAllReduce            带宽最优，2(P-1) 步，每 rank 传 2(P-1)/P·M   [大消息/DP 梯度]
 *   - RecursiveHalvingDoubling 带宽最优，2·log2(P) 步 (Rabenseifner)        [P 为 2 的幂]
 *   - DoubleBinaryTreeAllReduce 满带宽 + 对数延迟 (NCCL 2.4)                 [大规模]
 *   - AllToAll                 每 rank 传 (P-1)/P·M                          [MoE 专家并行 EP]
 *   - PipelineP2P              相邻流水级 send/recv                          [流水并行 PP]
 *   并由 ParallelPlan 把 DP/TP/PP/EP 映射到 GPU 集合，生成一个训练迭代的全部网络流量。
 *   （TP 通常落在服务器内 NVLink 域 → 可标记为 onNvlink 不上网络。）
 *
 * 纯 C++14，无依赖，可独立编译做单元测试。
 * ============================================================================
 */
#ifndef NCCL_COLLECTIVES_H
#define NCCL_COLLECTIVES_H

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace nccl {

enum Phase : uint8_t {
  RS = 0,        // reduce-scatter
  AG = 1,        // all-gather
  TREE_UP = 2,   // 树规约(上行)
  TREE_DN = 3,   // 树广播(下行)
  A2A = 4,       // all-to-all
  P2P = 5        // 点对点(流水)
};

struct CommOp {
  uint32_t src;
  uint32_t dst;
  uint64_t bytes;
  uint32_t step;        // 同 step 并发；step 间 barrier
  uint8_t  phase;
  bool     onNvlink;    // true = 服务器内 NVLink（不计入网络）
};

inline const char* PhaseName (uint8_t p)
{
  switch (p) { case RS:return "RS"; case AG:return "AG"; case TREE_UP:return "TREE_UP";
    case TREE_DN:return "TREE_DN"; case A2A:return "A2A"; case P2P:return "P2P"; }
  return "?";
}

// ───────────────────────── 1) Ring AllReduce ─────────────────────────
// 带宽最优。reduce-scatter (P-1 步) + all-gather (P-1 步)，每步沿环传 M/P。
// 每 rank 总发送 = 2(P-1)/P · M。ringOrder 可自定义(rail/torus 局部性优化)。
inline void RingAllReduce (const std::vector<uint32_t>& ranks, uint64_t M,
                           std::vector<CommOp>& out, uint32_t baseStep = 0,
                           bool onNvlink = false,
                           const std::vector<uint32_t>* ringOrder = nullptr)
{
  uint32_t P = ranks.size ();
  if (P < 2) return;
  uint64_t chunk = M / P;
  std::vector<uint32_t> ring (P);
  for (uint32_t i = 0; i < P; ++i)
    ring[i] = ringOrder ? ranks[(*ringOrder)[i]] : ranks[i];
  // reduce-scatter
  for (uint32_t s = 0; s < P - 1; ++s)
    for (uint32_t i = 0; i < P; ++i)
      out.push_back ({ring[i], ring[(i + 1) % P], chunk, baseStep + s, RS, onNvlink});
  // all-gather
  for (uint32_t s = 0; s < P - 1; ++s)
    for (uint32_t i = 0; i < P; ++i)
      out.push_back ({ring[i], ring[(i + 1) % P], chunk, baseStep + (P - 1) + s, AG, onNvlink});
}

// ─────────────── 2) Recursive Halving-Doubling (Rabenseifner) ───────────────
// 带宽最优，2·log2(P) 步。要求 P 为 2 的幂；否则请用 Ring。
// reduce-scatter: 第 d 步与距离 2^d 的伙伴交换 M/2^(d+1)；all-gather 镜像。
inline bool RecursiveHalvingDoubling (const std::vector<uint32_t>& ranks, uint64_t M,
                                      std::vector<CommOp>& out, uint32_t baseStep = 0,
                                      bool onNvlink = false)
{
  uint32_t P = ranks.size ();
  if (P < 2 || (P & (P - 1)) != 0) return false;     // 需 2 的幂
  uint32_t L = 0; while ((1u << L) < P) ++L;
  uint32_t st = baseStep;
  // reduce-scatter: 块大小 M/2, M/4, ...
  for (uint32_t d = 0; d < L; ++d) {
    uint64_t b = M >> (d + 1);
    for (uint32_t i = 0; i < P; ++i) {
      uint32_t partner = i ^ (1u << d);
      if (i < partner) {                              // 双向各一条
        out.push_back ({ranks[i], ranks[partner], b, st, RS, onNvlink});
        out.push_back ({ranks[partner], ranks[i], b, st, RS, onNvlink});
      }
    }
    ++st;
  }
  // all-gather: 块大小 ..., M/4, M/2
  for (uint32_t d = 0; d < L; ++d) {
    uint64_t b = M >> (L - d);
    for (uint32_t i = 0; i < P; ++i) {
      uint32_t partner = i ^ (1u << (L - 1 - d));
      if (i < partner) {
        out.push_back ({ranks[i], ranks[partner], b, st, AG, onNvlink});
        out.push_back ({ranks[partner], ranks[i], b, st, AG, onNvlink});
      }
    }
    ++st;
  }
  return true;
}

// ─────────────── 3) Double Binary Tree AllReduce (NCCL 2.4) ───────────────
// 满带宽 + 对数延迟。两棵互补二叉树各承载 M/2：规约上行 + 广播下行。
// 这里生成每条树边的传输(M/2 上 + M/2 下)，模式正确；逐节点字节随位置不同，
// 双树设计使总体接近满带宽。
inline void DoubleBinaryTreeAllReduce (const std::vector<uint32_t>& ranks, uint64_t M,
                                       std::vector<CommOp>& out, uint32_t baseStep = 0,
                                       bool onNvlink = false)
{
  uint32_t P = ranks.size ();
  if (P < 2) return;
  uint64_t half = M / 2;
  // 一棵树：节点 i 的父 = (i-1)/2（标准完全二叉树）。tree2 用 i' = P-1-i 映射(互补)。
  auto emitTree = [&](bool mirror) {
    auto idx = [&](uint32_t i) { return mirror ? ranks[P - 1 - i] : ranks[i]; };
    uint32_t depth = 0; while ((1u << depth) <= P) ++depth;   // 树高上界
    // 规约上行：叶→根，按深度分层(深的先)，子发父
    for (uint32_t d = depth; d-- > 0; ) {
      uint32_t lo = (1u << d) - 1, hi = (1u << (d + 1)) - 1;
      for (uint32_t i = lo; i < hi && i < P; ++i) {
        uint32_t parent = (i - 1) / 2;
        if (i > 0) out.push_back ({idx (i), idx (parent), half,
                                   baseStep + (depth - 1 - d), TREE_UP, onNvlink});
      }
    }
    // 广播下行：根→叶，父发子
    for (uint32_t d = 0; d < depth; ++d) {
      uint32_t lo = (1u << d) - 1, hi = (1u << (d + 1)) - 1;
      for (uint32_t i = lo; i < hi && i < P; ++i) {
        uint32_t parent = (i - 1) / 2;
        if (i > 0) out.push_back ({idx (parent), idx (i), half,
                                   baseStep + depth + d, TREE_DN, onNvlink});
      }
    }
  };
  emitTree (false);    // tree 1 承载 M/2
  emitTree (true);     // tree 2 承载 M/2（互补）
}

// ───────────────────────── 4) All-to-All (MoE EP) ─────────────────────────
// 每 rank 向其余每个 rank 发 M/P。MoE 专家并行 dispatch/combine 的本质模式。
inline void AllToAll (const std::vector<uint32_t>& ranks, uint64_t M,
                      std::vector<CommOp>& out, uint32_t baseStep = 0,
                      bool onNvlink = false)
{
  uint32_t P = ranks.size ();
  if (P < 2) return;
  uint64_t b = M / P;
  for (uint32_t i = 0; i < P; ++i)
    for (uint32_t j = 0; j < P; ++j)
      if (i != j)
        out.push_back ({ranks[i], ranks[j], b, baseStep, A2A, onNvlink});
}

// ───────────────────────── 5) Pipeline P2P (PP) ─────────────────────────
// 有序流水级：前向 stage k→k+1 发激活 Af；反向 k→k-1 发梯度 Ag。
inline void PipelineP2P (const std::vector<uint32_t>& stages, uint64_t actBytes,
                         uint64_t gradBytes, std::vector<CommOp>& out,
                         uint32_t baseStep = 0)
{
  uint32_t S = stages.size ();
  for (uint32_t k = 0; k + 1 < S; ++k) {
    out.push_back ({stages[k], stages[k + 1], actBytes, baseStep, P2P, false});       // 前向
    out.push_back ({stages[k + 1], stages[k], gradBytes, baseStep + 1, P2P, false});  // 反向
  }
}

// ═══════════════════ 并行策略映射：DP/TP/PP/EP → GPU 集合 ═══════════════════
// 全局 rank 分解(Megatron 约定，TP 最内→落 NVLink 域)：
//   rank = ((pp_idx*DP + dp_idx)*TP + tp_idx)，EP 复用 DP 维(MoE 时 DP→EP all-to-all)。
struct ParallelPlan {
  uint32_t DP = 1, TP = 1, PP = 1, EP = 1;   // 各维并行度
  uint32_t gpusPerServer = 8;                // NVLink 域大小
  uint64_t gradBytes = 0;                    // DP all-reduce 梯度量(每 rank)
  uint64_t tpBytes   = 0;                    // 每层 TP all-reduce 量
  uint64_t actBytes  = 0;                    // PP 激活量
  uint64_t moeBytes  = 0;                    // EP all-to-all 量
  bool     tpOnNvlink = true;                // TP 是否走 NVLink(域内则 true)

  uint32_t totalGpus () const { return DP * TP * PP; }   // EP 复用 DP 维
  // 三维坐标 → 全局 rank
  uint32_t rankOf (uint32_t pp, uint32_t dp, uint32_t tp) const
  { return (pp * DP + dp) * TP + tp; }
};

enum DpAlgo { DP_RING, DP_HALVING_DOUBLING, DP_DOUBLE_TREE };

// 生成一个训练迭代的**全部网络流量**(各组并发，组内按算法)。
inline std::vector<CommOp> GenerateTrainingStep (const ParallelPlan& p,
                                                 DpAlgo dpAlgo = DP_RING,
                                                 bool moe = false)
{
  std::vector<CommOp> ops;
  bool tpNv = p.tpOnNvlink && (p.TP <= p.gpusPerServer);

  // —— TP all-reduce：每个 (pp,dp) 固定，组内 tp 变 —— //
  if (p.TP > 1 && p.tpBytes) {
    for (uint32_t pp = 0; pp < p.PP; ++pp)
      for (uint32_t dp = 0; dp < p.DP; ++dp) {
        std::vector<uint32_t> g;
        for (uint32_t tp = 0; tp < p.TP; ++tp) g.push_back (p.rankOf (pp, dp, tp));
        RingAllReduce (g, p.tpBytes, ops, 0, tpNv);   // 域内常 NVLink
      }
  }

  // —— DP/EP：每个 (pp,tp) 固定，组内 dp 变 —— //
  for (uint32_t pp = 0; pp < p.PP; ++pp)
    for (uint32_t tp = 0; tp < p.TP; ++tp) {
      std::vector<uint32_t> g;
      for (uint32_t dp = 0; dp < p.DP; ++dp) g.push_back (p.rankOf (pp, dp, tp));
      if (g.size () < 2) continue;
      if (moe && p.moeBytes) {
        AllToAll (g, p.moeBytes, ops, 100);            // MoE 专家并行 all-to-all
      } else if (p.gradBytes) {
        if (dpAlgo == DP_HALVING_DOUBLING &&
            RecursiveHalvingDoubling (g, p.gradBytes, ops, 100)) { /* ok */ }
        else if (dpAlgo == DP_DOUBLE_TREE)
          DoubleBinaryTreeAllReduce (g, p.gradBytes, ops, 100);
        else
          RingAllReduce (g, p.gradBytes, ops, 100);    // 默认 ring
      }
    }

  // —— PP：每个 (dp,tp) 固定，组内 pp 变(有序流水) —— //
  if (p.PP > 1 && p.actBytes) {
    for (uint32_t dp = 0; dp < p.DP; ++dp)
      for (uint32_t tp = 0; tp < p.TP; ++tp) {
        std::vector<uint32_t> chain;
        for (uint32_t pp = 0; pp < p.PP; ++pp) chain.push_back (p.rankOf (pp, dp, tp));
        PipelineP2P (chain, p.actBytes, p.actBytes, ops, 1000);
      }
  }
  return ops;
}

// ═══════════════════════════ 工具：统计 / 输出 ═══════════════════════════
// 仅网络流量(排除 NVLink)的总字节
inline uint64_t NetworkBytes (const std::vector<CommOp>& ops)
{
  uint64_t s = 0;
  for (const auto& o : ops) if (!o.onNvlink) s += o.bytes;
  return s;
}
// 每 rank 发送字节(校验解析公式用)
inline std::map<uint32_t, uint64_t> PerRankSent (const std::vector<CommOp>& ops,
                                                 bool includeNvlink = false)
{
  std::map<uint32_t, uint64_t> m;
  for (const auto& o : ops) if (includeNvlink || !o.onNvlink) m[o.src] += o.bytes;
  return m;
}
// N×N 流量矩阵(仅网络)，写 CSV，便于画热力图("像 AI DC")
inline void WriteTrafficMatrix (const std::vector<CommOp>& ops, uint32_t N,
                                const std::string& path)
{
  std::vector<std::vector<uint64_t>> M (N, std::vector<uint64_t> (N, 0));
  for (const auto& o : ops)
    if (!o.onNvlink && o.src < N && o.dst < N) M[o.src][o.dst] += o.bytes;
  std::ofstream f (path);
  for (uint32_t i = 0; i < N; ++i)
    for (uint32_t j = 0; j < N; ++j)
      f << M[i][j] << (j + 1 < N ? ',' : '\n');
}
// 转包数(供 sim 回放：每 op 拆成 bytes/pktBytes 个包)
inline uint64_t ToPackets (uint64_t bytes, uint32_t pktBytes)
{ return (bytes + pktBytes - 1) / pktBytes; }

} // namespace nccl
#endif // NCCL_COLLECTIVES_H
