/* =============================================================================
 * vl2_simulation.cc
 *
 * NS-3 reproduction of:
 *   "VL2: A Scalable and Flexible Data Center Network"
 *   Greenberg et al., SIGCOMM 2009
 *
 * Topology (matches paper's 80-server testbed, Figure 5 & 8):
 *
 *   [Int-0]   [Int-1]   [Int-2]        <- Intermediate (spine) switches
 *      | \  /   | \  /   | \  /
 *   [Aggr-0] [Aggr-1] [Aggr-2]         <- Aggregation switches
 *      |  \   /  |  \  / |
 *   [ToR-0][ToR-1][ToR-2][ToR-3]       <- Top-of-Rack switches
 *      |       |       |       |
 *   [srv...]  ...    ...    ...         <- Servers (5 per ToR = 20 total)
 *
 * Links:
 *   Server  <-> ToR  : 1 Gbps, 100 µs
 *   ToR     <-> Aggr : 10 Gbps, 10 µs  (each ToR connects to 2 Aggr)
 *   Aggr    <-> Int  : 10 Gbps, 10 µs  (full bipartite)
 *
 * VLB implementation:
 *   NS-3's Ipv4GlobalRouting with RandomEcmpRouting=true performs per-flow
 *   random path selection.  In the Clos topology this naturally routes each
 *   flow through a randomly chosen Intermediate switch, exactly as described
 *   in VL2 Section 4.2.2.
 *
 * Experiments (--experiment=N):
 *   1  All-to-all data shuffle        (paper Fig 9,  Section 5.1)
 *   2  VLB fairness (Jain index)      (paper Fig 10, Section 5.2)
 *   3  Performance isolation          (paper Fig 11, Section 5.3)
 *   4  Convergence after link failure (paper Fig 13, Section 5.4)
 *
 * Build (inside your ns-3 source tree):
 *   cp vl2_simulation.cc scratch/
 *   ./ns3 build
 *   ./ns3 run "vl2-simulation --experiment=1"
 *
 * Outputs (written to current working directory):
 *   vl2_expN_goodput.dat   – time-series: time, Gbps, active-flows, fairness
 *   vl2_expN_flows.xml     – NS-3 FlowMonitor XML
 * =============================================================================
 */

#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/traffic-control-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("VL2Simulation");

/* =========================================================================
 *  Topology parameters (easily overridden from command line)
 * ========================================================================= */
static int    g_nInt         = 3;   // Intermediate (spine) switches
static int    g_nAggr        = 3;   // Aggregation switches
static int    g_nTor         = 4;   // ToR switches
static int    g_nSrvPerTor   = 5;   // Servers per ToR  (paper uses 20)

/* =========================================================================
 *  Global monitoring state
 * ========================================================================= */
static std::ofstream              g_outFile;
static std::vector<Ptr<PacketSink>> g_sinks;
static std::vector<uint64_t>      g_lastRx;
static double                     g_interval = 5.0;
// Experiment 3 only: number of sinks belonging to Service 1 (first g_nSrv1
// entries in g_sinks).  0 means no per-service split (all other experiments).
static int                        g_nSrv1    = 0;

/* =========================================================================
 *  Utility: Jain's fairness index
 * ========================================================================= */
static double
JainFairness(const std::vector<double>& v)
{
    if (v.empty()) return 1.0;
    double s1 = 0.0, s2 = 0.0;
    for (double x : v) { s1 += x; s2 += x * x; }
    int n = static_cast<int>(v.size());
    return (s1 * s1) / (static_cast<double>(n) * s2 + 1e-15);
}

/* =========================================================================
 *  Periodic goodput logger
 * ========================================================================= */
static void
LogGoodput()
{
    double total = 0.0, svc1 = 0.0, svc2 = 0.0;
    std::vector<double> perFlow;

    for (std::size_t i = 0; i < g_sinks.size(); i++) {
        uint64_t rx  = g_sinks[i]->GetTotalRx();
        double   bps = static_cast<double>(rx - g_lastRx[i]) * 8.0 / g_interval;
        g_lastRx[i]  = rx;
        if (bps > 1e4) {                   // ignore idle sinks
            perFlow.push_back(bps);
            total += bps;
            if (g_nSrv1 > 0) {
                if ((int)i < g_nSrv1) svc1 += bps;
                else                  svc2 += bps;
            }
        }
    }

    double gbps     = total / 1e9;
    double fairness = JainFairness(perFlow);

    g_outFile << Simulator::Now().GetSeconds()
          << " " << gbps
          << " " << static_cast<int>(perFlow.size())
          << " " << fairness;
    if (g_nSrv1 > 0) {
        // Extra columns for Experiment 3: svc1_gbps svc2_gbps
        g_outFile << " " << svc1 / 1e9
                  << " " << svc2 / 1e9;
    }
    g_outFile << "\n";
    g_outFile.flush();

    Simulator::Schedule(Seconds(g_interval), &LogGoodput);
}

/* =========================================================================
 *  Link failure helpers (Experiment 4)
 * ========================================================================= */
struct LinkInfo {
    Ptr<Node> nodeA;
    uint32_t  ifA;
    Ptr<Node> nodeB;
    uint32_t  ifB;
};

static void
LinkDown(LinkInfo li)
{
    NS_LOG_INFO("[t=" << Simulator::Now().GetSeconds() << "s] LINK DOWN "
                << li.nodeA->GetId() << " <-> " << li.nodeB->GetId());
    li.nodeA->GetObject<Ipv4>()->SetDown(li.ifA);
    li.nodeB->GetObject<Ipv4>()->SetDown(li.ifB);
    Ipv4GlobalRoutingHelper::RecomputeRoutingTables();
}

static void
LinkUp(LinkInfo li)
{
    NS_LOG_INFO("[t=" << Simulator::Now().GetSeconds() << "s] LINK UP "
                << li.nodeA->GetId() << " <-> " << li.nodeB->GetId());
    li.nodeA->GetObject<Ipv4>()->SetUp(li.ifA);
    li.nodeB->GetObject<Ipv4>()->SetUp(li.ifB);
    Ipv4GlobalRoutingHelper::RecomputeRoutingTables();
}

/* =========================================================================
 *  Main
 * ========================================================================= */
int
main(int argc, char* argv[])
{
    /* ----- command-line arguments ----- */
    int    experiment  = 1;
    bool   verbose     = false;
    bool   pcap        = false;

    CommandLine cmd(__FILE__);
    cmd.AddValue("experiment",  "1=shuffle 2=fairness 3=isolation 4=failure", experiment);
    cmd.AddValue("nSrvPerTor",  "Servers per ToR (paper=20, default=5)",      g_nSrvPerTor);
    cmd.AddValue("nInt",        "Number of Intermediate switches",             g_nInt);
    cmd.AddValue("nAggr",       "Number of Aggregation switches",              g_nAggr);
    cmd.AddValue("nTor",        "Number of ToR switches",                      g_nTor);
    cmd.AddValue("verbose",     "Enable verbose NS-3 logging",                 verbose);
    cmd.AddValue("pcap",        "Enable PCAP tracing (large files!)",          pcap);
    cmd.Parse(argc, argv);

    const int N_SRV = g_nTor * g_nSrvPerTor;

    /* ----- logging ----- */
    if (verbose) {
        LogComponentEnable("VL2Simulation", LOG_LEVEL_ALL);
    } else {
        LogComponentEnable("VL2Simulation", LOG_LEVEL_INFO);
    }

    NS_LOG_INFO("VL2 Simulation — Experiment " << experiment);
    NS_LOG_INFO("Topology: " << g_nInt << " Int / " << g_nAggr << " Aggr / "
                << g_nTor << " ToR / " << N_SRV << " servers");

    Time::SetResolution(Time::NS);

    /* ----- global NS-3 settings ----- */

    // RandomEcmpRouting=true：每个包随机选等价路径，近似模拟 VLB 流量散布。
    // 副作用是同一流的包可能乱序到达，触发 TCP DUPACK/重传。
    // 启用 TCP SACK（选择确认）后，接收方能精确告知哪些包缺失，
    // 发送方只重传缺失的包，大幅降低乱序对吞吐的影响。
    Config::SetDefault("ns3::Ipv4GlobalRouting::RandomEcmpRouting",
                       BooleanValue(true));

    // TCP settings
    Config::SetDefault("ns3::TcpSocket::SegmentSize",      UintegerValue(1448));
    Config::SetDefault("ns3::TcpSocket::SndBufSize",       UintegerValue(4194304));
    Config::SetDefault("ns3::TcpSocket::RcvBufSize",       UintegerValue(4194304));
    Config::SetDefault("ns3::TcpSocket::InitialCwnd",      UintegerValue(10));
    Config::SetDefault("ns3::TcpSocketBase::MinRto",       TimeValue(MilliSeconds(200)));
    // SACK：乱序时只重传缺失段，而非退回慢启动，减轻逐包 ECMP 的副作用
    Config::SetDefault("ns3::TcpSocketBase::Sack",         BooleanValue(true));

    // Queue depth at each output port (10G links → ~83 µs at 1500B MTU per packet)
    Config::SetDefault("ns3::DropTailQueue<Packet>::MaxSize",
                       QueueSizeValue(QueueSize("500p")));

    /* ===================================================================
     *  Create nodes
     * =================================================================== */
    NodeContainer intNodes, aggrNodes, torNodes, srvNodes;
    intNodes.Create(g_nInt);
    aggrNodes.Create(g_nAggr);
    torNodes.Create(g_nTor);
    srvNodes.Create(N_SRV);

    /* ===================================================================
     *  Install Internet stack
     * =================================================================== */
    InternetStackHelper stack;
    stack.Install(intNodes);
    stack.Install(aggrNodes);
    stack.Install(torNodes);
    stack.Install(srvNodes);

    /* ===================================================================
     *  Build links and assign IP addresses
     *
     *  Address plan:
     *    10.A.B.0/24  – switch-to-switch and switch-to-server subnets (LAs)
     *    Each /24 is used for a single point-to-point link.
     * =================================================================== */
    PointToPointHelper p2p;
    Ipv4AddressHelper  addr;

    /* rolling subnet counter → 10.1.1.0, 10.1.2.0, ... 10.1.255.0, 10.2.0.0 ... */
    int subA = 1, subB = 0;
    auto nextSubnet = [&]() -> std::string {
        if (++subB == 256) { subA++; subB = 0; }
        return "10." + std::to_string(subA) + "." + std::to_string(subB) + ".0";
    };

    /* helper: create a link and return (ifaceOnA, ifaceOnB) */
    auto makeLink = [&](Ptr<Node> a, Ptr<Node> b,
                         const std::string& bw, const std::string& delay)
        -> std::pair<uint32_t, uint32_t>
    {
        p2p.SetDeviceAttribute("DataRate", StringValue(bw));
        p2p.SetChannelAttribute("Delay",   StringValue(delay));
        NetDeviceContainer ndc = p2p.Install(a, b);
        addr.SetBase(nextSubnet().c_str(), "255.255.255.0");
        addr.Assign(ndc);
        uint32_t ifA = a->GetObject<Ipv4>()->GetInterfaceForDevice(ndc.Get(0));
        uint32_t ifB = b->GetObject<Ipv4>()->GetInterfaceForDevice(ndc.Get(1));
        return {ifA, ifB};
    };

    /* ---- 1.  Aggr ↔ Int  (full bipartite, 10 Gbps) ---- */
    std::vector<LinkInfo> aggrIntLinks; // needed for failure experiment
    for (int a = 0; a < g_nAggr; a++) {
        for (int i = 0; i < g_nInt; i++) {
            auto [ifA, ifI] = makeLink(aggrNodes.Get(a), intNodes.Get(i),
                                       "10Gbps", "10us");
            LinkInfo li;
            li.nodeA = aggrNodes.Get(a); li.ifA = ifA;
            li.nodeB = intNodes.Get(i);  li.ifB = ifI;
            aggrIntLinks.push_back(li);
        }
    }

    /* ---- 2.  ToR ↔ Aggr  (each ToR connects to 2 Aggr, 10 Gbps) ---- */
    //   ToR[t] → Aggr[ t % nAggr ] and Aggr[ (t+1) % nAggr ]
    for (int t = 0; t < g_nTor; t++) {
        makeLink(torNodes.Get(t), aggrNodes.Get(t % g_nAggr),       "10Gbps", "10us");
        makeLink(torNodes.Get(t), aggrNodes.Get((t + 1) % g_nAggr), "10Gbps", "10us");
    }

    /* ---- 3.  Server ↔ ToR  (1 Gbps) ---- */
    std::vector<Ipv4Address> srvIp(N_SRV);
    for (int t = 0; t < g_nTor; t++) {
        for (int s = 0; s < g_nSrvPerTor; s++) {
            int idx = t * g_nSrvPerTor + s;
            p2p.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
            p2p.SetChannelAttribute("Delay",   StringValue("100us"));
            NetDeviceContainer ndc = p2p.Install(srvNodes.Get(idx), torNodes.Get(t));
            addr.SetBase(nextSubnet().c_str(), "255.255.255.0");
            Ipv4InterfaceContainer ifc = addr.Assign(ndc);
            srvIp[idx] = ifc.GetAddress(0);  // server side
        }
    }

    /* ===================================================================
     *  Populate routing tables (Dijkstra + ECMP)
     * =================================================================== */
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    /* ===================================================================
     *  Open output log
     * =================================================================== */
    std::string logFile = "vl2_exp" + std::to_string(experiment) + "_goodput.dat";
    g_outFile.open(logFile);
    g_outFile << "# Time(s)  TotalGoodput(Gbps)  ActiveFlows  JainFairness\n";

    /* ===================================================================
     *  Application helpers
     * =================================================================== */
    auto addSink = [&](Ptr<Node> node, uint16_t port) -> Ptr<PacketSink> {
        PacketSinkHelper h("ns3::TcpSocketFactory",
                           InetSocketAddress(Ipv4Address::GetAny(), port));
        ApplicationContainer ac = h.Install(node);
        ac.Start(Seconds(0.0));
        ac.Stop(Seconds(9999.0));
        return DynamicCast<PacketSink>(ac.Get(0));
    };

    auto addBulk = [&](Ptr<Node> src, Ipv4Address dstIp, uint16_t dstPort,
                        uint64_t bytes, double startTime) {
        BulkSendHelper h("ns3::TcpSocketFactory",
                         InetSocketAddress(dstIp, dstPort));
        h.SetAttribute("MaxBytes",  UintegerValue(bytes));
        h.SetAttribute("SendSize",  UintegerValue(65536));
        ApplicationContainer ac = h.Install(src);
        ac.Start(Seconds(startTime));
        ac.Stop(Seconds(9999.0));
    };

    /* ===================================================================
     *  Experiment setup
     * =================================================================== */
    double simEnd = 200.0;

    Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable>();

    if (experiment == 1) {
        /* -----------------------------------------------------------
         * Experiment 1: All-to-all data shuffle  (Section 5.1, Fig 9)
         *
         * Paper:  75 servers × 500 MB = 2.7 TB shuffled in 395 s at 94% efficiency
         * Scaled: N_SRV servers × 50 MB (adjust with --nSrvPerTor)
         * ----------------------------------------------------------- */
        NS_LOG_INFO("Exp-1: All-to-all data shuffle");
        const uint16_t  BASE_PORT  = 5000;
        const uint64_t  BYTES      = 50ULL * 1024 * 1024;   // 50 MB per flow

        for (int i = 0; i < N_SRV; i++) {
            g_sinks.push_back(addSink(srvNodes.Get(i), BASE_PORT + i));
            g_lastRx.push_back(0);
        }

        for (int src = 0; src < N_SRV; src++) {
            for (int dst = 0; dst < N_SRV; dst++) {
                if (src == dst) continue;
                double jitter = rng->GetValue(0.0, 0.2);
                addBulk(srvNodes.Get(src), srvIp[dst], BASE_PORT + dst, BYTES, jitter);
            }
        }

        g_interval = 5.0;
        simEnd     = 200.0;

    } else if (experiment == 2) {
        /* -----------------------------------------------------------
         * Experiment 2: VLB fairness  (Section 5.2, Fig 10)
         *
         * Traffic mix matching DC workload (Section 3):
         *   Each server maintains ~10 concurrent flows drawn from the
         *   empirical flow-size distribution.  We approximate this with
         *   repeated 5–100 MB transfers to random destinations.
         * ----------------------------------------------------------- */
        NS_LOG_INFO("Exp-2: VLB fairness");
        const uint16_t BASE_PORT = 6000;

        for (int i = 0; i < N_SRV; i++) {
            g_sinks.push_back(addSink(srvNodes.Get(i), BASE_PORT + i));
            g_lastRx.push_back(0);
        }

        // Each server keeps 10 flows alive (approximated by many medium flows)
        for (int src = 0; src < N_SRV; src++) {
            // 10 concurrent "flow slots", each picking a random destination
            for (int slot = 0; slot < 10; slot++) {
                int    dst   = (src + 1 + slot) % N_SRV;   // round-robin destinations
                double bytes = rng->GetValue(5.0, 100.0) * 1024 * 1024;
                double start = rng->GetValue(0.0, 1.0);
                addBulk(srvNodes.Get(src), srvIp[dst], BASE_PORT + dst,
                        static_cast<uint64_t>(bytes), start);
            }
        }

        g_interval = 2.0;
        simEnd     = 120.0;

    } else if (experiment == 3) {
        /* -----------------------------------------------------------
         * Experiment 3: Performance isolation  (Section 5.3, Fig 11)
         *
         * Service 1: first N_SRV/2 servers, persistent 1 GB TCP flows from t=0
         * Service 2: remaining servers, starts at t=30 s, 1 new server every 2 s
         *            each server sends a 500 MB file as soon as it joins
         *
         * Expected result: service-1 goodput is unaffected by service 2.
         * ----------------------------------------------------------- */
        NS_LOG_INFO("Exp-3: Performance isolation");
        int nSrv1 = N_SRV / 2;
        int nSrv2 = N_SRV - nSrv1;
        const uint16_t PORT1 = 7001;
        const uint16_t PORT2 = 7002;

        // Install sinks for both services
        for (int i = 0; i < nSrv1; i++) {
            g_sinks.push_back(addSink(srvNodes.Get(i), PORT1));
            g_lastRx.push_back(0);
        }
        for (int i = nSrv1; i < N_SRV; i++) {
            g_sinks.push_back(addSink(srvNodes.Get(i), PORT2));
            g_lastRx.push_back(0);
        }

        // Service 1: 持续流（MaxBytes=0），贯穿整个仿真窗口。
        // 论文 Fig 11：Service 1 的吞吐应在 Service 2 加入前后保持不变。
        for (int i = 0; i < nSrv1; i++) {
            int dst = (i + 1) % nSrv1;
            addBulk(srvNodes.Get(i), srvIp[dst], PORT1, 0, 0.0); // 0 = 无限
        }

        // Service 2: 每 2s 加入一台服务器，每台发 500MB（有限流，模拟突发）
        for (int i = 0; i < nSrv2; i++) {
            int src = nSrv1 + i;
            int dst = nSrv1 + (i + 1) % nSrv2;
            addBulk(srvNodes.Get(src), srvIp[dst], PORT2,
                    500ULL * 1024 * 1024, 30.0 + i * 2.0);
        }

        // Tell LogGoodput the split point so it outputs extra svc1/svc2 columns.
        g_nSrv1 = nSrv1;

        g_interval = 2.0;
        simEnd     = 120.0;

    } else if (experiment == 4) {
        /* -----------------------------------------------------------
         * Experiment 4: Convergence after link failure  (Section 5.4, Fig 13)
         *
         * Run all-to-all shuffle.  Then progressively fail the links from
         * all Aggr switches to Int-0 (at t=50,55,60 s) and Int-1 (at t=100,105,110 s).
         * Restore them at t=200+.
         *
         * NS-3's RecomputeRoutingTables() re-runs Dijkstra immediately,
         * modelling the fast OSPF reconvergence described in the paper (<1 s).
         * ----------------------------------------------------------- */
        NS_LOG_INFO("Exp-4: Link failure convergence");
        const uint16_t BASE_PORT = 5000;
        // MaxBytes=0 → 无限流量，持续到 simEnd。
        // 原来 200MB/流 在 t≈30s 就跑完了，故障 t=50s 根本没流量可观察。
        const uint64_t BYTES = 0;

        for (int i = 0; i < N_SRV; i++) {
            g_sinks.push_back(addSink(srvNodes.Get(i), BASE_PORT + i));
            g_lastRx.push_back(0);
        }
        for (int src = 0; src < N_SRV; src++) {
            for (int dst = 0; dst < N_SRV; dst++) {
                if (src == dst) continue;
                double jitter = rng->GetValue(0.0, 0.1);
                addBulk(srvNodes.Get(src), srvIp[dst], BASE_PORT + dst, BYTES, jitter);
            }
        }

        // 故障/恢复时间表（压缩版，适配 160s 仿真）：
        //   t=20~30s  : Int[0] 的所有 Aggr 链路逐一断开
        //   t=60~70s  : Int[1] 的所有 Aggr 链路逐一断开
        //   t=100~110s: Int[0] 链路恢复
        //   t=130~140s: Int[1] 链路恢复
        for (int a = 0; a < g_nAggr; a++) {
            int idx0 = a * g_nInt + 0;   // Aggr[a] <-> Int[0]
            int idx1 = a * g_nInt + 1;   // Aggr[a] <-> Int[1]
            if (idx0 < (int)aggrIntLinks.size()) {
                Simulator::Schedule(Seconds(20.0 + a * 5.0),  &LinkDown, aggrIntLinks[idx0]);
                Simulator::Schedule(Seconds(100.0 + a * 5.0), &LinkUp,   aggrIntLinks[idx0]);
            }
            if (idx1 < (int)aggrIntLinks.size()) {
                Simulator::Schedule(Seconds(60.0 + a * 5.0),  &LinkDown, aggrIntLinks[idx1]);
                Simulator::Schedule(Seconds(130.0 + a * 5.0), &LinkUp,   aggrIntLinks[idx1]);
            }
        }

        g_interval = 1.0;
        simEnd     = 160.0;  // 缩短：t=50~110 故障，t=115~155 恢复，160s 足够

    } else {
        NS_FATAL_ERROR("Unknown experiment: " << experiment
                       << ".  Use --experiment=1|2|3|4");
    }

    /* ===================================================================
     *  PCAP (optional)
     * =================================================================== */
    if (pcap) {
        p2p.EnablePcapAll("vl2");
    }

    /* ===================================================================
     *  Flow monitor
     * =================================================================== */
    FlowMonitorHelper fmHelper;
    Ptr<FlowMonitor>  fm = fmHelper.InstallAll();

    /* ===================================================================
     *  Schedule first goodput log and run
     * =================================================================== */
    Simulator::Schedule(Seconds(g_interval), &LogGoodput);

    NS_LOG_INFO("Simulation end = " << simEnd << " s");
    Simulator::Stop(Seconds(simEnd));
    Simulator::Run();

    /* ===================================================================
     *  Post-processing
     * =================================================================== */
    fm->CheckForLostPackets();
    Ptr<Ipv4FlowClassifier> cls =
        DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());

    double txTotal = 0, rxTotal = 0;
    std::vector<double> throughputs;
    int nCompleted = 0;

    for (auto& [fid, st] : fm->GetFlowStats()) {
        txTotal += st.txBytes;
        rxTotal += st.rxBytes;

        // 用端口号区分数据流和 ACK 流：
        //   数据流 A→B：dstPort = sink 监听端口（5000~8000）
        //   ACK 流  B→A：dstPort = 临时端口（Linux 默认 32768~60999）
        // 只统计 dstPort < 30000 的流（即真正的数据方向）。
        Ipv4FlowClassifier::FiveTuple t = cls->FindFlow(fid);
        if (t.destinationPort >= 30000) continue;   // 跳过 ACK 方向流

        double dur = (st.timeLastRxPacket - st.timeFirstTxPacket).GetSeconds();
        if (dur > 0.5 && st.rxBytes > 0) {
            throughputs.push_back(st.rxBytes * 8.0 / dur);
            nCompleted++;
        }
    }

    double fairness = JainFairness(throughputs);

    // Max theoretical goodput: N_SRV servers each with 1 Gbps NIC
    double maxGbps = static_cast<double>(N_SRV) * 1.0;

    std::cout << "\n"
              << "====================================================\n"
              << " VL2 Simulation — Experiment " << experiment << "\n"
              << "====================================================\n"
              << " Topology  : " << g_nInt << " Int / " << g_nAggr << " Aggr / "
              << g_nTor << " ToR / " << N_SRV << " servers\n"
              << " Total TX  : " << txTotal / 1e9 << " GB\n"
              << " Total RX  : " << rxTotal / 1e9 << " GB\n"
              << " Flows done: " << nCompleted << "\n"
              << " Jain Fair : " << fairness
              << "  (paper target ≥0.995)\n"
              << " Max theor : " << maxGbps << " Gbps\n"
              << " Goodput log: " << logFile << "\n"
              << "====================================================\n";

    std::string xmlFile = "vl2_exp" + std::to_string(experiment) + "_flows.xml";
    fm->SerializeToXmlFile(xmlFile, true, true);

    g_outFile.close();
    Simulator::Destroy();
    return 0;
}
