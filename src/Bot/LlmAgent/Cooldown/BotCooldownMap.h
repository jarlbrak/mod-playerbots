#ifndef _PLAYERBOT_LLMAGENT_BOT_COOLDOWN_MAP_H
#define _PLAYERBOT_LLMAGENT_BOT_COOLDOWN_MAP_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

class BotCooldownMap {
  public:
    bool Eligible(uint64_t bot_guid) const;
    void Set(uint64_t bot_guid, std::chrono::steady_clock::time_point until);
    void Clear();

    // Returns nullopt if no cooldown set; else ms until expiry (may be negative if past).
    std::optional<int64_t> RemainingMs(uint64_t bot_guid) const;

  private:
    mutable std::mutex                                              mu_;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> cooldowns_;
};

#endif
