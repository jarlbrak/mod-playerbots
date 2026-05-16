/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AhBuyerTrigger.h"

#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

bool AhBuyerTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.ahBuyingEnabled)
        return false;
    if (bot->InBattleground() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
        return false;
    if (!bot->IsAlive() || bot->IsInCombat() || bot->GetTransport())
        return false;

    if (bot->GetMoney() < sPlayerbotAIConfig.ahBuyerMinGold)
        return false;

    uint32 freeSlots = 0;
    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            freeSlots++;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
        {
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (!pBag->GetItemByPos(slot))
                    freeSlots++;
        }
    }
    return freeSlots >= sPlayerbotAIConfig.ahBuyerMinFreeBagSlots;
}
