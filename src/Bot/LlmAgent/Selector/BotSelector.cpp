#include "Selector/BotSelector.h"

void BotSelector::Configure(uint32_t sample_pct, bool social_opt_in) {
    sample_pct_     = sample_pct > 100 ? 100 : sample_pct;
    social_enabled_ = social_opt_in;
}

bool BotSelector::IsLlmBot(uint64_t bot_guid) const {
    if (sample_pct_ > 0 && (FNV1aHash(bot_guid) % 100) < sample_pct_) return true;
    if (!social_enabled_) return false;
    std::lock_guard<std::mutex> g(opt_in_mu_);
    return opt_in_.count(bot_guid) > 0;
}

void BotSelector::OptInBot(uint64_t bot_guid) {
    std::lock_guard<std::mutex> g(opt_in_mu_);
    opt_in_.insert(bot_guid);
}

void BotSelector::Clear() {
    std::lock_guard<std::mutex> g(opt_in_mu_);
    opt_in_.clear();
}

uint64_t BotSelector::FNV1aHash(uint64_t v) {
    constexpr uint64_t kOffset = 14695981039346656037ULL;
    constexpr uint64_t kPrime  = 1099511628211ULL;
    uint64_t h = kOffset;
    for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xFFu);
        h *= kPrime;
        v >>= 8;
    }
    return h;
}
