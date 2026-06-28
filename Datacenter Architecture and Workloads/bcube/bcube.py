#!/usr/bin/env python3
"""
BCube: A High Performance, Server-centric Network Architecture
=============================================================
Python implementation of the core algorithms from:
  Chuanxiong Guo et al., SIGCOMM 2009

Implements (matching paper section numbers):
  §3.1  BCube topology & addressing
  §3.2  BCubeRouting  (single-path, diameter k+1)
  §3.3  BuildPathSet  (k+1 node-disjoint parallel paths, Theorem 3)
  §3.5  BuildMultipleSPTs  (k+1 edge-disjoint spanning trees, Theorem 5)
  §3.6  Aggregate Bottleneck Throughput — ABT (Theorem 6 + Appendix B)
  §6    Graceful degradation simulation

Usage:
  python bcube.py
"""

from itertools import combinations
import random


# ═══════════════════════════════════════════════════════════════════════════
# §3.1  BCube topology & addressing
# ═══════════════════════════════════════════════════════════════════════════

class BCube:
    """
    BCube_k with n-port switches.

    Address convention
    ------------------
    A server address is stored as a Python list  [a_0, a_1, …, a_k]
    where a_i ∈ [0, n-1].  The paper writes it MSB-first: a_k a_{k-1} … a_0.
    addr_str() converts to paper notation for display.

    Key numbers
    -----------
    N         = n^(k+1)          total servers
    diameter  = k+1              hops (Theorem 1)
    #parallel paths = k+1        between any two servers (Theorem 3)
    speedup   = k+1              for one-to-all (Theorem 5)
    ABT       = n/(n-1)·(N-1)·bw (Theorem 6)
    """

    def __init__(self, n: int, k: int):
        assert n >= 2 and k >= 0
        self.n = n
        self.k = k
        self.N = n ** (k + 1)

    # ── address helpers ────────────────────────────────────────────────────

    def digits(self, server_id: int) -> list:
        """Integer ID → digit list [a_0, …, a_k]."""
        d, x = [], server_id
        for _ in range(self.k + 1):
            d.append(x % self.n)
            x //= self.n
        return d

    def from_digits(self, d: list) -> int:
        """Digit list → integer ID."""
        x = 0
        for i in range(self.k, -1, -1):
            x = x * self.n + d[i]
        return x

    def addr_str(self, d) -> str:
        """Digit list → paper-style string  a_k … a_0  (MSB first)."""
        return ''.join(str(d[i]) for i in range(self.k, -1, -1))

    def all_servers(self) -> list:
        return [self.digits(i) for i in range(self.N)]

    # ── topology queries ───────────────────────────────────────────────────

    def hamming(self, A: list, B: list) -> int:
        """Number of differing digits."""
        return sum(a != b for a, b in zip(A, B))

    def neighbors_at(self, A: list, level: int) -> list:
        """
        Return the n-1 servers sharing a level-`level` switch with A.
        They differ from A only at digit index `level`.
        """
        out = []
        for v in range(self.n):
            if v != A[level]:
                B = A[:]
                B[level] = v
                out.append(B)
        return out

    def switch_id(self, A: list, level: int) -> tuple:
        """
        Canonical ID of the level-`level` switch that server A connects to.
        Represented as (level, digits_excluding_level).
        """
        return (level, tuple(A[i] for i in range(self.k + 1) if i != level))

    # ── §3.2  BCubeRouting ────────────────────────────────────────────────

    def bcube_routing(self, A: list, B: list, pi: list) -> list:
        """
        Single-path routing from A to B.

        pi[k], pi[k-1], …, pi[0]  — digit-correction order.
        The loop runs  i = k, k-1, …, 0  and at each step corrects digit pi[i].

        Returns list of server digit-lists: [A, …, B].
        Path length ≤ k+1  (Theorem 1: diameter = k+1).
        """
        node = A[:]
        path = [node[:]]
        for i in range(self.k, -1, -1):
            pos = pi[i]
            if node[pos] != B[pos]:
                node = node[:]
                node[pos] = B[pos]
                path.append(node[:])
        return path

    # ── §3.3  BuildPathSet — k+1 parallel paths ───────────────────────────

    def _dc_routing(self, A: list, B: list, start: int) -> list:
        """
        DCRouting (Fig. 3): permutation begins at digit `start`, cycles down.
        Used when A[start] ≠ B[start].
        """
        pi = [0] * (self.k + 1)
        m = self.k
        for j in range(start, start - self.k - 1, -1):
            pi[m] = j % (self.k + 1)
            m -= 1
        return self.bcube_routing(A, B, pi)

    def _alt_dc_routing(self, A: list, B: list, start: int, C: list) -> list:
        """
        AltDCRouting (Fig. 3): for the case A[start] == B[start].
        Routes  A → C → … → B  where C is a level-start neighbor of A.
        Permutation starts one position earlier than DCRouting.
        """
        pi = [0] * (self.k + 1)
        m  = self.k
        s  = start - 1
        for j in range(s, s - self.k - 1, -1):
            pi[m] = j % (self.k + 1)
            m -= 1
        sub = self.bcube_routing(C, B, pi)   # C → … → B
        return [A[:]] + sub                  # prepend A

    def build_path_set(self, A: list, B: list) -> list:
        """
        BuildPathSet (Fig. 3, Theorem 3):
        k+1 node-disjoint parallel paths from A to B.

        Path lengths: h(A,B) hops  (1st category, A[i]≠B[i])
                   or h(A,B)+2 hops (2nd category, A[i]==B[i])
        Time complexity: O(k²).

        Returns list of k+1 paths; each path is a list of digit-arrays.
        """
        paths = []
        for i in range(self.k, -1, -1):
            if A[i] != B[i]:
                p = self._dc_routing(A, B, i)
            else:
                C = A[:]
                C[i] = (A[i] + 1) % self.n   # any level-i neighbor of A
                p = self._alt_dc_routing(A, B, i, C)
            paths.append(p)
        return paths

    # ── §3.5  BuildMultipleSPTs — one-to-all ──────────────────────────────

    def build_multiple_spts(self, src: list) -> list:
        """
        BuildMultipleSPTs (Fig. 5, Theorem 5):
        k+1 edge-disjoint server spanning trees rooted at `src`.

        By splitting a file of size L into k+1 parts and sending each
        part along a separate spanning tree, one-to-all completes in L/(k+1)
        time — a k+1 speedup over tree/fat-tree structures.

        Returns list of k+1 dicts:
          'root'  : digit-list of the tree's root (level-i neighbor of src)
          'edges' : list of (parent_digits, child_digits) tuples
          'nodes' : set of integer server IDs covered (excludes src)
        """
        src    = src[:]
        src_id = self.from_digits(src)
        trees  = []

        for tree_level in range(self.k + 1):
            root      = src[:]
            root[tree_level] = (src[tree_level] + 1) % self.n
            root_id   = self.from_digits(root)

            visited   = {root_id: root[:]}          # id → digits
            edges     = [(src[:], root[:])]          # src → root

            self._build_single_spt(src, src_id, root, tree_level,
                                   visited, edges)
            trees.append({
                'root' : root[:],
                'edges': edges,
                'nodes': set(visited.keys()),
            })

        return trees

    def _build_single_spt(self, src, src_id, initial_root,
                          level, visited, edges):
        """
        Implement BuildSingleSPT (Fig. 5).

        Part I : expand the tree dimension-by-dimension.
                 Starting from the root, in each round extend every
                 current-leaf along the next dimension in a chain of n-1 nodes.

        Part II: attach remaining servers whose digit[level] == src[level].
                 They were unreachable in Part I because their chain would
                 pass through src.
        """
        T = [initial_root[:]]     # servers in tree (excluding src)

        # ── Part I ────────────────────────────────────────────────────────
        for i in range(self.k + 1):
            dim = (level + i) % (self.k + 1)
            T2  = []
            for A in T:
                node = A[:]
                prev = tuple(A)
                for _ in range(self.n - 1):
                    node    = node[:]
                    node[dim] = (node[dim] + 1) % self.n
                    nid     = self.from_digits(node)
                    if nid == src_id:
                        # Skip src; keep updating prev so chain continues
                        prev = tuple(node)
                        continue
                    if nid not in visited:
                        visited[nid] = node[:]
                        edges.append((list(prev), node[:]))
                        T2.append(node[:])
                    prev = tuple(node)
            T.extend(T2)

        # ── Part II ───────────────────────────────────────────────────────
        # Servers with S[level] == src[level] are not reachable in Part I
        # (their chains hit src). Connect them via S2 = S with S2[level]-1.
        for sid in range(self.N):
            if sid == src_id:
                continue
            S = self.digits(sid)
            if S[level] == src[level] and sid not in visited:
                S2          = S[:]
                S2[level]   = (S[level] - 1) % self.n
                visited[sid] = S[:]
                edges.append((S2[:], S[:]))

    # ── §3.6  Aggregate Bottleneck Throughput ─────────────────────────────

    def abt(self, link_bw: float = 1.0) -> dict:
        """
        Compute ABT analytically (Theorem 6 + Appendix B).

        Average path length (Appendix B):
            ave_plen = (n-1)·N / (n·(N-1)) · (k+1)

        Flows per link:
            f_num = N·(N-1)·ave_plen / (N·(k+1))

        Throughput per flow:  1 / f_num  (with unit link bandwidth)

        ABT = N·(N-1) / f_num = n/(n-1) · (N-1) · link_bw

        Args:
            link_bw: per-link bandwidth in Gb/s (default 1.0)

        Returns dict with:
            N, avg_path_len, flows_per_link, ABT_Gbps
        """
        n, k, N = self.n, self.k, self.N
        avg_plen     = (n - 1) * N / (n * (N - 1)) * (k + 1)
        flows_per_lk = N * (N - 1) * avg_plen / (N * (k + 1))
        abt_val      = n / (n - 1) * (N - 1) * link_bw
        return {
            'N'             : N,
            'avg_path_len'  : round(avg_plen, 4),
            'flows_per_link': round(flows_per_lk, 4),
            'ABT_Gbps'      : round(abt_val, 2),
        }


# ═══════════════════════════════════════════════════════════════════════════
# Verification
# ═══════════════════════════════════════════════════════════════════════════

def verify_parallel_paths(bc: BCube, n_trials: int = 200, seed: int = 0) -> bool:
    """
    Theorem 3: BuildPathSet produces exactly k+1 node-disjoint paths.
    Check by sampling random server pairs and confirming no intermediate
    server appears in more than one path.
    """
    rng      = random.Random(seed)
    failures = 0
    for _ in range(n_trials):
        A = bc.digits(rng.randrange(bc.N))
        B = bc.digits(rng.randrange(bc.N))
        if A == B:
            continue
        paths = bc.build_path_set(A, B)
        if len(paths) != bc.k + 1:
            failures += 1
            continue
        # Intermediate nodes only (exclude endpoints)
        intermediates = [
            frozenset(bc.from_digits(h) for h in p[1:-1])
            for p in paths
        ]
        for i, j in combinations(range(len(intermediates)), 2):
            if intermediates[i] & intermediates[j]:
                failures += 1
    return failures == 0


def verify_spts(bc: BCube) -> bool:
    """
    Theorem 5: k+1 spanning trees from server-0 are edge-disjoint
    and each covers all N-1 other servers.
    """
    src   = bc.digits(0)
    trees = bc.build_multiple_spts(src)
    ok    = True

    edge_sets = []
    for t_idx, tree in enumerate(trees):
        # Coverage check
        if len(tree['nodes']) != bc.N - 1:
            print(f"  [FAIL] Tree {t_idx}: covers {len(tree['nodes'])}, expected {bc.N-1}")
            ok = False

        # Collect internal edges (skip the src→root edge)
        es = set()
        for (p, c) in tree['edges'][1:]:
            e = frozenset([bc.from_digits(p), bc.from_digits(c)])
            if e in es:
                print(f"  [FAIL] Tree {t_idx}: duplicate edge")
                ok = False
            es.add(e)
        edge_sets.append(es)

    # Edge-disjointness across trees
    for i, j in combinations(range(len(edge_sets)), 2):
        overlap = edge_sets[i] & edge_sets[j]
        if overlap:
            print(f"  [FAIL] Trees {i} & {j} share {len(overlap)} edge(s)")
            ok = False

    return ok


# ═══════════════════════════════════════════════════════════════════════════
# Demos reproducing paper results
# ═══════════════════════════════════════════════════════════════════════════

def demo_topology():
    """Print BCube_1 (n=4) switch connectivity — corresponds to Fig. 1(b)."""
    bc = BCube(n=4, k=1)
    print("BCube_1  n=4  (Fig. 1b)")
    print(f"  {bc.N} servers, {2*bc.n} switches, diameter={bc.k+1}")
    print()
    print("  Level-0 switches  (each column shares a level-0 switch):")
    for sw in range(bc.n):
        members = [bc.addr_str([sw, j]) for j in range(bc.n)]
        print(f"    <0,{sw}>  ↔  " + "  ".join(members))
    print()
    print("  Level-1 switches  (each row shares a level-1 switch):")
    for sw in range(bc.n):
        members = [bc.addr_str([j, sw]) for j in range(bc.n)]
        print(f"    <1,{sw}>  ↔  " + "  ".join(members))


def demo_parallel_paths():
    """
    Reproduce Fig. 4: 4 parallel paths between 0001 and 1011
    in BCube_3 with n=8.
    """
    bc = BCube(n=8, k=3)
    # Paper addresses: a3 a2 a1 a0
    # 0001 → a0=1, a1=0, a2=0, a3=0  → digits [1,0,0,0]
    # 1011 → a0=1, a1=1, a2=0, a3=1  → digits [1,1,0,1]
    A = [1, 0, 0, 0]
    B = [1, 1, 0, 1]

    print(f"BCube_3  n=8 — paths from {bc.addr_str(A)} to {bc.addr_str(B)}")
    print(f"  h(A,B) = {bc.hamming(A,B)}  →  2 paths of len 2, 2 paths of len 4")
    print()
    paths = bc.build_path_set(A, B)
    for i, p in enumerate(paths):
        hops  = len(p) - 1
        nodes = "  →  ".join(bc.addr_str(v) for v in p)
        print(f"  P{bc.k - i}  (len={hops}):  {nodes}")


def demo_one_to_all():
    """
    Theorem 5: BCube_1 n=4 spanning trees from server 00.
    Matches Fig. 6 of the paper.
    """
    bc  = BCube(n=4, k=1)
    src = bc.digits(0)   # server "00"
    trees = bc.build_multiple_spts(src)

    print(f"BCube_1  n=4 — {bc.k+1} edge-disjoint spanning trees from {bc.addr_str(src)}")
    print(f"  File of size L delivered to all {bc.N-1} servers in L/{bc.k+1} time")
    print()
    for i, tree in enumerate(trees):
        root_s = bc.addr_str(tree['root'])
        print(f"  Tree {i}  (root={root_s}, {len(tree['nodes'])} servers):")
        for (p, c) in tree['edges']:
            print(f"    {bc.addr_str(p)}  →  {bc.addr_str(c)}")
        print()


def demo_abt():
    """
    Reproduce the no-failure ABT values from Section 6 / Fig. 8.
    2048-server container, 1 Gb/s links.
    """
    n   = 8
    N   = 2048    # partial BCube_3 with n=8
    bw  = 1.0     # Gb/s per link

    bcube_abt    = n / (n - 1) * (N - 1) * bw
    fat_tree_abt = N * bw          # ABT ≈ N for fat-tree (paper: 1895)
    dcell_abt    = 298.0           # from paper (DCell1 partial)

    print("Aggregate Bottleneck Throughput — 2048 servers, 1 Gb/s links")
    print(f"  BCube    ABT = {bcube_abt:7.1f} Gb/s   (paper ≈ 2006)")
    print(f"  Fat-tree ABT = {fat_tree_abt:7.1f} Gb/s   (paper ≈ 1895)")
    print(f"  DCell    ABT = {dcell_abt:7.1f} Gb/s   (paper ≈  298)")
    print(f"  BCube / fat-tree = {bcube_abt/fat_tree_abt:.3f}×")
    print(f"  BCube / DCell    = {bcube_abt/dcell_abt:.1f}×")


def demo_graceful_degradation():
    """
    Approximate Fig. 8(a): ABT vs. server failure rate.
    BCube degrades gracefully; fat-tree follows it (under server failures).
    Under switch failures fat-tree drops sharply — not modelled here.
    """
    n, N, bw = 8, 2048, 1.0

    print("\nGraceful degradation — server failure (approximation of Fig. 8a)")
    print(f"  {'Failure':>8}  {'BCube ABT':>12}  {'Fat-tree ABT':>14}")
    print("  " + "-" * 40)
    for pct in range(0, 22, 2):
        N_alive      = int(N * (1 - pct / 100))
        bcube_abt    = n / (n - 1) * (N_alive - 1) * bw
        fat_tree_abt = N_alive * bw
        print(f"  {pct:>6}%    {bcube_abt:>10.0f}    {fat_tree_abt:>12.0f}")


# ═══════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════

def run_all():
    sep = "═" * 62

    print(sep)
    print("  §3.1  Topology — BCube_1  n=4")
    print(sep)
    demo_topology()

    print()
    print(sep)
    print("  §3.3  Parallel paths — BCube_3  n=8  (Fig. 4)")
    print(sep)
    demo_parallel_paths()

    print()
    print(sep)
    print("  §3.5  One-to-all spanning trees — BCube_1  n=4  (Fig. 6)")
    print(sep)
    demo_one_to_all()

    print()
    print(sep)
    print("  §3.6  Aggregate Bottleneck Throughput  (Fig. 8, no failure)")
    print(sep)
    demo_abt()

    print()
    print(sep)
    print("  §6    Graceful degradation  (Fig. 8a approximation)")
    print(sep)
    demo_graceful_degradation()

    print()
    print(sep)
    print("  Verification")
    print(sep)
    configs = [(4, 1), (4, 2), (8, 1), (8, 2)]
    all_pass = True
    for n, k in configs:
        bc        = BCube(n, k)
        ok_paths  = verify_parallel_paths(bc, n_trials=300)
        ok_trees  = verify_spts(bc)
        status    = "✓" if (ok_paths and ok_trees) else "✗"
        all_pass  = all_pass and ok_paths and ok_trees
        print(f"  {status}  BCube_{k}  n={n}  "
              f"N={bc.N:5d}  parallel_paths={ok_paths}  spts={ok_trees}")

    print()
    print("  " + ("All tests passed ✓" if all_pass else "Some tests FAILED ✗"))


if __name__ == "__main__":
    run_all()
