/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_AHBUYERTRIGGER_H
#define _PLAYERBOT_AHBUYERTRIGGER_H

#include "PlayerbotAIConfig.h"
#include "Trigger.h"

class AhBuyerTrigger : public Trigger
{
public:
    AhBuyerTrigger(PlayerbotAI* ai) : Trigger(ai, "ah buyer", 30 * 1000) {}
    bool IsActive() override;
};

#endif
