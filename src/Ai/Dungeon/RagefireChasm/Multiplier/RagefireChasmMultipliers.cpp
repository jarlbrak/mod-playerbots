#include "RagefireChasmMultipliers.h"
#include "RagefireChasmActions.h"
#include "GenericSpellActions.h"
#include "ChooseTargetActions.h"

// Boost the named boss over adds by returning 1.0 for the boss and dampening
// generic add-target actions when the boss is present and targetable. All
// multipliers follow the same shape: find the boss, suppress competing
// DpsAssistAction targeting when boss is alive.

static float SuppressAddsIfBossAlive(PlayerbotAI* botAI, char const* bossName, Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", bossName);
    if (!boss) { return 1.0f; }
    if (!boss->isTargetableForAttack()) { return 1.0f; }
    if (dynamic_cast<DpsAssistAction*>(action))
    {
        // If the bot's current target is already the boss, allow assist;
        // otherwise dampen so DPS shifts onto the boss next decision tick.
        Unit* currentTarget = AI_VALUE(Unit*, "current target");
        if (currentTarget && currentTarget->GetGUID() == boss->GetGUID())
            return 1.0f;
        return 0.0f;
    }
    return 1.0f;
}

float OggleflintMultiplier::GetValue(Action* action)
{
    return SuppressAddsIfBossAlive(botAI, "oggleflint", action);
}

float JergoshTheInvokerMultiplier::GetValue(Action* action)
{
    return SuppressAddsIfBossAlive(botAI, "jergosh the invoker", action);
}

float BazzalanMultiplier::GetValue(Action* action)
{
    return SuppressAddsIfBossAlive(botAI, "bazzalan", action);
}

float TaragamanTheHungererMultiplier::GetValue(Action* action)
{
    return SuppressAddsIfBossAlive(botAI, "taragaman the hungerer", action);
}
