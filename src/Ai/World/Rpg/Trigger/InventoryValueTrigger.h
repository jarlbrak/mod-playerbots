/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_INVENTORYVALUETRIGGER_H
#define _PLAYERBOT_INVENTORYVALUETRIGGER_H

#include "PlayerbotAIConfig.h"
#include "Trigger.h"

class InventoryValueTrigger : public Trigger
{
public:
    InventoryValueTrigger(PlayerbotAI* ai) : Trigger(ai, "inventory value", 5 * 1000) {}
    bool IsActive() override;
};

#endif
