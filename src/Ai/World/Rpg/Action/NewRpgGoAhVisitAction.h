/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_NEWRPGGOAHVISITACTION_H
#define _PLAYERBOT_NEWRPGGOAHVISITACTION_H

#include "NewRpgBaseAction.h"

class NewRpgGoAhVisitAction : public NewRpgBaseAction
{
public:
    NewRpgGoAhVisitAction(PlayerbotAI* botAI)
        : NewRpgBaseAction(botAI, "new rpg go ah visit") {}
    bool Execute(Event event) override;
};

#endif
