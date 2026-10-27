#pragma once

#include <rex/ppc.h>

// Custom achievement observer for a successful local city founding. Called at
// the start of ApplyCityFoundingUnit, which owns the one hook at 0x82D1A314.
void ObserveCustomAchievementCityFounded(PPCRegister& stack, PPCRegister& citySlot, PPCRegister& owner, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr);
