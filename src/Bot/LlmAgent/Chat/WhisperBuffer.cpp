#include "Chat/WhisperBuffer.h"

#include <algorithm>

void WhisperBuffer::Push(uint64_t bot_guid, uint64_t sender_guid,
                         WhisperEntry::Direction dir, std::string text, int64_t ts) {
    std::lock_guard<std::mutex> g(mu_);
    auto& dq = by_pair_[{bot_guid, sender_guid}];
    dq.push_back(WhisperEntry{dir, std::move(text), ts});
}

std::vector<WhisperEntry>
WhisperBuffer::SnapshotFor(uint64_t bot_guid, uint64_t sender_guid,
                           int64_t now, int64_t window_seconds,
                           std::size_t max_n) const {
    std::vector<WhisperEntry> out;
    std::lock_guard<std::mutex> g(mu_);
    auto it = by_pair_.find({bot_guid, sender_guid});
    if (it == by_pair_.end()) return out;
    const int64_t cutoff = now - window_seconds;
    for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
        if (rit->ts <= cutoff) break;
        out.push_back(*rit);
        if (out.size() == max_n) break;
    }
    return out;
}

void WhisperBuffer::ExpireOlderThan(int64_t now, int64_t window_seconds) {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t cutoff = now - window_seconds;
    for (auto it = by_pair_.begin(); it != by_pair_.end(); ) {
        auto& dq = it->second;
        dq.erase(std::remove_if(dq.begin(), dq.end(),
                                [cutoff](const WhisperEntry& e){ return e.ts <= cutoff; }),
                 dq.end());
        if (dq.empty()) it = by_pair_.erase(it);
        else            ++it;
    }
}

void WhisperBuffer::Clear(uint64_t bot_guid, uint64_t sender_guid) {
    std::lock_guard<std::mutex> g(mu_);
    by_pair_.erase({bot_guid, sender_guid});
}

void WhisperBuffer::ClearAll() {
    std::lock_guard<std::mutex> g(mu_);
    by_pair_.clear();
}
