/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ListAtAuctionAction.h"

#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "World.h"

// Faction-default auction-house spot. Coords sit right next to the auctioneer trio in each city.
// Bots that fire InventoryValueTrigger outside an existing auctioneer's range get teleported here.
struct AhSpot { uint32 mapId; float x, y, z, o; };
static AhSpot const kAhSpotHorde     = { 1,  1683.0f, -4461.0f,  20.4f, 4.92f }; // Org Drag, between Wabang/Thathung/Grimful
static AhSpot const kAhSpotAlliance  = { 0, -8819.0f,   661.0f,  97.5f, 0.93f }; // SW Trade District, between Chilton/Fitch/Jaxon

// Known auctioneer creature entries — used by FindNearestAuctioneer for a direct grid lookup
// instead of the (post-teleport) stale "nearest npcs" AI_VALUE cache.
static uint32 const kAuctioneerEntriesAlliance[] = { 8670, 8719, 15659 };
static uint32 const kAuctioneerEntriesHorde[]    = { 8673, 8724, 9856  };

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
        AhSpot const& spot = (bot->GetTeamId() == TEAM_ALLIANCE) ? kAhSpotAlliance : kAhSpotHorde;
        bool needRoute = (bot->GetMapId() != spot.mapId ||
                          bot->GetDistance(spot.x, spot.y, spot.z) > 30.0f);
        if (needRoute)
        {
            bool ok = bot->TeleportTo(spot.mapId, spot.x, spot.y, spot.z, spot.o);
            LOG_INFO("playerbots", "ah/p4: teleport bot {} -> map{} spot result={}",
                     bot->GetName(), spot.mapId, ok);
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
    bool alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    uint32 const* entries = alliance ? kAuctioneerEntriesAlliance : kAuctioneerEntriesHorde;
    size_t const count = alliance
        ? sizeof(kAuctioneerEntriesAlliance) / sizeof(uint32)
        : sizeof(kAuctioneerEntriesHorde)    / sizeof(uint32);

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
    AuctionHouseEntry const* ahEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    if (!ahEntry)
        return 0;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
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

    double const mult = sRandomPlayerbotMgr.GetSellMultiplier(bot);
    float const margin = sPlayerbotAIConfig.ahProfitMargin;
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
        uint32 const estimatedAH = (uint32)(vendorTotal * mult);
        // List any item where this bot's per-bot price exceeds vendor.
        // AhProfitMargin (default 1.0) lets operators raise the bar if desired.
        if (estimatedAH <= (uint32)(margin * vendorTotal))
            return;

        uint32 const buyout = estimatedAH;
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
        AH->houseId = AuctionHouseId(ahEntry->houseId);
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
