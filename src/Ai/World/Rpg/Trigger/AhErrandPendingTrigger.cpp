/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AhErrandPendingTrigger.h"

#include "AhVisit.h"
#include "NewRpgInfo.h"
#include "PlayerbotAI.h"

bool AhErrandPendingTrigger::IsActive()
{
    // Stay active while the bot has an outstanding errand OR is mid-travel
    // (so the action keeps ticking MoveFarTo until arrival).
    if (botAI->rpgInfo.GetStatus() == RPG_GO_AH_VISIT)
        return true;
    if (!botAI->ahErrandPending)
        return false;
    if (AhCooldownActive(bot))
        return false;
    if (!bot->IsAlive() || bot->IsInCombat() || bot->GetTransport())
        return false;
    if (bot->InBattleground() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
        return false;
    return true;
}
