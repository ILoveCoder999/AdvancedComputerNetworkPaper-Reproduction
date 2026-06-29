# 跨拓扑 AllReduce 对比 — 接线说明与运行指南

把头文件 **真正接进** ns-3 仿真,用 **同一套 NCCL Ring AllReduce** 在
**九种拓扑**(leaf-spine / fat-tree / spectrum-x / rail-IB / dragonfly / RNG / 2D-mesh / 3D-mesh / 4D-mesh)上跑,
产出真实数据的 **对比图**(JCT / busbw / 每比特能耗 / 每瓦吞吐 / 对分带宽)。

> 本会话只做了 **ns-3 接线**(按你的选择)。沙箱里装不了 ns-3,所以最终的端到端
> 实跑与出图要在你自己装了 ns-3 的机器上完成。能在沙箱里验证的都验证了
> (见文末"已验证 / 待你验证")。

---

## 1. 接了什么

| 头文件 | 接入的 sim | 作用 |
|---|---|---|
| `nccl-collectives.h` | 九个全部 | `--scenario=nccl_ar`(rng 用 `--pattern=nccl_ar`):真 Ring AllReduce 流量(替朴素 `s→s+1`) |
| `energy-model.h` | 九个全部 | `main()` 末尾统一网络能耗 → 写 `energy_unified.csv` + `cmp_<topo>.csv` |
| `pfc-lossless.h` | 仅 leaf-spine(RoCE) | 真 PFC 状态机挂交换机入口,断言零丢 + 报 PAUSE/峰值占用 |
| `cmp-common.h`(新) | 九个全部 | 对比共享层:rank 跨网铺开、busbw 计算、`cmp_<topo>.csv` 写出 |

八种拓扑 + 各自的无损机制(按其本性保留,**不强行统一**——这才是正确的对比):

| cmp 标签 | sim | 拓扑/规模 | fabric / 无损 | 能耗清单 |
|---|---|---|---|---|
| `leaf_spine` | leaf-spine-sim | 2-tier Clos, **N=1296**(K=72,nLeaf=36) | RoCE + **真 PFC** | LeafSpine |
| `fat_tree` | fat_tree_sim | 3-tier Clos, **N=1024**(K=16) | RoCE/DCQCN(深缓冲) | FatTree |
| `spectrum_x` | spectrum-x-sim | 2-tier Clos, N=1332 | RoCE/SHIELD(激进 ECN) | LeafSpine |
| `rail` | rail-ib-sim | R=4 条独立 Clos, N=1332 | IB credit | Rail(R 条全计入) |
| `dragonfly` | dragonfly-ib-sim | 低直径, N=1332 | IB credit | Dragonfly |
| `rng` | rng-sim | 扁平随机正则图, N=100,d=16 **(待统一)** | flat / ECMP 喷射 | Switchless |
| `mesh2d` | mesh-sim | **36×36 每轴全连, N=1296** | DOR + **逐跳信用**(无损) | Switchless |
| `mesh3d` | mesh3d-sim | 11³ 每轴全连, N=1331 | DOR3 + **逐跳信用**(无损) | Switchless |
| `mesh4d` | mesh4dw6-sim | 6⁴=1296 每轴全连 | DOR4 + **逐跳信用**(无损) | Switchless |

统一的是 **工作负载(同一 all-reduce:相同 arRanks、相同 M、相同 Ring 算法)**、
**能耗口径**,以及(本版起)**物理规模 N**——这才是 apples-to-apples 的正确含义。

> **统一规模 N≈1296(重要)。** 早期各 sim 规模差到 13×(N=100~1332),这会让
> **能效类指标失真**:静态功耗按全网 N 台计,但只有 arRanks=32 个 rank 在干活,
> N 越大空载硬件越多 → pJ/bit 偏高、Gbps/W 偏低,**与拓扑优劣无关**。因此本版把
> 规模拉齐到 N≈1296:`leaf_spine` `--K=72 --nLeaf=36`→1296、`fat_tree` `K=16`→1024
> (K³/4 量化命不中 1296,**唯一离群**,其绝对量按 ~−20% 解读)、`mesh2d` `A=36`→1296,
> 其余(spectrum/rail/dragonfly 1332、mesh3d 1331、mesh4d 1296)本就在带内。
> 9 个里 8 个落 1296~1332(±3%)。**`rng` 暂仍 N=100**(需其源码统一规模旋钮),在统一前
> 其能效绝对量偏乐观,横比应排除或加注。

> **rail 特别说明**:rail 的 all-reduce 跑成 **R 条并行环**(每 rail 一条),这是
> rail-optimized 的核心优势(环间隔离、无竞争);能耗按 **全部 R 条 rail** 的硬件计入
> (rail 的成本代价)。其它八种是单环穿越各自 fabric。

---

## 2. 三步跑出对比图

### ① 编译(把 sim + 头拷进 ns-3 scratch 并 build)

```bash
./build_comparison.sh /path/to/ns-3-dev
```

脚本把九个 `*-sim.cc` 和六个头文件(含 `link-credit.h`)放进 `scratch/` 顶层
(引号包含会从同目录解析,**无需 CMakeLists**),然后 `./ns3 build`。

> **先冒烟再放大(强烈建议)。** 规模已做成**运行时可调**:mesh 用 `--side`、fat_tree/
> leaf-spine 用 `--K`(默认即统一规模 N≈1296,无需重编译)。先跑极小规模确认整条管线:
> ```bash
> NS3_DIR=/path/to/ns-3-dev ./smoke_test.sh    # 几十节点、arRanks=8、modelMB=1,几分钟
> ```
> 它会编译→小跑 6 个 sim(`--side=3~6`、fat_tree `--K=4`、leaf-spine `--K=8 --nLeaf=4`)
> →断言 cmp_*.csv 产出且 mesh 信用无损 dropped=0→出两张图,末尾报 PASS/FAIL。
> 通过后再跑下面的统一 N 真实规模。(`SMOKE_FULL=1` 连 spectrum/rail/dragonfly 一起冒烟。)

### ② 跑同一套 all-reduce(关键:三者参数完全相同)

```bash
cd /path/to/ns-3-dev
./ns3 run "leaf-spine-sim   --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
./ns3 run "fat_tree_sim     --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
./ns3 run "spectrum-x-sim   --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
./ns3 run "rail-ib-sim      --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
./ns3 run "dragonfly-ib-sim --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
./ns3 run "mesh-sim         --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps"
./ns3 run "mesh3d-sim       --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
./ns3 run "mesh4dw6-sim     --scenario=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
./ns3 run "rng-sim --topology=rng --pattern=nccl_ar --arRanks=32 --modelMB=8 --linkRate=400Gbps --queuePkts=256"
```

> **务必九者用相同的 `--arRanks` 与 `--modelMB`**,否则不是 apples-to-apples
> (`compare_topologies.py` 会检测并警告)。`mesh` 默认线速 100G、`fat_tree`/`rng` 默认
> 100G,做公平 busbw 对比时统一加 `--linkRate=400Gbps`。rng 用 `--pattern=nccl_ar`
> 且建议 `--queuePkts=256`(默认 8 是为 fig13 突发丢包实验设的,太小)。**mesh3d / mesh4dw6
> 同理**:默认队列 8p,对比时加 `--queuePkts=256`(无 PFC/credit,队列太浅会丢包)。
>
> 规模已统一到 **N≈1296**(见 §1 的统一规模说明):leaf_spine 加 `--K=72 --nLeaf=36`、
> fat_tree `K=16`(1024,离群)、mesh2d `A=36`;其余本就在带内。**rng 暂仍 N=100,横比排除/加注**。

每个 sim 在 ns-3 根目录写出 `cmp_<topo>.csv`(各一行,如 `cmp_leaf_spine.csv`、
`cmp_fat_tree.csv`、`cmp_spectrum_x.csv`、`cmp_rail.csv`、`cmp_dragonfly.csv`、
`cmp_rng.csv`、`cmp_mesh2d.csv`、`cmp_mesh3d.csv`、`cmp_mesh4d.csv`),并各追加一行到
`energy_unified.csv`。

### ③ 出对比表 + 图

```bash
cp cmp_leaf_spine.csv cmp_mesh2d.csv cmp_dragonfly.csv /path/to/aidatacenter/
cd /path/to/aidatacenter
python3 compare_topologies.py            # 默认读当前目录 cmp_*.csv → topo_comparison.png
# 或不拷贝,直接指目录:
python3 compare_topologies.py --dir /path/to/ns-3-dev --out topo_comparison.png
```

输出:一张 6 面板对比图 + 终端对比表。面板含 JCT、busbw、busbw 效率、
pJ/bit、Gbps/W、对分带宽;斜纹标该指标最优者。

---

## 3. 方法论(论文里要站得住的点)

**为什么用 `arRanks` 而不是各拓扑全部节点。** 完整 ring all-reduce 是 2(P-1) 步、
每步每 rank 传 M/P;若 P=512、M=256MB,报文级仿真要跑数亿包,跑不完。所以对比
all-reduce 固定一个可跑完的 `arRanks`(默认 32)+ 足够大的 `M`(进入带宽受限区),
三种拓扑用同一组参数。这正是 `SCALE_DOWN.md` / `scale_planner.py` 的缩放方法论:
小规模、带宽受限的代表性运行去推大规模。

**rank 跨网铺开(`StridedRanks`)。** 32 个逻辑 rank 不是取前 32 个节点(那样会
恰好全落在一个 leaf / 一个 group 上,不经骨干,对比失真),而是 `i*N/arRanks`
均匀铺开,让 all-reduce 真的穿越 fabric。

**为什么必须统一规模 N(纠正早期说法)。** 早期文档称"pJ/bit、Gbps/W 规模稳健"——
**这是错的**:静态功耗按全网 N 台器件计,而 all-reduce 只用 arRanks=32 个 rank,
故 pJ/bit ∝ N、Gbps/W ∝ 1/N,**规模越大能效看着越差,与拓扑无关**。busbw 效率去掉了
线速混淆但没去掉规模混淆(随路径长度/jct 变)。所以横比的前提是 **先把 N 拉齐**
(本版已做,见 §1)。规模对齐后:busbw 效率、pJ/bit、Gbps/W 才可横比;**JCT 与 busbw
绝对值** 仍随线速变(故统一 400G),也随拓扑直径/铺开方式变(这正是要比较的拓扑性质)。
每 GPU 归一量(`PowerPerGpuW` 等)对残余 N 差最稳健,可作交叉验证。

**GPU/端点归一化。** 为跨拓扑一致,三者都按"1 个 rank 端点 = 1 GPU 当量"归一
(leaf-spine server、dragonfly server、mesh host 各算 1)。mesh 的 host 实际是
8 GPU/32 端口,若要按真实 GPU 数归一(2312),把 `mesh` 的 `nGpu` 改成 2312 即可
(只影响 per-GPU 派生量,不影响 busbw/pJ-per-bit)。

**开环回放。** all-reduce 按"开环全注入/每源线速起拍"回放(不强加 step 间
barrier)。头文件说明:吞吐/能耗对比下开环足够;要更真实的逐步 FCT,可改成按
`CommOp.step` 加 barrier。

---

## 4. PFC:观测层 + 升级到主动门控

leaf-spine 把真 `pfc::PfcPort` 状态机挂到 **每个交换机入口队列盘** 的真
Enqueue/Dequeue 事件上,运行后报告:

- `drops`(应恒为 0)——真无损的断言;
- 峰值入口占用 `peak occ`、累计 PAUSE 时长 `sumPausedSec`、触发 PAUSE 的端口数
  ——这些正是深 DropTail "近似无损" 掩盖掉的 HOL/拥塞扩散信号(Guo SIGCOMM'16)。

当前是 **观测 + 断言** 层(状态机如实跟踪占用并报指标)。若要让 PFC 真正
**反压上游**(完整复现 HOL 对吞吐的影响),把 `PfcPort::SetSendPause` 回调接到
上游 `NetDevice` 的发送门控(暂停/恢复该端口 TX)即可——代码位置和 TODO 已在
`leaf-spine-sim.cc` 的 PFC 段注释标出。这一步需在你的 ns-3 版本上验证。

---

## 4b. mesh 家族:逐跳链路信用(无损,JCT 可比)

mesh-sim(2D)/ mesh3d-sim / mesh4dw6-sim 三者共用 **`link-credit.h`** 的逐跳
(hop-by-hop)链路级信用流控,默认开启(`--credit=1`)。

**为什么不是端到端信用。** `mesh-credit.h` 那套是 *端到端 per-destination* 信用,
对 incast(多源→一宿)有效;但 Ring AllReduce 是 **置换模式**——每个宿只有一个源,
宿端永不拥塞,真正的拥塞在 **被多条环流共享的中继链路** 上。端到端信用管不到中继
链路,对 all-reduce 形同虚设。只有 *逐跳链路信用* 能对中继链路反压 → 这才是直连
网格(IB/credit)真正的无损机制,也才让 JCT 反映真实拥塞,与 leaf-spine 的 PFC 同档可比。

**机制(标准 credit-based link-level FC)。** 每条有向链路 `self→nbr` 有
`creditPkts` 个信用 = 下游为该链路预留的缓冲槽;发包前必须持 1 信用(否则在该链路
队列等待);下游把包再转发出去 / 最终交付时腾出槽,延迟一个链路时延后回 1 信用给上游
(本仿真用 `Simulator::Schedule` 把信用作为控制事件投递,不占数据带宽、不会丢)。
源按线速起拍注入,网内由信用层层反压。

**无死锁 + 无丢包。** DOR(维序路由)逐维推进,信道依赖图无环 → 逐跳信用无死锁
(标准结论)。在飞包数 ≤ 信用窗 ≤ 队列容量(代码里 `qDepth=max(queuePkts,creditPkts)`)
→ 数据永不被 DropTail 丢。**运行后 `dropped` 应=0**(无损断言,和 leaf-spine PFC 同口径)。

**参数。** `--credit=1|0`(默认 1;`=0` 退回有损直发做对照),
`--creditPkts=N`(每链路信用窗,默认取 `--queuePkts`,对比时 256)。

> 实现说明:三个 mesh sim 的中继统一为 **scan 式逐跳转发**(按 path 定位本节点,
> 取下一跳),信用层只管"记账 + 排队 + 触发实际发送/回信用",核心是纯 C++ 模板
> `LinkCredit<Pkt>`(无 ns-3 依赖,见 `link_credit_test.cc` 的离散事件单测)。

**信用消融(无损 vs 有损,自动出图)。** `batch_test_all.sh` 跑完 9 拓扑主对比后,
默认再做一段消融(`CREDIT_ABLATION=1`):用 **同一组参数** 把 `mesh-sim / mesh3d-sim /
mesh4dw6-sim` 各跑两遍(`--credit=1` 与 `--credit=0`),cmp CSV 分别收进
`results_batch/credit_on/` 与 `credit_off/`,再由 **`compare_credit.py`** 出
`credit_ablation.png`(JCT / busbw / 丢包率 三面板,蓝=无损 红=有损)。
关掉用 `CREDIT_ABLATION=0 ./batch_test_all.sh`;也可单独:

```bash
python3 compare_credit.py --on results_batch/credit_on --off results_batch/credit_off
```

> 解读要点:`credit=0` 会丢包 → all-reduce **不完整(结果错误)**,其 JCT/busbw 仅对
> 已交付子集统计,**不能**当"更快"。信用的首要收益是 **无损/正确**(`credit=1` dropped=0);
> JCT 的高低是次要的、且常因反压而略升——这正是无损的代价,如实呈现。

---

## 5. 调参

| 参数 | 含义 | 默认 | 适用 sim |
|---|---|---|---|
| `--scenario=nccl_ar` | 启用对比 all-reduce(rng 用 `--pattern=nccl_ar`) | — | 全部 |
| `--arRanks` | all-reduce 逻辑 rank 数 | 32 | 全部 |
| `--modelMB` | 每 rank 梯度 M (MiB) | 8 | 全部 |
| `--linkRate` | 链路线速 | 400G(mesh/fat_tree/rng 默认 100G) | 全部 |
| `--simStop` | 仿真停止时刻(s) | 3.0(mesh 25, fat_tree 10) | 除 rng |
| `--pfc` | 挂真 PFC(1/0) | 1 | leaf-spine |
| `--rttUs` / `--bufferBDPs` | PFC 阈值(BDP 口径) | 2.0 / 3.0 | leaf-spine |
| `--fpgaPerNode` | switchless 每节点转发引擎数(能耗敏感) | 8 | mesh / rng |
| `--opticalFrac` | 走光端口占比(短链可<1 走 DAC) | 1.0 | mesh/dragonfly/rail/spectrum-x/rng |
| `--queuePkts` | 队列深度(rng/mesh 默认 8 太小,对比时设 256) | — | rng/mesh 需调 |
| `--credit` | 逐跳链路信用(1=无损/0=有损对照) | 1 | mesh-sim/mesh3d/mesh4dw6 |
| `--creditPkts` | 每链路信用窗(包);0=取 queuePkts | 0 | mesh-sim/mesh3d/mesh4dw6 |

**先小后大**:`--arRanks=32 --modelMB=8` 约 50 万包,几分钟级,先确认管线通;
再加大 `modelMB`(更进带宽受限区,busbw 更稳)或 `arRanks`(更大规模),并相应
调大 `--simStop`。

---

## 6. 已验证(本会话) / 待你验证(你的机器)

**本会话已在沙箱里验证(不需要 ns-3):**

- 三个头 + 新 `cmp-common.h` 单独/一起 `g++ -std=c++14 -Wall` 干净编译;
- `test_cmp_common.cc`:Ring all-reduce 每 rank 字节量对解析公式 2(P-1)/P·M
  误差 0%、步数 = 2(P-1)、rank 跨网铺开正确、busbw 公式正确;
- `cmp_pipeline_check.cc`:用与三个 sim **完全相同** 的
  `cmp::` / `EnergyModel` / `pfc::` 调用跑通,三种拓扑的能耗清单都产出合理数字、
  PFC 聚合接口可用 → 证明 sim 里那段织入的纯 C++ 部分接口用法正确;
- `compare_topologies.py` 用样例 CSV 跑通:对比表 + 出图(中文正常、零缺字)。

```bash
# 想在跑 ns-3 之前先验证出图管线(用人造占位值):
g++ -std=c++14 -O2 cmp_pipeline_check.cc -o cmp_pipeline_check && ./cmp_pipeline_check .
python3 compare_topologies.py        # 看到表和图即说明管线 OK(数字是假的)
rm -f cmp_*.csv energy_unified.csv    # 清掉假数据,再跑真 ns-3
g++ -std=c++14 -O2 test_cmp_common.cc -o test_cmp_common && ./test_cmp_common
```

**待你在装了 ns-3 的机器上验证:**

- 七个 sim 在你的 ns-3 版本下编译通过(ns-3 专有 API:`Simulator::Schedule`、
  `QueueDiscContainer`、`MakeBoundCallback`、QueueDisc 的 `"Enqueue"/"Dequeue"`
  trace 源、`QueueDiscItem::GetSize()`——都是标准用法,若版本差异按报错微调,
  同 `README.md` 既有提示);
- `--scenario=nccl_ar` 实跑产出 `cmp_*.csv`,`delivered` 应 ≈ 2(P-1)·M/pkt、
  `dropped`≈0(leaf-spine PFC `drops` 必须 = 0);
- `compare_topologies.py` 出最终 `topo_comparison.png`。

---

## 7. 文件清单(本次新增/改动)

| 文件 | 状态 | 说明 |
|---|---|---|
| `cmp-common.h` | 新增 | 对比共享层(rank 铺开 / busbw / cmp CSV),纯 C++ 可单测 |
| `link-credit.h` | 新增 | 逐跳链路级信用流控引擎(纯 C++ 模板 `LinkCredit<Pkt>`,mesh 家族共用) |
| `link_credit_test.cc` | 新增 | `LinkCredit` 离散事件单测(信用守恒/窗约束/零丢/反压) |
| `compare_topologies.py` | 新增/改动 | 读 `cmp_*.csv` → 对比表 + 6 面板对比图;+`mesh4d`(8 拓扑) |
| `compare_credit.py` | 新增 | mesh 信用消融:读 credit_on/off → JCT/busbw/丢包率 3 面板对比图 |
| `smoke_test.sh` | 新增 | 极小规模端到端冒烟(几分钟):编译+9 sim 小跑+两张图,先验证管线再放大 |
| `build_comparison.sh` | 新增/改动 | 拷 sim+头进 ns-3 build;+`mesh4dw6-sim`、+`link-credit.h`(9 拓扑) |
| `batch_test_all.sh` | 改动 | +`mesh4dw6-sim` 运行(9 拓扑批量) |
| `cmp_pipeline_check.cc` | 新增 | 出图管线自检(人造占位值,验证接口与画图) |
| `test_cmp_common.cc` | 新增 | `cmp-common.h` 单元测试(字节量/铺开/busbw) |
| `leaf-spine-sim.cc` | 改动 | +`nccl_ar` 场景、+真 PFC 入口挂载、+统一能耗/cmp 输出 |
| `mesh-sim.cc` | 改动 | +`nccl_ar`、+**逐跳信用**(scan 转发)、+Switchless 能耗/cmp(写 `cmp_mesh2d.csv`);**A=17→36(统一规模 N=1296)** |
| `dragonfly-ib-sim.cc` | 改动 | +`nccl_ar` 场景、+Dragonfly 能耗/cmp 输出 |
| `fat_tree_sim.cc` | 改动 | +`nccl_ar`、+FatTree 能耗/cmp;**K=8→16(统一规模 N=1024)** |
| `spectrum-x-sim.cc` | 改动 | +`nccl_ar` 场景、+LeafSpine 能耗/cmp 输出 |
| `rail-ib-sim.cc` | 改动 | +`nccl_ar` 场景(R 条并行环)、+Rail 能耗/cmp 输出 |
| `rng-sim.cc` | 改动 | +`nccl_ar` 模式(RunSim 内)、+Switchless 能耗/cmp 输出 |
| `mesh3d-sim.cc` | 改动 | +`nccl_ar`、+**逐跳信用**(scan 转发)、+Switchless 能耗/cmp 输出 |
| `mesh4dw6-sim.cc` | 改动 | +`nccl_ar`、+**逐跳信用**(scan 转发)、+Switchless 能耗/cmp、serSec 用实际线速 |
| `pfc-lossless.h` | 改动 | +聚合统计访问器(MaxPeak/TotalPausedSec/PausedPorts) |
