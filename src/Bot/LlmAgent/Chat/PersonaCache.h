#ifndef _PLAYERBOT_LLMAGENT_PERSONA_CACHE_H
#define _PLAYERBOT_LLMAGENT_PERSONA_CACHE_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class PersonaCache {
  public:
    using Fetcher = std::function<std::optional<std::string>(uint64_t bot_guid)>;
    using ClockFn = std::function<int64_t()>;          // returns unix seconds

    PersonaCache(Fetcher fetcher, int64_t ttl_seconds, ClockFn clock);

    // Returns cached value, or fetches and caches on miss/TTL expiry.
    // If fetcher returns nullopt, an empty string is cached (sticky-empty).
    std::string Get(uint64_t bot_guid);

    void Invalidate(uint64_t bot_guid);
    void ClearAll();

  private:
    struct Entry { std::string text; int64_t fetched_at = 0; };

    Fetcher          fetcher_;
    int64_t          ttl_seconds_;
    ClockFn          clock_;
    mutable std::mutex                          mu_;
    std::unordered_map<uint64_t, Entry>         by_bot_;
};

#endif  // _PLAYERBOT_LLMAGENT_PERSONA_CACHE_H
