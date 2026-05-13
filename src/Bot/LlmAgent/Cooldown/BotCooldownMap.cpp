#include "Cooldown/BotCooldownMap.h"

bool BotCooldownMap::Eligible(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = cooldowns_.find(bot_guid);
    if (it == cooldowns_.end()) return true;
    return std::chrono::steady_clock::now() >= it->second;
}

void BotCooldownMap::Set(uint64_t bot_guid, std::chrono::steady_clock::time_point until) {
    std::lock_guard<std::mutex> g(mu_);
    cooldowns_[bot_guid] = until;
}

void BotCooldownMap::Clear() {
    std::lock_guard<std::mutex> g(mu_);
    cooldowns_.clear();
}

std::optional<int64_t> BotCooldownMap::RemainingMs(uint64_t bot_guid) const {
    std::lock_guard<std::mutex> g(mu_);
    auto it = cooldowns_.find(bot_guid);
    if (it == cooldowns_.end()) return std::nullopt;
    auto delta = it->second - std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
}
