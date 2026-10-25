#pragma once

#include <cstdint>

namespace rerevved::ai_economy
{

struct Policy
{
    int32_t researchCostPercent             = 100;
    int32_t productionCostPercent           = 100;
    bool    operator==(const Policy&) const = default;
};

bool   IsValid(Policy policy) noexcept;
void   FreezeForGame(bool standardRules = false) noexcept;
Policy ForGame() noexcept;
Policy ForSave(bool networkGame) noexcept;
void   RestoreForGame(Policy policy) noexcept;

// Keep cost readers native until the loaded network context is available.
// Loads that never reach ResetForLoad retain the previous game's policy.
void BeginLoad() noexcept;
void ResetForLoad() noexcept;
void FinishLoad(bool networkGame) noexcept;

class LoadScope
{
public:
    LoadScope() noexcept;
    ~LoadScope();
    LoadScope(const LoadScope&)            = delete;
    LoadScope& operator=(const LoadScope&) = delete;

    void Finish(bool networkGame) noexcept;

private:
    bool finished = false;
};

int32_t ScaleCost(int32_t nativeCost, int32_t percent) noexcept;
int32_t ScaleUnitScalar(int32_t nativeScalar, int32_t factor, int32_t percent) noexcept;

// An unavailable guest network flag also requires native costs.
bool NativeCostsRequired() noexcept;

} // namespace rerevved::ai_economy
