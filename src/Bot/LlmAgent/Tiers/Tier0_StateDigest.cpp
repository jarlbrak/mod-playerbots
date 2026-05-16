#include "Tiers/Tier0_StateDigest.h"

nlohmann::json BuildDigestJson(const LlmBotState& s) {
    nlohmann::json j;

    j["self"] = {
        {"name",          s.self.name},
        {"race",          s.self.race},
        {"class",         s.self.character_class},
        {"spec",          s.self.spec},
        {"level",         s.self.level},
        {"hp_pct",        s.self.hp_pct},
        {"mana_pct",      s.self.mana_pct < 0 ? nlohmann::json(nullptr) : nlohmann::json(s.self.mana_pct)},
        {"gold_copper",   s.self.gold_copper},
        {"is_in_combat",  s.self.is_in_combat},
        {"is_resting",    s.self.is_resting},
        {"is_dead",       s.self.is_dead},
    };

    j["location"] = {
        {"map",       s.location.map},
        {"zone",      s.location.zone},
        {"subzone",   s.location.subzone},
        {"position",  s.location.position},
        {"near_npcs", s.location.near_npcs},
    };

    // goal.params is a verbatim JSON string; parse so the digest doesn't
    // contain a string that itself contains JSON.
    nlohmann::json goal_params = nlohmann::json::object();
    if (!s.goal.params_json.empty()) {
        try { goal_params = nlohmann::json::parse(s.goal.params_json); }
        catch (...) { goal_params = nlohmann::json::object(); }
    }
    j["goal"] = {
        {"current",         s.goal.current},
        {"params",          goal_params},
        {"progress_pct",    s.goal.progress_pct},
        {"elapsed_minutes", s.goal.elapsed_minutes},
        {"ttl_minutes",     s.goal.ttl_minutes},
    };

    j["quest_log"] = nlohmann::json::array();
    for (const auto& q : s.quest_log) {
        j["quest_log"].push_back({{"id", q.id}, {"title", q.title}, {"progress", q.progress}});
    }

    j["inventory_highlights"] = {
        {"bag_used",            s.inventory.bag_used},
        {"junk_value_copper",   s.inventory.junk_value_copper},
        {"consumables",         s.inventory.consumables},
        {"gear_vs_level_score", s.inventory.gear_vs_level_score},
    };

    nlohmann::json humans = nlohmann::json::array();
    for (const auto& h : s.social.nearby_humans) {
        humans.push_back({{"name", h.name}, {"level", h.level}, {"distance", h.distance}});
    }
    nlohmann::json whispers = nlohmann::json::array();
    for (const auto& w : s.social.recent_whispers) {
        whispers.push_back({{"from", w.from}, {"text", w.text}, {"age_s", w.age_s}});
    }
    j["social"] = {
        {"in_group",         s.social.in_group},
        {"group_members",    s.social.group_members},
        {"guild",            s.social.guild.empty() ? nlohmann::json(nullptr) : nlohmann::json(s.social.guild)},
        {"nearby_humans",    humans},
        {"recent_whispers",  whispers},
    };

    j["event_log"] = s.event_log;
    j["memory_hints"] = s.memory_hints;

    return j;
}

// ===========================================================================
// Worldserver-thread only. NOT linked into unit tests.
// ===========================================================================
#ifndef LLMAGENT_UNIT_TESTS

#include "PlayerbotAI.h"
#include "Player.h"
#include "Map.h"
#include "QuestDef.h"
#include "LlmAgentManager.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "AiObjectContext.h"
#include "Memory/PersonalityCard.h"
#include <algorithm>
#include <utility>

namespace {

std::string class_name_lower(uint8 cls) {
    switch (cls) {
        case CLASS_WARRIOR: return "warrior";
        case CLASS_PALADIN: return "paladin";
        case CLASS_HUNTER:  return "hunter";
        case CLASS_ROGUE:   return "rogue";
        case CLASS_PRIEST:  return "priest";
        case CLASS_DEATH_KNIGHT: return "death_knight";
        case CLASS_SHAMAN:  return "shaman";
        case CLASS_MAGE:    return "mage";
        case CLASS_WARLOCK: return "warlock";
        case CLASS_DRUID:   return "druid";
        default: return "unknown";
    }
}

std::string race_name_lower(uint8 r) {
    switch (r) {
        case RACE_HUMAN:    return "human";
        case RACE_ORC:      return "orc";
        case RACE_DWARF:    return "dwarf";
        case RACE_NIGHTELF: return "night_elf";
        case RACE_UNDEAD_PLAYER: return "undead";
        case RACE_TAUREN:   return "tauren";
        case RACE_GNOME:    return "gnome";
        case RACE_TROLL:    return "troll";
        case RACE_BLOODELF: return "blood_elf";
        case RACE_DRAENEI:  return "draenei";
        default: return "unknown";
    }
}

}  // anon

LlmBotState SnapshotBot(PlayerbotAI* botAI) {
    LlmBotState s;
    if (!botAI) return s;
    Player* bot = botAI->GetBot();
    if (!bot) return s;

    s.self.name             = bot->GetName();
    s.self.race             = race_name_lower(bot->getRace());
    s.self.character_class  = class_name_lower(bot->getClass());
    s.self.level            = bot->GetLevel();
    s.self.hp_pct           = bot->GetMaxHealth() > 0 ? int(100.0f * bot->GetHealth() / bot->GetMaxHealth()) : 0;
    s.self.mana_pct         = bot->GetMaxPower(POWER_MANA) > 0 ? int(100.0f * bot->GetPower(POWER_MANA) / bot->GetMaxPower(POWER_MANA)) : -1;
    s.self.gold_copper      = static_cast<int64_t>(bot->GetMoney());
    s.self.is_in_combat     = bot->IsInCombat();
    s.self.is_resting       = bot->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_RESTING);
    s.self.is_dead          = bot->isDead();

    if (Map* m = bot->GetMap()) {
        s.location.map      = m->GetMapName() ? m->GetMapName() : "";
    }
    s.location.zone         = sAreaTableStore.LookupEntry(bot->GetZoneId()) ?
                              sAreaTableStore.LookupEntry(bot->GetZoneId())->area_name[0] : "";
    s.location.subzone      = sAreaTableStore.LookupEntry(bot->GetAreaId()) ?
                              sAreaTableStore.LookupEntry(bot->GetAreaId())->area_name[0] : "";
    s.location.position     = {bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()};

    // Goal block from rpgInfo.
    auto& rpg = botAI->rpgInfo;
    switch (rpg.GetStatus()) {
        case RPG_IDLE:          s.goal.current = "Idle";         break;
        case RPG_GO_GRIND:      s.goal.current = "GoGrind";      break;
        case RPG_GO_CAMP:       s.goal.current = "GoCamp";       break;
        case RPG_WANDER_NPC:    s.goal.current = "WanderNpc";    break;
        case RPG_WANDER_RANDOM: s.goal.current = "WanderRandom"; break;
        case RPG_DO_QUEST:      s.goal.current = "DoQuest";      break;
        case RPG_TRAVEL_FLIGHT: s.goal.current = "TravelFlight"; break;
        case RPG_REST:          s.goal.current = "Rest";         break;
        case RPG_OUTDOOR_PVP:   s.goal.current = "OutdoorPvp";   break;
        default:                s.goal.current = "Idle";         break;
    }
    if (auto* dq = std::get_if<NewRpgInfo::DoQuest>(&rpg.data)) {
        nlohmann::json p;
        p["quest_id"] = dq->questId;
        p["objective_idx"] = dq->objectiveIdx;
        s.goal.params_json = p.dump();
    }

    // Quest log: walk the first N slots.
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE && s.quest_log.size() < 10; ++slot) {
        uint32 qid = bot->GetQuestSlotQuestId(slot);
        if (!qid) continue;
        const Quest* q = sObjectMgr->GetQuestTemplate(qid);
        if (!q) continue;
        QuestLogEntry e;
        e.id = qid;
        e.title = q->GetTitle();
        QuestStatus st = bot->GetQuestStatus(qid);
        e.progress = (st == QUEST_STATUS_COMPLETE) ? "complete, turn in" : "in progress";
        s.quest_log.push_back(std::move(e));
    }

    // Inventory highlights: bag-used summary; consumable detection deferred.
    // Iterate slots directly (AC's Bag::GetItemCount requires an item-id arg).
    uint32 used = 0, total = 0;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag) {
        if (Bag* b = bot->GetBagByPos(bag)) {
            uint32 size = b->GetBagSize();
            total += size;
            for (uint32 slot = 0; slot < size; ++slot) {
                if (b->GetItemByPos(slot))
                    ++used;
            }
        }
    }
    s.inventory.bag_used = std::to_string(used) + "/" + std::to_string(total);

    // ===== Phase 2 enrichment =====

    // event_log: pull from LlmAgentManager's per-bot ring buffer.
    s.event_log = LlmAgentManager::Instance().Events().Snapshot(
        bot->GetGUID().GetRawValue());

    // social.nearby_humans: pull from existing AI value "nearest friendly
    // players" (a GuidVector), resolve each guid to a Player*, filter out
    // playerbots, cap at 5 by distance.
    {
        GuidVector nearest = botAI->GetAiObjectContext()
            ->GetValue<GuidVector>("nearest friendly players")->Get();
        std::vector<std::pair<float, Player*>> humans;
        for (ObjectGuid const& g : nearest) {
            Player* p = ObjectAccessor::FindPlayer(g);
            if (!p || p == bot) continue;
            if (sPlayerbotsMgr.GetPlayerbotAI(p) != nullptr) continue;
            humans.emplace_back(bot->GetDistance(p), p);
        }
        std::sort(humans.begin(), humans.end(),
                  [](auto& a, auto& b){ return a.first < b.first; });
        for (size_t i = 0; i < humans.size() && i < 5; ++i) {
            NearbyHuman nh;
            nh.name     = humans[i].second->GetName();
            nh.level    = humans[i].second->GetLevel();
            nh.distance = humans[i].first;
            s.social.nearby_humans.push_back(std::move(nh));
        }
    }

    // social.recent_whispers: empty in Phase 2; whisper hook (Task 9) drops
    // whisper content into event_log instead. If smoke data shows the LLM
    // wants the structured form, Phase 3 adds a second sliding window.

    // ===== Phase 3: memory_hints + persona =====
    {
        auto& mgr = LlmAgentManager::Instance();
        auto& mem = mgr.MemoryClient();
        const auto& cfg = mgr.Config();
        uint64_t guid = bot->GetGUID().GetRawValue();
        size_t budget = cfg.MemorySidecar_HintMaxChars;

        auto append_hints = [&](const std::string& entity) {
            if (entity.empty()) return;
            auto hints = mem.RecallAbout(
                guid, entity, /*max_hops*/ 2, cfg.MemorySidecar_RecallTopK);
            for (const auto& h : hints) {
                if (budget == 0) return;
                std::string clipped = h.size() <= budget ? h : h.substr(0, budget);
                s.memory_hints.push_back(clipped);
                budget -= clipped.size();
            }
        };

        append_hints(s.location.zone);
        for (size_t i = 0; i < s.social.nearby_humans.size() && i < 3 && budget > 0; ++i)
            append_hints(s.social.nearby_humans[i].name);
        if (s.goal.current == "DoQuest" && !s.goal.params_json.empty() && budget > 0) {
            try {
                auto p = nlohmann::json::parse(s.goal.params_json);
                if (p.contains("title")) append_hints(p["title"].get<std::string>());
            } catch (...) {}
        }

        // Persona: lazy stub on first access.
        auto persona = mem.GetPersonality(guid);
        if (!persona.has_value()) {
            std::string stub = LlmAgentPersonality::StubPersonaText(s);
            mem.SetPersonality(guid, stub);
        }
    }

    return s;
}

nlohmann::json BuildDigest(PlayerbotAI* botAI) {
    return BuildDigestJson(SnapshotBot(botAI));
}

#endif  // LLMAGENT_UNIT_TESTS
