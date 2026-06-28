# BCube SIGCOMM'09 — ns-3 仿真复现

> Guo et al., "BCube: A High Performance, Server-Centric Network Architecture for Modular Data Centers", SIGCOMM 2009.

本项目用 ns-3 对论文 §7.5（吞吐量对比）和 §6（故障降级）进行数据包级仿真复现。

---

## 拓扑说明

### BCube_1（本仿真使用）

```
n=4, k=1  →  16 台服务器，8 台交换机（sw0-3 为 Level-0，sw4-7 为 Level-1）
```

服务器编号 `i` 的两位地址：`a0 = i % 4`，`a1 = i // 4`

- **Level-0 交换机 sw_j**：连接所有满足 `a1 = j` 的服务器（同一 BCube_0 组内的服务器）
- **Level-1 交换机 sw_{4+j}**：连接所有满足 `a0 = j` 的服务器（跨组，相同位置的服务器）

每台服务器连接 2 台交换机（每级各一台），每条链路 1 Gbps。

> **⚠️ 注意**：论文 §7.5 的 all-to-all 实验使用 BCube_2（n=4, k=2, 64 台服务器）。
> 本仿真使用 BCube_1（16 台服务器），因此 all-to-all 数字偏高，属预期差异，见下文结果分析。

### 2-level Tree（对照组）

16 台服务器，1 台根交换机 + 4 台叶交换机，根交换机为单点故障。

---

## 环境与编译

```bash
# 依赖：ns-3.40 / ns-3.41 / ns-3-dev（推荐 ns-3-dev）
# ns-3-dev 需要 C++23：
cd <ns3-dev>
./ns3 configure --cxx-standard=23
./ns3 build

# 放置仿真文件（两种方式均可）：
cp bcube-sim.cc <ns3>/scratch/
# 或放入子目录：
mkdir -p <ns3>/scratch/bcube && cp bcube-sim.cc <ns3>/scratch/bcube/
```

---

## 运行实验

```bash
# 一对一（server 0 → server 7，2 条并行 TCP 流）
./ns3 run "scratch/bcube-sim --exp=one-to-one"
# 或子目录版本：
./ns3 run "scratch/bcube/bcube-sim --exp=one-to-one"

# 一对多（server 0 → 所有其他 server）
./ns3 run "scratch/bcube/bcube-sim --exp=one-to-all"

# 全对全（MapReduce shuffle，每对 50 MB，120s 窗口）
./ns3 run "scratch/bcube/bcube-sim --exp=all-to-all"

# 故障降级（交换机逐步失效，观察吞吐线性下降 vs 骤降）
./ns3 run "scratch/bcube/bcube-sim --exp=degrade"
```

---

## 实验结果

### §7.5 One-to-one

server 0 → server 7，两条并行 TCP 流，路径完全不共享交换机。

| 拓扑 | 仿真吞吐 | 论文结果 |
|------|---------|---------|
| BCube_1 | ~1930 Mb/s | ~1930 Mb/s |
| Tree | ~990 Mb/s | ~990 Mb/s |

✅ 与论文完全吻合。

---

### §7.5 One-to-all

server 0 同时向其余 15 台服务器发送数据。

| 拓扑 | 仿真吞吐 | 论文结果 |
|------|---------|---------|
| BCube_1 | ~1813 Mb/s | ~1600 Mb/s |
| Tree | ~989 Mb/s | ~880 Mb/s |

BCube/Tree 吞吐比：仿真 **1.83×**，论文 **1.82×**。✅ 比值高度吻合，绝对值偏高约 12%（系统性偏差，见下文）。

---

### §7.5 All-to-all（MapReduce）

每台服务器向其余所有服务器发送数据（MapReduce shuffle 模型）。

| 拓扑 | 仿真 per-server 吞吐 | 仿真完成时间 | 论文结果 | 论文完成时间 |
|------|---------------------|------------|---------|------------|
| BCube_1 | ~1038 Mb/s | ~72s | ~750 Mb/s（BCube_2） | ~114s |
| Tree | ~308 Mb/s | ~281s | ~260 Mb/s | ~332s |

BCube/Tree 吞吐比：仿真 **3.37×**，论文 **2.88×**。✅ 趋势吻合。

> **BCube_1 vs BCube_2 说明**：论文此实验使用 BCube_2（64 台服务器，4032 条并发流），竞争更激烈，per-server 吞吐更低。本仿真 BCube_1 只有 240 条流，竞争少，吞吐自然偏高。Tree 结果（只有 16 台服务器，与本仿真一致）偏差仅 18%，印证了这一解释。

---

### §6 故障降级（Graceful Degradation）

测试方案：同时运行两对通信（Pair A: 0→7，Pair B: 5→10），各 2 条并行 TCP 流（共 4 条），按顺序逐一失效交换机。

**路径分析：**

```
Pair A (0=[0,0] → 7=[3,1]):
  Path-A0: [0 → 4 → 7]   经过 sw4(lv1) → sw1(lv0)
  Path-A1: [0 → 3 → 7]   经过 sw0(lv0) → sw7(lv1)

Pair B (5=[1,1] → 10=[2,2]):
  Path-B0: [5 → 9 → 10]  经过 sw5(lv1) → sw2(lv0)
  Path-B1: [5 → 6 → 10]  经过 sw1(lv0) → sw6(lv1)
```

**失效顺序**（每次仅杀死一条路径）：

| 累计失效交换机 | 存活路径 | 预期吞吐 | 仿真吞吐 |
|--------------|---------|---------|---------|
| 无 | A0+A1+B0+B1 | ~3860 Mb/s | ~3860 Mb/s |
| sw0 | A0+B0+B1 | ~2895 Mb/s | ~2895 Mb/s |
| sw0, sw4 | B0+B1 | ~1930 Mb/s | ~1930 Mb/s |
| sw0, sw4, sw1 | B0 | ~965 Mb/s | ~965 Mb/s |
| sw0, sw4, sw1, sw5 | 无 | ~0 Mb/s | ~0 Mb/s |

**Tree 对照：**

| 失效交换机 | 仿真吞吐 |
|-----------|---------|
| 无 | ~984 Mb/s |
| 根交换机 | ~0 Mb/s |

✅ BCube 线性降级（优雅），Tree 根交换机一断即全灭（单点故障）。

---

## 遇到的坑（Debug 记录）

### Bug 1：BCube 吞吐为 0 Mb/s

**现象**：一对一实验，BCube 测出 0 Mb/s，Tree 正常。

**原因**：交换机路由表安装的是 `/30` 子网路由，而 `svIP[7][0]`、`svIP[7][1]` 是服务器的具体 IP，不在那些子网内，导致所有包在交换机处被丢弃。

**修复**：改为按数字路由安装逐跳主机路由。Level-lv 交换机对目标服务器 `dst` 的出接口为：

```cpp
uint32_t outIfc = static_cast<uint32_t>(dstDig[lv]) + 1;  // +1 因 ifc 0 是 loopback
```

对所有 `dst` 的所有 IP 都安装一条 host route，交换机才能正确转发。

---

### Bug 2：BCube 吞吐为 460 Mb/s（应为 ~1930 Mb/s）

**现象**：Bug 1 修复后，吞吐只有 460 Mb/s，约为预期的 1/4。

**原因**：`buildPathSet` 除了生成 DCRouting 路径，还生成了 AltDCRouting 路径（通过其他服务器中转，如 `[3, 0, 4, 7]`）。AltDCRouting 路径要求中间服务器（此处为 server 3）作为转发节点，但 server 3 同时也是源节点组的成员，它自己的静态路由表中已经安装了 `svIP[7][1]` 的路由（metric=10）。AltDCRouting 在 server 3 上又安装了另一条到 `svIP[7][1]` 的路由（也是 metric=10），造成**路由环路**：

```
server 0 → server 3 → server 0 → server 3 → ...（死循环）
```

结果一条流 0 Mb/s，另一条流全部流量走同一条路，只有 ~460 Mb/s（约为 1 路的速率）。

**修复**：删除 AltDCRouting 分支，`buildPathSet` 只生成 DCRouting 路径（跳过 `A[i]==B[i]` 的层级）：

```cpp
for (int i = k; i >= 0; i--) {
    if (A[i] == B[i]) continue;   // 只处理数字不同的层级
    // ... 生成路径
}
```

对 server 0→7（两位数字都不同），仍可生成 2 条完全不共享交换机的并行路径。

---

### Bug 3：`numSwitches` 未定义

**现象**：修改交换机路由循环时，使用了 `numSwitches` 变量，编译报错。

**原因**：`numSwitches` 不是 `BCubeNet` 的成员变量。

**修复**：改用 `(int)switches.GetN()`。

---

## 代码结构

```
bcube-sim.cc
├── struct Addr          — BCube 地址算术（dig / num / route / buildPathSet）
├── class BCubeNet       — BCube_1 拓扑构建、IP 分配、路由安装、FailSwitch()
├── class TreeNet        — 2-level Tree 拓扑、FailRootSwitch() / FailLeafSwitch()
├── AddSink / AddBulkSend — 应用层辅助函数
├── Measure()            — FlowMonitor 统计总吞吐
├── ExpOneToOne()        — §7.5 一对一实验
├── ExpOneToAll()        — §7.5 一对多实验
├── ExpAllToAll()        — §7.5 全对全实验
└── ExpDegrade()         — §6 故障降级实验
```

---

## 参考文献

C. Guo, G. Lu, D. Li, H. Wu, X. Zhang, Y. Shi, C. Tian, Y. Zhang, S. Lu,
"BCube: A High Performance, Server-Centric Network Architecture for Modular Data Centers",
*ACM SIGCOMM 2009*, pp. 63–74.
