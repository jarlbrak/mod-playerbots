#ifndef _PLAYERBOT_LLMAGENT_INTERACTION_EVENT_BUFFER_H
#define _PLAYERBOT_LLMAGENT_INTERACTION_EVENT_BUFFER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct PendingInvite {
    std::string from_name;
    uint64_t    from_guid = 0;
    int64_t     ts = 0;
};

struct UnhandledWhisper {
    std::string from_name;
    uint64_t    from_guid = 0;
    std::string text;
    int64_t     ts = 0;
};

struct RecentGroupJoin {
    std::string leader_name;
    uint64_t    leader_guid = 0;
    int64_t     ts = 0;
};

struct InteractionPayload {
    std::vector<PendingInvite>    pending_invites;
    std::vector<UnhandledWhisper> recent_whispers;
    std::vector<RecentGroupJoin>  recent_group_joins;
};

class InteractionEventBuffer {
  public:
    void PushWhisper(uint64_t bot_guid, std::string from_name, uint64_t from_guid,
                     std::string text, int64_t ts);
    void PushInvite (uint64_t bot_guid, std::string from_name, uint64_t from_guid,
                     int64_t ts);
    void PushJoin   (uint64_t bot_guid, std::string leader_name, uint64_t leader_guid,
                     int64_t ts);

    bool HasPending(uint64_t bot_guid) const;
    InteractionPayload SnapshotFor(uint64_t bot_guid) const;
    void ExpireOlderThan(uint64_t bot_guid, int64_t now, int64_t window_seconds);
    void Clear(uint64_t bot_guid);
    void ClearAll();

  private:
    mutable std::mutex                                          mu_;
    std::unordered_map<uint64_t, std::vector<PendingInvite>>    invites_;
    std::unordered_map<uint64_t, std::vector<UnhandledWhisper>> whispers_;
    std::unordered_map<uint64_t, std::vector<RecentGroupJoin>>  joins_;
};

#endif
