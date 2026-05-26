# LensX

eBPF 配置驱动的延迟追踪工具。写 YAML 声明探针位置和配对规则，LensX 自动生成 BPF 代码、编译、附着、采集、配对、出报告。

```bash
lensx run test/redis.yaml --pid $(pgrep redis-server)
```

## 探针

| 类型 | 写法 | 用途 |
|------|------|------|
| uprobe | `type: uprobe / symbol: <函数名>` | 用户态函数 |
| kprobe | `type: kprobe / symbol: <内核函数>` | 内核函数入口 |
| kretprobe | `type: kretprobe / symbol: <内核函数>` | 内核函数返回 |
| tracepoint | `type: tracepoint / event: <分类/事件名>` | 内核预定义事件 |

## 配对模式

同一请求的多个事件散在不同探针和线程里，配对是 LensX 的核心：

| 模式 | 原理 | 适用场景 |
|------|------|---------|
| seq | 同一 tid 按到达顺序配对 | 单线程流水线（Redis） |
| fifo | 两组 seq 通过 FIFO 队列关联 | 生产者-消费者（NebulaX SPSC ring）|
| key | 按请求 ID 分组，跨线程不依赖顺序 | 跨线程/跨进程（io_uring） |

## 用法

```bash
# 编译
cmake -B build && cmake --build build

# 按进程名追踪
sudo ./build/lensx run test/nebulaX.yaml

# 按 PID 追踪（覆盖配置里的 process 名）
sudo ./build/lensx run test/redis.yaml --pid 12345

# CSV 原始数据输出（在 YAML 里配 output.csv）
sudo ./build/lensx run test/nebulaX.yaml
cat /tmp/nebulaX_raw.csv
```

## 验证

5 个测试场景全部通过，V1（硬编码）和 V2（配置驱动）交叉验证数据一致。实测数据保存在 `test/results/`。

| 测试 | 配对 | 样本 | 说明 |
|------|------|------|------|
| NebulaX 全链路 | seq+fifo | 6 段延迟 | 双线程，IO+Send |
| Redis | seq | parse=12us exec=3us | 单线程，第三方项目 |
| c_test | key | 8000 对 | 多线程并发 key 配对 |
| io_uring | key | 512 对 | 跨内核，uprobe↔tracepoint |
| kretprobe | kprobe→kretprobe | 206K | read 系统调用耗时 |

## 依赖

- clang（运行时编译 BPF）
- libyaml-cpp-dev
- libelf、zlib
- bpftool（生成 vmlinux.h）
- Linux 内核 5.8+（BPF CO-RE）

## 分支

- `main` — V2 配置驱动版（当前）
- `v1` — V1 硬编码版（NebulaX 专用，存档）
