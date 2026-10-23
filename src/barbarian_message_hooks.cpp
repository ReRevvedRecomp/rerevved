// First-contact hides use the unseen-village path; captured-village and
// removed-civ messages use 0x82CE87A8 to clear text/flags and return -1.

#include "ai_civilizations.h"
#include "barbarian_messages.h"

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, hide_barbarian_messages);
REXCVAR_DECLARE(bool, hide_village_barbarian_messages);

namespace
{

thread_local rerevved::barbarian_messages::LeaderMessageGate gate;

} // namespace

// In sub_82CCEFA8 after the band-shown check, before the visibility check
// that natively skips to 0x82CD4738.
bool SkipBarbarianContactMessage()
{
    return REXCVAR_GET(hide_barbarian_messages);
}

// In sub_82CC85D0 after its prologue. r12 still holds the entry LR that the
// prologue saved with mflr r12, so it names the calling site, and r3 still
// holds the leader's player.
void BeginBarbarianLeaderMessage(PPCRegister& stack, PPCRegister& entryLr, PPCRegister& player)
{
    namespace barbarian = rerevved::barbarian_messages;
    namespace ai_civs   = rerevved::ai_civilizations;

    const bool village = REXCVAR_GET(hide_village_barbarian_messages) &&
                         barbarian::IsVillageReturn(entryLr.u32);
    // The removed set alone decides, so turning the option off before the
    // first round ends does not bring back the farewells it promised to hide.
    const bool farewell = entryLr.u32 == ai_civs::kFarewellReturn && ai_civs::IsRemovedCivilization(player.u32);
    gate.Begin(village || farewell, stack.u32, entryLr.u32 == barbarian::kNestedMessageReturn);
}

// At the only exit of sub_82CC85D0, after its show returns.
void EndBarbarianLeaderMessage(PPCRegister& stack)
{
    gate.End(stack.u32);
}

bool SuppressBarbarianLeaderShow(PPCRegister& stack)
{
    return gate.SuppressShow(stack.u32);
}
