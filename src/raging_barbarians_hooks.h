#pragma once

#include <rex/ppc.h>

void FreezeRagingBarbariansForGame();
void ApplyRagingBarbariansFecundity(PPCRegister& fecundity, PPCRegister& cityBase, PPCRegister& cityOffset);
void ApplyRagingBarbariansAggression(PPCRegister& aggression, PPCRegister& cityOrdinal);
void ObserveRagingBarbariansDeparture(PPCRegister& defenders, PPCRegister& threshold, PPCRegister& unitBase, PPCRegister& unitOffset, PPCRegister& origin);
void ObserveRagingBarbariansCredit(PPCRegister& credit, PPCRegister& presence, PPCRegister& cityBase, PPCRegister& cityOffset);
void ObserveRagingBarbariansCreation(PPCRegister& result, PPCRegister& owner, PPCRegister& type, PPCRegister& cityBase, PPCRegister& stack);
