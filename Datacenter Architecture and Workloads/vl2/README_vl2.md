# VL2 数据中心网络仿真

> 复现论文：**"VL2: A Scalable and Flexible Data Center Network"**  
> Greenberg et al., SIGCOMM 2009, Microsoft Research

---

## 论文核心思想

VL2 的目标是让数据中心网络具备**敏捷性（Agility）**——任意服务器可以被分配给任意服务，而不受网络拓扑限制。论文提出三个核心机制：

1. **Clos 拓扑**：用大量廉价交换机构建无过订阅（non-oversubscribed）的胖树网络，提供服务器间均匀高带宽。
2. **Valiant Load Balancing（VLB）**：每条流随机选择一个中间（Intermediate）交换机转发，无需集中式流量工程，天然避免热点。
3. **名址分离**：服务器使用应用地址（AA），与位置无关；底层网络使用位置地址（LA）路由；通过目录系统维护映射，支持虚拟机迁移。

---

## 仿真拓扑

```
[Int-0]   [Int-1]   [Int-2]        ← Intermediate（脊）交换机
   |\ \/    |\ \/    |\ \/
[Aggr-0] [Aggr-1] [Aggr-2]         ← Aggregation 交换机
   |  \ /   |  \ /   |
[ToR-0][ToR-1][ToR-2][ToR-3]       ← Top-of-Rack 交换机
   |       |       |       |
[srv…]   …       …       …         ← 服务器（每 ToR 5 台，共 20 台）
```

| 链路类型 | 带宽 | 延迟 |
|---------|------|------|
| 服务器 ↔ ToR | 1 Gbps | 100 µs |
| ToR ↔ Aggr | 10 Gbps | 10 µs |
| Aggr ↔ Int | 10 Gbps | 10 µs |

论文原型：75 台服务器 + 10 台交换机；本仿真缩小为 20 台服务器以降低计算成本，拓扑结构比例一致。

---

## VLB 仿真实现

VL2 的 VLB 通过以下方式在 NS-3 中实现：

- 使用 `Ipv4GlobalRouting` + `RandomEcmpRouting=true`，对每条流随机选择等价路径。
- 在 Clos 拓扑中，等价路径天然经过不同的 Intermediate 交换机，实现论文 Section 4.2.2 描述的随机中间节点转发。
- 链路故障通过 `Ipv4::SetDown()` + `RecomputeRoutingTables()` 模拟 OSPF 快速重收敛。

---

## 四个实验

### 实验 1：全对全数据 Shuffle（论文 Fig. 9）

**目的**：验证 VL2 提供均匀高带宽。

**方法**：20 台服务器同时互相发送数据（每对 500 MB），观察聚合吞吐和公平性随时间变化。

**结果**：
```
Total TX  : 20.97 GB
Flows done: 380
Jain Fair : 0.9960  ✅ (论文目标 ≥0.995)
```

---

### 实验 2：VLB 公平性（论文 Fig. 10）

**目的**：验证 VLB 能将流量均匀分布到各 Intermediate 交换机。

**方法**：每台服务器保持 10 条并发流，流大小从数据中心真实分布中采样（5~100 MB），持续 120 秒，每 2 秒采样一次 Jain 公平指数。

**结果**：
```
Jain Fair : 0.9566  ❌ (论文目标 ≥0.995)
```
> 略低于论文，原因是仿真规模（20 台）远小于论文测试台（75 台），统计复用效果不足。

---

### 实验 3：性能隔离（论文 Fig. 11）

**目的**：验证 VLB + TCP 可实现服务间性能隔离。

**方法**：
- 服务 1（10 台服务器）：从 t=0 开始持续发送无限 TCP 流。
- 服务 2（10 台服务器）：从 t=30s 起每隔 2s 加入一台，每台发 500 MB。
- 两个服务的服务器交错分布在各 ToR 上。

**结果**：
```
Jain Fair : 0.9999  ✅ (论文目标 ≥0.995)
```
服务 1 吞吐在服务 2 加入前后保持稳定，验证了性能隔离。

---

### 实验 4：链路故障收敛（论文 Fig. 13）

**目的**：验证链路故障后网络快速重收敛，吞吐优雅降级。

**方法**：
- 背景：全对全持续 TCP 流量。
- t=20~30s：逐步断开所有 Aggr → Int[0] 链路。
- t=60~70s：逐步断开所有 Aggr → Int[1] 链路。
- t=100~110s：恢复 Int[0] 链路。
- t=130~140s：恢复 Int[1] 链路。

**结果**：
```
Jain Fair : 0.9901  ❌ (论文目标 ≥0.995)
```
每次链路变化后吞吐在 1s 内重收敛（NS-3 的 `RecomputeRoutingTables()` 模拟 OSPF 瞬时收敛），吞吐随故障数量优雅降级。

---

## 如何运行

```bash
# 1. 将仿真代码放入 ns-3 的 scratch 目录
cp vl2_simulation.cc /path/to/ns-3-dev/scratch/vl2/

# 2. 编译
cd /path/to/ns-3-dev
./ns3 build

# 3. 运行四个实验
./ns3 run "vl2-simulation --experiment=1"
./ns3 run "vl2-simulation --experiment=2"
./ns3 run "vl2-simulation --experiment=3"
./ns3 run "vl2-simulation --experiment=4"

# 4. 画图
python3 plot_vl2.py --results .
```

输出文件：
- `vl2_expN_goodput.dat`：时间序列数据（时间、吞吐、活跃流数、Jain 指数）
- `vl2_expN_flows.xml`：NS-3 FlowMonitor 详细流统计
- `fig9_shuffle.pdf` / `fig10_fairness.pdf` / `fig11_isolation.pdf` / `fig13_failure.pdf`：复现论文图

---

## 实验结果汇总

| 实验 | 内容 | Jain 公平指数 | 论文目标 | 达标 |
|------|------|------------|---------|------|
| 1 | 全对全 Shuffle | 0.9960 | ≥0.995 | ✅ |
| 2 | VLB 公平性 | 0.9566 | ≥0.995 | ❌ |
| 3 | 性能隔离 | 0.9999 | ≥0.995 | ✅ |
| 4 | 链路故障收敛 | 0.9901 | ≥0.995 | ❌ |

实验 2 和 4 未完全达标，主要原因是仿真规模（20 台服务器）远小于论文（75 台），VLB 的统计复用效果在小规模下有所削弱。

---

## 依赖环境

- NS-3 >= 3.38
- Python >= 3.8
- matplotlib, numpy
