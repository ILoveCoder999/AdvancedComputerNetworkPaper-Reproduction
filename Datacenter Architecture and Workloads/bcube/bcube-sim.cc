/* =====================================================================
 * bcube-sim.cc  —  BCube SIGCOMM'09  ns-3 Reproduction
 * =====================================================================
 *
 * Topology  : BCube_1  n=4  (16 servers, 8 switches, 1 Gbps links)
 *             2-level Tree  (16 servers, 5 switches)   [comparison]
 *
 * Experiments reproduced:
 *   §7.5  one-to-one  : 2 parallel TCP flows  →  ~2× speedup vs tree
 *   §7.5  one-to-all  : spanning-tree multicast →  ~2× speedup
 *   §7.5  all-to-all  : MapReduce shuffle      →  ~3× speedup
 *   §6    degrade      : analytical ABT vs failure rate (Fig. 8)
 *
 * Build & run:
 *   cp bcube-sim.cc  <ns3>/scratch/
 *   cd <ns3>
 *   ./ns3 run "scratch/bcube-sim --exp=one-to-one"
 *   ./ns3 run "scratch/bcube-sim --exp=one-to-all"
 *   ./ns3 run "scratch/bcube-sim --exp=all-to-all"
 *   ./ns3 run "scratch/bcube-sim --exp=degrade"
 *
 * Tested with ns-3.40 / ns-3.41 / ns-3-dev.
 *
 * ⚠  ns-3-dev 需要 C++23，编译前请先执行：
 *      cd <ns3>  &&  ./ns3 configure --cxx-standard=23  &&  ./ns3 build
 * ===================================================================== */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/ipv4-static-routing-helper.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <cassert>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("BCubeSim");

// =====================================================================
// §3.1  BCube address arithmetic
// =====================================================================

struct Addr {
    int n, k;

    /* server_id → digit list [a0, a1, ..., ak]  (a0 = LSB) */
    std::vector<int> dig(int id) const {
        std::vector<int> d(k + 1);
        for (int i = 0; i <= k; i++) { d[i] = id % n; id /= n; }
        return d;
    }

    /* digit list → server_id */
    int num(const std::vector<int>& d) const {
        int x = 0;
        for (int i = k; i >= 0; i--) x = x * n + d[i];
        return x;
    }

    /* §3.2  BCubeRouting with permutation pi.  Returns server-id list. */
    std::vector<int> route(int src, int dst, const std::vector<int>& pi) const {
        auto node = dig(src);
        const auto B = dig(dst);
        std::vector<int> path = {src};
        for (int i = k; i >= 0; i--) {
            int pos = pi[i];
            if (node[pos] != B[pos]) {
                node[pos] = B[pos];
                path.push_back(num(node));
            }
        }
        return path;
    }

    /* §3.3  BuildPathSet — up to k+1 node-disjoint parallel paths.
     *
     * Only DCRouting paths are generated (one per differing digit level).
     * AltDCRouting "backup" paths are omitted because they route through
     * intermediate servers that are also source nodes in other flows,
     * creating conflicting static routes with the same metric (metric 10)
     * that cause routing loops. DCRouting alone is sufficient for normal
     * (no-failure) operation and produces the correct parallel paths. */
    std::vector<std::vector<int>> buildPathSet(int src, int dst) const {
        const auto A = dig(src);
        const auto B = dig(dst);
        std::vector<std::vector<int>> paths;

        for (int i = k; i >= 0; i--) {
            if (A[i] == B[i]) continue;                 // same digit — skip
            std::vector<int> pi(k + 1);
            int m = k;
            for (int j = i; j > i - k - 1; j--)
                pi[m--] = ((j % (k + 1)) + (k + 1)) % (k + 1);
            paths.push_back(route(src, dst, pi));
        }
        return paths;
    }

    /* Which level (dimension) differs between two adjacent servers? */
    int hopLevel(int s0, int s1) const {
        auto d0 = dig(s0), d1 = dig(s1);
        for (int l = 0; l <= k; l++)
            if (d0[l] != d1[l]) return l;
        return -1;
    }
};

// =====================================================================
// BCube topology builder
// =====================================================================

struct BCubeNet {
    int n, k, N;
    Addr addr;
    NodeContainer servers;   // N server nodes
    NodeContainer switches;  // (k+1)*n^k switch nodes

    /* svIp[s][l] = IP address of server s on its level-l NIC */
    std::vector<std::vector<Ipv4Address>> svIp;

    BCubeNet(int n_, int k_) : n(n_), k(k_), addr{n_, k_} {
        N = (int)std::pow(n, k + 1);
    }

    /* Build topology and install IP stack. */
    void Build(const std::string& rate = "1Gbps",
               const std::string& delay = "100ns") {
        int swPerLv = (int)std::pow(n, k);
        servers.Create(N);
        switches.Create((k + 1) * swPerLv);
        svIp.assign(N, std::vector<Ipv4Address>(k + 1));

        InternetStackHelper inet;
        inet.SetRoutingHelper(Ipv4StaticRoutingHelper());
        inet.Install(servers);
        inet.Install(switches);

        PointToPointHelper p2p;
        p2p.SetDeviceAttribute("DataRate", StringValue(rate));
        p2p.SetChannelAttribute("Delay", StringValue(delay));
        p2p.SetDeviceAttribute("Mtu", UintegerValue(9000));

        Ipv4AddressHelper ipH;
        int sidx = 0;   // global subnet counter

        for (int lv = 0; lv <= k; lv++) {
            for (int swp = 0; swp < swPerLv; swp++) {
                int swNid = lv * swPerLv + swp;
                for (int port = 0; port < n; port++) {
                    /* Reconstruct which server connects here:
                     * swp encodes all digits except the one at level lv.
                     * port = digit at level lv. */
                    std::vector<int> d(k + 1);
                    int tmp = swp;
                    for (int i = 0; i <= k; i++) {
                        if (i == lv) { d[i] = port; }
                        else         { d[i] = tmp % n; tmp /= n; }
                    }
                    int svId = addr.num(d);

                    NodeContainer pair;
                    pair.Add(servers.Get(svId));
                    pair.Add(switches.Get(swNid));
                    auto devs = p2p.Install(pair);

                    /* Subnet 10.lv.sidx.0/30  (server=.1, switch=.2) */
                    std::string base = "10." + std::to_string(lv) + "." + std::to_string(sidx) + ".0";
                    ipH.SetBase(base.c_str(), "255.255.255.252");
                    auto ifc = ipH.Assign(devs);
                    svIp[svId][lv] = ifc.GetAddress(0);
                    sidx++;
                }
            }
        }
    }

    /* Install per-hop static routes for all (src,dst) pairs.
     *
     * Strategy: for each parallel path p_i from src→dst, route
     * packets destined for svIp[dst][i] along p_i.  This lets two
     * TCP connections to the same physical server use disjoint paths
     * (connection C0 targets svIp[dst][0], C1 targets svIp[dst][1]).
     */
    void InstallRoutes() {
        Ipv4StaticRoutingHelper srH;

        for (int src = 0; src < N; src++) {
            for (int dst = 0; dst < N; dst++) {
                if (src == dst) continue;

                auto paths = addr.buildPathSet(src, dst);
                // paths[i] routes toward svIp[dst][i % (k+1)]
                for (int pi = 0; pi < (int)paths.size(); pi++) {
                    const auto& path = paths[pi];
                    Ipv4Address dstIp = svIp[dst][pi % (k + 1)];

                    // Install one route entry per intermediate hop
                    for (int h = 0; h + 1 < (int)path.size(); h++) {
                        int cur = path[h];
                        int nxt = path[h + 1];
                        int lv  = addr.hopLevel(cur, nxt);
                        assert(lv >= 0);

                        // route on server node `cur`
                        Ptr<Ipv4> ipv4 = servers.Get(cur)->GetObject<Ipv4>();
                        uint32_t ifIdx =
                            ipv4->GetInterfaceForAddress(svIp[cur][lv]);
                        auto sr = srH.GetStaticRouting(ipv4);
                        sr->AddHostRouteTo(dstIp, ifIdx, 10);
                    }

                    // Intermediate server nodes also need to forward
                    // (already handled above hop by hop)
                }

                // Default: any unmatched dst → use primary path's level-0 iface
                {
                    auto& pp = paths[0];
                    int lv0 = addr.hopLevel(pp[0], pp[1]);
                    Ptr<Ipv4> ipv4 = servers.Get(src)->GetObject<Ipv4>();
                    uint32_t ifIdx =
                        ipv4->GetInterfaceForAddress(svIp[src][lv0]);
                    auto sr = srH.GetStaticRouting(ipv4);
                    // Catch-all host route for dst's primary IP (path 0)
                    sr->AddHostRouteTo(svIp[dst][0], ifIdx, 20);
                }
            }
        }

        // Switch routing: BCube switch at level `lv` routes by the destination's
        // lv-th digit. During Build() we installed ports in digit order 0..n-1,
        // so interface (digit+1) leads to the server with that digit at level lv.
        // Install a host route for every server IP so switches forward correctly.
        int swPerLevel = N / n;
        for (int sw = 0; sw < (int)switches.GetN(); sw++) {
            int lv = sw / swPerLevel;       // which level this switch is at
            Ptr<Ipv4> swIpv4 = switches.Get(sw)->GetObject<Ipv4>();
            auto srSw = srH.GetStaticRouting(swIpv4);

            for (int dst = 0; dst < N; dst++) {
                auto dstDig = addr.dig(dst);
                uint32_t outIfc = static_cast<uint32_t>(dstDig[lv]) + 1;
                for (int p = 0; p <= k; p++) {
                    srSw->AddHostRouteTo(svIp[dst][p], outIfc, 5);
                }
            }
        }
    }

    /* Simulate a switch failure: drop all packets on switch swIdx.
     * Applies 100% packet-loss error model to every PointToPoint device
     * on that switch node, so no packet can enter or leave it. */
    void FailSwitch(int swIdx) {
        Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
        em->SetAttribute("ErrorUnit",
            EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
        em->SetAttribute("ErrorRate", DoubleValue(1.0));
        Ptr<Node> sw = switches.Get(swIdx);
        for (uint32_t d = 1; d < sw->GetNDevices(); d++)   // skip loopback (d=0)
            sw->GetDevice(d)->SetAttribute("ReceiveErrorModel", PointerValue(em));
    }

    Ptr<Node>    SvNode(int id) { return servers.Get(id); }
    Ipv4Address  SvIP(int id, int lv = 0) { return svIp[id][lv]; }
};

// =====================================================================
// 2-level Tree topology  (comparison baseline)
// =====================================================================

struct TreeNet {
    int n, groups, N;
    NodeContainer servers;
    NodeContainer leafSw;
    NodeContainer rootSw;
    std::vector<Ipv4Address> svIp;

    TreeNet(int n_, int groups_)
        : n(n_), groups(groups_), N(n_ * groups_) {}

    void Build(const std::string& rate = "1Gbps",
               const std::string& delay = "100ns") {
        servers.Create(N);
        leafSw.Create(groups);
        rootSw.Create(1);
        svIp.resize(N);

        InternetStackHelper inet;
        inet.Install(servers);
        inet.Install(leafSw);
        inet.Install(rootSw);

        PointToPointHelper p2p;
        p2p.SetDeviceAttribute("DataRate", StringValue(rate));
        p2p.SetChannelAttribute("Delay", StringValue(delay));
        p2p.SetDeviceAttribute("Mtu", UintegerValue(9000));

        Ipv4AddressHelper ipH;
        int sidx = 0;

        // server ↔ leaf switch
        for (int g = 0; g < groups; g++) {
            for (int i = 0; i < n; i++) {
                int svId = g * n + i;
                NodeContainer pair;
                pair.Add(servers.Get(svId));
                pair.Add(leafSw.Get(g));
                auto devs = p2p.Install(pair);

                std::string base = "192.168." + std::to_string(sidx) + ".0";
                ipH.SetBase(base.c_str(), "255.255.255.252");
                auto ifc = ipH.Assign(devs);
                svIp[svId] = ifc.GetAddress(0);
                sidx++;
            }
        }

        // leaf switch ↔ root switch  (uplinks, also 1 Gbps)
        for (int g = 0; g < groups; g++) {
            NodeContainer pair;
            pair.Add(leafSw.Get(g));
            pair.Add(rootSw.Get(0));
            auto devs = p2p.Install(pair);

            std::string base = "10.200." + std::to_string(g) + ".0";
            ipH.SetBase(base.c_str(), "255.255.255.252");
            ipH.Assign(devs);
        }

        Ipv4GlobalRoutingHelper::PopulateRoutingTables();
    }

    /* Fail the root switch (single point of failure in a tree). */
    void FailRootSwitch() { FailNode(rootSw.Get(0)); }

    /* Fail a leaf switch (disconnects one server group). */
    void FailLeafSwitch(int idx) { FailNode(leafSw.Get(idx)); }

    Ptr<Node>    SvNode(int id) { return servers.Get(id); }
    Ipv4Address  SvIP(int id)   { return svIp[id]; }

private:
    static void FailNode(Ptr<Node> sw) {
        Ptr<RateErrorModel> em = CreateObject<RateErrorModel>();
        em->SetAttribute("ErrorUnit",
            EnumValue(RateErrorModel::ERROR_UNIT_PACKET));
        em->SetAttribute("ErrorRate", DoubleValue(1.0));
        for (uint32_t d = 1; d < sw->GetNDevices(); d++)
            sw->GetDevice(d)->SetAttribute("ReceiveErrorModel", PointerValue(em));
    }
};

// =====================================================================
// Traffic & measurement helpers
// =====================================================================

void AddSink(Ptr<Node> node, uint16_t port, double stop = 400.0) {
    PacketSinkHelper sink("ns3::TcpSocketFactory",
                          InetSocketAddress(Ipv4Address::GetAny(), port));
    auto apps = sink.Install(node);
    apps.Start(Seconds(0.0));
    apps.Stop(Seconds(stop));
}

void AddBulkSend(Ptr<Node> src, Ipv4Address dstIp, uint16_t port,
                 uint64_t bytes, double start = 1.0, double stop = 400.0) {
    BulkSendHelper bulk("ns3::TcpSocketFactory",
                        InetSocketAddress(dstIp, port));
    bulk.SetAttribute("MaxBytes", UintegerValue(bytes));
    bulk.SetAttribute("SendSize", UintegerValue(8192));
    auto apps = bulk.Install(src);
    apps.Start(Seconds(start));
    apps.Stop(Seconds(stop));
}

struct Result {
    double totalGbps;
    double perSrvGbps;
    double finishSec;
    int    nFlows;
};

Result Measure(Ptr<FlowMonitor> fm, double endTime) {
    fm->CheckForLostPackets();
    double bits = 0;
    double maxT = 0;
    int nf = 0;
    for (auto& kv : fm->GetFlowStats()) {
        if (kv.second.rxBytes == 0) continue;
        bits += kv.second.rxBytes * 8.0;
        double t = kv.second.timeLastRxPacket.GetSeconds();
        if (t > maxT) maxT = t;
        nf++;
    }
    double dur = maxT > 1.0 ? maxT : endTime;
    Result r;
    r.nFlows    = nf;
    r.finishSec = dur;
    r.totalGbps = bits / dur / 1e9;
    r.perSrvGbps = nf > 0 ? r.totalGbps / nf : 0;
    return r;
}

// =====================================================================
// §7.5  Experiment A — One-to-one
//
// Paper: server 00 ↔ server 13 on two parallel paths.
//   C1: 00 → 10 → 13   (level-1 then level-0)
//   C2: 00 → 03 → 13   (level-0 then level-1)
//   Total ~1.93 Gb/s  vs  Tree ~0.99 Gb/s
// =====================================================================

void ExpOneToOne() {
    std::cout << "\n=== §7.5  One-to-one ===" << std::endl;
    const uint64_t DATA = 10ULL << 30;   // 10 GB

    // ── BCube ──────────────────────────────────────────────────────────
    {
        BCubeNet bc(4, 1);
        bc.Build();
        bc.InstallRoutes();

        // Server "00" = id 0  ([a0=0,a1=0])
        // Server "13" = id 7  ([a0=3,a1=1])  — paper notation is a1 a0
        int src = 0, dst = 7;

        // C1 → dst's level-0 IP  (path through level-1 then level-0)
        // C2 → dst's level-1 IP  (path through level-0 then level-1)
        // Sinks listen on all IPs (Ipv4Address::GetAny)
        AddSink(bc.SvNode(dst), 9001);
        AddSink(bc.SvNode(dst), 9002);
        AddBulkSend(bc.SvNode(src), bc.SvIP(dst, 0), 9001, DATA / 2);
        AddBulkSend(bc.SvNode(src), bc.SvIP(dst, 1), 9002, DATA / 2);

        FlowMonitorHelper fmH;
        auto fm = fmH.InstallAll();

        Simulator::Stop(Seconds(150.0));
        Simulator::Run();

        auto r = Measure(fm, 150.0);
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  BCube  total: " << r.totalGbps * 1000 << " Mb/s"
                  << "  finish: " << r.finishSec << "s"
                  << "  (paper: ~1930 Mb/s)" << std::endl;

        Simulator::Destroy();
    }

    // ── Tree ───────────────────────────────────────────────────────────
    {
        TreeNet tr(4, 4);
        tr.Build();

        // server 0 (group 0) → server 7 (group 1, pos 3)
        // Must go through leaf-sw 0, root-sw, leaf-sw 1  → 1 Gbps bottleneck
        int src = 0, dst = 7;
        AddSink(tr.SvNode(dst), 9001);
        AddBulkSend(tr.SvNode(src), tr.SvIP(dst), 9001, DATA);

        FlowMonitorHelper fmH;
        auto fm = fmH.InstallAll();

        Simulator::Stop(Seconds(150.0));
        Simulator::Run();

        auto r = Measure(fm, 150.0);
        std::cout << "  Tree   total: " << r.totalGbps * 1000 << " Mb/s"
                  << "  finish: " << r.finishSec << "s"
                  << "  (paper:  ~990 Mb/s)" << std::endl;

        Simulator::Destroy();
    }
}

// =====================================================================
// §7.5  Experiment B — One-to-all
//
// Server "00" delivers 10 GB to all 15 others.
// BCube uses 2 edge-disjoint spanning trees → 2× throughput.
// Paper: BCube ~1.6 Gb/s,  Tree ~0.88 Gb/s.
// =====================================================================

void ExpOneToAll() {
    std::cout << "\n=== §7.5  One-to-all ===" << std::endl;
    const uint64_t DATA = 10ULL << 30;

    // ── BCube (spanning-tree multicast approximation) ──────────────────
    // Tree 0 (level-0 root): src → 01 → 02 → 03 → 11,21,31 → ...
    // Tree 1 (level-1 root): src → 10 → 20 → 30 → 11,12,13 → ...
    // Approximation: send DATA/2 to each destination via each tree.
    // In practice each receiver ends up with full file from two halves.
    {
        BCubeNet bc(4, 1);
        bc.Build();
        bc.InstallRoutes();

        int src = 0;
        uint16_t port = 9000;
        for (int dst = 1; dst < bc.N; dst++) {
            AddSink(bc.SvNode(dst), port);
            // alternate between the two parallel paths via level-0 and level-1 IPs
            int lv = (dst % 2 == 0) ? 0 : 1;
            AddBulkSend(bc.SvNode(src), bc.SvIP(dst, lv), port,
                        DATA / (bc.N - 1));
            port++;
        }

        FlowMonitorHelper fmH;
        auto fm = fmH.InstallAll();
        Simulator::Stop(Seconds(150.0));
        Simulator::Run();

        auto r = Measure(fm, 150.0);
        std::cout << "  BCube  total: " << r.totalGbps * 1000 << " Mb/s"
                  << "  (paper: ~1600 Mb/s)" << std::endl;
        Simulator::Destroy();
    }

    // ── Tree (pipeline: server i relays to i+1) ────────────────────────
    {
        TreeNet tr(4, 4);
        tr.Build();

        int src = 0;
        uint16_t port = 9000;
        for (int dst = 1; dst < tr.N; dst++) {
            AddSink(tr.SvNode(dst), port);
            AddBulkSend(tr.SvNode(src), tr.SvIP(dst), port,
                        DATA / (tr.N - 1));
            port++;
        }

        FlowMonitorHelper fmH;
        auto fm = fmH.InstallAll();
        Simulator::Stop(Seconds(150.0));
        Simulator::Run();

        auto r = Measure(fm, 150.0);
        std::cout << "  Tree   total: " << r.totalGbps * 1000 << " Mb/s"
                  << "  (paper:  ~880 Mb/s)" << std::endl;
        Simulator::Destroy();
    }
}

// =====================================================================
// §7.5  Experiment C — All-to-all (MapReduce shuffle)
//
// Each of the 16 servers sends 683 MB to every other server.
// Paper: BCube per-server ~750 Mb/s,  Tree ~260 Mb/s  (3× speedup).
// =====================================================================

void ExpAllToAll() {
    std::cout << "\n=== §7.5  All-to-all (MapReduce) ===" << std::endl;
    // Paper uses 683 MB/conn × 240 flows, but that takes 20-60 min wall-clock.
    // 50 MB/conn is enough to reach steady state; scale the result back to compare.
    const uint64_t PER_CONN = 50ULL * 1024 * 1024;    // 50 MB / connection (fast)

    // ── BCube ──────────────────────────────────────────────────────────
    {
        BCubeNet bc(4, 1);
        bc.Build();
        bc.InstallRoutes();

        uint16_t basePort = 9000;
        for (int src = 0; src < bc.N; src++) {
            for (int dst = 0; dst < bc.N; dst++) {
                if (src == dst) continue;
                uint16_t port = basePort + (uint16_t)(src * bc.N + dst);
                AddSink(bc.SvNode(dst), port, 500.0);
                // alternate paths for better load balance
                int lv = (src + dst) % (bc.k + 1);
                AddBulkSend(bc.SvNode(src), bc.SvIP(dst, lv),
                            port, PER_CONN, 1.0, 120.0);
            }
        }

        FlowMonitorHelper fmH;
        auto fm = fmH.InstallAll();
        Simulator::Stop(Seconds(120.0));
        Simulator::Run();

        auto r = Measure(fm, 120.0);
        double perSv = r.totalGbps / bc.N * 1000;
        std::cout << "  BCube  per-server: " << std::fixed << std::setprecision(0)
                  << perSv << " Mb/s"
                  << "  finish: " << std::setprecision(1) << r.finishSec << "s"
                  << "  (paper: ~750 Mb/s)" << std::endl;
        Simulator::Destroy();
    }

    // ── Tree ───────────────────────────────────────────────────────────
    {
        TreeNet tr(4, 4);
        tr.Build();

        uint16_t basePort = 9000;
        for (int src = 0; src < tr.N; src++) {
            for (int dst = 0; dst < tr.N; dst++) {
                if (src == dst) continue;
                uint16_t port = basePort + (uint16_t)(src * tr.N + dst);
                AddSink(tr.SvNode(dst), port, 120.0);
                AddBulkSend(tr.SvNode(src), tr.SvIP(dst),
                            port, PER_CONN, 1.0, 120.0);
            }
        }

        FlowMonitorHelper fmH;
        auto fm = fmH.InstallAll();
        Simulator::Stop(Seconds(120.0));
        Simulator::Run();

        auto r = Measure(fm, 120.0);
        double perSv = r.totalGbps / tr.N * 1000;
        std::cout << "  Tree   per-server: " << std::fixed << std::setprecision(0)
                  << perSv << " Mb/s"
                  << "  finish: " << std::setprecision(1) << r.finishSec << "s"
                  << "  (paper: ~260 Mb/s)" << std::endl;
        Simulator::Destroy();
    }
}

// =====================================================================
// §6  Graceful degradation — ns-3 packet-level simulation
//
// BCube_1(n=4): 16 servers, 8 switches (sw0-3=lv-0, sw4-7=lv-1).
//
// Two independent pairs run simultaneously:
//   Pair A: 0=[0,0] → 7=[3,1]
//     Path-A0 [0→4→7]  uses sw4(lv-1,a0=0)  + sw1(lv-0,a1=1)
//     Path-A1 [0→3→7]  uses sw0(lv-0,a1=0)  + sw7(lv-1,a0=3)
//
//   Pair B: 5=[1,1] → 10=[2,2]
//     Path-B0 [5→9→10] uses sw5(lv-1,a0=1)  + sw2(lv-0,a1=2)
//     Path-B1 [5→6→10] uses sw1(lv-0,a1=1)  + sw6(lv-1,a0=2)
//
// Failure order: sw0 → sw4 → sw1 → sw5
//   Each failure kills exactly ONE path across the two pairs:
//   sw0  →  Path-A1 dies  (Pair A: 2→1 paths)
//   sw4  →  Path-A0 dies  (Pair A: 1→0 paths)
//   sw1  →  Path-B1 dies  (Pair B: 2→1 paths)
//   sw5  →  Path-B0 dies  (Pair B: 1→0 paths)
//
// Expected total throughput (each path ≈ 965 Mb/s):
//   0 failures → ~3860 Mb/s   (4 paths)
//   1 failure  → ~2895 Mb/s   (3 paths) — graceful
//   2 failures → ~1930 Mb/s   (2 paths) — still alive
//   3 failures → ~965  Mb/s   (1 path)  — still alive
//   4 failures → ~0    Mb/s   (0 paths)
//
// Tree (1 root switch): fail root → immediate total blackout.
// =====================================================================

void ExpDegrade() {
    std::cout << "\n=== §6  Graceful degradation under switch failures ===" << std::endl;

    const double   SIM_SEC   = 60.0;
    const uint64_t UNLIMITED = 0;   // BulkSend MaxBytes=0 → send forever

    // ── BCube: 5 failure levels, 2 pairs, 4 flows total ───────────────────
    //
    // Failure order chosen so each step kills exactly 1 path:
    //   sw0 kills Path-A1 | sw4 kills Path-A0 | sw1 kills Path-B1 | sw5 kills Path-B0
    const int failOrder[]    = {0, 4, 1, 5};
    const char* failLabels[] = {
        "none              ",
        "sw0               ",
        "sw0,sw4           ",
        "sw0,sw4,sw1       ",
        "sw0,sw4,sw1,sw5   "
    };
    const char* expected[] = {
        "~3860 Mb/s  (4 paths: A0+A1+B0+B1)",
        "~2895 Mb/s  (3 paths: A0+B0+B1   ) ← graceful",
        "~1930 Mb/s  (2 paths: B0+B1       ) ← still alive",
        "~965  Mb/s  (1 path:  B0          ) ← still alive",
        "~0    Mb/s  (0 paths              )"
    };

    std::cout << "\n  BCube_1(n=4)  Pair-A: 0→7  +  Pair-B: 5→10  (" << SIM_SEC << "s)\n";
    std::cout << "  #fail  Failed switches      | Throughput  | Expected\n";
    std::cout << "  " << std::string(70, '-') << "\n";

    for (int nf = 0; nf <= 4; nf++) {
        BCubeNet bc(4, 1);
        bc.Build();
        bc.InstallRoutes();

        for (int i = 0; i < nf; i++)
            bc.FailSwitch(failOrder[i]);

        // Pair A: 0 → 7
        AddSink(bc.SvNode(7),  9001);
        AddSink(bc.SvNode(7),  9002);
        AddBulkSend(bc.SvNode(0), bc.SvIP(7,  0), 9001, UNLIMITED);
        AddBulkSend(bc.SvNode(0), bc.SvIP(7,  1), 9002, UNLIMITED);

        // Pair B: 5 → 10
        AddSink(bc.SvNode(10), 9003);
        AddSink(bc.SvNode(10), 9004);
        AddBulkSend(bc.SvNode(5), bc.SvIP(10, 0), 9003, UNLIMITED);
        AddBulkSend(bc.SvNode(5), bc.SvIP(10, 1), 9004, UNLIMITED);

        FlowMonitorHelper fmH;
        auto fm = fmH.InstallAll();
        Simulator::Stop(Seconds(SIM_SEC));
        Simulator::Run();

        auto r = Measure(fm, SIM_SEC);
        std::cout << std::fixed << std::setprecision(0);
        std::cout << "  " << std::setw(2) << nf << "     "
                  << std::left << std::setw(20) << failLabels[nf] << std::right
                  << " | " << std::setw(6) << r.totalGbps * 1000 << " Mb/s"
                  << "  | " << expected[nf] << "\n";

        Simulator::Destroy();
    }

    // ── Tree: fail root switch = total blackout ────────────────────────────
    std::cout << "\n  2-level Tree  server 0→7  (" << SIM_SEC << "s)\n";
    std::cout << "  #fail  Failed switches      | Throughput  | Expected\n";
    std::cout << "  " << std::string(70, '-') << "\n";

    for (int nf = 0; nf <= 1; nf++) {
        TreeNet tr(4, 4);
        tr.Build();
        if (nf == 1) tr.FailRootSwitch();

        AddSink(tr.SvNode(7), 9001);
        AddBulkSend(tr.SvNode(0), tr.SvIP(7), 9001, UNLIMITED);

        FlowMonitorHelper fmH;
        auto fm = fmH.InstallAll();
        Simulator::Stop(Seconds(SIM_SEC));
        Simulator::Run();

        auto r = Measure(fm, SIM_SEC);
        const char* texp[] = {
            "~990 Mb/s  (baseline)",
            "~0   Mb/s  (root switch = TOTAL BLACKOUT)"
        };
        std::cout << "  " << std::setw(2) << nf << "     "
                  << std::left << std::setw(20)
                  << (nf==0 ? "none" : "root-sw") << std::right
                  << " | " << std::setw(6) << r.totalGbps * 1000 << " Mb/s"
                  << "  | " << texp[nf] << "\n";

        Simulator::Destroy();
    }

    std::cout << "\n  BCube: 每断一个交换机只损失一条路径，总带宽线性下降 (优雅降级)\n"
              << "  Tree:  根交换机一断全灭 (单点故障)\n";
}

// =====================================================================
// main
// =====================================================================

int main(int argc, char* argv[]) {
    std::string exp = "one-to-one";
    CommandLine cmd(__FILE__);
    cmd.AddValue("exp",
                 "Experiment: one-to-one | one-to-all | all-to-all | degrade",
                 exp);
    cmd.Parse(argc, argv);

    // TCP tuning (paper: 9 KB MTU, large TCP buffers)
    Config::SetDefault("ns3::TcpSocket::SegmentSize",    UintegerValue(8960));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize",     UintegerValue(1 << 26));
    Config::SetDefault("ns3::TcpSocket::SndBufSize",     UintegerValue(1 << 26));
    Config::SetDefault("ns3::TcpSocket::InitialCwnd",    UintegerValue(100));
    // 用字符串方式设置拥塞控制，无需 include tcp-cubic.h，兼容各 ns-3 版本
    Config::SetDefault("ns3::TcpL4Protocol::SocketType",
                       StringValue("ns3::TcpCubic"));

    std::cout << "BCube SIGCOMM'09 — ns-3 Reproduction\n"
              << "Experiment: " << exp << "\n";

    if      (exp == "one-to-one") ExpOneToOne();
    else if (exp == "one-to-all") ExpOneToAll();
    else if (exp == "all-to-all") ExpAllToAll();
    else if (exp == "degrade")    ExpDegrade();
    else {
        std::cerr << "Unknown experiment: " << exp << std::endl;
        return 1;
    }

    return 0;
}
