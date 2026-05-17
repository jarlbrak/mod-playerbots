/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "AhSpotTable.h"

#include "Player.h"

#include <limits>

// Alliance pool: SW, IF, Darnassus, Exodar + neutral Booty Bay, Gadgetzan.
// Coords picked at the center of each city's auctioneer cluster; orientation
// is borrowed from a representative auctioneer so the bot faces inward on
// arrival.
AhSpot const kAhSpotsAlliance[] = {
    { 0,    -8819.0f,    661.0f,    97.5f,  0.93f,  "Stormwind"  },
    { 0,    -4955.0f,   -908.0f,   505.2f,  3.80f,  "Ironforge"  },
    { 1,     9863.0f,   2341.0f,  1321.7f,  3.52f,  "Darnassus"  },
    { 530,  -4025.0f, -11735.0f,  -151.8f,  0.50f,  "Exodar"     },
    { 0,   -14420.0f,    460.0f,     6.0f,  1.51f,  "Booty Bay"  },
    { 1,    -7239.0f,  -3803.0f,     0.8f,  0.02f,  "Gadgetzan"  },
};
std::size_t const kAhSpotsAllianceCount = sizeof(kAhSpotsAlliance) / sizeof(AhSpot);

// Horde pool: Org, UC, TB, Silvermoon + neutral Booty Bay, Gadgetzan.
AhSpot const kAhSpotsHorde[] = {
    { 1,     1683.0f,  -4461.0f,    20.4f,  4.92f,  "Orgrimmar"     },
    { 0,     1605.0f,    240.0f,   -56.8f,  2.63f,  "Undercity"     },
    { 1,    -1203.0f,    102.0f,   134.7f,  3.05f,  "Thunder Bluff" },
    { 530,   9655.0f,  -7130.0f,    17.0f,  2.00f,  "Silvermoon"    },
    { 0,   -14420.0f,    460.0f,     6.0f,  1.51f,  "Booty Bay"     },
    { 1,    -7239.0f,  -3803.0f,     0.8f,  0.02f,  "Gadgetzan"     },
};
std::size_t const kAhSpotsHordeCount = sizeof(kAhSpotsHorde) / sizeof(AhSpot);

// Union of every auctioneer entry seen in any of the spots above.
// FindNearestCreature filters by entry within ~100 yd of the bot, so
// entries that don't match the post-teleport city are inert (never in
// range of the bot's current cell).
uint32 const kAuctioneerEntries[] = {
    // Stormwind Trade District
    8670, 8719, 15659,
    // Ironforge Commons
    8671, 8720, 9859,
    // Darnassus Tradesmen's Terrace
    8669, 8723, 15678, 15679,
    // Exodar Trader's Tier
    16707, 18348, 18349,
    // Orgrimmar Drag
    8673, 8724, 9856,
    // Undercity Trade Quarter (8)
    8672, 8721, 15675, 15676, 15682, 15683, 15684, 15686,
    // Thunder Bluff lower rise
    8674, 8722,
    // Silvermoon Bazaar + Court of the Sun
    17627, 17628, 17629, 18761, 16627, 16628, 16629,
    // Booty Bay docks
    9858, 15677, 15681,
    // Gadgetzan central tents
    8661,
};
std::size_t const kAuctioneerEntriesCount = sizeof(kAuctioneerEntries) / sizeof(uint32);

AhSpot const& PickNearestAhSpot(Player* bot)
{
    bool const alliance = bot->GetTeamId() == TEAM_ALLIANCE;
    AhSpot const* pool   = alliance ? kAhSpotsAlliance      : kAhSpotsHorde;
    std::size_t const count = alliance ? kAhSpotsAllianceCount : kAhSpotsHordeCount;

    uint32 const botMap = bot->GetMapId();
    float  const bx     = bot->GetPositionX();
    float  const by     = bot->GetPositionY();

    AhSpot const* bestSameMap = nullptr;
    float bestDistSq = std::numeric_limits<float>::max();

    for (std::size_t i = 0; i < count; ++i)
    {
        AhSpot const& s = pool[i];
        if (s.mapId != botMap)
            continue;
        float const dx = s.x - bx;
        float const dy = s.y - by;
        float const d2 = dx * dx + dy * dy;
        if (d2 < bestDistSq)
        {
            bestDistSq  = d2;
            bestSameMap = &s;
        }
    }

    if (bestSameMap)
        return *bestSameMap;

    // Fallback: pool[0] (SW for Alliance, Org for Horde). Under Bracket 1
    // every reachable map (0, 1, 530) has a same-map spot in both pools,
    // so this path is effectively dead code today.
    return pool[0];
}

bool HearthBindMatchesSpot(Player const* bot, AhSpot const& spot)
{
    if (bot->m_homebindMapId != spot.mapId)
        return false;
    float const dx = bot->m_homebindX - spot.x;
    float const dy = bot->m_homebindY - spot.y;
    return (dx * dx + dy * dy) <= (500.0f * 500.0f);
}
