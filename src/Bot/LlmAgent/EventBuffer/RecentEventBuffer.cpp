#include "EventBuffer/RecentEventBuffer.h"

void RecentEventBuffer::Configure(uint32_t capacity) {
    std::lock_guard<std::mutex> g(mu_);
    cap_ = capacity;
}

void RecentEventBuffer::Push(uint64_t bot_guid, std::string event) {
    std::lock_guard<std::mutex> g(mu_);
    if (cap_ == 0) return;
    auto& dq = buffers_[bot_guid];
    dq.push_back(std::move(event));
    while (dq.size() > cap_) dq.pop_front();
}

std::vector<std::string> RecentEventBuffer::Snapshot(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = buffers_.find(bot_guid);
    if (it == buffers_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

void RecentEventBuffer::Clear(uint64_t bot_guid) {
    std::lock_guard<std::mutex> g(mu_);
    buffers_.erase(bot_guid);
}

void RecentEventBuffer::ClearAll() {
    std::lock_guard<std::mutex> g(mu_);
    buffers_.clear();
}
