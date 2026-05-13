#ifndef _PLAYERBOT_LLMAGENT_MEMORY_HTTP_CLIENT_H
#define _PLAYERBOT_LLMAGENT_MEMORY_HTTP_CLIENT_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

class MemoryHttpClient {
  public:
    MemoryHttpClient(std::string endpoint, std::chrono::milliseconds timeout);

    std::vector<std::string> RecallAbout(uint64_t bot_guid,
                                          const std::string& entity,
                                          uint32_t max_hops = 2,
                                          uint32_t top_k = 3);

    bool Remember(uint64_t bot_guid,
                  const std::string& text,
                  const std::vector<std::string>& entities,
                  double salience,
                  const std::vector<std::tuple<std::string, std::string, std::string>>& relations = {});

    std::optional<std::string> GetPersonality(uint64_t bot_guid);
    bool SetPersonality(uint64_t bot_guid, const std::string& persona);

    bool Available() const;

  private:
    bool ShortCircuitBecauseDown() const;
    void MarkDown();
    void MarkUp();

    std::string                                       endpoint_;
    std::chrono::milliseconds                         timeout_;
    mutable std::atomic<int64_t>                      down_until_ms_{0};
};

#endif
