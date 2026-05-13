/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ListAtAuctionAction.h"

#include "AuctionHouseMgr.h"
#include "ItemUsageValue.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "Playerbots.h"

bool ListAtAuctionAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.ahListingEnabled)
        return false;
    if (bot->InBattleground() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
        return false;

    Creature* auctioneer = FindNearestAuctioneer();
    if (!auctioneer)
        return false;

    if (bot->GetDistance(auctioneer) > 5.0f)
    {
        bot->GetMotionMaster()->MovePoint(0,
            auctioneer->GetPositionX(),
            auctioneer->GetPositionY(),
            auctioneer->GetPositionZ());
        return true;
    }

    uint32 listed = ListItemsAt(auctioneer);
    if (listed > 0)
        LOG_INFO("playerbots", "Bot {} listed {} items at AH", bot->GetName(), listed);
    return listed > 0;
}

Creature* ListAtAuctionAction::FindNearestAuctioneer()
{
    GuidVector npcs = AI_VALUE(GuidVector, "nearest npcs");
    Creature* best = nullptr;
    float bestDist = 200.0f;
    for (ObjectGuid const& guid : npcs)
    {
        Creature* c = dynamic_cast<Creature*>(botAI->GetUnit(guid));
        if (!c)
            continue;
        if (!(c->GetCreatureTemplate()->npcflag & UNIT_NPC_FLAG_AUCTIONEER))
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

uint32 ListAtAuctionAction::ListItemsAt(Creature* auctioneer)
{
    AuctionHouseEntry const* ahEntry = AuctionHouseMgr::GetAuctionHouseEntry(auctioneer->getFaction());
    if (!ahEntry)
        return 0;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(ahEntry);
    if (!auctionHouse)
        return 0;

    // Count this bot's currently active listings so we can respect ahListingMaxConcurrent.
    uint32 currentBotListings = 0;
    {
        ObjectGuid::LowType botGuidLow = bot->GetGUID().GetCounter();
        AuctionEntryMap const& existingAuctions = auctionHouse->GetAuctions();
        for (auto const& pair : existingAuctions)
        {
            AuctionEntry const* ae = pair.second;
            if (ae && ae->owner == botGuidLow)
                ++currentBotListings;
        }
    }

    double const mult = sRandomPlayerbotMgr.GetSellMultiplier(bot);
    float const margin = sPlayerbotAIConfig.ahProfitMargin;
    uint32 const maxActive = sPlayerbotAIConfig.ahListingMaxConcurrent;

    uint32 listed = 0;

    auto tryList = [&](Item* item) -> void
    {
        if (currentBotListings + listed >= maxActive)
            return;
        if (!item)
            return;
        ItemTemplate const* tpl = item->GetTemplate();
        if (!tpl || tpl->SellPrice == 0)
            return;
        if (tpl->Flags & ITEM_FLAG_NO_USER_DESTROY)
            return;

        ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", item->GetEntry());
        if (usage != ITEM_USAGE_AH)
            return;

        uint32 vendorTotal = tpl->SellPrice * item->GetCount();
        uint32 estimatedAH = (uint32)(vendorTotal * mult);
        // Only list if this bot's multiplier makes AH profitable vs. vendor.
        if (estimatedAH < (uint32)(margin * vendorTotal))
            return;

        uint32 buyout = estimatedAH;
        uint32 bidStart = (uint32)(buyout * 0.95f);
        if (!buyout || !bidStart)
            return;

        // Auction duration: 12 hours expressed in seconds, scaled by the world rate.
        uint32 auction_time = uint32(12 * HOUR * sWorld->getConfig(CONFIG_FLOAT_RATE_AUCTION_TIME));

        // Create a detached item to represent the listing (mirrors LootAction skeleton).
        Item* auctionItem = Item::CreateItem(tpl->ItemId, item->GetCount());
        if (!auctionItem)
            return;

        AuctionEntry* auctionEntry = new AuctionEntry;
        auctionEntry->Id = sObjectMgr->GenerateAuctionID();
        auctionEntry->itemGuidLow = auctionItem->GetGUID().GetCounter();
        auctionEntry->itemTemplate = auctionItem->GetEntry();
        auctionEntry->itemCount = auctionItem->GetCount();
        auctionEntry->itemRandomPropertyId = auctionItem->GetItemRandomPropertyId();
        auctionEntry->owner = bot->GetGUID().GetCounter();
        auctionEntry->startbid = bidStart;
        auctionEntry->bidder = 0;
        auctionEntry->bid = 0;
        auctionEntry->buyout = buyout;
        auctionEntry->expireTime = time(nullptr) + auction_time;
        auctionEntry->deposit = 0;
        auctionEntry->auctionHouseEntry = ahEntry;

        auctionHouse->AddAuction(auctionEntry);
        sAuctionMgr->AddAItem(auctionItem);
        auctionItem->SaveToDB();
        auctionEntry->SaveToDB();

        // Remove the item from the bot's inventory now that it is listed.
        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

        ++listed;
    };

    for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        tryList(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag))
            for (uint32 slot = 0; slot < pBag->GetBagSize(); ++slot)
                tryList(pBag->GetItemByPos(slot));
    }
    return listed;
}
