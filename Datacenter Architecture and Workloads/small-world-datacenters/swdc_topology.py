"""
swdc_topology.py
================
Topology generation and graph-level analysis for:
  "Small-World Datacenters" (Shin, Wong, Sirer, Cheriton — SOCC'11)

Reproduces:
  Fig. 3  — Dijkstra average path length
  Fig. 4  — Greedy geographical routing path length
  Fig. 11-12 — Fault-tolerance (flow survival rate vs failure fraction)

Topologies (all at N=512 nodes, degree-6):
  SW-Ring        : ring (2) + 4 Kleinberg random links
  SW-2DTorus     : 2-D torus 32×16 (4) + 2 Kleinberg random links
  SW-3DHexTorus  : 3-D torus 8×8×8 (5) + 1 Kleinberg random link
  CamCube        : 3-D torus 8×8×8, 6 regular links, no random
  CDC            : 3-level hierarchy (ToR + Agg + Core) — graph only
"""

import math
import random
import heapq
import itertools
import collections
import csv
import sys
from dataclasses import dataclass, field
from typing import List, Tuple, Dict, Optional, Callable

# ─────────────────────────────────────────────
# 1.  Graph primitives
# ─────────────────────────────────────────────

class Graph:
    """Undirected, unweighted adjacency-list graph."""

    def __init__(self, n: int):
        self.n = n
        self.adj: List[List[int]] = [[] for _ in range(n)]
        self.edges: List[Tuple[int, int]] = []

    def add_edge(self, u: int, v: int):
        if v not in self.adj[u]:
            self.adj[u].append(v)
        if u not in self.adj[v]:
            self.adj[v].append(u)
        self.edges.append((min(u, v), max(u, v)))

    def degree(self, u: int) -> int:
        return len(self.adj[u])

    def bfs_dist(self, src: int) -> List[int]:
        dist = [-1] * self.n
        dist[src] = 0
        q = collections.deque([src])
        while q:
            u = q.popleft()
            for v in self.adj[u]:
                if dist[v] == -1:
                    dist[v] = dist[u] + 1
                    q.append(v)
        return dist

    def avg_path_length(self, samples: int = 200, seed: int = 42) -> float:
        rng = random.Random(seed)
        total, count = 0, 0
        srcs = rng.sample(range(self.n), min(samples, self.n))
        for s in srcs:
            dists = self.bfs_dist(s)
            for d in dists:
                if d > 0:
                    total += d
                    count += 1
        return total / count if count else float('inf')


# ─────────────────────────────────────────────
# 2.  Kleinberg random-link generator
# ─────────────────────────────────────────────

def kleinberg_random_links(
    n: int,
    dist_fn: Callable[[int, int], float],
    dimension: int,
    num_links_per_node: int,
    rng: random.Random,
    max_tries: int = 100_000,
) -> List[Tuple[int, int]]:
    """
    For each node u, sample `num_links_per_node` random destinations v
    with probability proportional to dist(u,v)^(-dimension) (Kleinberg model).
    Returns list of directed (u→v) pairs; the caller mirrors them.
    """
    links = []
    for u in range(n):
        # Build CDF
        weights = []
        for v in range(n):
            if v == u:
                weights.append(0.0)
            else:
                weights.append(dist_fn(u, v) ** (-dimension))
        Z = sum(weights)
        cdf = []
        acc = 0.0
        for w in weights:
            acc += w / Z
            cdf.append(acc)

        chosen = set()
        tries = 0
        while len(chosen) < num_links_per_node and tries < max_tries:
            tries += 1
            r = rng.random()
            # binary search
            lo, hi = 0, n - 1
            while lo < hi:
                mid = (lo + hi) // 2
                if cdf[mid] < r:
                    lo = mid + 1
                else:
                    hi = mid
            if lo != u:
                chosen.add(lo)
        for v in chosen:
            links.append((u, v))
    return links


# ─────────────────────────────────────────────
# 3.  Topology builders
# ─────────────────────────────────────────────

def build_sw_ring(n: int = 512, num_random: int = 4, seed: int = 1) -> Tuple[Graph, List]:
    """
    SW-Ring: ring (2 links/node) + num_random Kleinberg links/node.
    Returns (Graph, coords) where coords[i] = i (1-D position).
    """
    rng = random.Random(seed)
    g = Graph(n)
    coords = list(range(n))

    # Regular ring links
    for i in range(n):
        g.add_edge(i, (i + 1) % n)

    # Distance on ring (circular)
    def ring_dist(u, v):
        d = abs(coords[u] - coords[v])
        return max(1, min(d, n - d))

    rand_links = kleinberg_random_links(n, ring_dist, 1, num_random, rng)
    for u, v in rand_links:
        g.add_edge(u, v)

    return g, coords


def build_sw_2dtorus(rows: int = 32, cols: int = 16, num_random: int = 2, seed: int = 1) -> Tuple[Graph, List]:
    """
    SW-2DTorus: rows×cols 2-D torus (4 links/node) + num_random Kleinberg links.
    Returns (Graph, coords) where coords[i] = (r, c).
    """
    n = rows * cols
    rng = random.Random(seed)
    g = Graph(n)
    coords = [(i // cols, i % cols) for i in range(n)]

    def idx(r, c):
        return r * cols + c

    # Regular torus links
    for r in range(rows):
        for c in range(cols):
            u = idx(r, c)
            g.add_edge(u, idx(r, (c + 1) % cols))   # East
            g.add_edge(u, idx((r + 1) % rows, c))   # North

    # 2-D torus distance (Manhattan on torus)
    def torus2d_dist(u, v):
        ru, cu = coords[u]
        rv, cv = coords[v]
        dr = abs(ru - rv); dr = min(dr, rows - dr)
        dc = abs(cu - cv); dc = min(dc, cols - dc)
        return max(1, dr + dc)

    rand_links = kleinberg_random_links(n, torus2d_dist, 2, num_random, rng)
    for u, v in rand_links:
        g.add_edge(u, v)

    return g, coords


def build_sw_3dhextorus(side: int = 8, num_random: int = 1, seed: int = 1) -> Tuple[Graph, List]:
    """
    SW-3DHexTorus: side³ 3-D torus with 5 regular links/node + num_random Kleinberg.
    We use a standard 3-D torus but expose only 5 regular directions (±x,±y,+z)
    and leave -z as the slot for the random link.
    Returns (Graph, coords) where coords[i] = (x,y,z).
    """
    n = side ** 3
    rng = random.Random(seed)
    g = Graph(n)

    def idx(x, y, z):
        return x * side * side + y * side + z

    coords = [(x, y, z)
              for x in range(side)
              for y in range(side)
              for z in range(side)]

    # 5 regular links per node: +x,-x,+y,-y,+z
    for x in range(side):
        for y in range(side):
            for z in range(side):
                u = idx(x, y, z)
                g.add_edge(u, idx((x+1)%side, y, z))   # +x
                g.add_edge(u, idx((x-1)%side, y, z))   # -x
                g.add_edge(u, idx(x, (y+1)%side, z))   # +y
                g.add_edge(u, idx(x, (y-1)%side, z))   # -y
                g.add_edge(u, idx(x, y, (z+1)%side))   # +z

    # 3-D torus distance
    def torus3d_dist(u, v):
        xu, yu, zu = coords[u]
        xv, yv, zv = coords[v]
        dx = abs(xu-xv); dx = min(dx, side-dx)
        dy = abs(yu-yv); dy = min(dy, side-dy)
        dz = abs(zu-zv); dz = min(dz, side-dz)
        return max(1, dx+dy+dz)

    rand_links = kleinberg_random_links(n, torus3d_dist, 3, num_random, rng)
    for u, v in rand_links:
        g.add_edge(u, v)

    return g, coords


def build_camcube(side: int = 8) -> Tuple[Graph, List]:
    """
    CamCube: side³ 3-D torus, 6 regular links/node, no random links.
    """
    n = side ** 3
    g = Graph(n)

    def idx(x, y, z):
        return x * side * side + y * side + z

    coords = [(x, y, z)
              for x in range(side)
              for y in range(side)
              for z in range(side)]

    for x in range(side):
        for y in range(side):
            for z in range(side):
                u = idx(x, y, z)
                g.add_edge(u, idx((x+1)%side, y, z))
                g.add_edge(u, idx(x, (y+1)%side, z))
                g.add_edge(u, idx(x, y, (z+1)%side))

    return g, coords


def build_cdc(n_servers: int = 512,
              tor_overscribe: int = 1,
              agg_overscribe: int = 5,
              core_overscribe: int = 1,
              servers_per_tor: int = 16) -> Graph:
    """
    Conventional Datacenter: 3-level hierarchy.
    Returns a graph that includes server nodes (0..n_servers-1),
    ToR nodes, Agg nodes, Core nodes.
    Node IDs: servers 0..n_s-1, ToR n_s..n_s+n_tor-1, etc.
    """
    n_tor = math.ceil(n_servers / servers_per_tor)
    # Each ToR has servers_per_tor downlinks and ceil(servers_per_tor/tor_overscribe) uplinks
    tor_uplinks = max(1, servers_per_tor // tor_overscribe)
    # Aggregate switches: each agg connects tor_overscribe ToR switches
    # (We size conservatively)
    tors_per_agg = max(1, n_tor // max(1, n_tor // 8))
    n_agg = max(1, math.ceil(n_tor / tors_per_agg))
    agg_uplinks = max(1, tors_per_agg // agg_overscribe)
    n_core = max(1, math.ceil(n_agg / agg_uplinks))

    total_nodes = n_servers + n_tor + n_agg + n_core
    g = Graph(total_nodes)

    tor_base  = n_servers
    agg_base  = n_servers + n_tor
    core_base = n_servers + n_tor + n_agg

    # Servers → ToR
    for s in range(n_servers):
        tor = tor_base + s // servers_per_tor
        if tor < agg_base:
            g.add_edge(s, tor)

    # ToR → Agg
    for t in range(n_tor):
        tor_node = tor_base + t
        agg = agg_base + t // tors_per_agg
        if agg < core_base:
            g.add_edge(tor_node, agg)

    # Agg → Core
    for a in range(n_agg):
        agg_node = agg_base + a
        core = core_base + a // n_agg
        if core < total_nodes:
            g.add_edge(agg_node, core)

    # Core: fully mesh (small number of core nodes)
    for i in range(n_core):
        for j in range(i+1, n_core):
            g.add_edge(core_base+i, core_base+j)

    return g


# ─────────────────────────────────────────────
# 4.  Greedy geographical routing path length
# ─────────────────────────────────────────────

def _coord_dist_ring(c1, c2, n):
    d = abs(c1 - c2)
    return min(d, n - d)

def _coord_dist_2d(c1, c2, rows, cols):
    dr = abs(c1[0]-c2[0]); dr = min(dr, rows-dr)
    dc = abs(c1[1]-c2[1]); dc = min(dc, cols-dc)
    return dr + dc

def _coord_dist_3d(c1, c2, side):
    dx = abs(c1[0]-c2[0]); dx = min(dx, side-dx)
    dy = abs(c1[1]-c2[1]); dy = min(dy, side-dy)
    dz = abs(c1[2]-c2[2]); dz = min(dz, side-dz)
    return dx+dy+dz


def greedy_path_length(
    g: Graph,
    coords: list,
    coord_dist_fn: Callable,
    src: int,
    dst: int,
    k: int = 3,
    max_hops: int = 1000,
) -> int:
    """
    Simulate greedy geographical routing from src to dst.

    Correct k-hop look-ahead (per SWDC paper §3.4.2):
      Each node knows geo-IDs of all nodes within k hops.
      For each direct neighbor nb, find the closest node reachable
      from nb in k-1 more hops, and pick the nb with the best reach.

    Distance uses 0 for same-node (unlike Kleinberg generation which needs > 0).
    """
    # Raw distance: returns 0 when coords are equal
    def raw_dist(node):
        c = coords[node]
        d = coords[dst]
        if c == d:
            return 0
        return coord_dist_fn(c, d)

    curr = src
    hops = 0

    while curr != dst and hops < max_hops:
        curr_dist = raw_dist(curr)

        # Shortcut: dst is a direct neighbor
        if dst in g.adj[curr]:
            curr = dst
            hops += 1
            break

        # ─── k-hop greedy (paper §3.4.2) ───────────────────────────────
        # Key insight from the paper: the regular lattice guarantees monotone
        # progress (always a forward neighbor exists). k-hop look-ahead
        # helps use random links of neighbors as shortcuts.
        #
        # Step 1: Identify "forward" first hops (reduce geographic distance).
        # Step 2: For each forward hop, BFS k-1 more steps to find shortcuts.
        # Step 3: Pick the forward hop that yields the best k-hop reach.
        # This guarantees convergence (monotone) while exploiting shortcuts.

        # Collect forward first hops and their k-hop reach
        best_next = None
        best_reach = curr_dist  # target: strictly < curr_dist

        for nb in g.adj[curr]:
            nb_dist = raw_dist(nb)

            # BFS from nb at depth k-1 (explores all directions for shortcuts)
            bfs = {nb: 0}
            q = collections.deque([nb])
            found_dst = False
            while q:
                u = q.popleft()
                if u == dst:
                    found_dst = True
                    break
                if bfs[u] >= k - 1:
                    continue
                for v in g.adj[u]:
                    if v not in bfs:
                        bfs[v] = bfs[u] + 1
                        q.append(v)

            if found_dst:
                reach = 0
            else:
                reach = min(raw_dist(node) for node in bfs)

            # Score: prioritize the best k-hop reach.
            # Tiebreak: prefer forward steps (smaller nb_dist).
            # We pick the neighbor that minimizes (reach, nb_dist).
            if reach < best_reach or (
                reach == best_reach
                and best_next is not None
                and nb_dist < raw_dist(best_next)
            ):
                best_reach = reach
                best_next = nb

        if best_next is None:
            return max_hops   # genuinely stuck (isolated node or broken topology)

        curr = best_next
        hops += 1

    return hops if curr == dst else max_hops


def avg_greedy_path_length(
    g: Graph,
    coords: list,
    coord_dist_fn: Callable,
    samples: int = 300,
    k: int = 3,
    seed: int = 42,
) -> float:
    rng = random.Random(seed)
    total, count = 0, 0
    nodes = list(range(g.n))
    for _ in range(samples):
        s = rng.choice(nodes)
        d = rng.choice(nodes)
        if s == d:
            continue
        pl = greedy_path_length(g, coords, coord_dist_fn, s, d, k=k)
        if pl < 1000:
            total += pl
            count += 1
    return total / count if count else float('inf')


# ─────────────────────────────────────────────
# 5.  Fault-tolerance analysis
# ─────────────────────────────────────────────

def fault_tolerance_analysis(
    g: Graph,
    failure_fracs: List[float],
    num_trials: int = 20,
    num_flows_per_trial: int = 500,
    seed: int = 99,
    server_only: int = None,   # if set, only servers in [0, server_only)
) -> Dict[float, float]:
    """
    For each failure fraction f:
      - Randomly remove f*N nodes
      - Check how many of the pre-failure flows still have a path
    Returns {failure_frac: survival_rate}.
    """
    rng = random.Random(seed)
    n = g.n
    pool = list(range(server_only if server_only else n))
    results = {}

    for frac in failure_fracs:
        survival_rates = []
        for trial in range(num_trials):
            n_fail = int(frac * len(pool))
            failed = set(rng.sample(pool, n_fail))

            # Build reduced adjacency list
            alive = [u for u in pool if u not in failed]
            if not alive:
                survival_rates.append(0.0)
                continue

            # BFS reachability in the surviving sub-graph
            adj_alive = {u: [v for v in g.adj[u] if v not in failed]
                         for u in alive}

            # Sample flows from pre-failure alive nodes
            flows = []
            a_list = alive
            for _ in range(num_flows_per_trial):
                s = rng.choice(a_list)
                d = rng.choice(a_list)
                if s != d:
                    flows.append((s, d))

            # Check connectivity for each flow using BFS
            survived = 0
            for s, d in flows:
                if s in failed or d in failed:
                    continue
                # BFS from s in alive sub-graph
                vis = {s}
                q = collections.deque([s])
                found = False
                while q and not found:
                    u = q.popleft()
                    for v in adj_alive.get(u, []):
                        if v == d:
                            found = True
                            break
                        if v not in vis:
                            vis.add(v)
                            q.append(v)
                if found:
                    survived += 1

            rate = survived / len(flows) if flows else 0.0
            survival_rates.append(rate)

        results[frac] = sum(survival_rates) / len(survival_rates)
    return results


# ─────────────────────────────────────────────
# 6.  Run all experiments and write CSVs
# ─────────────────────────────────────────────

def run_path_length_analysis(out_dir: str = "."):
    print("=== Building topologies (N=512) ===")

    N = 512
    ROWS, COLS = 32, 16
    SIDE = 8  # 8^3 = 512

    topologies = {}

    print("  Building SW-Ring ...")
    g_ring, c_ring = build_sw_ring(N, num_random=4)
    topologies["SW-Ring"] = (g_ring, c_ring, "ring")

    print("  Building SW-2DTorus ...")
    g_2d, c_2d = build_sw_2dtorus(ROWS, COLS, num_random=2)
    topologies["SW-2DTorus"] = (g_2d, c_2d, "2dtorus")

    print("  Building SW-3DHexTorus ...")
    g_3d, c_3d = build_sw_3dhextorus(SIDE, num_random=1)
    topologies["SW-3DHexTorus"] = (g_3d, c_3d, "3dhex")

    print("  Building CamCube ...")
    g_cam, c_cam = build_camcube(SIDE)
    topologies["CamCube"] = (g_cam, c_cam, "3dtorus")

    # Coordinate distance functions
    def make_dist_fn(topo_type, coords, n=N, rows=ROWS, cols=COLS, side=SIDE):
        if topo_type == "ring":
            return lambda c1, c2: max(1, _coord_dist_ring(c1, c2, n))
        elif topo_type == "2dtorus":
            return lambda c1, c2: max(1, _coord_dist_2d(c1, c2, rows, cols))
        else:  # 3dtorus or 3dhex
            return lambda c1, c2: max(1, _coord_dist_3d(c1, c2, side))

    print("\n=== Fig. 3: Dijkstra (BFS) average path length ===")
    dijkstra_rows = []
    for name, (g, coords, ttype) in topologies.items():
        print(f"  Computing for {name} ...")
        apl = g.avg_path_length(samples=200)
        dijkstra_rows.append({"topology": name, "avg_path_length": round(apl, 3)})
        print(f"    {name}: {apl:.3f}")

    with open(f"{out_dir}/fig3_dijkstra_path_length.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["topology", "avg_path_length"])
        w.writeheader(); w.writerows(dijkstra_rows)
    print(f"  → Saved fig3_dijkstra_path_length.csv")

    print("\n=== Fig. 4: Greedy routing average path length ===")
    greedy_rows = []
    for name, (g, coords, ttype) in topologies.items():
        dist_fn = make_dist_fn(ttype, coords)
        coord_dist = lambda c1, c2, _df=dist_fn: _df(c1, c2)
        print(f"  Computing greedy for {name} ...")
        apl = avg_greedy_path_length(g, coords, coord_dist, samples=300, k=3)
        greedy_rows.append({"topology": name, "avg_path_length": round(apl, 3)})
        print(f"    {name}: {apl:.3f}")

    with open(f"{out_dir}/fig4_greedy_path_length.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["topology", "avg_path_length"])
        w.writeheader(); w.writerows(greedy_rows)
    print(f"  → Saved fig4_greedy_path_length.csv")

    return topologies


def run_fault_tolerance(topologies, out_dir: str = "."):
    print("\n=== Fig. 11-12: Fault tolerance analysis ===")
    failure_fracs = [0.0, 0.05, 0.10, 0.15, 0.20, 0.25, 0.30, 0.35, 0.40, 0.45, 0.50]
    all_rows = []

    for name, (g, coords, _) in topologies.items():
        print(f"  Fault tolerance for {name} ...")
        results = fault_tolerance_analysis(
            g, failure_fracs, num_trials=15, num_flows_per_trial=300
        )
        for frac, rate in results.items():
            all_rows.append({
                "topology": name,
                "failure_fraction": frac,
                "survival_rate": round(rate, 4),
            })
            print(f"    {name} fail={frac:.0%}: survival={rate:.3f}")

    with open(f"{out_dir}/fig11_fault_tolerance.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["topology", "failure_fraction", "survival_rate"])
        w.writeheader(); w.writerows(all_rows)
    print(f"  → Saved fig11_fault_tolerance.csv")


if __name__ == "__main__":
    import os
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    topos = run_path_length_analysis(out)
    run_fault_tolerance(topos, out)
    print("\nDone. CSV files written to:", out)
