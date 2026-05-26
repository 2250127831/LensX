#include "config.h"
#include <yaml-cpp/yaml.h>
#include <iostream>
#include <cstdlib>

Config parse_config(const std::string &path)
{
    YAML::Node root = YAML::LoadFile(path);

    Config cfg;

    // ── target（仅 process 名，PID 由命令行 --pid 传入）──
    if (root["target"]) {
        auto t = root["target"];
        if (t["process"])
            cfg.process = t["process"].as<std::string>();
    }

    // ── output ──
    if (root["output"] && root["output"]["csv"])
        cfg.csv_path = root["output"]["csv"].as<std::string>();

    // ── probes ──
    if (root["probes"]) {
        for (auto node : root["probes"]) {
            ProbeConfig p;
            p.name  = node["name"].as<std::string>();
            p.type  = node["type"].as<std::string>();
            p.stage = node["stage"].as<int>();
            if (node["symbol"])
                p.symbol = node["symbol"].as<std::string>();
            if (node["event"])
                p.event = node["event"].as<std::string>();
            if (node["key"])
                p.key = node["key"].as<std::string>();
            cfg.probes.push_back(p);
        }
    }

    // ── matchers ──
    if (root["matchers"]) {
        for (auto node : root["matchers"]) {
            MatcherConfig m;
            m.id    = node["id"].as<std::string>();
            m.mode  = node["mode"].as<std::string>();
            for (auto s : node["stages"])
                m.stages.push_back(s.as<int>());
            if (node["queue_size"])
                m.queue_size = node["queue_size"].as<int>();
            cfg.matchers.push_back(m);
        }
    }

    // ── reports ──
    if (root["reports"]) {
        for (auto node : root["reports"]) {
            ReportConfig r;
            r.name = node["name"].as<std::string>();
            r.from = node["from"].as<int>();
            r.to   = node["to"].as<int>();
            cfg.reports.push_back(r);
        }
    }

    // 验证
    if (cfg.probes.empty()) {
        std::cerr << "error: no probes defined\n";
        exit(1);
    }
    for (auto &p : cfg.probes) {
        if (p.type == "tracepoint" && p.event.empty()) {
            std::cerr << "error: probe '" << p.name << "' needs 'event'\n";
            exit(1);
        }
        if ((p.type == "uprobe" || p.type == "kprobe") && p.symbol.empty()) {
            std::cerr << "error: probe '" << p.name << "' needs 'symbol'\n";
            exit(1);
        }
    }

    return cfg;
}
