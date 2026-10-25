#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace rerevved::ai_civilizations
{

class SetupScope
{
public:
    SetupScope(int32_t requested, bool standardRules, uint8_t* base, int32_t mode = 0) noexcept;
    ~SetupScope();
    SetupScope(const SetupScope&)            = delete;
    SetupScope& operator=(const SetupScope&) = delete;
};

// Commit requires both the setup recount and a normal return from new-game init.
void NoteNewGameRecount() noexcept;
void CommitNewGame() noexcept;
// The frozen request is available only after the early hook accepted expansion.
int32_t ExpandedAiLimit() noexcept;
// Loading native state must never normalize its serialized participant layout.
void ResetOnLoad() noexcept;

} // namespace rerevved::ai_civilizations

void ConfigureExpandedAiSetup();                                          // 0x82D21FF8, after the native human-mask rebuild.
void PreserveExpandedAiSetup(PPCRegister& count, PPCRegister& barbarian); // 0x82D22918, before both count stores.
