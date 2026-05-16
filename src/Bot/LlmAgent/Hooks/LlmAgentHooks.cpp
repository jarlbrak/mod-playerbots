#include "Hooks/LlmAgentHooks.h"

#ifndef LLMAGENT_UNIT_TESTS
#include "LlmAgentManager.h"
#include "Chat/WhisperBuffer.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "DBCStores.h"
#include "SharedDefines.h"
#endif

#include <ctime>
#include <sstream>

namespace {
constexpr size_t kMaxWhisperChars = 80;

std::string truncate_whisper(const std::string& s) {
    return s.size() <= kMaxWhisperChars ? s : s.substr(0, kMaxWhisperChars) + "...";
}

#ifndef LLMAGENT_UNIT_TESTS
std::string get_bot_zone_name(Player* bot) {
    if (!bot) return {};
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(bot->GetZoneId());
    if (!area || !area->area_name[0]) return {};
    return std::string(area->area_name[0]);
}
#endif
}  // namespace

namespace LlmAgentHooks {

void OnWhisperReceived(Player* bot, Player* sender, const std::string& text, uint32_t chat_type) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot || !sender) return;
    // Phase 5.2: filter out addon/system spam (DBM, Skada, LibGroupTalents,
    // LOOT_OPENED, etc. all flood the bot's chat queue when it's grouped
    // with a real player). Only feed real conversational types to T3.
    switch (chat_type) {
        case CHAT_MSG_WHISPER:
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_SAY:
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        case CHAT_MSG_YELL:
            break;
        case 0:  // legacy callers (no type) — treat as whisper
            chat_type = CHAT_MSG_WHISPER;
            break;
        default:
            return;
    }
    if (sPlayerbotsMgr.GetPlayerbotAI(sender) != nullptr) return;  // bot-to-bot whisper, ignore

    // Phase 5.2 v7: addon traffic in 3.3.5a uses regular chat channels (CHAT_MSG_PARTY
    // typically) with a tab-delimited "AddonPrefix\tpayload" string. The chat_type
    // filter can't catch these because the wire type is identical to real chat.
    // Heuristic: if a TAB appears in the first 25 chars, treat as addon traffic.
    // (Real player chat doesn't typically contain tabs early.)
    {
        size_t scan_end = text.size() < 25 ? text.size() : 25;
        for (size_t i = 0; i < scan_end; ++i) {
            if (text[i] == '\t') {
                LOG_INFO("server.loading",
                         "[LlmAgent] OnWhisperReceived skipped addon traffic: bot='{}' sender='{}' type={} text='{}'",
                         bot->GetName(), sender->GetName(), chat_type, truncate_whisper(text));
                return;
            }
        }
    }
    LOG_INFO("server.loading",
             "[LlmAgent] OnWhisperReceived fired: bot='{}' sender='{}' type={} text='{}'",
             bot->GetName(), sender->GetName(), chat_type, truncate_whisper(text));

    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return;

    const uint64_t bot_guid = bot->GetGUID().GetRawValue();

    if (mgr.Config().SocialOptIn) {
        mgr.Selector().OptInBot(bot_guid);
    }

    std::ostringstream ev;
    ev << "received whisper from " << sender->GetName() << ": " << truncate_whisper(text);
    mgr.Events().Push(bot_guid, ev.str());

    if (mgr.Config().MemorySidecar_EnableWrites) {
        std::vector<std::string> entities;
        entities.push_back(sender->GetName());
        std::string zone = get_bot_zone_name(bot);
        if (!zone.empty()) entities.push_back(zone);
        std::ostringstream txt;
        txt << "received whisper from " << sender->GetName();
        if (!zone.empty()) txt << " in " << zone;
        txt << ": " << truncate_whisper(text);
        std::vector<std::tuple<std::string, std::string, std::string>> relations;
        if (!zone.empty()) relations.emplace_back(sender->GetName(), "encountered_in", zone);
        mgr.MemoryClient().Remember(
            bot_guid, txt.str(), entities, /*salience*/ 0.7, relations);
    }
    mgr.Interactions().PushWhisper(
        bot_guid, sender->GetName(), sender->GetGUID().GetRawValue(),
        truncate_whisper(text), static_cast<int64_t>(time(nullptr)), chat_type);
    mgr.Whispers().Push(
        bot_guid, sender->GetGUID().GetRawValue(),
        WhisperEntry::Incoming,
        truncate_whisper(text),
        static_cast<int64_t>(time(nullptr)));
#endif
    (void)bot; (void)sender; (void)text;  // silence unused-param in unit-test build
}

void OnKill(Player* bot, const std::string& victim_name) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot) return;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return;
    const uint64_t bot_guid = bot->GetGUID().GetRawValue();
    mgr.Events().Push(bot_guid, "killed " + victim_name);

    if (mgr.Config().MemorySidecar_EnableWrites) {
        std::vector<std::string> entities;
        entities.push_back(victim_name);
        std::string zone = get_bot_zone_name(bot);
        if (!zone.empty()) entities.push_back(zone);
        std::string text = "killed " + victim_name;
        if (!zone.empty()) text += " in " + zone;
        std::vector<std::tuple<std::string, std::string, std::string>> relations;
        if (!zone.empty()) relations.emplace_back(victim_name, "located_in", zone);
        mgr.MemoryClient().Remember(
            bot_guid, text, entities, /*salience*/ 0.1, relations);
    }
#endif
    (void)bot; (void)victim_name;
}

void OnPartyInviteReceived(Player* bot, Player* inviter) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot || !inviter) return;
    if (sPlayerbotsMgr.GetPlayerbotAI(inviter) != nullptr) return;  // bot-to-bot
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return;
    const uint64_t bot_guid = bot->GetGUID().GetRawValue();
    if (mgr.Config().SocialOptIn) mgr.Selector().OptInBot(bot_guid);
    mgr.Interactions().PushInvite(
        bot_guid, inviter->GetName(), inviter->GetGUID().GetRawValue(),
        static_cast<int64_t>(time(nullptr)));
#endif
    (void)bot; (void)inviter;
}

void OnGroupJoined(Player* bot, Player* leader) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot || !leader) return;
    auto& mgr = LlmAgentManager::Instance();
    if (!mgr.Enabled()) return;
    mgr.Interactions().PushJoin(
        bot->GetGUID().GetRawValue(),
        leader->GetName(),
        leader->GetGUID().GetRawValue(),
        static_cast<int64_t>(time(nullptr)));
#endif
    (void)bot; (void)leader;
}

}  // namespace LlmAgentHooks
