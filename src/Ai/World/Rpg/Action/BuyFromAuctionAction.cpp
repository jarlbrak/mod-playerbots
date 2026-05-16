/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BuyFromAuctionAction.h"

#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"

// AH-spot table — shared shape with ListAtAuctionAction. Coords sit right next to the
// auctioneer trio in each city so the bot is in range immediately on arrival.
namespace {
struct AhSpot { uint32 mapId; float x, y, z, o; };
AhSpot const kAhSpotHorde    = { 1,  1683.0f, -4461.0f,  20.4f, 4.92f };
AhSpot const kAhSpotAlliance = { 0, -8819.0f,   661.0f,  97.5f, 0.93f };
uint32 const kAuctioneerEntriesAlliance[] = { 8670, 8719, 15659 };
uint32 const kAuctioneerEntriesHorde[]    = { 8673, 8724, 9856  };
} // namespace

bool BuyFromAuctionAction::Execute(Event /*event*/)
{
    if (!sPlayerbotAIConfig.ahBuyingEnabled)
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
            LOG_INFO("playerbots", "ah/p4b: teleport bot {} -> map{} spot result={}",
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

    uint32 bought = BuyAtAuctioneer(auctioneer);
    if (bought > 0)
        LOG_INFO("playerbots", "ah/p4b: bot {} bought {} items from AH", bot->GetName(), bought);
    return bought > 0;
}

Creature* BuyFromAuctionAction::FindNearestAuctioneer()
{
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

uint32 BuyFromAuctionAction::BuyAtAuctioneer(Creature* auctioneer)
{
    AuctionHouseEntry const* ahEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(auctioneer->GetFaction());
    if (!ahEntry)
        return 0;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMap(auctioneer->GetFaction());
    if (!auctionHouse)
        return 0;

    ObjectGuid const botGuid = bot->GetGUID();
    float const factor    = sPlayerbotAIConfig.ahBuyerWillingnessFactor;
    uint32 const maxBuys  = sPlayerbotAIConfig.ahBuyerMaxBuysPerCycle;
    uint32 const minGold  = sPlayerbotAIConfig.ahBuyerMinGold;
    uint32 bought = 0;

    // Snapshot the candidate auction IDs first — RemoveAuction during iteration would
    // invalidate map iterators. We never buy our own listings.
    std::vector<uint32> candidateIds;
    {
        AuctionHouseObject::AuctionEntryMap const& all = auctionHouse->GetAuctions();
        candidateIds.reserve(all.size());
        for (auto const& kv : all)
        {
            AuctionEntry const* ae = kv.second;
            if (!ae || ae->owner == botGuid || !ae->buyout)
                continue;
            candidateIds.push_back(ae->Id);
        }
    }

    for (uint32 id : candidateIds)
    {
        if (bought >= maxBuys)
            break;

        AuctionEntry* auction = auctionHouse->GetAuction(id);
        if (!auction)
            continue; // got bought out by another bot mid-loop

        ItemTemplate const* tpl = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!tpl || tpl->SellPrice == 0)
            continue;

        // Only buy items the bot has a real use for — drives need-driven trade,
        // not arbitrage-driven churn. The "want" set covers gear upgrades,
        // quest items, profession materials, consumables, and ammo. We
        // explicitly skip AH/VENDOR/DISENCHANT/KEEP/NONE etc. so bots don't
        // buy just to immediately relist or vendor.
        ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", auction->item_template);
        bool wanted = (usage == ITEM_USAGE_EQUIP
                    || usage == ITEM_USAGE_REPLACE
                    || usage == ITEM_USAGE_QUEST
                    || usage == ITEM_USAGE_SKILL
                    || usage == ITEM_USAGE_USE
                    || usage == ITEM_USAGE_AMMO);
        if (!wanted)
            continue;

        uint32 const willingness = (uint32)(tpl->SellPrice * auction->itemCount * factor);
        if (auction->buyout > willingness)
            continue;

        // Leave a floor of running-around money in the bot's pocket.
        if (bot->GetMoney() < uint64(auction->buyout) + uint64(minGold))
            continue;

        // Buyout — mirrors HandleAuctionPlaceBid (AuctionHouseHandler.cpp:510-536).
        bot->ModifyMoney(-int32(auction->buyout));
        auction->bidder = botGuid;
        auction->bid    = auction->buyout;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        sAuctionMgr->SendAuctionSalePendingMail(auction, trans);
        sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
        sAuctionMgr->SendAuctionWonMail(auction, trans);
        sScriptMgr->OnAuctionSuccessful(auctionHouse, auction);
        auction->DeleteFromDB(trans);
        bot->SaveInventoryAndGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);

        sAuctionMgr->RemoveAItem(auction->item_guid);
        auctionHouse->RemoveAuction(auction);

        ++bought;
    }

    return bought;
}
