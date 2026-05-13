#ifndef _PLAYERBOT_CLASSICDUNGEONRFCTRIGGERCONTEXT_H
#define _PLAYERBOT_CLASSICDUNGEONRFCTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "AiObjectContext.h"
#include "RagefireChasmTriggers.h"

class ClassicDungeonRFCTriggerContext : public NamedObjectContext<Trigger>
{
public:
    ClassicDungeonRFCTriggerContext()
    {
        // No triggers registered yet — see RagefireChasmTriggers.h note.
    }
};

#endif
