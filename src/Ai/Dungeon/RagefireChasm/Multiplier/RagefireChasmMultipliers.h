#ifndef _PLAYERBOT_CLASSICDUNGEONRFCMULTIPLIERS_H
#define _PLAYERBOT_CLASSICDUNGEONRFCMULTIPLIERS_H

#include "Multiplier.h"

// Prioritize Oggleflint (chief) over the Searing Blade adds spawned near him.
// Drop adds last; killing Oggleflint usually causes the room to clear via reset.
class OggleflintMultiplier : public Multiplier
{
public:
    OggleflintMultiplier(PlayerbotAI* ai) : Multiplier(ai, "oggleflint") {}
    virtual float GetValue(Action* action);
};

// Kill Jergosh while ignoring his summoned imp (Searing Imp) — the imp despawns
// on Jergosh death.
class JergoshTheInvokerMultiplier : public Multiplier
{
public:
    JergoshTheInvokerMultiplier(PlayerbotAI* ai) : Multiplier(ai, "jergosh the invoker") {}
    virtual float GetValue(Action* action);
};

// Bazzalan is the rogue near Jergosh — focus before he stealths if seen alone,
// otherwise stay on Jergosh.
class BazzalanMultiplier : public Multiplier
{
public:
    BazzalanMultiplier(PlayerbotAI* ai) : Multiplier(ai, "bazzalan") {}
    virtual float GetValue(Action* action);
};

// Taragaman the Hungerer — straight tank-and-spank, but keep tank threat
// primary. Mostly here for completeness and future trigger expansion.
class TaragamanTheHungererMultiplier : public Multiplier
{
public:
    TaragamanTheHungererMultiplier(PlayerbotAI* ai) : Multiplier(ai, "taragaman the hungerer") {}
    virtual float GetValue(Action* action);
};

#endif
