#include "EventBuffer/InteractionEventBuffer.h"
#include <algorithm>

void InteractionEventBuffer::PushWhisper(uint64_t bot_guid, std::string from_name,
                                          uint64_t from_guid, std::string text,
                                          int64_t ts) {
    std::lock_guard<std::mutex> g(mu_);
    whispers_[bot_guid].push_back({std::move(from_name), from_guid, std::move(text), ts});
}

void InteractionEventBuffer::PushInvite(uint64_t bot_guid, std::string from_name,
                                         uint64_t from_guid, int64_t ts) {
    std::lock_guard<std::mutex> g(mu_);
    invites_[bot_guid].push_back({std::move(from_name), from_guid, ts});
}

void InteractionEventBuffer::PushJoin(uint64_t bot_guid, std::string leader_name,
                                       uint64_t leader_guid, int64_t ts) {
    std::lock_guard<std::mutex> g(mu_);
    joins_[bot_guid].push_back({std::move(leader_name), leader_guid, ts});
}

bool InteractionEventBuffer::HasPending(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    auto inv = invites_.find(bot_guid);
    if (inv != invites_.end() && !inv->second.empty()) return true;
    auto wh = whispers_.find(bot_guid);
    if (wh != whispers_.end() && !wh->second.empty()) return true;
    auto jn = joins_.find(bot_guid);
    if (jn != joins_.end() && !jn->second.empty()) return true;
    return false;
}

InteractionPayload InteractionEventBuffer::SnapshotFor(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    InteractionPayload p;
    if (auto it = invites_.find(bot_guid);  it != invites_.end())  p.pending_invites = it->second;
    if (auto it = whispers_.find(bot_guid); it != whispers_.end()) p.recent_whispers = it->second;
    if (auto it = joins_.find(bot_guid);    it != joins_.end())    p.recent_group_joins = it->second;
    return p;
}

void InteractionEventBuffer::ExpireOlderThan(uint64_t bot_guid, int64_t now, int64_t window_seconds) {
    std::lock_guard<std::mutex> g(mu_);
    const int64_t cutoff = now - window_seconds;
    auto drop_older = [&](auto& vec) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                  [cutoff](const auto& e){ return e.ts < cutoff; }),
                  vec.end());
    };
    if (auto it = invites_.find(bot_guid);  it != invites_.end())  drop_older(it->second);
    if (auto it = whispers_.find(bot_guid); it != whispers_.end()) drop_older(it->second);
    if (auto it = joins_.find(bot_guid);    it != joins_.end())    drop_older(it->second);
}

void InteractionEventBuffer::Clear(uint64_t bot_guid) {
    std::lock_guard<std::mutex> g(mu_);
    invites_.erase(bot_guid);
    whispers_.erase(bot_guid);
    joins_.erase(bot_guid);
}

void InteractionEventBuffer::ClearAll() {
    std::lock_guard<std::mutex> g(mu_);
    invites_.clear();
    whispers_.clear();
    joins_.clear();
}
