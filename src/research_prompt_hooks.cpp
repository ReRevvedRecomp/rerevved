// hide_research_prompt suppresses automatic prompts until research is chosen;
// sub_82D259E8 is never armed.

#include "research_prompt.h"

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, hide_research_prompt);

namespace
{

thread_local rerevved::research_prompt::AutomaticPromptGate gate;

} // namespace

void ArmAutomaticResearchPrompt(PPCRegister& stack)
{
    gate.Arm(stack.u32);
}

void DisarmAutomaticResearchPrompt()
{
    gate.Disarm();
}

void HideAutomaticResearchPrompt(PPCRegister& stack)
{
    gate.BeginShow(REXCVAR_GET(hide_research_prompt), stack.u32);
}

bool SuppressResearchPromptShow(PPCRegister& stack)
{
    return gate.SuppressShow(stack.u32);
}
