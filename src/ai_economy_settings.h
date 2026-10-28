#pragma once

#include <cstdint>

namespace rerevved::ai_economy
{

// AI bonus percentages: 0 keeps native behavior, 200 is the maximum.
constexpr int32_t kMaxBonusPercent = 200;

struct Policy
{
    int32_t researchBonusPercent            = 0;
    int32_t productionBonusPercent          = 0;
    int32_t goldBonusPercent                = 0;
    int32_t foodBonusPercent                = 0;
    int32_t cultureBonusPercent             = 0;
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

// Research and production bonuses divide the native cost by (1 + bonus / 100).
int32_t ScaleCost(int32_t nativeCost, int32_t bonusPercent) noexcept;
int32_t ScaleUnitScalar(int32_t nativeScalar, int32_t factor, int32_t bonusPercent) noexcept;

// Gold, food and culture bonuses multiply a positive completed city
// contribution. Values above the native halfword cache range stay native.
int32_t ScaleYield(int32_t nativeYield, int32_t bonusPercent) noexcept;

// An unavailable guest network flag also requires native costs.
bool NativeCostsRequired() noexcept;

} // namespace rerevved::ai_economy
