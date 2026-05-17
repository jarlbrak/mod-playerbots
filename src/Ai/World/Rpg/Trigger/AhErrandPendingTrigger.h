/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_AHERRANDPENDINGTRIGGER_H
#define _PLAYERBOT_AHERRANDPENDINGTRIGGER_H

#include "Trigger.h"

class AhErrandPendingTrigger : public Trigger
{
public:
    // 10-second check interval (Trigger's third arg is milliseconds — see
    // existing AhBuyerTrigger at 30*1000 and InventoryValueTrigger at 5*1000).
    // The action itself ticks every interval while travel is in progress.
    AhErrandPendingTrigger(PlayerbotAI* botAI)
        : Trigger(botAI, "ah errand pending", 10 * 1000) {}
    bool IsActive() override;
};

#endif
