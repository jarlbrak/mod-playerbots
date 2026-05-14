#ifndef _PLAYERBOT_LLMAGENT_WHISPER_BUFFER_H
#define _PLAYERBOT_LLMAGENT_WHISPER_BUFFER_H

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct WhisperEntry {
    enum Direction : uint8_t { Incoming = 0, Outgoing = 1 };
    Direction   direction = Incoming;
    std::string text;
    int64_t     ts = 0;
};

class WhisperBuffer {
  public:
    void Push(uint64_t bot_guid, uint64_t sender_guid,
              WhisperEntry::Direction dir, std::string text, int64_t ts);

    // Returns last up-to-max_n entries newest-first whose ts > now-window.
    std::vector<WhisperEntry> SnapshotFor(uint64_t bot_guid, uint64_t sender_guid,
                                          int64_t now, int64_t window_seconds,
                                          std::size_t max_n) const;

    void ExpireOlderThan(int64_t now, int64_t window_seconds);
    void Clear(uint64_t bot_guid, uint64_t sender_guid);
    void ClearAll();

  private:
    using Key = std::pair<uint64_t, uint64_t>;

    mutable std::mutex                       mu_;
    std::map<Key, std::deque<WhisperEntry>>  by_pair_;
};

#endif  // _PLAYERBOT_LLMAGENT_WHISPER_BUFFER_H
