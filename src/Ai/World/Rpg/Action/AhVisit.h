/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_AHVISIT_H
#define _PLAYERBOT_AHVISIT_H

#include "Define.h"

class Player;
class Creature;

// True if the bot has completed an AH visit within sPlayerbotAIConfig.ahCooldownMs.
bool AhCooldownActive(Player* bot);

// Direct-grid auctioneer lookup using the entries in AhSpotTable's
// kAuctioneerEntries union. Returns the nearest live auctioneer within 100yd
// of the bot, or nullptr.
Creature* FindNearestAuctioneerNearBot(Player* bot);

// Pre: bot is within ~5yd of `auctioneer`.
//
// Lists eligible items (today's ListItemsAt body), then attempts to buy
// wanted items (today's BuyAtAuctioneer body). ALWAYS sets
// bot->lastAhVisitMs to now and clears bot->ahErrandPending — even if no
// list or buy succeeded. The visit attempt is what consumes the cooldown,
// not the transactional outcome. Without this, a bot whose items are all
// filtered out would re-route to the same AH every status cycle.
//
// Returns true if any list or buy happened (for log purposes only — the
// caller does not depend on this for correctness).
bool PerformVisitAtAuctioneer(Player* bot, Creature* auctioneer);

#endif
