/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ListAtAuctionAction.h"
#include "AhSpotTable.h"
#include "AhVisit.h"

#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

bool ListAtAuctionAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.ahListingEnabled)
        return false;
    if (bot->InBattleground() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
        return false;
    if (!bot->IsAlive() || bot->IsInCombat() || bot->GetTransport())
        return false;

    Creature* auctioneer = FindNearestAuctioneer();
    if (!auctioneer)
    {
        // No auctioneer in range. Queue an AH errand for the
        // NewRpgGoAhVisitAction to pick up — but only if the cooldown is
        // clear. The action will travel via MoveFarTo, no teleport.
        if (!AhCooldownActive(bot))
            botAI->ahErrandPending = true;
        return true;
    }

    if (bot->GetDistance(auctioneer) > 5.0f)
    {
        bot->GetMotionMaster()->MovePoint(0,
            auctioneer->GetPositionX(),
            auctioneer->GetPositionY(),
            auctioneer->GetPositionZ());
        return true;
    }

    // Opportunistic path — bot is already at an AH for unrelated reasons.
    // PerformVisitAtAuctioneer sets cooldown + clears flag.
    return PerformVisitAtAuctioneer(bot, auctioneer);
}

Creature* ListAtAuctionAction::FindNearestAuctioneer()
{
    // Direct grid lookup via bot->FindNearestCreature avoids the "nearest npcs"
    // AI_VALUE cache, which goes stale right after a teleport.
    uint32 const* entries = kAuctioneerEntries;
    size_t const  count   = kAuctioneerEntriesCount;

    Creature* best = nullptr;
    float bestDist = 100.0f;
    for (size_t i = 0; i < count; ++i)
    {
        Creature* c = bot->FindNearestCreature(entries[i], 100.0f, true);
        if (!c)
            continue;
        float d = bot->GetDistance(c);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}
