#ifndef _PLAYERBOT_LLMAGENT_BOT_SELECTOR_H
#define _PLAYERBOT_LLMAGENT_BOT_SELECTOR_H

#include <cstdint>
#include <mutex>
#include <unordered_set>

class BotSelector {
  public:
    void Configure(uint32_t sample_pct, bool social_opt_in);
    bool IsLlmBot(uint64_t bot_guid) const;
    void OptInBot(uint64_t bot_guid);
    void Clear();

  private:
    static uint64_t FNV1aHash(uint64_t v);

    uint32_t                       sample_pct_     = 0;
    bool                           social_enabled_ = true;
    mutable std::mutex             opt_in_mu_;
    std::unordered_set<uint64_t>   opt_in_;
};

#endif
