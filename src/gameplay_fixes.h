#pragma once

// Selection and arithmetic for the gameplay fixes in gameplay_fixes_hooks.cpp,
// kept free of guest memory so it can be tested on the host.

#include <array>
#include <cstdint>
#include <string_view>

namespace rerevved::gameplay_fixes
{

// Temple's native recipient score is population, without a building-specific
// increment. Its separate score > best gate still picks the largest city.
constexpr bool BypassTemplePopulationThreshold(int32_t buildingIndex) noexcept
{
    return buildingIndex == 3;
}

// Preserve the native discovery flag for Egypt's free Irrigation. Phase 1
// applies the reward. Phase 2 presents it without repeating population writes.
constexpr bool PreserveEgyptianIrrigationDiscovery(
    int32_t technology, int32_t source, int32_t phase, int32_t civilization) noexcept
{
    return technology == 11 && source == 6 && (phase == 1 || phase == 2) && civilization == 1;
}

// The first-discovery building join matches Courthouse (normalized building 14)
// to Literacy (technology 12) through its definition+0x42 field. Treating only
// that pair as a non-match keeps every other technology/building pair native.
constexpr bool SuppressLiteracyCourthouseReward(int32_t technology, int32_t buildingIndex) noexcept
{
    return technology == 12 && buildingIndex == 14;
}

// The first-discovery caption rereads the saved recipient city. Skip its format
// call when that city is outside the 128-record table or its current owner is
// not the acquiring player. Delivery and the owner classifier stay native.
constexpr int32_t kFirstDiscoveryCityCount  = 128;
constexpr int32_t kFirstDiscoveryCityStride = 188;

constexpr bool SuppressStaleFirstDiscoveryCaption(
    int32_t savedCity, int32_t currentOwner, int32_t acquirer) noexcept
{
    return savedCity < 0 || savedCity >= kFirstDiscoveryCityCount || currentOwner < 0 ||
           currentOwner != acquirer;
}

// sub_82CECCD8 stores technologies at ordinals 0..N-1 and -1 at N, retaining N+1;
// starting the loop at ordinal 0 scores the first candidate, not the sentinel.
constexpr uint32_t kAiResearchFirstOrdinal = 0;

constexpr uint32_t AiResearchScanStart(uint32_t listBase) noexcept
{
    return listBase + kAiResearchFirstOrdinal * 4;
}

// Native trip count of the scoring loop; the N=0 case branches past it.
constexpr uint32_t AiResearchScanCount(int32_t retained) noexcept
{
    return retained > 1 ? static_cast<uint32_t>(retained - 1) : 0;
}

// The spaceship launch dialog returns a signed arrival year, or -1 when
// cancelled or incomplete. Year zero and BC years are valid arrivals.
constexpr bool AdmitsSpaceshipArrivalYear(int32_t dialogResult) noexcept
{
    return dialogResult != -1;
}

// AI nuclear arm 0x82CBF570 requires reverse pair-history bit 0x20, whose known
// writer chain needs a nuclear threat, matching reply and lost city.
// Ordinary AI bypasses only this bit; humans keep the native rule.
constexpr bool AdmitAiNuclearOpponent(bool ordinaryAi, uint32_t maskedHistory) noexcept
{
    return ordinaryAi && maskedHistory == 0;
}

// ActiveCivilizationBonusLookup (sub_82CF0CB0) result filter. Each rule matches
// the captured bonus ID, cumulative mode 0 and the caller's native return PC.
constexpr int32_t kGoldBonusAbility   = 25;
constexpr int32_t kGrowthBonusAbility = 8;

// UEA25 adds bonus * era to the rush numerator (1 + bonus) * era + 2. These
// callers price, display or convert rushed Production. City Gold income at
// 0x82D148C0 and the candidate valuations stay native.
constexpr std::array<uint32_t, 16> kGoldBonusRateReturns = {
    0x82CB3468,
    0x82CB67C0,
    0x82CB68E4,
    0x82CB69E8,
    0x82CB6BD0,
    0x82CB6CE0,
    0x82CB6DC4,
    0x82CE28B0,
    0x82CE2994,
    0x82CE2A4C,
    0x82CEC324,
    0x82CEC454,
    0x82CEC564,
    0x82D17974,
    0x82D17A08,
    0x82D17A50,
};

// UEA8's growth threshold is max(20, B - 10) with the composed baseline B in
// r31 at the call. The false branch returns B. Unsigned keeps negative B native.
constexpr uint32_t kGrowthThresholdReturn = 0x82CF10FC;
constexpr uint32_t kGrowthThresholdFloor  = 20;

constexpr bool SuppressBonusLookupResult(
    int32_t ability, int32_t mode, uint64_t returnAddress, uint32_t growthBaseline) noexcept
{
    if (mode != 0)
    {
        return false;
    }
    if (ability == kGoldBonusAbility)
    {
        for (const uint32_t rateReturn : kGoldBonusRateReturns)
        {
            if (returnAddress == rateReturn)
            {
                return true;
            }
        }
        return false;
    }
    return ability == kGrowthBonusAbility && returnAddress == kGrowthThresholdReturn &&
           growthBaseline < kGrowthThresholdFloor;
}

// Domination needs currentTurn - startTurn >= 10 in the native palace-count arm.
constexpr int32_t kNativeDominationMinimumTurns = 10;

// Accepts only the offered choices "10" and "5".
bool ParseDominationMinimumTurns(std::string_view text, int32_t& turns) noexcept;

// Applies only to a game started in this process, including a Game of the Week
// with Standard Rules. Loaded saves do not record it and keep the native minimum.
void NoteNewGame() noexcept;
void NoteGameLoad() noexcept;
bool ShortenedMinimumAllowed() noexcept;

int32_t DominationMinimumTurns(std::string_view setting, bool shortenedAllowed) noexcept;

// Rebase the elapsed turns so the native compare against 10 tests the chosen
// minimum. The native value is unchanged at 10.
int32_t RebaseDominationElapsed(int32_t elapsed, int32_t minimumTurns) noexcept;

} // namespace rerevved::gameplay_fixes
