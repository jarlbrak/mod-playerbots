#include "Apply/ApplyGoal.h"

#ifndef LLMAGENT_UNIT_TESTS

#include "PlayerbotAI.h"
#include "ObjectMgr.h"
#include "NewRpgInfo.h"
#include "Log.h"

namespace LlmAgentApply {

bool ApplyGoalToRpgInfo(const ParsedGoal& g, PlayerbotAI* botAI) {
    if (!botAI) return false;
    Player* bot = botAI->GetBot();
    if (!bot) return false;
    NewRpgInfo& info = botAI->rpgInfo;

    try {
        switch (g.goal) {
            case GoalKind::Idle:
                info.ChangeToIdle();
                return true;

            case GoalKind::Rest:
                info.ChangeToRest();
                return true;

            case GoalKind::WanderRandom:
                info.ChangeToWanderRandom();
                return true;

            case GoalKind::WanderNpc:
                info.ChangeToWanderNpc();
                return true;

            case GoalKind::DoQuest: {
                const auto& p = std::get<DoQuestParams>(g.params);
                const Quest* q = sObjectMgr->GetQuestTemplate(p.quest_id);
                if (!q) {
                    info.ChangeToIdle();
                    return false;
                }
                info.ChangeToDoQuest(p.quest_id, q);
                return true;
            }

            case GoalKind::GoGrind: {
                const auto& p = std::get<GoGrindParams>(g.params);
                WorldPosition wp(p.map_id, static_cast<float>(p.x),
                                 static_cast<float>(p.y),
                                 static_cast<float>(p.z), 0.0f);
                info.ChangeToGoGrind(wp);
                return true;
            }

            case GoalKind::GoCamp: {
                const auto& p = std::get<GoCampParams>(g.params);
                WorldPosition wp(p.map_id, static_cast<float>(p.x),
                                 static_cast<float>(p.y),
                                 static_cast<float>(p.z), 0.0f);
                info.ChangeToGoCamp(wp);
                return true;
            }

            case GoalKind::TravelFlight: {
                const auto& p = std::get<TravelFlightParams>(g.params);
                std::vector<uint32> path = {p.destination_node_id};
                info.ChangeToTravelFlight(ObjectGuid(uint64(p.from_flightmaster_guid)), path);
                return true;
            }

            case GoalKind::OutdoorPvp: {
                const auto& p = std::get<OutdoorPvpParams>(g.params);
                info.ChangeToOutdoorPvp(p.capture_point_spawn_id);
                return true;
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyGoal threw: {} (recovering to Idle)", e.what());
        info.ChangeToIdle();
        return false;
    } catch (...) {
        LOG_ERROR("playerbots", "[LlmAgent] ApplyGoal threw unknown exception (recovering to Idle)");
        info.ChangeToIdle();
        return false;
    }
    return false;
}

}  // namespace LlmAgentApply

#endif
