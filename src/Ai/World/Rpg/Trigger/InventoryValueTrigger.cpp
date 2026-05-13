/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "InventoryValueTrigger.h"

#include "ItemUsageValue.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "Playerbots.h"

bool InventoryValueTrigger::IsActive()
{
    if (!sPlayerbotAIConfig.ahListingEnabled)
        return false;
    if (bot->InBattleground() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
        return false;

    // (a) bag pressure: trigger when free inventory slots drop to or below the threshold.
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
    if (freeSlots <= sPlayerbotAIConfig.ahBagPressureFreeSlots)
        return true;

    // (b) any AH-eligible item whose estimated AH price meets the high-value threshold.
    // GetSellMultiplier may hit DB on first call per bot (acceptable from a trigger check).
    double const mult = sRandomPlayerbotMgr.GetSellMultiplier(bot);
    uint32 const threshold = sPlayerbotAIConfig.ahHighValueItemThreshold;

    auto checkItem = [&](Item* item) -> bool
    {
        if (!item)
            return false;
        ItemTemplate const* tpl = item->GetTemplate();
        if (!tpl || tpl->SellPrice == 0)
            return false;
        ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
        if (usage != ITEM_USAGE_AH)
            return false;
        uint32 est = (uint32)(item->GetCount() * tpl->SellPrice * mult);
        return est >= threshold;
    };

    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        if (checkItem(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i)))
            return true;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
        {
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                if (checkItem(pBag->GetItemByPos(slot)))
                    return true;
        }
    }
    return false;
}
