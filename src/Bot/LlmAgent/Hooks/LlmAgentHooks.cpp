#include "Hooks/LlmAgentHooks.h"

#ifndef LLMAGENT_UNIT_TESTS
#include "LlmAgentManager.h"
#include "Chat/WhisperBuffer.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "DBCStores.h"
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

void OnWhisperReceived(Player* bot, Player* sender, const std::string& text) {
#ifndef LLMAGENT_UNIT_TESTS
    if (!bot || !sender) return;
    if (sPlayerbotsMgr.GetPlayerbotAI(sender) != nullptr) return;  // bot-to-bot whisper, ignore

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
    LOG_INFO("playerbots", "[LlmAgent] OnWhisperReceived: bot_guid={} sender='{}'",
             bot_guid, sender->GetName());
    mgr.Interactions().PushWhisper(
        bot_guid, sender->GetName(), sender->GetGUID().GetRawValue(),
        truncate_whisper(text), static_cast<int64_t>(time(nullptr)));
    LOG_INFO("playerbots", "[LlmAgent] OnWhisperReceived: PushWhisper done; about to Whispers().Push");
    mgr.Whispers().Push(
        bot_guid, sender->GetGUID().GetRawValue(),
        WhisperEntry::Incoming,
        truncate_whisper(text),
        static_cast<int64_t>(time(nullptr)));
    LOG_INFO("playerbots", "[LlmAgent] OnWhisperReceived: Whispers().Push done");
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
