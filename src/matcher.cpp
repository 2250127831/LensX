#include "matcher.h"
#include <cstring>
#include <cstdio>

// ── SeqMatcher ──

SeqMatcher::SeqMatcher(const std::string &id, std::vector<int> stages)
    : id_(id), stages_(std::move(stages))
{
    std::memset(slots_, 0, sizeof(slots_));
    stages_mask_ = 0;
    first_stage_ = 64;
    for (int s : stages_) {
        stages_mask_ |= (1ULL << s);
        if (s < first_stage_) first_stage_ = s;
    }
}

void SeqMatcher::feed(const RawEvent &e)
{
    int stage = e.stage;
    if (!(stages_mask_ & (1ULL << stage)))
        return;

    auto &slot = slots_[tid_hash(e.tid)];

    if (stage == first_stage_) {
        slot.filled = 0;
        slot.tid = e.tid;
    }

    slot.ts[stage] = e.ts;
    slot.filled |= (1ULL << stage);

    if ((slot.filled & stages_mask_) == stages_mask_) {
        // 输出所有 stage 对间的 delta
        for (size_t i = 0; i < stages_.size(); i++) {
            for (size_t j = i + 1; j < stages_.size(); j++) {
                int from = stages_[i], to = stages_[j];
                pending_.push_back({slot.ts[to] - slot.ts[from], from, to,
                                    slot.ts[from], slot.ts[to], slot.tid});
            }
        }

        // 通知完成回调（带完整时间戳快照）
        StageSnapshot snap;
        snap.stages = stages_mask_;
        snap.tid = slot.tid;
        for (int s : stages_)
            snap.ts[s] = slot.ts[s];

        for (auto &cb : on_complete_)
            cb(snap);

        slot.filled = 0;
    }
}

std::vector<Delta> SeqMatcher::drain()
{
    auto ret = std::move(pending_);
    pending_.clear();
    return ret;
}

// ── KeyMatcher ──

KeyMatcher::KeyMatcher(const std::string &id, std::vector<int> stages)
    : id_(id), stages_(std::move(stages))
{
    stages_mask_ = 0;
    first_stage_ = 64;
    for (int s : stages_) {
        stages_mask_ |= (1ULL << s);
        if (s < first_stage_) first_stage_ = s;
    }
}

void KeyMatcher::feed(const RawEvent &e)
{
    int stage = e.stage;
    if (!(stages_mask_ & (1ULL << stage)))
        return;

    auto &rec = records_[e.key];
    if (stage == first_stage_)
        rec.tid = e.tid;
    rec.ts[stage] = e.ts;
    rec.filled |= (1ULL << stage);

    if ((rec.filled & stages_mask_) == stages_mask_) {
        for (size_t i = 0; i < stages_.size(); i++) {
            for (size_t j = i + 1; j < stages_.size(); j++) {
                int from = stages_[i], to = stages_[j];
                pending_.push_back({rec.ts[to] - rec.ts[from], from, to,
                                    rec.ts[from], rec.ts[to], rec.tid});
            }
        }
        records_.erase(e.key);
    }
}

std::vector<Delta> KeyMatcher::drain()
{
    auto ret = std::move(pending_);
    pending_.clear();
    return ret;
}

// ── FifoQueue ──

FifoQueue::FifoQueue(const std::string &id, int capacity)
    : id_(id), capacity_(capacity)
{
    queue_ = new uint64_t[capacity_];
}

FifoQueue::~FifoQueue() { delete[] queue_; }

void FifoQueue::push(int stage, uint64_t ts)
{
    (void)stage;
    uint32_t slot = tail_++ % capacity_;
    if (tail_ - head_ > capacity_) {
        fprintf(stderr, "FifoQueue[%s] overflow\n", id_.c_str());
        head_ = tail_ - capacity_;
    }
    queue_[slot] = ts;
}

bool FifoQueue::pop(int stage, uint64_t &ts)
{
    (void)stage;
    if (head_ >= tail_) return false;
    uint32_t slot = head_++ % capacity_;
    ts = queue_[slot];
    return true;
}
