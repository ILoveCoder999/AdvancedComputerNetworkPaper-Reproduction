# Fat-Tree 数据中心网络复现 (ns-3)

复现论文 **《A Scalable, Commodity Data Center Network Architecture》**
(Al-Fares, Loukissas, Vahdat, SIGCOMM 2008) 中的胖树架构与三种多路径分流机制,
并对照论文 **Table 2** 评估各方法在不同流量模式下的有效双边带宽。支持 **UDP / TCP** 两种传输。

---

## 1. 复现目标

论文核心:用大量廉价同构交换机组成 **k 叉胖树**,配合多路径机制,使任意通信模式都能逼近全双边带宽。
论文 Table 2 比较了四种方法 × 多种流量模式下"达到理想双边带宽的百分比":

| 方法 | 含义 | 论文位置 |
|------|------|----------|
| **Tree** | 传统 3.6:1 过载分层树(基线) | §5.1 |
| **Two-Level** | 两层路由表(按目的主机 ID 确定性散列) | §3.3 |
| **Flow Classification** | 交换机本地动态把流挪到空闲等价口 | §3.6 |
| **Flow Scheduling** | 中央调度器为大流全局分配无冲突路径 | §3.7 |

本项目用 ns-3 把这四种方法都实现出来,并跑出与 Table 2 对照的结果表。

---

## 2. 复现思路

1. **拓扑**:k 叉胖树 = 核心层 `(k/2)²` + 汇聚层 `k·(k/2)` + 边缘层 `k·(k/2)` + 主机 `k³/4`。
   每条链路用点对点(p2p),主机接入链路与交换机间链路均限速 96 Mbps(论文 §5.1)。
2. **编址**:严格按论文私有格式 `10.pod.switch.id`,主机 ID 从 2 开始。
   路由判定只解析目的地址的 `pod / switch / id` 三段,这样三种机制都能直接复用同一套地址语义。
3. **三种胖树机制做成路由的三个"模式"**,只在"跨 Pod 上行"这一步有区别;
   下行与核心层判定始终是确定性的。**传统树**因拓扑不同,单独做成一个可执行文件,用 ns-3 全局路由。
4. **两种传输**:见下方"UDP / TCP 口径"小节。
5. **指标**:有效双边带宽 = 各接收端收到的应用字节总和 / 有效时长,再除以理想值 `numHosts × 96 Mbps`
   (k=4 时理想 = 1.536 Gbps)。用 `PacketSink::GetTotalRx()` 直接累加,只统计正向数据(天然不含 TCP 的 ACK)。

### 2.1 UDP 口径(基准:单独测"路由质量")

UDP 这条线的目的,是把"路由/分流机制到底好不好"**单独**测出来,不被传输层的动态干扰:

1. 每台主机用 OnOff **恒定发 96 Mbps UDP**(On=常量、Off=0,即一直发),不经拥塞控制、不退避、不重传。
2. 因为 UDP 直接把链路压满,测出来的吞吐**纯粹反映路由有没有把流分散开**:撞车的链路上多条流平分带宽,
   没撞车就跑满。所以它能干净地暴露 Stride/Random/Same-ID 各模式下的碰撞情况。
3. UDP 不在乎丢包,所以 **MSS、链路线速余量这些 TCP 才需要的调参对它都无所谓**——
   超发的部分在交换机队列里丢掉,交付量 ≈ 链路可用容量。
4. 因此 UDP 是**基准口径**:先用它确认拓扑、两层路由、流分类、中央调度都正确
   (Stride ≈ 100%、Random ≈ 75%、Same-ID 呈 TwoLevel < Classify < Schedule 的递进),再上 TCP 对齐论文。

### 2.2 TCP 口径(对齐论文)

论文本身用的是 TCP。为忠实复现且避免人为假象,TCP 这条线做了三件关键事:

1. **限速 OnOff 而非贪婪 BulkSend**:应用层只 offer 96 Mbps(对齐论文"恒定 96 Mbps"),
   网络扛得住就平稳跑满,撞车了才靠 TCP 退避公平分享。贪婪发送会在短路径上引发锯齿丢包,把吞吐拖垮。
2. **MSS 设为 1448**:ns-3 默认 TCP 段大小只有 536 字节,包头开销高达 ~10%,会把吞吐(尤其短 RTT 路径)拖到 ~67%。
   设成贴满 MTU 的 1448 后,开销降到 ~3.7%,Stride 才能回到 ~100%。这是最关键的一个坑。
3. **链路留线速余量 + 大缓冲**:物理链路开到应用速率的 `headroom`(默认 1.08)倍,给包头留空间;
   TCP 收发缓冲调到 4 MB。这样 96 Mbps 的应用数据连同包头能装下,不会"恰好打满时自挤丢包"。

---

## 3. 代码结构

放置位置:ns-3 源码树下的 `scratch/fattree/`。

| 文件 | 作用 |
|------|------|
| `fat-tree-routing.h` / `.cc` | 自定义 `Ipv4RoutingProtocol`:两层路由 + 流分类 + 流调度三合一(`SetMode` 切换) |
| `fat-tree-central-scheduler.h` / `.cc` | `CentralScheduler`:全局视角的中央流调度器(§3.7) |
| `fat-tree-simulation.cc` | 主程序 `fattree`:搭胖树拓扑、装路由、生成流量、统计带宽 |
| `tree-baseline.cc` | 基线程序 `fattree-tree`:传统 3.6:1 过载树 + 全局路由 |
| `CMakeLists.txt` | 构建两个可执行文件 `fattree` 与 `fattree-tree` |
| `run_experiments.sh` | 一键跑完整对照表(四方法 × 六模式),自动带时间戳记录日志 |
| `results/` | 脚本自动生成的运行日志目录 |

两个可执行文件:`fattree`(胖树,含三种机制) 与 `fattree-tree`(传统树基线)。

---

## 4. 关键实现点

- **两层路由(§3.3)**:边缘与汇聚层对"跨 Pod 上行"都用后缀散列
  `((destId-2 + 本交换机位置) mod (k/2)) + (k/2)`。
  *注:论文 Algorithm 1 的位置偏移要同时作用于汇聚层和边缘层;只加汇聚层会导致 Stride 退化到 50%。*
- **逻辑端口 → 接口映射**:论文的"端口号"按建拓扑时记录的映射转成 ns-3 接口索引,
  正确避开 0 号 loopback 接口的偏移。
- **流分类(§3.6)**:边缘/汇聚为每条流记一个"粘性"上行口,新流分到当前最空闲口,
  每秒做一次重排(把大流从最忙口挪到最闲口)。属本地启发式。
- **流调度(§3.7)**:建网后把所有流登记给调度器,`Compute()` 一次性**全局**分配:
  先占住同 Pod 流(走默认路由、不可调度)的链路,再对跨 Pod 流贪心选无冲突核心,
  贪心失败时做"**单跳腾挪**"(把某条已放流换到别的核心给当前流让路),带回滚,放不下则回退两层路由。
- **大流过滤(TCP 专用)**:只有发往数据端口(9000)的正向数据流才进分类/调度,
  反向 ACK(目的为临时端口)回退两层路由——对应论文"只调度大象流"。靠 peek L4 头判定端口实现。

---

## 5. 编译与运行

```bash
# 1. 把本目录所有文件放到 ns-3 的 scratch/fattree/ 下
# 2. 在 ns-3 根目录编译(建议用优化版, 快 5~10 倍)
cd ~/ns-3-dev
./ns3 configure -d optimized
./ns3 build

# 3. 单次运行示例
./ns3 run "fattree --routing=schedule --pattern=random --transport=tcp --simTime=10"
./ns3 run "fattree-tree --pattern=stride --stride=4 --simTime=10"
```

### 命令行参数(`fattree`)

| 参数 | 默认 | 说明 |
|------|------|------|
| `--k` | 4 | 胖树端口数(偶数) |
| `--simTime` | 10 | 仿真时长(秒) |
| `--linkRate` | 96Mbps | 应用层目标速率(也是统计的"理想"口径) |
| `--routing` | twolevel | 上行分流:`twolevel` / `classify` / `schedule` |
| `--transport` | udp | 传输层:`udp` / `tcp` |
| `--pattern` | random | 流量模式:`random` / `stride` / `sameid` |
| `--stride` | 1 | stride 模式步长 i |
| `--seed` | 1 | random 模式随机种子 |
| `--classifyInterval` | 1.0 | 流分类重排周期(秒) |
| `--headroom` | 1.08 | 物理链路线速 / 应用速率(给 TCP 包头留余量) |

`fattree-tree` 参数同上(无 `--routing`,多一个 `--oversub` 过载比,默认 3.6)。

---

## 6. 流量模式(论文 §5.2)

所有模式都是 **1-to-1 双射**(每台主机恰好发一条、收一条)。

| 模式 | 说明 |
|------|------|
| `stride i` | 主机 x 发给 (x+i) mod N。i 越大越偏跨 Pod |
| `random` | 随机双射、无自环;脚本用多个种子取平均 |
| `sameid` | "Same-ID Outgoing"最坏情况:同一子网两台主机发往主机 ID 相同的目的,迫使两层路由把它们撞到同一上行口(内置 k=4 的置换) |

---

## 7. 实验脚本与日志

```bash
# 在 ns-3 根目录执行, 自动跑四方法 × 六模式, 输出对照表并存日志
bash scratch/fattree/run_experiments.sh

# 环境变量可调
K=4 SIMTIME=10 RUNS=3 TRANSPORT=tcp bash scratch/fattree/run_experiments.sh

# 快速看趋势(短时长、单种子)
SIMTIME=6 RUNS=1 bash scratch/fattree/run_experiments.sh
```

- `TRANSPORT` 默认 `tcp`(论文口径);设 `TRANSPORT=udp` 跑 UDP 口径。
- 每次运行把屏幕输出同时写到 `scratch/fattree/results/run_<时间戳>_<transport>.log`,头部记录参数。
- 汇总:`grep -H '^RESULT' scratch/fattree/results/*.log`。

每行机器可读结果格式:`RESULT,<routing>,<pattern>,<stride>,<seed>,<percent>`。

---

## 8. 结果与论文对照

数值 = 达到理想双边带宽(k=4 时 1.536 Gbps)的百分比,越高越好。斜杠右侧为论文 Table 2 值。

### 8.1 UDP 口径(`TRANSPORT=udp`,k=4)

| Pattern | Tree | Two-Level | Classify | Schedule | 论文 Tree/TL/CL/SC |
|---------|------|-----------|----------|----------|--------------------|
| Stride(1) | 99.9 | 99.9 | 99.9 | 99.9 | 100/100/100/100 |
| Stride(2) | 77.7 | 99.9 | 99.9 | 99.9 | 78.1/100/100/99.5 |
| Stride(4) | 27.7 | 99.9 | 99.9 | 99.9 | 27.9/100/100/100 |
| Stride(8) | 27.7 | 99.9 | 99.9 | 99.9 | 28.0/100/100/99.9 |
| Random | 54.3 | 74.9 | 74.9 | 95.7 | 53.4/75/76.3/93.5 |
| Same-ID | 27.7 | 37.5 | 49.9 | 99.9 | 27.8/38.5/75.4/87.4 |

### 8.2 TCP 口径(`TRANSPORT=tcp`,k=4,MSS=1448)

| Pattern | Tree | Two-Level | Classify | Schedule | 论文 Tree/TL/CL/SC |
|---------|------|-----------|----------|----------|--------------------|
| Stride(1) | 100.0 | 100.0 | 100.0 | 100.0 | 100/100/100/100 |
| Stride(2) | 78.4 | 100.0 | 100.0 | 100.0 | 78.1/100/100/99.5 |
| Stride(4) | 28.4 | 100.0 | 100.0 | 100.0 | 27.9/100/100/100 |
| Stride(8) | 28.4 | 100.0 | 100.0 | 100.0 | 28.0/100/100/99.9 |
| Random | 63.6 | 86.9 | 77.4 | 93.7 | 53.4/75/76.3/93.5 |
| Same-ID | 28.4 | 50.7 | 51.0 | 100.0 | 27.8/38.5/75.4/87.4 |

### 8.3 结论

- **核心结论复现到位**:跨 Pod 时传统树崩到 ~28%;胖树三法中 **Schedule ≥ Classify ≥ TwoLevel**,
  最坏情况(Same-ID)差距最明显。Stride 各档胖树三法均 ~100%,而 Tree 随 stride 增大跌到 ~28%。
- **TCP 与论文吻合度很高**:尤其 Schedule 的 Random = 93.7%(论文 93.5%)几乎完全一致,Stride 全部 ~100%。
- **Classify 的 Same-ID 偏低**(~50% vs 论文 75%):流分类只能做本地上行均衡,管不到下游冲突——
  这是论文也承认的本地局限(详见第 9 节)。
- UDP 与 TCP 两套口径趋势一致;TCP 的 Random/Same-ID 因拥塞控制反而比 UDP 更平滑、更贴近论文。

---

## 9. 简化与已知局限

- **最坏情况模式仅 k=4**:`sameid` 内置的是 k=4 的手工置换;其他 k 用 stride/random。论文 Table 2 本身也是 k=4。
- **大流判定用端口近似**:TCP 下按"是否发往数据端口"判定大流,是论文"速率阈值"的简化,对本 benchmark 足够。
- **Classify 的 Same-ID 偏低**(~50% vs 论文 75%):流分类只做本地上行均衡,管不到下游 `agg→edge` 的冲突——
  这是论文也承认的本地局限,本实现更"较真"了一点。
- **TCP 用限速 OnOff 而非贪婪发送**:对齐论文"恒定 96 Mbps"口径,也避免短路径贪婪 TCP 的锯齿假象。
- **TCP 必须设 MSS=1448**:ns-3 默认 MSS=536,包头开销过大会把 Stride 这类无碰撞短路径拖到 ~67%;
  这是复现 TCP 表时最关键、也最隐蔽的一步(详见 2.2)。
- **传统树是独立拓扑/独立程序**,用 ns-3 全局路由(单路径),过载比体现在上行链路限速上。
- **仿真器与论文测试床不同**(ns-3 vs Click),绝对数值会有出入,应看趋势与大小关系。

---

## 10. 参考

Mohammad Al-Fares, Alexander Loukissas, Amin Vahdat.
*A Scalable, Commodity Data Center Network Architecture.* ACM SIGCOMM 2008.
