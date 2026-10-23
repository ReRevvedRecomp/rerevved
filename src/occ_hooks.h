#pragma once

#include <rex/ppc.h>

void FreezeOccForGame();
void ApplyOccCandidateThreshold(PPCRegister& threshold, PPCRegister& type, PPCRegister& population);
void ApplyOccCompletionThreshold(PPCRegister& threshold, PPCRegister& type, PPCRegister& population);
bool GuardOccAiChoice(PPCRegister& stack, PPCRegister& ordinal);
bool ApplyOccGoldReward(PPCRegister& owner, PPCRegister& applying);
