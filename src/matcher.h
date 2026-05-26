#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

// 单条延迟
struct Delta {
    uint64_t val_ns;
    int from_stage;
    int to_stage;
    uint64_t ts_from;
    uint64_t ts_to;
    uint32_t tid;       // from_event 所在线程
};

// 原始事件（从 BPF ring buffer 读入）
struct RawEvent {
    uint64_t ts;
    uint32_t tid;
    uint8_t  stage;
    uint64_t key;       // key 模式用的请求 ID（非 key 模式为 0）
};

// 一组 stage 完成时的完整快照
struct StageSnapshot {
    uint64_t ts[64];
    uint64_t stages;
    uint64_t key;       // 这条记录的 key（key 模式用）
    uint32_t tid;
};

// matcher 基类
class Matcher {
public:
    virtual ~Matcher() = default;
    virtual void feed(const RawEvent &e) = 0;
    virtual std::vector<Delta> drain() = 0;
};

// 同线程顺序配对：同一 tid 上收齐指定 stages 后触发回调
class SeqMatcher : public Matcher {
public:
    using Cb = std::function<void(const StageSnapshot &)>;

    SeqMatcher(const std::string &id, std::vector<int> stages);
    void onComplete(Cb cb) { on_complete_.push_back(cb); }

    bool hasStage(int stage) const { return (stages_mask_ & (1ULL << stage)) != 0; }
    void feed(const RawEvent &e) override;
    std::vector<Delta> drain() override;

private:
    struct Record {
        uint64_t ts[64];
        uint64_t filled;
        uint32_t tid;
    };
    std::string id_;
    std::vector<int> stages_;
    uint64_t stages_mask_;
    int first_stage_;
    std::vector<Cb> on_complete_;

    static constexpr int TID_SLOTS = 65536;
    Record slots_[TID_SLOTS];
    uint32_t tid_hash(uint32_t tid) const { return tid % TID_SLOTS; }

    std::vector<Delta> pending_;
};

// 按 key 配对：不依赖线程或顺序，按请求 ID 分组
class KeyMatcher : public Matcher {
public:
    KeyMatcher(const std::string &id, std::vector<int> stages);
    void feed(const RawEvent &e) override;
    std::vector<Delta> drain() override;

private:
    struct Record {
        uint64_t ts[64];
        uint64_t filled;
        uint32_t tid;
    };
    std::string id_;
    std::vector<int> stages_;
    uint64_t stages_mask_;
    int first_stage_;
    std::unordered_map<uint64_t, Record> records_;  // key → record
    std::vector<Delta> pending_;
};

// FIFO 队列
class FifoQueue {
public:
    FifoQueue(const std::string &id, int capacity = 4096);
    ~FifoQueue();

    void push(int stage, uint64_t ts);
    bool pop(int stage, uint64_t &ts);

private:
    std::string id_;
    int capacity_, head_ = 0, tail_ = 0;
    uint64_t *queue_;
};
