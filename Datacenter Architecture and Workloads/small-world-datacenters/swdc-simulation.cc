/* ============================================================
 * swdc-simulation.cc
 * NS-3 reproduction of "Small-World Datacenters"
 * Shin, Wong, Sirer, Cheriton — SOCC'11
 *
 * Reproduces:
 *   Fig. 5  — Packet delivery latency
 *   Fig. 6  — Maximum aggregate bandwidth
 *   (Path-length & fault-tolerance are handled by swdc_topology.py)
 *
 * Build:
 *   Copy to <ns3-root>/scratch/swdc-simulation.cc
 *   ./ns3 build
 *   ./ns3 run "swdc-simulation --topo=SW-3DHexTorus --traffic=uniform \
 *               --pktSize=64 --nNodes=512 --simTime=0.5"
 *
 * Topology choices: SW-Ring | SW-2DTorus | SW-3DHexTorus | CamCube | CDC
 * Traffic choices : uniform | local | mapreduce
 * ============================================================ */

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/ipv4-routing-table-entry.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SwdcSimulation");

// ────────────────────────────────────────────────────────────
// 1.  Graph helpers (topology generation, BFS, routing tables)
// ────────────────────────────────────────────────────────────

struct Edge {
    int u, v;
};

struct AdjGraph {
    int n;
    std::vector<std::vector<int>> adj;
    std::vector<Edge> edges;

    AdjGraph() : n(0) {}
    explicit AdjGraph(int n) : n(n), adj(n) {}

    void addEdge(int u, int v) {
        // avoid duplicates
        for (int x : adj[u]) if (x == v) return;
        adj[u].push_back(v);
        adj[v].push_back(u);
        edges.push_back({std::min(u,v), std::max(u,v)});
    }

    // BFS shortest path distances from src
    std::vector<int> bfsDist(int src) const {
        std::vector<int> dist(n, -1);
        std::queue<int> q;
        dist[src] = 0;
        q.push(src);
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int v : adj[u]) {
                if (dist[v] == -1) {
                    dist[v] = dist[u] + 1;
                    q.push(v);
                }
            }
        }
        return dist;
    }

    // BFS parent table (for route reconstruction)
    std::vector<int> bfsParent(int src) const {
        return bfsParentExcluding(src, {});
    }

    // BFS parent table excluding failed nodes (for fault-tolerance routing)
    std::vector<int> bfsParentExcluding(int src, const std::set<int>& failed) const {
        std::vector<int> parent(n, -1);
        std::vector<bool> vis(n, false);
        std::queue<int> q;
        vis[src] = true;
        q.push(src);
        while (!q.empty()) {
            int u = q.front(); q.pop();
            for (int v : adj[u]) {
                if (!vis[v] && !failed.count(v)) {
                    vis[v] = true;
                    parent[v] = u;
                    q.push(v);
                }
            }
        }
        return parent;
    }

    // Returns next_hop[src][dst] = direct neighbor of src on shortest path to dst
    // (only for src's row — call for each src as needed)
    int nextHopTo(int src, int dst) const {
        if (src == dst) return src;
        auto parent = bfsParent(src);
        int cur = dst;
        while (parent[cur] != src) {
            if (parent[cur] == -1) return -1; // unreachable
            cur = parent[cur];
        }
        return cur;
    }
};

// ────────────────────────────────────────────────────────────
// 2.  Kleinberg random link generation
// ────────────────────────────────────────────────────────────

std::vector<std::pair<int,int>> kleinbergLinks(
    int n,
    std::function<double(int,int)> distFn,
    int dimension,
    int numPerNode,
    std::mt19937& rng)
{
    std::vector<std::pair<int,int>> links;
    std::uniform_real_distribution<double> ud(0.0, 1.0);

    for (int u = 0; u < n; u++) {
        // Build weight array
        std::vector<double> w(n, 0.0);
        double Z = 0.0;
        for (int v = 0; v < n; v++) {
            if (v == u) continue;
            double d = distFn(u, v);
            w[v] = std::pow(d, -(double)dimension);
            Z += w[v];
        }
        // Normalize -> CDF
        std::vector<double> cdf(n, 0.0);
        double acc = 0.0;
        for (int v = 0; v < n; v++) {
            acc += w[v] / Z;
            cdf[v] = acc;
        }

        std::set<int> chosen;
        int tries = 0;
        while ((int)chosen.size() < numPerNode && tries < 200000) {
            tries++;
            double r = ud(rng);
            int lo = 0, hi = n - 1;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (cdf[mid] < r) lo = mid + 1;
                else              hi = mid;
            }
            if (lo != u) chosen.insert(lo);
        }
        for (int v : chosen) links.push_back({u, v});
    }
    return links;
}

// ────────────────────────────────────────────────────────────
// 3.  Topology builders
// ────────────────────────────────────────────────────────────

// Coordinate variant (polymorphic via std::vector<std::vector<int>>)
using Coords = std::vector<std::vector<int>>;

AdjGraph buildSwRing(int n, int numRandom, std::mt19937& rng, Coords& coords) {
    AdjGraph g(n);
    coords.resize(n);
    for (int i = 0; i < n; i++) coords[i] = {i};

    for (int i = 0; i < n; i++) g.addEdge(i, (i+1)%n);

    auto distFn = [&](int u, int v) -> double {
        int d = std::abs(u - v);
        return std::max(1, std::min(d, n - d));
    };
    auto rlinks = kleinbergLinks(n, distFn, 1, numRandom, rng);
    for (auto [u,v] : rlinks) g.addEdge(u, v);
    return g;
}

AdjGraph buildSw2DTorus(int rows, int cols, int numRandom, std::mt19937& rng, Coords& coords) {
    int n = rows * cols;
    AdjGraph g(n);
    coords.resize(n);
    auto idx = [&](int r, int c){ return r * cols + c; };
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            coords[idx(r,c)] = {r, c};

    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            g.addEdge(idx(r,c), idx(r, (c+1)%cols));
            g.addEdge(idx(r,c), idx((r+1)%rows, c));
        }

    auto distFn = [&](int u, int v) -> double {
        int ru=u/cols, cu=u%cols, rv=v/cols, cv=v%cols;
        int dr=std::abs(ru-rv); dr=std::min(dr,rows-dr);
        int dc=std::abs(cu-cv); dc=std::min(dc,cols-dc);
        return std::max(1, dr+dc);
    };
    auto rlinks = kleinbergLinks(n, distFn, 2, numRandom, rng);
    for (auto [u,v] : rlinks) g.addEdge(u, v);
    return g;
}

AdjGraph buildSw3DHexTorus(int side, int numRandom, std::mt19937& rng, Coords& coords) {
    int n = side*side*side;
    AdjGraph g(n);
    coords.resize(n);
    auto idx = [&](int x,int y,int z){ return x*side*side + y*side + z; };
    for (int x=0;x<side;x++) for (int y=0;y<side;y++) for (int z=0;z<side;z++)
        coords[idx(x,y,z)] = {x,y,z};

    // 5 regular links: ±x, ±y, +z
    for (int x=0;x<side;x++) for (int y=0;y<side;y++) for (int z=0;z<side;z++) {
        int u = idx(x,y,z);
        g.addEdge(u, idx((x+1)%side, y, z));
        g.addEdge(u, idx((x-1+side)%side, y, z));
        g.addEdge(u, idx(x, (y+1)%side, z));
        g.addEdge(u, idx(x, (y-1+side)%side, z));
        g.addEdge(u, idx(x, y, (z+1)%side));
    }

    auto distFn = [&](int u, int v) -> double {
        int xu=coords[u][0],yu=coords[u][1],zu=coords[u][2];
        int xv=coords[v][0],yv=coords[v][1],zv=coords[v][2];
        int dx=std::abs(xu-xv); dx=std::min(dx,side-dx);
        int dy=std::abs(yu-yv); dy=std::min(dy,side-dy);
        int dz=std::abs(zu-zv); dz=std::min(dz,side-dz);
        return std::max(1, dx+dy+dz);
    };
    auto rlinks = kleinbergLinks(n, distFn, 3, numRandom, rng);
    for (auto [u,v] : rlinks) g.addEdge(u, v);
    return g;
}

AdjGraph buildCamCube(int side, Coords& coords) {
    int n = side*side*side;
    AdjGraph g(n);
    coords.resize(n);
    auto idx = [&](int x,int y,int z){ return x*side*side + y*side + z; };
    for (int x=0;x<side;x++) for (int y=0;y<side;y++) for (int z=0;z<side;z++)
        coords[idx(x,y,z)] = {x,y,z};
    for (int x=0;x<side;x++) for (int y=0;y<side;y++) for (int z=0;z<side;z++) {
        int u = idx(x,y,z);
        g.addEdge(u, idx((x+1)%side, y, z));
        g.addEdge(u, idx(x, (y+1)%side, z));
        g.addEdge(u, idx(x, y, (z+1)%side));
    }
    return g;
}

// CDC: 3-level tree (no random links)
// Returns number of server nodes and total nodes
AdjGraph buildCDC(int nServers, int serversPerTor,
                  int torOversub, int aggOversub,
                  Coords& coords) {
    int nTor = (nServers + serversPerTor - 1) / serversPerTor;
    int torsPerAgg = std::max(1, nTor / 8);
    int nAgg = std::max(1, (nTor + torsPerAgg - 1) / torsPerAgg);
    int nCore = std::max(2, (nAgg + 3) / 4);
    int total = nServers + nTor + nAgg + nCore;

    AdjGraph g(total);
    coords.resize(total);
    // Assign dummy coords
    for (int i = 0; i < total; i++) coords[i] = {i};

    int torBase  = nServers;
    int aggBase  = nServers + nTor;
    int coreBase = nServers + nTor + nAgg;

    for (int s = 0; s < nServers; s++)
        g.addEdge(s, torBase + s / serversPerTor);
    for (int t = 0; t < nTor; t++)
        g.addEdge(torBase + t, aggBase + t / torsPerAgg);
    for (int a = 0; a < nAgg; a++)
        g.addEdge(aggBase + a, coreBase + a / (nAgg > 1 ? (nAgg+nCore-1)/nCore : 1));
    for (int i = 0; i < nCore; i++)
        for (int j = i+1; j < nCore; j++)
            g.addEdge(coreBase+i, coreBase+j);
    return g;
}

// ────────────────────────────────────────────────────────────
// 4.  NS-3 node/link creation + routing
// ────────────────────────────────────────────────────────────

struct SimContext {
    NodeContainer nodes;
    int nServers;           // servers = nodes[0..nServers-1]
    AdjGraph graph;
    Coords coords;

    // link_map[{min,max}] = NetDevicePair
    std::map<std::pair<int,int>, std::pair<Ptr<NetDevice>, Ptr<NetDevice>>> linkDevices;
    // node_addr[node][neighbor] = IPv4Address of the interface toward neighbor
    std::map<int, std::map<int,Ipv4Address>> ifaceAddr;
};

// Assign IPs to a P2P link and record them
static uint32_t s_linkIdx = 0;

void installLink(SimContext& ctx,
                 int u, int v,
                 PointToPointHelper& p2p) {
    NodeContainer pair;
    pair.Add(ctx.nodes.Get(u));
    pair.Add(ctx.nodes.Get(v));
    NetDeviceContainer devs = p2p.Install(pair);

    Ipv4AddressHelper addr;
    uint32_t idx = s_linkIdx++;
    // Use 10.link_hi.link_lo.0/30
    std::ostringstream ss;
    ss << "10." << ((idx >> 8) & 0xFF) << "." << (idx & 0xFF) << ".0";
    addr.SetBase(ss.str().c_str(), "255.255.255.252");
    Ipv4InterfaceContainer ifaces = addr.Assign(devs);

    ctx.ifaceAddr[u][v] = ifaces.GetAddress(0);
    ctx.ifaceAddr[v][u] = ifaces.GetAddress(1);

    auto key = std::make_pair(std::min(u,v), std::max(u,v));
    ctx.linkDevices[key] = {devs.Get(0), devs.Get(1)};
}

// Build full static routing tables (Dijkstra / BFS)
void buildStaticRouting(SimContext& ctx,
                        const std::set<int>& failedNodes = {}) {
    int n = ctx.graph.n;
    Ipv4StaticRoutingHelper staticHelper;

    // For each source node, BFS and install routes
    for (int src = 0; src < n; src++) {
        if (failedNodes.count(src)) continue;  // skip failed nodes
        auto parent = ctx.graph.bfsParentExcluding(src, failedNodes);
        Ptr<Ipv4StaticRouting> sr =
            staticHelper.GetStaticRouting(ctx.nodes.Get(src)->GetObject<Ipv4>());

        for (int dst = 0; dst < n; dst++) {
            if (dst == src) continue;
            if (failedNodes.count(dst)) continue;  // skip routes to failed nodes
            // Find first hop
            int cur = dst;
            while (parent[cur] != src) {
                if (parent[cur] == -1) goto no_route;
                cur = parent[cur];
            }
            {
                // cur is the direct neighbor of src toward dst
                Ipv4Address nhAddr = ctx.ifaceAddr[cur][src];  // cur's IP = gateway
                // Find interface index toward cur (src's own address on that link)
                Ptr<Ipv4> ipv4 = ctx.nodes.Get(src)->GetObject<Ipv4>();
                uint32_t ifIdx = 0;
                for (uint32_t i = 1; i < (uint32_t)ipv4->GetNInterfaces(); i++) {
                    for (uint32_t j = 0; j < ipv4->GetNAddresses(i); j++) {
                        if (ipv4->GetAddress(i, j).GetLocal() == ctx.ifaceAddr[src][cur]) {
                            ifIdx = i;
                        }
                    }
                }
                // Route to dst's loopback / any of its IPs
                // We route to every address that dst has
                Ptr<Ipv4> dstIpv4 = ctx.nodes.Get(dst)->GetObject<Ipv4>();
                for (uint32_t di = 1; di < (uint32_t)dstIpv4->GetNInterfaces(); di++) {
                    for (uint32_t dj = 0; dj < dstIpv4->GetNAddresses(di); dj++) {
                        Ipv4Address dstAddr = dstIpv4->GetAddress(di, dj).GetLocal();
                        sr->AddHostRouteTo(dstAddr, nhAddr, ifIdx);
                    }
                }
            }
            no_route:;
        }
    }
}

// ────────────────────────────────────────────────────────────
// 5.  Traffic generators
// ────────────────────────────────────────────────────────────

// Return a representative IP of a server node
Ipv4Address serverAddr(SimContext& ctx, int node) {
    Ptr<Ipv4> ipv4 = ctx.nodes.Get(node)->GetObject<Ipv4>();
    for (uint32_t i = 1; i < (uint32_t)ipv4->GetNInterfaces(); i++) {
        if (ipv4->GetNAddresses(i) > 0)
            return ipv4->GetAddress(i, 0).GetLocal();
    }
    return Ipv4Address("0.0.0.0");
}

ApplicationContainer installUdpSender(
    Ptr<Node> srcNode,
    Ipv4Address dstAddr,
    uint16_t port,
    uint32_t pktSize,
    uint32_t nPkts,
    double startTime,
    double stopTime,
    double intervalSec = 0.0)   // 0 = burst (as fast as possible)
{
    UdpClientHelper client(dstAddr, port);
    client.SetAttribute("MaxPackets", UintegerValue(nPkts));
    client.SetAttribute("PacketSize", UintegerValue(pktSize));
    double interval = (intervalSec > 0) ? intervalSec : 1e-9;
    client.SetAttribute("Interval", TimeValue(Seconds(interval)));
    ApplicationContainer apps = client.Install(srcNode);
    apps.Start(Seconds(startTime));
    apps.Stop(Seconds(stopTime));
    return apps;
}

ApplicationContainer installUdpSink(
    Ptr<Node> dstNode,
    uint16_t port,
    double startTime,
    double stopTime)
{
    UdpServerHelper server(port);
    ApplicationContainer apps = server.Install(dstNode);
    apps.Start(Seconds(startTime));
    apps.Stop(Seconds(stopTime));
    return apps;
}

void setupUniformRandomTraffic(
    SimContext& ctx,
    uint32_t pktSize,
    uint32_t nPkts,
    double startTime,
    double stopTime,
    std::mt19937& rng,
    const std::set<int>& failedNodes = {})
{
    int n = ctx.nServers;
    // Only active nodes
    std::vector<int> active;
    for (int i = 0; i < n; i++) if (!failedNodes.count(i)) active.push_back(i);
    if (active.size() < 2) return;

    std::uniform_int_distribution<int> ud(0, (int)active.size()-1);
    uint16_t basePort = 9000;

    for (int si = 0; si < (int)active.size(); si++) {
        int src = active[si];
        int di = ud(rng);
        while (active[di] == src) di = ud(rng);
        int dst = active[di];
        uint16_t port = basePort + si;
        installUdpSink(ctx.nodes.Get(dst), port, startTime - 0.1, stopTime + 0.1);
        Ipv4Address dstAddr = serverAddr(ctx, dst);
        installUdpSender(ctx.nodes.Get(src), dstAddr, port, pktSize, nPkts,
                         startTime, stopTime);
    }
}

void setupLocalRandomTraffic(
    SimContext& ctx,
    uint32_t pktSize,
    uint32_t nPkts,
    double startTime,
    double stopTime,
    std::mt19937& rng,
    const std::set<int>& failedNodes = {},
    int clusterSize = 64)
{
    int n = ctx.nServers;
    std::vector<int> active;
    for (int i = 0; i < n; i++) if (!failedNodes.count(i)) active.push_back(i);
    if (active.size() < 2) return;

    uint16_t basePort = 10000;

    for (int si = 0; si < (int)active.size(); si++) {
        int src = active[si];
        // Pick a dst in the "local cluster" among active nodes
        int lo = std::max(0, si - clusterSize/2);
        int hi = std::min((int)active.size()-1, si + clusterSize/2);
        std::uniform_int_distribution<int> ud(lo, hi);
        int di = ud(rng);
        while (active[di] == src) di = ud(rng);
        int dst = active[di];
        uint16_t port = basePort + si;
        installUdpSink(ctx.nodes.Get(dst), port, startTime - 0.1, stopTime + 0.1);
        Ipv4Address dstAddr = serverAddr(ctx, dst);
        installUdpSender(ctx.nodes.Get(src), dstAddr, port, pktSize, nPkts,
                         startTime, stopTime);
    }
}

void setupMapReduceTraffic(
    SimContext& ctx,
    uint32_t pktSize,
    uint32_t nPkts,
    double startTime,
    double stopTime,
    std::mt19937& rng,
    const std::set<int>& failedNodes = {},
    int rackSize = 16)
{
    int n = ctx.nServers;
    // Only use active nodes
    std::vector<int> active;
    for (int i = 0; i < n; i++) if (!failedNodes.count(i)) active.push_back(i);
    if (active.size() < 2) return;

    int na = (int)active.size();
    int nRacks = (na + rackSize - 1) / rackSize;
    uint16_t basePort = 11000;
    int portCtr = 0;

    // Phase 1: within-rack row shuffle
    for (int rack = 0; rack < nRacks; rack++) {
        int lo = rack * rackSize;
        int hi = std::min(na, lo + rackSize);
        for (int si = lo; si < hi; si++) {
            int src = active[si];
            std::uniform_int_distribution<int> ud(lo, hi-1);
            int di = ud(rng);
            while (di == si) di = ud(rng);
            int dst = active[di];
            uint16_t port = basePort + portCtr++;
            installUdpSink(ctx.nodes.Get(dst), port, startTime - 0.1, stopTime + 0.1);
            installUdpSender(ctx.nodes.Get(src), serverAddr(ctx, dst), port,
                             pktSize, nPkts/3, startTime, stopTime * 0.33);
        }
    }

    // Phase 2: cross-rack (same column) shuffle
    for (int col = 0; col < rackSize; col++) {
        for (int rack = 0; rack < nRacks; rack++) {
            int si = rack * rackSize + col;
            if (si >= na) continue;
            int dstRack = (rack + 1) % nRacks;
            int di = dstRack * rackSize + col;
            if (di >= na) continue;
            int src = active[si], dst = active[di];
            uint16_t port = basePort + portCtr++;
            installUdpSink(ctx.nodes.Get(dst), port, startTime - 0.1, stopTime + 0.1);
            installUdpSender(ctx.nodes.Get(src), serverAddr(ctx, dst), port,
                             pktSize, nPkts/3, startTime * 0.33, stopTime * 0.66);
        }
    }

    // Phase 3: 50% local cluster, 50% remote
    std::uniform_int_distribution<int> ud(0, na-1);
    for (int si = 0; si < na; si++) {
        int src = active[si];
        int di;
        bool local = (rng() % 2 == 0);
        if (local) {
            int lo = std::max(0, si - rackSize * 2);
            int hi = std::min(na-1, si + rackSize * 2);
            std::uniform_int_distribution<int> lud(lo, hi);
            di = lud(rng);
        } else {
            di = ud(rng);
        }
        while (di == si) di = ud(rng);
        int dst = active[di];
        uint16_t port = basePort + portCtr++;
        installUdpSink(ctx.nodes.Get(dst), port, startTime - 0.1, stopTime + 0.1);
        installUdpSender(ctx.nodes.Get(src), serverAddr(ctx, dst), port,
                         pktSize, nPkts/3, stopTime * 0.66, stopTime);
    }
}

// ────────────────────────────────────────────────────────────
// 6.  Main
// ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // ── Command-line parameters ──────────────────────────────
    std::string topoStr    = "SW-3DHexTorus";
    std::string trafficStr = "uniform";
    uint32_t    pktSize    = 1024;       // bytes (64 or 1024)
    uint32_t    nNodes     = 512;        // server count (must be 8³=512, or 32×16=512)
    double      simTime    = 0.5;        // seconds
    uint32_t    nPkts      = 500;        // packets per source (bandwidth test)
    uint32_t    seed       = 42;
    bool        faultTest  = false;      // run fault-tolerance measurement
    double      faultFrac  = 0.0;        // fraction of nodes to fail
    std::string outFile    = "swdc_results.csv";
    int         torOversub = 1;          // CDC oversubscription
    int         aggOversub = 5;

    CommandLine cmd(__FILE__);
    cmd.AddValue("topo",       "Topology: SW-Ring|SW-2DTorus|SW-3DHexTorus|CamCube|CDC",   topoStr);
    cmd.AddValue("traffic",    "Traffic: uniform|local|mapreduce",                          trafficStr);
    cmd.AddValue("pktSize",    "Packet size in bytes",                                      pktSize);
    cmd.AddValue("nNodes",     "Number of server nodes",                                    nNodes);
    cmd.AddValue("simTime",    "Simulation duration (s)",                                   simTime);
    cmd.AddValue("nPkts",      "Packets per source (burst bandwidth test)",                 nPkts);
    cmd.AddValue("seed",       "Random seed",                                               seed);
    cmd.AddValue("faultTest",  "Enable fault tolerance test",                               faultTest);
    cmd.AddValue("faultFrac",  "Fraction of nodes to fail (0..0.5)",                        faultFrac);
    cmd.AddValue("outFile",    "CSV output file",                                           outFile);
    cmd.AddValue("torOversub", "CDC top-of-rack oversubscription ratio",                    torOversub);
    cmd.AddValue("aggOversub", "CDC aggregation oversubscription ratio",                    aggOversub);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(seed);
    std::mt19937 rng(seed);

    LogComponentEnable("SwdcSimulation", LOG_LEVEL_INFO);
    NS_LOG_INFO("Topology=" << topoStr << " Traffic=" << trafficStr
                << " PktSize=" << pktSize << " N=" << nNodes);

    // ── Build topology graph ──────────────────────────────────
    SimContext ctx;
    ctx.nServers = (int)nNodes;
    Coords coords;

    s_linkIdx = 0;  // reset global link counter

    if (topoStr == "SW-Ring") {
        ctx.graph = buildSwRing(nNodes, 4, rng, coords);
    } else if (topoStr == "SW-2DTorus") {
        // 32×16 = 512
        int rows = 32, cols = 16;
        if (nNodes != (uint32_t)(rows * cols)) {
            rows = (int)std::round(std::sqrt((double)nNodes));
            cols = nNodes / rows;
        }
        ctx.graph = buildSw2DTorus(rows, cols, 2, rng, coords);
    } else if (topoStr == "SW-3DHexTorus") {
        int side = (int)std::round(std::cbrt((double)nNodes));
        ctx.graph = buildSw3DHexTorus(side, 1, rng, coords);
        ctx.nServers = side * side * side;
    } else if (topoStr == "CamCube") {
        int side = (int)std::round(std::cbrt((double)nNodes));
        ctx.graph = buildCamCube(side, coords);
        ctx.nServers = side * side * side;
    } else if (topoStr == "CDC") {
        ctx.graph = buildCDC(nNodes, 16, torOversub, aggOversub, coords);
        // ctx.nServers already set to nNodes (servers only)
    } else {
        NS_FATAL_ERROR("Unknown topology: " << topoStr);
    }

    int graphN = ctx.graph.n;
    NS_LOG_INFO("Graph: " << graphN << " nodes, " << ctx.graph.edges.size() << " edges");

    // ── Create NS-3 nodes ─────────────────────────────────────
    ctx.nodes.Create(graphN);
    InternetStackHelper internet;
    internet.Install(ctx.nodes);

    // Fault simulation: bring down failed nodes' links early
    std::set<int> failedNodes;
    if (faultTest && faultFrac > 0.0) {
        int nFail = (int)(faultFrac * ctx.nServers);
        std::vector<int> serverIds(ctx.nServers);
        std::iota(serverIds.begin(), serverIds.end(), 0);
        std::shuffle(serverIds.begin(), serverIds.end(), rng);
        for (int i = 0; i < nFail; i++) failedNodes.insert(serverIds[i]);
        NS_LOG_INFO("Failing " << nFail << " nodes (" << faultFrac * 100 << "%)");
    }

    // ── Install P2P links ─────────────────────────────────────
    // Switching delay model from paper (Table 2):
    //   NetFPGA: 64B → 4.3µs, 1KB → 11.7µs
    // Approximate as fixed channel delay + queue processing
    // We model: link delay = 2µs (propagation), DataRate = 1Gbps
    // Per-node processing captured by queue discipline delay
    std::string linkBw    = "1Gbps";
    std::string linkDelay;
    if (pktSize <= 64)
        linkDelay = "2000ns";   // ~4µs round-trip ≈ paper's 4.3µs
    else
        linkDelay = "5000ns";   // ~10µs ≈ paper's 11.7µs

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue(linkBw));
    p2p.SetChannelAttribute("Delay",   StringValue(linkDelay));

    // Use a small queue to model realistic buffering
    p2p.SetQueue("ns3::DropTailQueue",
                 "MaxSize", StringValue("100p"));

    std::set<std::pair<int,int>> installedEdges;
    for (auto& e : ctx.graph.edges) {
        auto key = std::make_pair(e.u, e.v);
        if (installedEdges.count(key)) continue;
        installedEdges.insert(key);

        // Skip links connected to failed nodes if doing fault test
        if (failedNodes.count(e.u) || failedNodes.count(e.v)) continue;

        installLink(ctx, e.u, e.v, p2p);
    }

    // ── Static routing ────────────────────────────────────────
    NS_LOG_INFO("Building routing tables...");
    buildStaticRouting(ctx, failedNodes);

    // ── Traffic ───────────────────────────────────────────────
    double trafficStart = 0.15;
    double trafficStop  = simTime - 0.05;

    // For latency measurement: 300 pkts at 1 ms interval each
    // For bandwidth test: 500 pkts burst (nPkts)
    bool burstMode = (trafficStr != "latency");
    uint32_t actualPkts = burstMode ? nPkts : 300;

    if (trafficStr == "uniform" || trafficStr == "latency") {
        setupUniformRandomTraffic(ctx, pktSize, actualPkts, trafficStart, trafficStop, rng, failedNodes);
    } else if (trafficStr == "local") {
        setupLocalRandomTraffic(ctx, pktSize, actualPkts, trafficStart, trafficStop, rng, failedNodes);
    } else if (trafficStr == "mapreduce") {
        setupMapReduceTraffic(ctx, pktSize, actualPkts, trafficStart, trafficStop, rng, failedNodes);
    } else {
        NS_FATAL_ERROR("Unknown traffic: " << trafficStr);
    }

    // ── Flow Monitor ──────────────────────────────────────────
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor> fm = fmHelper.InstallAll();

    // ── Run ───────────────────────────────────────────────────
    NS_LOG_INFO("Running simulation for " << simTime << "s ...");
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // ── Collect results ───────────────────────────────────────
    fm->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());
    FlowMonitor::FlowStatsContainer stats = fm->GetFlowStats();

    double totalDelay    = 0.0;
    double totalTx       = 0.0;
    double totalRx       = 0.0;
    uint64_t totalRxPkts = 0;
    uint64_t totalTxPkts = 0;
    int      flowCount   = 0;

    for (auto& [id, s] : stats) {
        if (s.txPackets == 0) continue;
        flowCount++;
        totalTxPkts += s.txPackets;
        totalRxPkts += s.rxPackets;
        totalTx     += s.txBytes * 8.0;
        totalRx     += s.rxBytes * 8.0;
        if (s.rxPackets > 0) {
            totalDelay += s.delaySum.GetSeconds();
        }
    }

    double avgDelay_us   = (totalRxPkts > 0) ?
        (totalDelay / totalRxPkts) * 1e6 : 0.0;
    double aggBw_Gbps    = totalRx / simTime / 1e9;
    double deliveryRatio = (totalTxPkts > 0) ?
        (double)totalRxPkts / totalTxPkts : 0.0;

    NS_LOG_INFO("=== Results ===");
    NS_LOG_INFO("Topology:       " << topoStr);
    NS_LOG_INFO("Traffic:        " << trafficStr);
    NS_LOG_INFO("Packet size:    " << pktSize << " B");
    NS_LOG_INFO("Flows:          " << flowCount);
    NS_LOG_INFO("Avg latency:    " << avgDelay_us << " µs");
    NS_LOG_INFO("Agg bandwidth:  " << aggBw_Gbps << " Gbps");
    NS_LOG_INFO("Delivery ratio: " << deliveryRatio * 100 << "%");
    if (faultTest)
        NS_LOG_INFO("Failed nodes:   " << failedNodes.size()
                    << " (" << faultFrac*100 << "%)");

    // ── Write CSV ─────────────────────────────────────────────
    bool writeHeader = false;
    {
        std::ifstream test(outFile);
        writeHeader = !test.good();
    }
    std::ofstream ofs(outFile, std::ios::app);
    if (writeHeader) {
        ofs << "topology,traffic,pkt_size_B,n_nodes,"
            << "avg_latency_us,agg_bw_Gbps,delivery_ratio,"
            << "fault_frac,flows\n";
    }
    ofs << topoStr << "," << trafficStr << "," << pktSize << "," << ctx.nServers
        << "," << avgDelay_us << "," << aggBw_Gbps << "," << deliveryRatio
        << "," << faultFrac << "," << flowCount << "\n";
    ofs.close();

    NS_LOG_INFO("Results appended to " << outFile);

    Simulator::Destroy();
    return 0;
}
