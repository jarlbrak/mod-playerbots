/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ListAtAuctionAction.h"
#include "AhSpotTable.h"

#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "World.h"

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
        AhSpot const& spot = PickNearestAhSpot(bot);
        bool needRoute = (bot->GetMapId() != spot.mapId ||
                          bot->GetDistance(spot.x, spot.y, spot.z) > 30.0f);
        if (needRoute)
        {
            bool ok = bot->TeleportTo(spot.mapId, spot.x, spot.y, spot.z, spot.o);
            LOG_INFO("playerbots", "ah/p4: route bot {} -> {} (map{}) result={}",
                     bot->GetName(), spot.label, spot.mapId, ok);
        }
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

    uint32 listed = ListItemsAt(auctioneer);
    if (listed > 0)
        LOG_INFO("playerbots", "ah/p4: bot {} listed {} items at AH", bot->GetName(), listed);
    return listed > 0;
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

uint32 ListAtAuctionAction::ListItemsAt(Creature* auctioneer)
{
    // Unified single AH — Alliance, Horde, and Neutral auctioneers all post into
    // and read from the same Neutral house. No faction split, no neutral surcharge.
    AuctionHouseEntry const* ahEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId::Neutral);
    if (!ahEntry)
        return 0;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Neutral);
    if (!auctionHouse)
        return 0;

    ObjectGuid const botGuid = bot->GetGUID();

    uint32 currentBotListings = 0;
    {
        AuctionHouseObject::AuctionEntryMap const& existingAuctions = auctionHouse->GetAuctions();
        for (auto const& pair : existingAuctions)
        {
            AuctionEntry const* ae = pair.second;
            if (ae && ae->owner == botGuid)
                ++currentBotListings;
        }
    }

    // Mutual-aid AH: every listing priced at the same flat markup over the
    // vendor sell price. No per-bot persona, no merchant role-play, no
    // gaming the market. Bots farm + profit from their labor; the AH is
    // regulated against runaway capitalism.
    float const markup = sPlayerbotAIConfig.ahFlatMarkup;
    uint32 const maxActive = sPlayerbotAIConfig.ahListingMaxConcurrent;
    uint32 const auctionTime =
        uint32(12 * HOUR * sWorld->getRate(RATE_AUCTION_TIME));

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

        uint32 const vendorTotal = tpl->SellPrice * item->GetCount();
        uint32 const buyout = (uint32)(vendorTotal * markup);
        uint32 const bidStart = (uint32)(buyout * 0.95f);
        if (!buyout || !bidStart)
            return;

        // Bots pay the deposit like a real player — keeps spamming honest.
        uint32 const deposit =
            sAuctionMgr->GetAuctionDeposit(ahEntry, 12 * HOUR, item, item->GetCount());
        if (!bot->HasEnoughMoney(deposit))
            return;

        bot->ModifyMoney(-int32(deposit));

        AuctionEntry* AH = new AuctionEntry;
        AH->Id = sObjectMgr->GenerateAuctionID();
        AH->houseId = AuctionHouseId::Neutral;
        AH->item_guid = item->GetGUID();
        AH->item_template = item->GetEntry();
        AH->itemCount = item->GetCount();
        AH->owner = botGuid;
        AH->startbid = bidStart;
        AH->bidder = ObjectGuid::Empty;
        AH->bid = 0;
        AH->buyout = buyout;
        AH->expire_time = GameTime::GetGameTime().count() + auctionTime;
        AH->deposit = deposit;
        AH->auctionHouseEntry = ahEntry;

        sAuctionMgr->AddAItem(item);
        auctionHouse->AddAuction(AH);

        bot->MoveItemFromInventory(item->GetBagSlot(), item->GetSlot(), true);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        item->DeleteFromInventoryDB(trans);
        item->SaveToDB(trans);
        AH->SaveToDB(trans);
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

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
