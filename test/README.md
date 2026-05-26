# LensX 验证示例

每个示例独立可运行。

---

## 1. nebulaX.yaml — 全链路追踪（seq + fifo）

```
IO 线程 (tid_A)                  Send 线程 (tid_B)
                                   ring
CQE ──→ onRecv ──→ match ──→ push ──→ consume ──→ send
 S0       S1         S3        S4        S5        S6
```

```bash
lensx run test/nebulaX.yaml --pid $(pgrep nebulaX)
```

---

## 2. redis.yaml — 单线程请求追踪（seq）

```
客户端 ──→ readQueryFromClient ──→ processCommand ──→ addReply ──→ 响应
               S0                   S1               S2
```

```bash
lensx run test/redis.yaml --pid $(pgrep redis-server)
```

---

## 3. kretprobe_test — 内核函数耗时（kprobe + kretprobe）

```
用户态 read()  ──→ [kprobe]  __x64_sys_read 入口   ← S0
                    │        
                    │         ←执行系统调用
                    │       
                    └── [kretprobe] 返回             ← S1
```

```bash
gcc -O2 -o kretprobe_test test/kretprobe_test.c
./kretprobe_test &
lensx run test/kretprobe_test.yaml
```

---

## 4. c_test — key 配对

```
线程 A: mark_s0(key=1) → mark_s1(key=1) → mark_s2(key=1)
线程 B: mark_s0(key=2) → mark_s1(key=2) → mark_s2(key=2)

配对池按 key 分组，同 key 的三次事件收齐即配对成功。
```

```bash
gcc -O0 -o c_test test/c_test.c -lpthread
./c_test &
lensx run test/c_test_key.yaml
```

---

## 5. io_uring_test — 跨内核 key 配对

```
用户态                           内核态
mark_submit(42) → uprobe S0 ──→  io_uring 处理 NOP
                                  io_uring_complete → tracepoint S1
                                  (ctx->user_data == 42，与 S0 配对)
```

```bash
gcc -O2 -o io_uring_test test/io_uring_test.c -luring
./io_uring_test &
lensx run test/io_uring_key.yaml
```
