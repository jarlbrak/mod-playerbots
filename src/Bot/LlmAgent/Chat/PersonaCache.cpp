#include "Chat/PersonaCache.h"

PersonaCache::PersonaCache(Fetcher fetcher, int64_t ttl_seconds, ClockFn clock)
    : fetcher_(std::move(fetcher)),
      ttl_seconds_(ttl_seconds),
      clock_(std::move(clock)) {}

std::string PersonaCache::Get(uint64_t bot_guid) {
    const int64_t now = clock_();
    {
        std::lock_guard<std::mutex> g(mu_);
        auto it = by_bot_.find(bot_guid);
        if (it != by_bot_.end() && (now - it->second.fetched_at) < ttl_seconds_)
            return it->second.text;
    }
    std::string text;
    if (auto fetched = fetcher_(bot_guid))
        text = std::move(*fetched);
    {
        std::lock_guard<std::mutex> g(mu_);
        by_bot_[bot_guid] = Entry{text, now};
    }
    return text;
}

void PersonaCache::Invalidate(uint64_t bot_guid) {
    std::lock_guard<std::mutex> g(mu_);
    by_bot_.erase(bot_guid);
}

void PersonaCache::ClearAll() {
    std::lock_guard<std::mutex> g(mu_);
    by_bot_.clear();
}
