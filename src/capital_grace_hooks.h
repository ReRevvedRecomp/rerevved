#pragma once

#include <rex/ppc.h>

bool GuardCapitalGraceAdmission(PPCRegister& record, PPCRegister& command, PPCRegister& player, PPCRegister& argument, PPCRegister& direction);
bool GuardCapitalGraceCapture(PPCRegister& city, PPCRegister& newOwner);
void BeginCapitalGraceSubmit(PPCRegister& frame, PPCRegister& queue, PPCRegister& ring);
void AppendCapitalGraceToken(PPCRegister& frame, PPCRegister& queue, PPCRegister& record);
void CompleteCapitalGraceAppend(PPCRegister& frame);
void ObserveCapitalGraceDrainSerial(PPCRegister& frame, PPCRegister& queue, PPCRegister& serial);
void BindCapitalGraceDrain(PPCRegister& frame);
void EndCapitalGraceSubmit(PPCRegister& frame);
void BeginCapitalGraceBlitz(PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra);
void BeginCapitalGraceOrdinary(PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra);
void BeginCapitalGracePacked(PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra);
void BeginCapitalGraceLinked(PPCRegister& frame, PPCRegister& command, PPCRegister& player, PPCRegister& slot, PPCRegister& direction, PPCRegister& extra);
bool EndCapitalGraceBlitz(PPCRegister& frame);
bool EndCapitalGraceOrdinary(PPCRegister& frame);
bool EndCapitalGracePacked(PPCRegister& frame);
void PrepareCapitalGraceParent(PPCRegister& frame, PPCRegister& units, PPCRegister& globals, PPCRegister& parent, PPCRegister& selected, PPCRegister& oldFlags);
bool EndCapitalGraceLinked(PPCRegister& frame, PPCRegister& units, PPCRegister& globals, PPCRegister& parent, PPCRegister& selected);
bool FinishCapitalGraceParent(PPCRegister& frame);
