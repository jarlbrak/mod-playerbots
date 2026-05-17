/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_AHSPOTTABLE_H
#define _PLAYERBOT_AHSPOTTABLE_H

#include "Define.h"

#include <cstddef>

class Player;

// One auction-house "stand here" coord, near a cluster of auctioneer NPCs.
// label is for human-readable logging only (lifetime: static storage).
struct AhSpot
{
    uint32      mapId;
    float       x;
    float       y;
    float       z;
    float       o;
    char const* label;
};

// Per-faction spot pools. Each pool ends with the two neutral hubs so
// neutral spots are always considered alongside faction-city spots.
extern AhSpot const  kAhSpotsAlliance[];
extern std::size_t const kAhSpotsAllianceCount;

extern AhSpot const  kAhSpotsHorde[];
extern std::size_t const kAhSpotsHordeCount;

// Union of every auctioneer NPC entry across both faction pools.
// FindNearestCreature(entryId, range) walks only the bot's perception
// cell so cross-city entries never collide after teleport.
extern uint32 const  kAuctioneerEntries[];
extern std::size_t const kAuctioneerEntriesCount;

// Choose the best AhSpot for `bot`. Prefers the closest spot on the bot's
// current map (squared 2D distance, x*x + y*y); falls back to pool[0]
// (Stormwind for Alliance, Orgrimmar for Horde) when no same-map spot
// exists in the pool.
AhSpot const& PickNearestAhSpot(Player* bot);

// Returns true if the bot's hearthstone bind is on the same map as `spot`
// and within 500 yards (squared 2D distance) of the spot's coord. Used to
// short-circuit MoveFarTo with a hearth cast when the bind is right at
// (or near) the chosen AH destination.
bool HearthBindMatchesSpot(Player const* bot, AhSpot const& spot);

#endif
