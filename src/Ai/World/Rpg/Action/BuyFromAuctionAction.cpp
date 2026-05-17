/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BuyFromAuctionAction.h"
#include "AhSpotTable.h"

#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"

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
        AhSpot const& spot = PickNearestAhSpot(bot);
        bool needRoute = (bot->GetMapId() != spot.mapId ||
                          bot->GetDistance(spot.x, spot.y, spot.z) > 30.0f);
        if (needRoute)
        {
            bool ok = bot->TeleportTo(spot.mapId, spot.x, spot.y, spot.z, spot.o);
            LOG_INFO("playerbots", "ah/p4b: route bot {} -> {} (map{}) result={}",
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

    uint32 bought = BuyAtAuctioneer(auctioneer);
    if (bought > 0)
        LOG_INFO("playerbots", "ah/p4b: bot {} bought {} items from AH", bot->GetName(), bought);
    return bought > 0;
}

Creature* BuyFromAuctionAction::FindNearestAuctioneer()
{
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

uint32 BuyFromAuctionAction::BuyAtAuctioneer(Creature* auctioneer)
{
    // Unified single AH — read from the same Neutral house regardless of which
    // auctioneer (Alliance, Horde, Neutral) the bot is standing at.
    AuctionHouseEntry const* ahEntry =
        AuctionHouseMgr::GetAuctionHouseEntryFromHouse(AuctionHouseId::Neutral);
    if (!ahEntry)
        return 0;

    AuctionHouseObject* auctionHouse = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Neutral);
    if (!auctionHouse)
        return 0;

    ObjectGuid const botGuid = bot->GetGUID();
    float const markup    = sPlayerbotAIConfig.ahFlatMarkup;
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

        // Sanity ceiling — in the mutual-aid AH every listing is priced at
        // exactly markup * vendor by the seller, so this gate normally never
        // trips. Acts as a safety net against any legacy or third-party
        // listings priced above the flat markup.
        uint32 const ceiling = (uint32)(tpl->SellPrice * auction->itemCount * markup);
        if (auction->buyout > ceiling)
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
