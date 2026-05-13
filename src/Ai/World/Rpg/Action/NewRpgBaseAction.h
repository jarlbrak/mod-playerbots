#ifndef _PLAYERBOT_NEWRPGBASEACTION_H
#define _PLAYERBOT_NEWRPGBASEACTION_H

#include "Duration.h"
#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
};

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    bool MoveFarTo(WorldPosition dest);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    /* QUEST RELATED CHECK */
    ObjectGuid ChooseNpcOrGameObjectToInteract(bool questgiverOnly = false, float distanceLimit = 0.0f);
    bool HasQuestToAcceptOrReward(WorldObject* object);
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool CanInteractWithQuestGiver(Object* questGiver);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);
    bool IsQuestWorthDoing(Quest const* quest);
    bool IsQuestCapableDoing(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();

protected:
    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false);
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    bool SelectRandomFlightTaxiNode(ObjectGuid& flightMaster, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    bool CheckRpgStatusAvailable(NewRpgStatus status);
    /**
     * F4 P2: returns true if the bot has any in-progress quest with an
     * objective in the current zone, OR any accessible/eligible quest
     * giver in the current zone. Used by DO_QUEST persistence to keep
     * a bot in DO_QUEST until the area is drained.
     */
    bool LocalQuestsRemaining();

    // F4 P3: pick a material this bot should farm. Returns 0 if no suitable
    // target found (caller falls back to legacy random grind).
    uint32 SelectFarmTargetItem();

    // F4 P3: populate sourceCreatureEntries + sourceGameobjectEntries for
    // a given item id by querying creature_loot_template and
    // gameobject_loot_template. Cached on first lookup per (botId, itemId).
    void GetFarmSourcesForItem(uint32 itemId,
                               std::vector<uint32>& outCreatureEntries,
                               std::vector<uint32>& outGoEntries);

    // F4 P3: find the nearest spawn (creature or gameobject) of any entry
    // in the candidate lists, within ~2 zones of the bot. Returns invalid
    // WorldPosition if none found.
    WorldPosition SelectFarmSpawnPos(const std::vector<uint32>& creatureEntries,
                                     const std::vector<uint32>& goEntries);

protected:
    /* FOR MOVE FAR */
    const float pathFinderDis = 70.0f;
    // Time without real progress toward dest before MoveFarTo
    // falls back to teleport recovery. Kept short enough that a
    // bot truly oscillating around an unreachable destination
    // (mmap returning non-progressing partial paths, or NOPATH +
    // cone fallback wandering) doesn't spin for 5 minutes before
    // the teleport fires, but long enough that a genuine long
    // walk that is slowly making progress never triggers it.
    const uint32 stuckTime = 90 * 1000;
};

#endif
