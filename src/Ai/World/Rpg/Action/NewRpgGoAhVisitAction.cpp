/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "NewRpgGoAhVisitAction.h"

#include "AhSpotTable.h"
#include "AhVisit.h"
#include "NewRpgInfo.h"
#include "Player.h"
#include "PlayerbotAI.h"

bool NewRpgGoAhVisitAction::Execute(Event /*event*/)
{
    // Refuse if the bot is in a state where travel is unsafe.
    if (!bot->IsAlive() || bot->IsInCombat() || bot->GetTransport())
        return false;
    if (bot->InBattleground() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
        return false;

    NewRpgInfo& info = botAI->rpgInfo;

    // First invocation (or interrupted from another status): pick the
    // destination and transition into RPG_GO_AH_VISIT.
    if (info.GetStatus() != RPG_GO_AH_VISIT)
    {
        AhSpot const& spot = PickNearestAhSpot(bot);
        WorldPosition pos(spot.mapId, spot.x, spot.y, spot.z, spot.o);
        info.ChangeToGoAhVisit(pos, spot.label);
        LOG_INFO("playerbots", "ah/visit: bot {} starting trip to {} (map{})",
                 bot->GetName(), spot.label, spot.mapId);
        // Fall through to the travel branch below.
    }

    auto* data = std::get_if<NewRpgInfo::GoAhVisit>(&info.data);
    if (!data)
    {
        // Defensive — should be unreachable after the transition above.
        info.ChangeToIdle();
        return false;
    }

    // Travel — use the existing MoveFarTo primitive (pathfinding + flight
    // masters). Returns true while still in motion.
    if (MoveFarTo(data->pos))
        return true;

    // MoveFarTo returned false. Either we've arrived (close enough) or we're
    // stuck. Distinguish by distance to the AhSpot center.
    float const arrivalRadius = 30.0f;
    bool arrived = (bot->GetMapId() == data->pos.GetMapId()
                 && bot->GetDistance(data->pos.GetPositionX(),
                                     data->pos.GetPositionY(),
                                     data->pos.GetPositionZ()) <= arrivalRadius);

    if (!arrived)
    {
        // Try a small nudge to unstick; if persistent stuck, bail to IDLE.
        if (botAI->rpgInfo.stuckAttempts >= 5)
        {
            // Honor the 2h cooldown on the bail path too — otherwise a bot
            // stuck on the way to one AH would re-pick a destination on the
            // very next status cycle and burn ticks looping.
            botAI->lastAhVisitMs = getMSTime();
            botAI->ahErrandPending = false;
            LOG_INFO("playerbots", "ah/visit: bot {} stuck en route to {} — bailing to IDLE + 2h cooldown",
                     bot->GetName(), data->cityLabel ? data->cityLabel : "<null>");
            info.ChangeToIdle();
            return true;
        }
        return MoveRandomNear(10.0f);
    }

    // Arrived. Try to find the auctioneer cluster and execute the visit.
    Creature* auctioneer = FindNearestAuctioneerNearBot(bot);
    if (auctioneer)
    {
        // Move to within 5yd if not already.
        if (bot->GetDistance(auctioneer) > 5.0f)
        {
            bot->GetMotionMaster()->MovePoint(0,
                auctioneer->GetPositionX(),
                auctioneer->GetPositionY(),
                auctioneer->GetPositionZ());
            return true;
        }
        PerformVisitAtAuctioneer(bot, auctioneer);
        // PerformVisitAtAuctioneer sets lastAhVisitMs + clears ahErrandPending
        // unconditionally. Now drop back to IDLE so the bot resumes other
        // behaviors.
    }
    else
    {
        // No auctioneer in range at this AhSpot — should never happen given the
        // table data, but if it does, set the cooldown anyway so we don't loop.
        botAI->lastAhVisitMs = getMSTime();
        botAI->ahErrandPending = false;
        LOG_INFO("playerbots", "ah/visit: bot {} arrived at {} but found NO auctioneer (coord/data bug?)",
                 bot->GetName(), data->cityLabel ? data->cityLabel : "<null>");
    }

    info.ChangeToIdle();
    return true;
}
