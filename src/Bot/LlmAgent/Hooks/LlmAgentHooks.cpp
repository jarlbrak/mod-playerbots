#include "Hooks/LlmAgentHooks.h"

#ifndef LLMAGENT_UNIT_TESTS
#include "LlmAgentManager.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#endif

#include <sstream>

namespace {
constexpr size_t kMaxWhisperChars = 80;

std::string truncate_whisper(const std::string& s) {
    return s.size() <= kMaxWhisperChars ? s : s.substr(0, kMaxWhisperChars) + "...";
}
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
#endif
    (void)bot; (void)victim_name;
}

}  // namespace LlmAgentHooks
