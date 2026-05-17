/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AhVisit.h"
#include "AhSpotTable.h"

#include "AuctionHouseMgr.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "World.h"

bool AhCooldownActive(Player* bot)
{
    if (!bot)
        return false;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;
    if (botAI->lastAhVisitMs == 0)
        return false;
    return GetMSTimeDiffToNow(botAI->lastAhVisitMs) < sPlayerbotAIConfig.ahCooldownMs;
}

Creature* FindNearestAuctioneerNearBot(Player* bot)
{
    if (!bot)
        return nullptr;
    Creature* best = nullptr;
    float bestDist = 100.0f;
    for (size_t i = 0; i < kAuctioneerEntriesCount; ++i)
    {
        Creature* c = bot->FindNearestCreature(kAuctioneerEntries[i], 100.0f, true);
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

// Internal helpers — defined below PerformVisitAtAuctioneer.
// auctioneer parameter is accepted for API symmetry with PerformVisitAtAuctioneer
// but unused: both bodies talk directly to the Neutral auction house via sAuctionMgr.
static uint32 PerformListingAt(Player* bot, Creature* /*auctioneer*/);
static uint32 PerformBuyingAt(Player* bot, Creature* /*auctioneer*/);

bool PerformVisitAtAuctioneer(Player* bot, Creature* auctioneer)
{
    if (!bot || !auctioneer)
        return false;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return false;

    uint32 listed = 0;
    uint32 bought = 0;
    if (sPlayerbotAIConfig.ahListingEnabled)
        listed = PerformListingAt(bot, auctioneer);
    if (sPlayerbotAIConfig.ahBuyingEnabled)
        bought = PerformBuyingAt(bot, auctioneer);

    // Always set cooldown + clear flag, regardless of transactional outcome.
    botAI->lastAhVisitMs = getMSTime();
    botAI->ahErrandPending = false;

    if (listed > 0 || bought > 0)
    {
        LOG_INFO("playerbots", "ah/visit: bot {} completed (listed {}, bought {})",
                 bot->GetName(), listed, bought);
        return true;
    }
    return false;
}

static uint32 PerformListingAt(Player* bot, Creature* /*auctioneer*/)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return 0;
    AiObjectContext* context = botAI->GetAiObjectContext();  // required by AI_VALUE2 macro

    // === BODY COPIED VERBATIM FROM ListAtAuctionAction::ListItemsAt ===
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

static uint32 PerformBuyingAt(Player* bot, Creature* /*auctioneer*/)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return 0;
    AiObjectContext* context = botAI->GetAiObjectContext();  // required by AI_VALUE2 macro

    // === BODY COPIED VERBATIM FROM BuyFromAuctionAction::BuyAtAuctioneer ===
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
