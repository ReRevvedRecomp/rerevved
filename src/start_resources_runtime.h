#pragma once

#include <rex/ppc.h>

namespace rerevved::start_resources
{

// Called only by the successful new-game recount ingress, after AI removal.
void    OnNewGameSetup(PPCContext& context, uint8_t* base);
bool    SuppressIndiaStartingResource();
void    ResetOnLoad();
int32_t ForGame();
void    RestoreForGame(int32_t resources);

} // namespace rerevved::start_resources
