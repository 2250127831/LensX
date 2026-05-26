#pragma once

#include <string>
#include "config.h"

// 根据 config 生成 BPF C 源码并编译为 .o 文件。
//
// 生成流程：
//   generate_bpf_source(config) → /tmp/lensx_XXXXXX.c
//   compile_bpf(c_source)       → /tmp/lensx_XXXXXX.o
//
// 返回 .o 文件路径，或抛异常。

std::string generate_bpf_source(const Config &cfg, const std::string &out_c);
std::string compile_bpf(const std::string &c_path);
