# LensX — 项目摘要（给简历 AI 用）

本文档是为制作简历的 AI 准备的。它需要从中提取关键信息，理解项目背景、技术深度和成果，然后写进简历。

---

## 项目定位

一句话：**一个 eBPF 延迟追踪工具，把请求的端到端延迟按路径拆成多段来分析。**

对标：bpftrace（单点观测）、Pixie（自动 k8s 监控）、perf（CPU 事件统计）。LensX 的核心差异是**跨函数/跨线程的请求追踪能力**——把同一请求的多个事件关联起来算延迟。

---

## 项目背景

作者在开发高频交易引擎 NebulaX 时遇到一个问题：常规 profiling 工具只能看到 CPU 时间花在哪，回答不了"一个请求从网卡到应用到底走了多远、哪一段最慢"。具体来说：

- perf：统计 CPU 事件，不知道请求边界
- 火焰图：看函数耗时，但无法关联到同一个请求的多个阶段
- bpftrace：适合临时 ad-hoc 调试，不能做结构化分析

LensX 用 eBPF 在请求的全路径上打时间戳，从内核 IO 完成追踪到数据被发回网络，把每一段延迟拆开来看。

---

## 技术架构

### 数据流

```
BPF 探针（内核态）                     用户态程序
┌──────────────────┐               ┌──────────────────────┐
│ uprobe/kprobe/   │──ring buffer──→  YAML 配置解析        │
│ tracepoint       │               │  BPF 动态生成 + 编译  │
│ 写 event{ts,     │               │  配对引擎 (seq/fifo)  │
│     tid, stage,  │               │  直方图 + CSV 输出    │
│     key}         │               └──────────────────────┘
└──────────────────┘
```

### 探针类型

| 类型 | 用途 | BPF SEC |
|------|------|---------|
| uprobe | 用户态函数 | `SEC("uprobe")` |
| kprobe | 内核函数入口 | `SEC("kprobe/xxx")` |
| kretprobe | 内核函数返回 | `SEC("kretprobe/xxx")` |
| tracepoint | 内核预定义事件 | `SEC("tracepoint/cat/name")` |

### 三种配对模式

配对是 LensX 的核心——同一请求的多个事件散落在不同探针、不同线程、甚至不同进程中，LensX 负责把它们关联起来。

**seq（顺序配对）：** 同一线程上事件按到达顺序匹配。适用于单线程流水线（Redis 的 read → process → reply）。依赖同一 tid 上事件不交错。

**fifo（队列配对）：** 两组 seq 通过 FIFO 队列关联。适用于生产者-消费者架构，如 NebulaX 的 IO 线程 push → Send 线程 pop。依赖通道严格 FIFO。

**key（ID 配对）：** 按请求 ID 分组，不依赖线程和顺序。适用于跨线程/跨进程场景。用户需要在代码中加入带 ID 的标记函数，或利用现有 ID（io_uring 的 user_data、HTTP request-id）。

### 配对可靠性的前提

- seq：同线程上事件不能交错。如果线程并发处理多个请求（如线程池），seq 配对错乱。
- fifo：中间通道必须是严格 FIFO。如果通道是优先级队列或多生产者队列，配对错位。
- key：最可靠，但目标程序必须有可提取的请求 ID（需用户添加标记函数或利用现有 ID）。

---

## 发展历程

### V1 — 硬编码原型（1 天）

NebulaX 专用的硬编码版本，7 个探针固定写在代码里，配对逻辑写死在 handle_event 中。

```
探针：CQE → onRecv → match → pushRing → popRing → send
配对：IO 线程 seq + Send 线程 seq + FIFO 跨线程
输出：终端直方图
```

**V1 小流量测试（单连接 pipeline 5000 笔）：**

| 段 | P50 | P99 | 说明 |
|----|-----|-----|------|
| S0→S1 | 768ns | 1us | 内核调度延迟 |
| S1→S4 | 393us | 786us | IO 批处理（~130 笔/批）|
| S3→S4 | 3us | 6us | 撮合引擎 |
| S5→S6 | 3us | 3us | 发送准备 |
| S4→S5 | 3us | 3us | 跨线程 ring（低负载）|
| S0→S6 | 393us | 786us | 端到端 |

**V1 饱和测试（4 连接并行 pipeline，50M 笔混合命令）：**

| 段 | P50 | P99 | 说明 |
|----|-----|-----|------|
| S0→S1 | 767ns | 1us | 内核调度（不变）|
| S1→S4 | 98us | 196us | 流水线饱满时批处理更快 |
| S3→S4 | 1us | 3us | 撮合引擎（不是瓶颈）|
| S5→S6 | 3us | 3us | 发送准备 |
| S4→S5 | **1us** | **402ms** | 跨线程 ring——中位数 1us 但长尾 402ms |
| S0→S6 | 98us | 402ms | 中位数 0.1ms，长尾 400ms |

**关键发现：** 跨线程通信是最大瓶颈。低负载时因 Send 线程休眠+唤醒产生 3ms 延迟；高负载时流水线填满后中位数降到 1us，但 TCP 背压导致 SEND_ZC 阻塞，Send 线程被挂起，ring 积压，P99 飙升到 402ms。

**局限：** 只跑 NebulaX，换个程序全部重写。

### V2 — 配置驱动（3 天）

重新设计为通用工具。用户写 YAML 声明探针和配对规则，LensX 自动生成 BPF 代码、编译、加载、配对、出报告。

```
v2 分支 → 合并到 main
src/   → config.h/cpp（YAML 解析）、bpf_gen.h/cpp（BPF 生成）、
          matcher.h/cpp（配对引擎）、lensx.cpp（主程序）
test/  → 5 个验证示例 + 实测数据
```

**关键改动：**
- YAML 解析器 → 配置驱动而非代码驱动
- BPF 代码生成器 → 不手写 BPF，运行时根据配置生成 + clang 编译
- 通用配对引擎 → seq/fifo/key 三种模式，配置文件选择
- CSV 输出 → 原始数据包含 tid 列，可做逐请求分析

### 验证

V1 和 V2 在 NebulaX 上交叉验证，数据一致。5 个测试全部通过：

| 测试 | 配对 | 样本 | 说明 |
|------|------|------|------|
| NebulaX 全链路 | seq+fifo | 6 段延迟 | 双线程，IO+Send |
| Redis | seq | parse=12us exec=3us | 单线程，第三方项目 |
| c_test（key 模式）| key | 8000 对 | 多线程并发，按 ID 配对 |
| io_uring（跨内核）| key | 512 对 | uprobe↔tracepoint 按 user_data 配对 |
| kretprobe | kprobe→kretprobe | 206K | __x64_sys_read 执行耗时 |

所有实验数据和图表保存在 `test/results/` 目录下，直接打开即可看到。

V1 的代码和数据在 `v1` 分支：
```bash
git checkout v1
# src/lensx.cpp — V1 硬编码版本
# 测试数据在 V2 的 test/results/ 中有对比
# V1 本身的输出在会话记录中，V1 的主要作用是作为校准基线
```

V2（当前 `main` 分支）的测试数据在 `test/results/`：
- `test/results/nebulaX.txt` + `nebulaX.csv` — 全链路 6 段延迟
- `test/results/redis.txt` + `redis.csv` — Redis 延迟分解
- `test/results/c_test.txt` + `c_test.csv` — key 模式 8000 配对
- `test/results/io_uring.txt` + `io_uring.csv` — 跨内核 512 配对
- `test/results/kretprobe.txt` + `kretprobe.csv` — 内核函数 206K 样本
- `test/config_full.yaml` — 完整配置示例，含关键模式教程

只需阅读 `test/results/*.txt`（控制台直方图输出）即可了解测试结果，不需要运行代码。

---

## 简历 AI 需要提取的关键信息

### 核心卖点

1. **问题驱动：** 不是为了造轮子，是真实性能分析需求催生的工具
2. **从 V1 到 V2 的演进：** 先验证可行性，再通用化，展示了架构设计能力
3. **跨项目验证：** 不仅跑自己的项目，Redis 和 io_uring 也能追踪
4. **数据可复现：** 每个测试都有保存的原始数据

### 技术深度的体现

- eBPF 编程（BPF 字节码、verifier、CO-RE）
- 内核/用户态编程（libbpf、ring buffer、kprobe/uprobe）
- C++20、YAML、CMake、clang/LLVM
- 多线程编程、lock-free 数据结构
- 性能分析方法论（逐段分解、交叉验证）
- 动态代码生成（运行时生成 BPF C 源码并编译）

### 简历话术（参考）

**项目经历：LensX — 配置驱动的 eBPF 延迟追踪工具**

> 用 eBPF 在请求的全路径上打时间戳，把端到端延迟逐段拆解分析

- 硬编码原型验证可行性 → 重构为配置驱动通用工具
- YAML 声明探针位置，动态生成 BPF 代码并编译
- 三种配对模式（seq/fifo/key）覆盖不同线程模型
- 5 个测试场景验证，V1/V2 交叉确认数据一致
- 支持 uprobe/kprobe/kretprobe/tracepoint 全部探针类型

**相关技能：** eBPF、libbpf、C++20、Linux 内核、性能分析

---

## 代码结构

```
LensX/
├── src/                     # V2（当前 main）
│   ├── lensx.cpp            # 主程序：CLI + BPF 加载 + 配对 + 输出
│   ├── config.h/cpp         # YAML 配置解析器
│   ├── bpf_gen.h/cpp        # 动态 BPF 代码生成 + clang 编译
│   └── matcher.h/cpp        # 配对引擎（seq/fifo/key）
├── test/
│   ├── README.md            # 测试说明 + 时间线图
│   ├── config_full.yaml     # 完整配置示例（含 key 模式教程）
│   ├── nebulaX.yaml         # NebulaX 全链路配置
│   ├── redis.yaml           # Redis 配置
│   ├── c_test.c             # key 模式验证程序
│   ├── io_uring_test.c      # 跨内核 key 验证
│   ├── kretprobe_test.c     # kretprobe 验证
│   └── results/             # 5 个测试的实测数据
├── CMakeLists.txt            # yaml-cpp + libbpf
├── libbpf/                   # 静态链接 libbpf
├── build/                    # 编译产物
├── v1/                       # ----
└── README.md                 # V1 成果总结
```

## 分支

```
main ← V2（当前）
v1   ← V1 硬编码版（存档）
v2   ← 合并前 V2 快照
```

## 编译运行

```bash
cmake -B build && cmake --build build
sudo ./build/lensx run test/redis.yaml    # 追踪 Redis
sudo ./build/lensx run test/nebulaX.yaml  # 追踪 NebulaX
```

依赖：clang、libyaml-cpp-dev、libelf、zlib、bpftool（生成 vmlinux.h）。
