/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_LISTATAUCTIONACTION_H
#define _PLAYERBOT_LISTATAUCTIONACTION_H

#include "InventoryAction.h"

class ListAtAuctionAction : public InventoryAction
{
public:
    ListAtAuctionAction(PlayerbotAI* ai, std::string const name = "list at auction")
        : InventoryAction(ai, name)
    {
    }
    bool Execute(Event event) override;

private:
    Creature* FindNearestAuctioneer();
};

#endif
