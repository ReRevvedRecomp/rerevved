#include "ai_economy_settings.h"

#include <algorithm>
#include <atomic>
#include <limits>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(ai_research_cost_percent, 100, "ReRevved", "AI research cost percentage for the next new game; multiplayer uses the host's value; 100 keeps native costs")
    .range(1, 100);
REXCVAR_DEFINE_INT32(ai_production_cost_percent, 100, "ReRevved", "AI production cost percentage for the next new game; multiplayer uses the host's value; 100 keeps native costs")
    .range(1, 100);

namespace rerevved::ai_economy
{
namespace
{

constexpr uint32_t    kNativePolicy = 100 | (100 << 8);
std::atomic<uint32_t> frozenPolicy{ kNativePolicy };
std::atomic<bool>     loading{ false };
std::atomic<uint32_t> priorPolicy{ kNativePolicy };
std::atomic<bool>     loadResetReached{ false };

bool validPercent(int32_t percent) noexcept
{
    return percent >= 1 && percent <= 100;
}

} // namespace

bool IsValid(Policy policy) noexcept
{
    return validPercent(policy.researchCostPercent) && validPercent(policy.productionCostPercent);
}

void RestoreForGame(Policy policy) noexcept
{
    const uint32_t encoded = IsValid(policy)
                                 ? static_cast<uint32_t>(policy.researchCostPercent) | (static_cast<uint32_t>(policy.productionCostPercent) << 8)
                                 : kNativePolicy;
    frozenPolicy.store(encoded, std::memory_order_release);
}

void FreezeForGame(bool standardRules) noexcept
{
    RestoreForGame(standardRules ? Policy{} : Policy{ REXCVAR_GET(ai_research_cost_percent), REXCVAR_GET(ai_production_cost_percent) });
    loading.store(false, std::memory_order_release);
}

Policy ForGame() noexcept
{
    if (loading.load(std::memory_order_acquire))
    {
        return {};
    }
    const uint32_t encoded = frozenPolicy.load(std::memory_order_acquire);
    return { static_cast<int32_t>(encoded & 0xFF), static_cast<int32_t>((encoded >> 8) & 0xFF) };
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
    // An interrupted load has no certified new mode after the reset seam.
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

int32_t ScaleCost(int32_t nativeCost, int32_t percent) noexcept
{
    if (nativeCost <= 0 || percent == 100 || !validPercent(percent))
    {
        return nativeCost;
    }
    return static_cast<int32_t>(std::max<int64_t>(1, static_cast<int64_t>(nativeCost) * percent / 100));
}

int32_t ScaleUnitScalar(int32_t nativeScalar, int32_t factor, int32_t percent) noexcept
{
    if (nativeScalar <= 0 || factor <= 0 || percent == 100 || !validPercent(percent))
    {
        return nativeScalar;
    }
    const int64_t nativeProduct = static_cast<int64_t>(nativeScalar) * factor;
    if (nativeProduct < 2 || nativeProduct > std::numeric_limits<int32_t>::max())
    {
        return nativeScalar;
    }
    // Native unit cost is trunc(factor * scalar / 2); retain a positive cost.
    const int64_t minimumScalar = factor == 1 ? 2 : 1;
    return static_cast<int32_t>(std::max(minimumScalar, static_cast<int64_t>(nativeScalar) * percent / 100));
}

} // namespace rerevved::ai_economy
