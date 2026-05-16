#ifndef _PLAYERBOT_LLMAGENT_RECENT_EVENT_BUFFER_H
#define _PLAYERBOT_LLMAGENT_RECENT_EVENT_BUFFER_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class RecentEventBuffer {
  public:
    void Configure(uint32_t capacity);
    void Push(uint64_t bot_guid, std::string event);
    std::vector<std::string> Snapshot(uint64_t bot_guid) const;
    void Clear(uint64_t bot_guid);
    void ClearAll();

  private:
    mutable std::mutex                                       mu_;
    uint32_t                                                 cap_ = 20;
    std::unordered_map<uint64_t, std::deque<std::string>>    buffers_;
};

#endif
