#ifndef _PLAYERBOT_CLASSICDUNGEONRFCACTIONCONTEXT_H
#define _PLAYERBOT_CLASSICDUNGEONRFCACTIONCONTEXT_H

#include "NamedObjectContext.h"
#include "AiObjectContext.h"
#include "RagefireChasmActions.h"

class ClassicDungeonRFCActionContext : public NamedObjectContext<Action>
{
public:
    ClassicDungeonRFCActionContext()
    {
        // No actions registered yet.
    }
};

#endif
