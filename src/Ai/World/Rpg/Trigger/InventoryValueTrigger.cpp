/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "InventoryValueTrigger.h"

#include "ItemUsageValue.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"

bool InventoryValueTrigger::IsActive()
{
    static std::atomic<uint32> sCallCount{0};
    uint32 const seq = ++sCallCount;
    if (seq <= 5 || seq % 500 == 0)
        LOG_INFO("playerbots", "ah/p4-trig: IsActive #{} bot={}", seq, bot->GetName());

    if (!sPlayerbotAIConfig.ahListingEnabled)
        return false;
    if (bot->InBattleground() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
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
    if (freeSlots <= sPlayerbotAIConfig.ahBagPressureFreeSlots)
        return true;

    // Mutual-aid AH: any AH-eligible item with positive sell price is worth
    // listing — flat markup means every listing is fair-priced by definition.
    // No per-bot persona, no merchant role-play, no gaming the market.
    auto checkItem = [&](Item* item) -> bool
    {
        if (!item)
            return false;
        ItemTemplate const* tpl = item->GetTemplate();
        if (!tpl || tpl->SellPrice == 0)
            return false;
        ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
        return usage == ITEM_USAGE_AH;
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
