/* =============================================================================
 * ns3-logging-demo.cc
 *
 * 目的：全面展示 ns-3 日志系统的每一个用法。
 * 用法：将本文件放入 ns-3 根目录的 scratch/ 文件夹，然后执行：
 *
 *   # 方式1：代码内已启用日志（直接运行即可看到输出）
 *   ./ns3 run scratch/ns-3-logging-demo/ns3-logging-demo
 *
 *   # 方式2：通过环境变量控制（不修改代码）
 *   NS_LOG="LoggingDemo=all|prefix_time|prefix_func" \
 *   ./ns3 run scratch/ns-3-logging-demo/ns3-logging-demo
 *
 *   # 方式3：同时开启多个组件
 *   NS_LOG="LoggingDemo=all:UdpEchoClientApplication=info" \
 *   ./ns3 run scratch/ns-3-logging-demo/ns3-logging-demo
 *
 * ns-3 日志级别从低到高（数字越大越严重，开启某级别会同时输出该级别及以上）：
 *   LOG_LOGIC   (1)  内部逻辑流
 *   LOG_DEBUG   (2)  调试细节
 *   LOG_FUNCTION(3)  函数进入/退出
 *   LOG_INFO    (4)  一般信息
 *   LOG_WARN    (5)  警告
 *   LOG_ERROR   (6)  错误
 *   LOG_UNCOND  (*)  无条件输出，不受级别开关影响
 * =============================================================================
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

using namespace ns3;

// -----------------------------------------------------------------------------
// ❶ NS_LOG_COMPONENT_DEFINE
//    为本文件声明一个具名日志组件"LoggingDemo"。
//    后续所有 NS_LOG_* 宏输出的内容都归属于此组件。
//    每个 .cc 文件只能定义一次，且必须在全局作用域。
// -----------------------------------------------------------------------------
NS_LOG_COMPONENT_DEFINE("LoggingDemo");

// =============================================================================
// 自定义类：演示类内部各日志宏的惯用写法
// =============================================================================
class Router : public Object
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid =
            TypeId("Router").SetParent<Object>().AddConstructor<Router>();
        return tid;
    }

    Router()
        : m_id(0),
          m_txBytes(0)
    {
        // ---------------------------------------------------------------------
        // ❷ NS_LOG_FUNCTION(this)
        //    记录"进入构造函数"，自动打印函数签名 + 对象指针。
        //    惯例：成员函数第一行写 NS_LOG_FUNCTION(this)。
        // ---------------------------------------------------------------------
        NS_LOG_FUNCTION(this);
    }

    ~Router()
    {
        NS_LOG_FUNCTION(this);
    }

    // 带参数的函数：把参数也传给 NS_LOG_FUNCTION，用 << 连接
    void Configure(uint32_t id, std::string name)
    {
        // ❷b NS_LOG_FUNCTION(this << arg1 << arg2 ...)
        NS_LOG_FUNCTION(this << id << name);

        m_id   = id;
        m_name = name;

        // ---------------------------------------------------------------------
        // ❸ NS_LOG_INFO
        //    一般性信息，描述"发生了什么"。
        //    适用于用户关心的关键流程节点。
        // ---------------------------------------------------------------------
        NS_LOG_INFO("路由器 [" << m_name << "] 配置完成，ID=" << m_id);
    }

    void Forward(uint32_t pktSize, uint32_t dstNode)
    {
        NS_LOG_FUNCTION(this << pktSize << dstNode);

        // ---------------------------------------------------------------------
        // ❹ NS_LOG_LOGIC
        //    描述内部判断/流程分支，粒度最细。
        //    仅在排查算法逻辑时开启，生产仿真中一般关闭。
        // ---------------------------------------------------------------------
        NS_LOG_LOGIC("检查转发表，目标节点=" << dstNode);

        if (pktSize == 0)
        {
            // -----------------------------------------------------------------
            // ❺ NS_LOG_ERROR
            //    严重错误，操作无法继续。优先级最高（UNCOND 除外）。
            //    注意：ns-3 不会自动终止仿真，需要自行处理。
            // -----------------------------------------------------------------
            NS_LOG_ERROR("丢弃！包大小为0，节点=" << m_id);
            return;
        }

        if (pktSize > 9000)
        {
            // -----------------------------------------------------------------
            // ❻ NS_LOG_WARN
            //    警告：操作仍可继续，但存在潜在问题。
            // -----------------------------------------------------------------
            NS_LOG_WARN("Jumbo frame 检测到，大小=" << pktSize
                        << "，可能需要分片。节点=" << m_id);
        }

        // ---------------------------------------------------------------------
        // ❼ NS_LOG_DEBUG
        //    调试细节：内部变量值、中间计算结果。
        //    比 INFO 更细，通常只在开发阶段开启。
        // ---------------------------------------------------------------------
        m_txBytes += pktSize;
        NS_LOG_DEBUG("转发成功 size=" << pktSize
                     << "  累计txBytes=" << m_txBytes
                     << "  dst=" << dstNode);

        NS_LOG_LOGIC("更新路由统计表完成");
    }

    // 无有意义参数的函数使用 FUNCTION_NOARGS
    void Reset()
    {
        // ---------------------------------------------------------------------
        // ❽ NS_LOG_FUNCTION_NOARGS()
        //    当函数无参数（或参数对调试无意义）时，替代 NS_LOG_FUNCTION(this)。
        // ---------------------------------------------------------------------
        NS_LOG_FUNCTION_NOARGS();
        m_txBytes = 0;
        NS_LOG_INFO("[" << m_name << "] 统计已重置");
    }

    uint64_t GetTxBytes() const
    {
        return m_txBytes;
    }

  private:
    uint32_t    m_id;
    std::string m_name;
    uint64_t    m_txBytes;
};

NS_OBJECT_ENSURE_REGISTERED(Router);

// =============================================================================
// 仿真事件回调：展示在普通函数中使用日志
// =============================================================================
static void
OnPacketSent(Ptr<Router> router, uint32_t size, uint32_t dst)
{
    // 普通函数也可以用 NS_LOG_FUNCTION，参数直接列出（无 this）
    NS_LOG_FUNCTION(size << dst);
    NS_LOG_INFO("t=" << Simulator::Now().As(Time::S)
                << "  发送包  size=" << size << "  dst=" << dst);
    router->Forward(size, dst);
}

static void
PrintStats(Ptr<Router> router)
{
    NS_LOG_FUNCTION_NOARGS();

    // -------------------------------------------------------------------------
    // ❾ NS_LOG_UNCOND
    //    无条件输出：无论日志组件是否开启、级别如何设置都会打印。
    //    适用于仿真结束后必须显示的统计摘要。
    // -------------------------------------------------------------------------
    NS_LOG_UNCOND("===== 统计摘要 =====");
    NS_LOG_UNCOND("总发送字节数: " << router->GetTxBytes());
    NS_LOG_UNCOND("====================");
}

// =============================================================================
// main
// =============================================================================
int
main(int argc, char* argv[])
{
    NS_LOG_UNCOND("hello");
    // =========================================================================
    // 第一部分：通过代码启用/配置日志
    // =========================================================================

    // -------------------------------------------------------------------------
    // ❿ LogComponentEnable(组件名, 级别)
    //    在代码中显式开启某组件的日志，适合固化到脚本里的调试配置。
    //
    //    可用级别常量：
    //      LOG_LEVEL_ERROR    只看 ERROR
    //      LOG_LEVEL_WARN     WARN + ERROR
    //      LOG_LEVEL_INFO     INFO + WARN + ERROR
    //      LOG_LEVEL_DEBUG    DEBUG + INFO + WARN + ERROR
    //      LOG_LEVEL_FUNCTION FUNCTION + DEBUG + INFO + WARN + ERROR
    //      LOG_LEVEL_LOGIC    全部（LOGIC 是最低级别）
    //      LOG_LEVEL_ALL      等同于 LOG_LEVEL_LOGIC
    // -------------------------------------------------------------------------

    // 为本文件的组件开启所有级别
    //LogComponentEnable("LoggingDemo", LOG_LEVEL_ALL);

    // 为内置 UDP Echo 组件只开启 INFO 及以上
    //LogComponentEnable("UdpEchoClientApplication", LOG_LEVEL_INFO);
    //LogComponentEnable("UdpEchoServerApplication", LOG_LEVEL_INFO);

    // -------------------------------------------------------------------------
    // ⓫ 日志前缀（Log Prefix）—— 用位或 | 叠加多个前缀
    //
    //    LOG_PREFIX_TIME   输出仿真时间戳，如 "+2.000000000s"
    //    LOG_PREFIX_NODE   输出当前节点 ID，如 "[node 0]"
    //    LOG_PREFIX_FUNC   输出函数名，如 "LoggingDemo:main()"
    //    LOG_PREFIX_LEVEL  输出日志级别，如 "[INFO]"
    //
    //    注意：添加前缀时需用位或同时包含级别标志，否则前缀被忽略。
    // -------------------------------------------------------------------------
    /*
    LogComponentEnable("LoggingDemo",
                       (LogLevel)(LOG_PREFIX_TIME  |
                                  LOG_PREFIX_NODE  |
                                  LOG_PREFIX_FUNC  |
                                  LOG_PREFIX_LEVEL |
                                  LOG_LEVEL_ALL));
    */
    // -------------------------------------------------------------------------
    // ⓬ LogComponentEnableAll(级别)
    //    一次性开启所有组件的日志（输出量极大，谨慎使用）。
    //    通常配合环境变量使用：NS_LOG="*=all"
    //
    //    以下行被注释掉，取消注释可看到 ns-3 内部所有组件的日志：
    // LogComponentEnableAll(LOG_LEVEL_WARN);
    // -------------------------------------------------------------------------

    // =========================================================================
    // 第二部分：CommandLine 集成 —— 运行时动态指定日志组件
    // =========================================================================

    // -------------------------------------------------------------------------
    // ⓭ CommandLine + --LogLevel 参数（ns-3 内置支持）
    //    运行时可这样控制：
    //      ./ns3 run "scratch/ns3-logging-demo --ns3::LogLevel=LoggingDemo=warn"
    //
    //    也可以用 CommandLine 加自定义参数控制日志：
    // -------------------------------------------------------------------------
    bool verbose = false;
    CommandLine cmd(__FILE__);
    cmd.AddValue("verbose", "true 时额外开启 DEBUG 级别", verbose);
    cmd.Parse(argc, argv);

    if (verbose)
    {
        LogComponentEnable("LoggingDemo", LOG_LEVEL_DEBUG);
        NS_LOG_DEBUG("verbose 模式已开启");
    }

    // =========================================================================
    // 第三部分：构建仿真拓扑
    // =========================================================================

    NS_LOG_INFO("========== 仿真初始化开始 ==========");

    NS_LOG_DEBUG("Step1: 创建 2 个节点");
    NodeContainer nodes;
    nodes.Create(2);

    NS_LOG_DEBUG("Step2: 配置 P2P 链路");
    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", StringValue("10Mbps"));
    p2p.SetChannelAttribute("Delay",   StringValue("5ms"));
    NetDeviceContainer devices = p2p.Install(nodes);

    NS_LOG_DEBUG("Step3: 安装 IP 协议栈");
    InternetStackHelper stack;
    stack.Install(nodes);

    Ipv4AddressHelper addr;
    addr.SetBase("10.0.0.0", "255.255.255.0");
    Ipv4InterfaceContainer ifaces = addr.Assign(devices);

    NS_LOG_INFO("节点0 IP: " << ifaces.GetAddress(0)
                << "  节点1 IP: " << ifaces.GetAddress(1));

    // =========================================================================
    // 第四部分：演示自定义类中的各日志宏
    // =========================================================================

    NS_LOG_INFO("---------- 自定义 Router 类日志演示 ----------");

    Ptr<Router> router = CreateObject<Router>();
    router->Configure(0, "CoreRouter");

    // 正常包 → INFO + DEBUG
    Simulator::Schedule(Seconds(1.0), &OnPacketSent, router, 512,  1);

    // Jumbo frame → WARN
    Simulator::Schedule(Seconds(2.0), &OnPacketSent, router, 9100, 1);

    // 大包 + 正常
    Simulator::Schedule(Seconds(3.0), &OnPacketSent, router, 1500, 1);

    // 空包 → ERROR，会提前 return
    Simulator::Schedule(Seconds(4.0), &OnPacketSent, router, 0,    1);

    // 重置统计
    Simulator::Schedule(Seconds(5.0), &Router::Reset, router);

    // =========================================================================
    // 第五部分：内置 UDP Echo 应用（展示内置组件日志）
    // =========================================================================

    NS_LOG_INFO("---------- UDP Echo 应用日志演示 ----------");

    UdpEchoServerHelper server(9);
    ApplicationContainer serverApps = server.Install(nodes.Get(1));
    serverApps.Start(Seconds(1.0));
    serverApps.Stop(Seconds(10.0));

    UdpEchoClientHelper client(ifaces.GetAddress(1), 9);
    client.SetAttribute("MaxPackets", UintegerValue(3));
    client.SetAttribute("Interval",   TimeValue(Seconds(1.0)));
    client.SetAttribute("PacketSize", UintegerValue(256));
    ApplicationContainer clientApps = client.Install(nodes.Get(0));
    clientApps.Start(Seconds(2.0));
    clientApps.Stop(Seconds(10.0));

    // =========================================================================
    // 第六部分：运行仿真
    // =========================================================================

    NS_LOG_INFO("========== 开始运行仿真 ==========");
    Simulator::Stop(Seconds(11.0));
    Simulator::Run();
    Simulator::Destroy();
    NS_LOG_INFO("========== 仿真结束 ==========");

    // ❾ NS_LOG_UNCOND 打印最终统计（无论日志开关状态）
    PrintStats(router);

    return 0;
}

/* =============================================================================
 * 附：环境变量速查表（Shell 中运行时使用，无需修改代码）
 *
 * 开启单个组件全部级别：
 *   NS_LOG="LoggingDemo=all" ./ns3 run scratch/ns3-logging-demo
 *
 * 开启单个组件并附加前缀（时间+函数名）：
 *   NS_LOG="LoggingDemo=all|prefix_time|prefix_func" \
 *   ./ns3 run scratch/ns3-logging-demo
 *
 * 开启多个组件（冒号分隔）：
 *   NS_LOG="LoggingDemo=info:UdpEchoClientApplication=all" \
 *   ./ns3 run scratch/ns3-logging-demo
 *
 * 开启全部组件（慎用，输出量巨大）：
 *   NS_LOG="*=all" ./ns3 run scratch/ns3-logging-demo
 *
 * 级别关键字（环境变量中使用字符串而非枚举）：
 *   error | warn | info | debug | function | logic | all
 *
 * 前缀关键字：
 *   prefix_time | prefix_node | prefix_func | prefix_level
 *
 * 取消特定组件（在 all 基础上屏蔽）：
 *   NS_LOG="*=all:LoggingDemo=none" ./ns3 run scratch/ns3-logging-demo
 * =============================================================================
 */
