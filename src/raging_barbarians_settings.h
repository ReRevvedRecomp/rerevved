#pragma once

#include <cstdint>

namespace rerevved::raging_barbarians
{

struct Evaluation
{
    int32_t nativeValue;
    int32_t finalValue;
    bool    enabled;
};

// New-game and load ingress snapshot the title config for subsequent updates.
void       FreezeForGame(bool standardRules = false);
void       RestoreForGame(bool enabled) noexcept;
Evaluation Evaluate(int32_t nativeValue) noexcept;

// Creation diagnostics accept slot zero and reject the native failure sentinel.
bool IsCreationResultValid(int32_t owner, int32_t type, int32_t slot) noexcept;

} // namespace rerevved::raging_barbarians
