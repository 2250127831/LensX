// LensX V2 — config-driven eBPF latency tracer
// Usage: lensx run config.yaml

#include "config.h"
#include "bpf_gen.h"
#include "matcher.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <iostream>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

/* ── 全局 ── */
static volatile bool exiting = false;
static void sig_handler(int) { exiting = true; }

/* ── 符号解析（uprobe 用）── */
struct Resolved { std::string binary; size_t offset; };

// 精确匹配 demangled 符号，返回偏移
static Resolved resolve_sym(int pid, const std::string &symbol)
{
    char exe[4096];
    ssize_t len = readlink(("/proc/" + std::to_string(pid) + "/exe").c_str(),
                           exe, sizeof(exe) - 1);
    if (len < 0) { perror("readlink"); exit(1); }
    exe[len] = '\0';

    std::string cmd = "nm -C '" + std::string(exe) + "' 2>/dev/null "
                      "| grep -E '^[0-9a-f]+ [A-Z] .*" + symbol + "' "
                      "| head -1";
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) { perror("popen"); exit(1); }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        std::cerr << "symbol '" << symbol << "' not found in " << exe << "\n";
        exit(1);
    }
    pclose(fp);
    unsigned long long off = 0;
    sscanf(buf, "%llx", &off);
    return {exe, (size_t)off};
}

// 从进程名 pgrep 获取 PID
static int resolve_pid(const std::string &name)
{
    std::string cmd = "pgrep -x '" + name + "' | head -1";
    FILE *fp = popen(cmd.c_str(), "r");
    if (!fp) { perror("pgrep"); exit(1); }
    char buf[64];
    if (!fgets(buf, sizeof(buf), fp)) { pclose(fp); return 0; }
    pclose(fp);
    return std::stoi(buf);
}

/* ── 直方图 ── */
#define HIST_BUCKETS 48

static FILE *csv_file = nullptr;

struct Hist {
    uint64_t buckets[HIST_BUCKETS] = {};
    int count = 0;

    void record(uint64_t val_ns, int from = -1, int to = -1,
                uint64_t ts_from = 0, uint64_t ts_to = 0, uint32_t tid = 0) {
        count++;
        int idx = 63 - __builtin_clzll(val_ns | 1);
        if (idx >= HIST_BUCKETS) idx = HIST_BUCKETS - 1;
        buckets[idx]++;
        if (csv_file)
            fprintf(csv_file, "%d,%d,%lu,%lu,%lu,%u\n", from, to,
                    (unsigned long)val_ns, (unsigned long)ts_from,
                    (unsigned long)ts_to, tid);
    }

    void print(const char *title) const {
        if (count == 0) { printf("  %s: (no samples)\n", title); return; }

        uint64_t max_b = 0;
        for (auto v : buckets) if (v > max_b) max_b = v;

        printf("\n%s (%d samples):\n", title, count);
        int bw = 30;
        for (int i = 0; i < HIST_BUCKETS; i++) {
            if (buckets[i] == 0) continue;
            uint64_t lo = 1ULL << i, hi = (1ULL << (i + 1)) - 1;
            double pct = 100.0 * buckets[i] / count;
            int bar = bw * buckets[i] / max_b;
            if (bar < 1) bar = 1;
            auto fmt = [](uint64_t ns) {
                if (ns >= 1000000000ULL) return std::to_string(ns / 1000000000) + "s";
                if (ns >= 1000000) return std::to_string(ns / 1000000) + "ms";
                if (ns >= 1000) return std::to_string(ns / 1000) + "us";
                return std::to_string(ns) + "ns";
            };
            printf("  %7s - %-7s |%-*s| %8lu (%5.1f%%)\n",
                   fmt(lo).c_str(), hi > lo ? fmt(hi).c_str() : fmt(lo).c_str(),
                   bw, std::string(bar, '#').c_str(), (unsigned long)buckets[i], pct);
        }

        auto pct = [&](double p) -> uint64_t {
            uint64_t target = (uint64_t)(count * p / 100.0), cum = 0;
            for (int i = 0; i < HIST_BUCKETS; i++) {
                cum += buckets[i];
                if (cum >= target) return ((1ULL << i) + (1ULL << (i + 1))) / 2;
            }
            return 0;
        };
        auto f = [](uint64_t ns) {
            if (ns >= 1000000000ULL) return std::to_string(ns / 1000000000) + "s";
            if (ns >= 1000000) return std::to_string(ns / 1000000) + "ms";
            if (ns >= 1000) return std::to_string(ns / 1000) + "us";
            return std::to_string(ns) + "ns";
        };
        printf("  P50=%s  P90=%s  P99=%s  P999=%s\n",
               f(pct(50)).c_str(), f(pct(90)).c_str(),
               f(pct(99)).c_str(), f(pct(99.9)).c_str());
    }
};

/* ── 主函数 ── */
int main(int argc, char **argv)
{
    if (argc < 3 || std::string(argv[1]) != "run") {
        fprintf(stderr, "Usage: lensx run config.yaml [--pid <pid>] [--csv <file>]\n");
        return 1;
    }

    std::string config_path = argv[2];

    // 解析可选参数
    int override_pid = 0;
    for (int i = 3; i < argc - 1; i++) {
        if (std::string(argv[i]) == "--pid")
            override_pid = std::stoi(argv[i + 1]);
    }

    // 1. 解析配置
    printf("LensX V2 — loading %s\n", config_path.c_str());
    Config cfg = parse_config(config_path);

    // 2. 确定 PID（命令行 --pid 优先，回退到 pgrep）
    int target_pid = override_pid;
    if (target_pid == 0 && !cfg.process.empty())
        target_pid = resolve_pid(cfg.process);
    if (target_pid == 0) {
        std::cerr << "error: target pid not found\n"
                  << "  pass --pid <pid> or set target.process in config\n";
        return 1;
    }
    printf("  target PID: %d\n", target_pid);

    // 3. 生成 BPF C 代码并编译
    char tmp_template[] = "/tmp/lensx_XXXXXX";
    int fd = mkstemp(tmp_template);
    close(fd);
    std::string c_path(tmp_template);
    unlink(tmp_template);       // 删掉空文件
    c_path += ".c";

    generate_bpf_source(cfg, c_path);
    printf("  BPF source: %s\n", c_path.c_str());

    std::string obj_path = compile_bpf(c_path);
    printf("  BPF object: %s\n", obj_path.c_str());

    // 4. 加载 BPF
    struct bpf_object *obj = bpf_object__open(obj_path.c_str());
    if (!obj) { std::cerr << "bpf_object__open failed\n"; return 1; }
    if (bpf_object__load(obj)) { std::cerr << "bpf_object__load failed\n"; return 1; }

    int rb_fd = -1;
    struct bpf_map *rb_map = bpf_object__find_map_by_name(obj, "rb");
    if (rb_map) rb_fd = bpf_map__fd(rb_map);
    if (rb_fd < 0) { std::cerr << "ring buffer map not found\n"; return 1; }

    // 5. attach 所有探针
    std::vector<struct bpf_link *> links;
    for (auto &p : cfg.probes) {
        // 从符号名查找 BPF 程序（生成时的命名规则：tp_xxx / kp_xxx / up_xxx）
        std::string prog_name = (p.type == "tracepoint") ? "tp_" :
                                (p.type == "kprobe" || p.type == "kretprobe") ? "kp_" : "up_";
        prog_name += p.name;

        struct bpf_program *prog = bpf_object__find_program_by_name(obj, prog_name.c_str());
        if (!prog) {
            std::cerr << "  " << p.name << ": program not found in BPF object\n";
            continue;
        }

        struct bpf_link *link = nullptr;

        if (p.type == "uprobe") {
            auto rs = resolve_sym(target_pid, p.symbol);
            link = bpf_program__attach_uprobe(prog, false, target_pid,
                                              rs.binary.c_str(), rs.offset);
            printf("  uprobe %-20s @ 0x%lx\n", p.name.c_str(), rs.offset);
        } else if (p.type == "kprobe" || p.type == "kretprobe") {
            bool retprobe = (p.type == "kretprobe");
            link = bpf_program__attach_kprobe(prog, retprobe, p.symbol.c_str());
            printf("  %-7s %-20s %s\n", p.type.c_str(), p.name.c_str(), p.symbol.c_str());
        } else if (p.type == "tracepoint") {
            // event: "category/name"
            auto slash = p.event.find('/');
            std::string cat = p.event.substr(0, slash);
            std::string name = p.event.substr(slash + 1);
            link = bpf_program__attach_tracepoint(prog, cat.c_str(), name.c_str());
            printf("  tp     %-20s %s\n", p.name.c_str(), p.event.c_str());
        }

        if (!link) {
            std::cerr << "  " << p.name << ": attach failed\n";
            continue;
        }
        links.push_back(link);
    }

    // 6. 构造 matchers + fifo 队列
    std::vector<Matcher *> matchers;
    std::unordered_map<std::string, SeqMatcher *> seq_map;
    std::unordered_map<std::string, FifoQueue *> fifo_map;
    std::unordered_map<std::string, Hist> hists;

    for (auto &mc : cfg.matchers) {
        if (mc.mode == "seq") {
            auto *m = new SeqMatcher(mc.id, mc.stages);
            seq_map[mc.id] = m;
            matchers.push_back(m);
        } else if (mc.mode == "key") {
            auto *m = new KeyMatcher(mc.id, mc.stages);
            matchers.push_back(m);
        } else if (mc.mode == "fifo") {
            // fifo stages = [push_stage, pop_stage]
            if (mc.stages.size() < 2) {
                std::cerr << "error: fifo needs push_stage and pop_stage\n";
                continue;
            }
            // fifo 不作为独立的 Matcher（不接收原始事件），仅作为队列
            fifo_map[mc.id] = new FifoQueue(mc.id, mc.queue_size);
        }
    }

    // 建立 stage 对 → 报告名 映射
    std::map<std::pair<int,int>, std::string> report_keys;
    for (auto &r : cfg.reports) {
        report_keys[{r.from, r.to}] = r.name;
    }

    // 为所有 seq matcher 注册完成回调：同 seq 内 stage 对输出到直方图 + CSV
    for (auto &[id, seq] : seq_map) {
        seq->onComplete([id, &hists, &report_keys](const StageSnapshot &s) {
            // 遍历所有 stage 对
            for (int i = 0; i < 64; i++) {
                if (!(s.stages & (1ULL << i))) continue;
                for (int j = i + 1; j < 64; j++) {
                    if (!(s.stages & (1ULL << j))) continue;
                    auto it = report_keys.find({i, j});
                    if (it != report_keys.end()) {
                        hists[it->second].record(
                            s.ts[j] - s.ts[i], i, j, s.ts[i], s.ts[j], s.tid);
                    }
                }
            }
        });
    }

    // 连接 seq → fifo：根据报告配置自动推断
    for (auto &r : cfg.reports) {
        // 找 from 和 to 分别属于哪个 seq
        SeqMatcher *push_seq = nullptr;
        SeqMatcher *pop_seq = nullptr;
        int push_stage = r.from, pop_stage = r.to;

        for (auto &[id, seq] : seq_map) {
            if (seq->hasStage(push_stage)) push_seq = seq;
            if (seq->hasStage(pop_stage))  pop_seq = seq;
        }
        if (!push_seq || !pop_seq) continue;

        // 同 seq 内的报告（如 0→1, 1→4 等）由 seq 自己的 delta 处理
        if (push_seq == pop_seq) continue;

        // 跨 seq：创建 fifo 并连接回调
        std::string fkey = r.name;  // 用报告名作为 fifo key
        if (!fifo_map.count(fkey)) {
            fifo_map[fkey] = new FifoQueue(fkey, 4096);
        }

        auto *fifo = fifo_map[fkey];

        push_seq->onComplete([fifo, push_stage](const StageSnapshot &s) {
            if (s.stages & (1ULL << push_stage))
                fifo->push(push_stage, s.ts[push_stage]);
        });

        pop_seq->onComplete([fifo, pop_stage, name = r.name, from = r.from, to = r.to, &hists](const StageSnapshot &s) {
            if (s.stages & (1ULL << pop_stage)) {
                uint64_t push_ts;
                if (fifo->pop(pop_stage, push_ts)) {
                    hists[name].record(s.ts[pop_stage] - push_ts, from, to,
                                        push_ts, s.ts[pop_stage], s.tid);
                }
            }
        });
    }

    // 7. 可选 CSV 输出
    if (!cfg.csv_path.empty()) {
        csv_file = fopen(cfg.csv_path.c_str(), "w");
        if (csv_file) {
            fprintf(csv_file, "from_stage,to_stage,delta_ns,ts_from,ts_to,tid\n");
            printf("  CSV raw data: %s\n", cfg.csv_path.c_str());
        } else {
            std::cerr << "warning: cannot open " << cfg.csv_path << "\n";
        }
    }

    // 8. ring buffer + 事件循环
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    auto handle_ring_event = [](void *ctx, void *data, size_t size) -> int {
        if (size < sizeof(RawEvent)) return 0;
        auto *e = (RawEvent *)data;
        auto *ms = (std::vector<Matcher *> *)ctx;
        RawEvent re{e->ts, e->tid, e->stage, e->key};
        for (auto *m : *ms) m->feed(re);
        return 0;
    };

    struct ring_buffer *rb = ring_buffer__new(rb_fd, handle_ring_event, &matchers, nullptr);

    if (!rb) { std::cerr << "ring_buffer__new failed\n"; return 1; }
    printf("\nCollecting... (Ctrl+C to stop)\n");

    while (!exiting)
        ring_buffer__poll(rb, 200);

    // 8. 输出报告（matcher 产生的 delta + fifo 直方图 + 未配对的报告）
    printf("\n=== Results ===\n");

    // drain 残留（seq 由 onComplete 处理，key/fifo 依赖 drain）
    for (auto &m : matchers) {
        auto deltas = m->drain();
        for (auto &d : deltas) {
            auto it = report_keys.find({d.from_stage, d.to_stage});
            if (it != report_keys.end())
                hists[it->second].record(d.val_ns, d.from_stage, d.to_stage,
                                          d.ts_from, d.ts_to, d.tid);
        }
    }

    if (!cfg.reports.empty()) {
        for (auto &r : cfg.reports) {
            auto it = hists.find(r.name);
            if (it != hists.end())
                it->second.print(r.name.c_str());
            else
                printf("\n  %s: (no samples)\n", r.name.c_str());
        }
    } else {
        for (auto &[key, hist] : hists)
            hist.print(key.c_str());
    }

    // 9. 清理
    ring_buffer__free(rb);
    for (auto *l : links) bpf_link__destroy(l);
    bpf_object__close(obj);
    for (auto *m : matchers) delete m;

    // 删除临时文件
    unlink(c_path.c_str());
    unlink(obj_path.c_str());
    if (csv_file) fclose(csv_file);
    return 0;
}
