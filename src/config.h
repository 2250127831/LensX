#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct ProbeConfig {
    std::string name;
    std::string type;       // uprobe / kprobe / tracepoint
    std::string symbol;     // uprobe/kprobe: 函数名（用户态 demangled / 内核原生）
    std::string event;      // tracepoint: "category/name"
    int stage = 0;
    std::string key;        // key 模式：从哪里提取请求 ID，如 "arg1" / "arg2"
};

struct MatcherConfig {
    std::string id;
    std::vector<int> stages;
    std::string mode;       // seq / fifo / key
    int queue_size = 4096;
};

struct ReportConfig {
    std::string name;
    int from = 0;
    int to = 0;
};

struct Config {
    std::string process;    // 用于 pgrep（与 --pid 二选一）
    std::string csv_path;   // 原始数据 CSV 输出路径（可选）
    std::vector<ProbeConfig> probes;
    std::vector<MatcherConfig> matchers;
    std::vector<ReportConfig> reports;
};

Config parse_config(const std::string &path);
