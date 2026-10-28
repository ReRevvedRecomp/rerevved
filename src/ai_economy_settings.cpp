#include "ai_economy_settings.h"

#include <algorithm>
#include <atomic>
#include <limits>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(ai_research_bonus_percent, 0, "ReRevved", "AI research bonus percentage for the next new game; multiplayer uses the host's value; 0 keeps native costs")
    .range(0, 200);
REXCVAR_DEFINE_INT32(ai_production_bonus_percent, 0, "ReRevved", "AI production bonus percentage for the next new game; multiplayer uses the host's value; 0 keeps native costs")
    .range(0, 200);
REXCVAR_DEFINE_INT32(ai_gold_bonus_percent, 0, "ReRevved", "AI gold bonus percentage for the next new game; multiplayer uses the host's value; 0 keeps native gold")
    .range(0, 200);
REXCVAR_DEFINE_INT32(ai_food_bonus_percent, 0, "ReRevved", "AI food bonus percentage for the next new game; multiplayer uses the host's value; 0 keeps native food")
    .range(0, 200);
REXCVAR_DEFINE_INT32(ai_culture_bonus_percent, 0, "ReRevved", "AI culture bonus percentage for the next new game; multiplayer uses the host's value; 0 keeps native culture")
    .range(0, 200);

namespace rerevved::ai_economy
{
namespace
{

constexpr uint64_t    kNativePolicy = 0;
std::atomic<uint64_t> frozenPolicy{ kNativePolicy };
std::atomic<bool>     loading{ false };
std::atomic<uint64_t> priorPolicy{ kNativePolicy };
std::atomic<bool>     loadResetReached{ false };

bool validBonus(int32_t bonus) noexcept
{
    return bonus >= 0 && bonus <= kMaxBonusPercent;
}

// One byte per field, in Policy order.
uint64_t encode(Policy policy) noexcept
{
    uint64_t encoded = 0;
    unsigned shift   = 0;
    for (int32_t bonus : { policy.researchBonusPercent, policy.productionBonusPercent, policy.goldBonusPercent, policy.foodBonusPercent, policy.cultureBonusPercent })
    {
        encoded |= static_cast<uint64_t>(bonus) << shift;
        shift += 8;
    }
    return encoded;
}

Policy decode(uint64_t encoded) noexcept
{
    const auto field = [encoded](unsigned index)
    {
        return static_cast<int32_t>((encoded >> (index * 8)) & 0xFF);
    };
    return { field(0), field(1), field(2), field(3), field(4) };
}

} // namespace

bool IsValid(Policy policy) noexcept
{
    return validBonus(policy.researchBonusPercent) && validBonus(policy.productionBonusPercent) && validBonus(policy.goldBonusPercent) &&
           validBonus(policy.foodBonusPercent) && validBonus(policy.cultureBonusPercent);
}

void RestoreForGame(Policy policy) noexcept
{
    frozenPolicy.store(IsValid(policy) ? encode(policy) : kNativePolicy, std::memory_order_release);
}

void FreezeForGame(bool standardRules) noexcept
{
    RestoreForGame(standardRules ? Policy{}
                                 : Policy{ REXCVAR_GET(ai_research_bonus_percent), REXCVAR_GET(ai_production_bonus_percent), REXCVAR_GET(ai_gold_bonus_percent), REXCVAR_GET(ai_food_bonus_percent), REXCVAR_GET(ai_culture_bonus_percent) });
    loading.store(false, std::memory_order_release);
}

Policy ForGame() noexcept
{
    if (loading.load(std::memory_order_acquire))
    {
        return {};
    }
    return decode(frozenPolicy.load(std::memory_order_acquire));
}

Policy ForSave(bool networkGame) noexcept
{
    return networkGame ? Policy{} : ForGame();
}

void BeginLoad() noexcept
{
    priorPolicy.store(frozenPolicy.load(std::memory_order_acquire), std::memory_order_relaxed);
    loadResetReached.store(false, std::memory_order_relaxed);
    loading.store(true, std::memory_order_release);
}

void ResetForLoad() noexcept
{
    RestoreForGame({});
    if (loading.load(std::memory_order_acquire))
    {
        loadResetReached.store(true, std::memory_order_release);
    }
}

void FinishLoad(bool networkGame) noexcept
{
    if (!loading.load(std::memory_order_acquire))
    {
        return;
    }
    if (!loadResetReached.load(std::memory_order_acquire))
    {
        frozenPolicy.store(priorPolicy.load(std::memory_order_relaxed), std::memory_order_release);
    }
    else if (networkGame)
    {
        RestoreForGame({});
    }
    loading.store(false, std::memory_order_release);
}

LoadScope::LoadScope() noexcept
{
    BeginLoad();
}

LoadScope::~LoadScope()
{
    // An interrupted load leaves the new mode unknown after reset.
    Finish(true);
}

void LoadScope::Finish(bool networkGame) noexcept
{
    if (!finished)
    {
        FinishLoad(networkGame);
        finished = true;
    }
}

int32_t ScaleCost(int32_t nativeCost, int32_t bonusPercent) noexcept
{
    if (nativeCost <= 0 || bonusPercent == 0 || !validBonus(bonusPercent))
    {
        return nativeCost;
    }
    return static_cast<int32_t>(std::max<int64_t>(1, static_cast<int64_t>(nativeCost) * 100 / (100 + bonusPercent)));
}

int32_t ScaleUnitScalar(int32_t nativeScalar, int32_t factor, int32_t bonusPercent) noexcept
{
    if (nativeScalar <= 0 || factor <= 0 || bonusPercent == 0 || !validBonus(bonusPercent))
    {
        return nativeScalar;
    }
    const int64_t nativeProduct = static_cast<int64_t>(nativeScalar) * factor;
    if (nativeProduct < 2 || nativeProduct > std::numeric_limits<int32_t>::max())
    {
        return nativeScalar;
    }
    // Native unit cost is trunc(factor * scalar / 2). Retain a positive cost.
    const int64_t minimumScalar = factor == 1 ? 2 : 1;
    return static_cast<int32_t>(std::max(minimumScalar, static_cast<int64_t>(nativeScalar) * 100 / (100 + bonusPercent)));
}

int32_t ScaleYield(int32_t nativeYield, int32_t bonusPercent) noexcept
{
    // Keep values within the native city caches' signed halfword range.
    constexpr int64_t kHalfwordMax = 32767;
    if (nativeYield <= 0 || nativeYield > kHalfwordMax || bonusPercent == 0 || !validBonus(bonusPercent))
    {
        return nativeYield;
    }
    return static_cast<int32_t>(std::min<int64_t>(kHalfwordMax, static_cast<int64_t>(nativeYield) * (100 + bonusPercent) / 100));
}

} // namespace rerevved::ai_economy
