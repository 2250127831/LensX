# LensX

eBPF 配置驱动的延迟追踪工具。写 YAML 声明探针和配对规则，LensX 自动生成 BPF 代码、编译、附着、采集、配对、出报告。

```bash
# 编译
cmake -B build && cmake --build build

# 追踪 Redis 的单线程请求处理
sudo ./build/lensx run test/redis.yaml --pid $(pgrep redis-server)

# 追踪自己的程序
sudo ./build/lensx run config_full.yaml --pid <pid>
```

## 快速开始

1. 看 [`config_full.yaml`](config_full.yaml) — 完整的配置示例，每个字段都有注释
2. 看 [`test/README.md`](test/README.md) — 5 个验证示例的时间线图解
3. 看 [`test/results/`](test/results/) — 实测数据（直方图 + CSV）

```yaml
# config_full.yaml 的核心结构（简化）
probes:               # 定义探针
  - name: recv
    type: uprobe
    symbol: <函数名>
    stage: 0
    key: arg1         # 可选：用于 key 配对模式

matchers:              # 定义配对规则
  - id: flow
    stages: [0, 1, 2]
    mode: seq          # seq / fifo / key

reports:               # 定义输出报告
  - name: my_report
    from: 0
    to: 2
```

## 探针类型

| 类型 | 写法 | 用途 |
|------|------|------|
| uprobe | `symbol: <函数名>` | 用户态函数 |
| kprobe | `symbol: <内核函数>` | 内核函数入口 |
| kretprobe | `symbol: <内核函数>` | 内核函数返回 |
| tracepoint | `event: <分类/事件名>` | 内核预定义事件 |

## 三种配对模式

配对是 LensX 的核心——同一请求的多个事件散在不同探针和线程里，需要关联起来。

| 模式 | 原理 | 适用场景 |
|------|------|---------|
| **seq** | 同一 tid 按到达顺序配对 | 单线程流水线（Redis read→process→reply） |
| **fifo** | 两组 seq 通过队列关联 | 生产者-消费者（NebulaX SPSC ring） |
| **key** | 按请求 ID 分组 | 跨线程/跨进程。需传 ID 到探针函数 |

三种模式的详细对比见 [`config_full.yaml`](config_full.yaml) 的注释。

## 验证

5 个测试全部通过，V1（硬编码）和 V2（配置驱动）交叉验证数据一致。

| 测试 | 配对 | 数据 | 说明 |
|------|------|------|------|
| [NebulaX](test/nebulaX.yaml) | seq+fifo | 6 段延迟 | 双线程全链路追踪 |
| [Redis](test/redis.yaml) | seq | parse=12us exec=3us | 第三方项目单线程请求 |
| [c_test](test/c_test.c) | key | 8000 对 | 多线程按 ID 配对 |
| [io_uring](test/io_uring_test.c) | key | 512 对 | 跨内核（uprobe↔tracepoint） |
| [kretprobe](test/kretprobe_test.c) | kprobe→kretprobe | 206K | read 系统调用耗时 |

实测数据在 `test/results/` 下，包含直方图和 CSV。

## 依赖

- clang（运行时生成 BPF 需要编译）
- libyaml-cpp-dev、libelf、zlib
- bpftool（生成 vmlinux.h）
- Linux 5.8+（BPF CO-RE）

```bash
sudo apt install clang libyaml-cpp-dev libelf-dev zlib1g-dev
sudo apt install linux-tools-common  # bpftool
```

## 分支

- `main` — V2 配置驱动版（当前）
- `v1` — V1 硬编码版（NebulaX 专用，存档）
