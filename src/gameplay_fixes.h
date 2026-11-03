#pragma once

// Selection and arithmetic for the gameplay fixes in gameplay_fixes_hooks.cpp,
// kept free of guest memory so it can be tested on the host.

#include "ai_civilizations.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
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

// Unlimited bypasses native warning and deadline at 0x82D11378.
constexpr int32_t                kTurnLimitNative    = 0;
constexpr int32_t                kTurnLimitUnlimited = 4;
constexpr std::array<int32_t, 4> kTurnLimitYears     = { 2100, 2200, 2300, 2500 };

constexpr bool IsValidTurnLimit(int32_t limit) noexcept
{
    return limit <= kTurnLimitUnlimited;
}

constexpr bool ExtendsTurnLimit(int32_t limit) noexcept
{
    return limit > kTurnLimitNative && limit < kTurnLimitUnlimited;
}

int32_t ParseTurnLimit(std::string_view text) noexcept;

void    RestoreTurnLimit(int32_t limit) noexcept;
int32_t TurnLimitForGame() noexcept;

// From turn 118 the calendar year is 2 * turn + 1714 (turn 193 is 2100 AD).
constexpr int32_t TurnLimitCapTurn(int32_t limit) noexcept
{
    return (kTurnLimitYears[static_cast<size_t>(limit)] - 1714) / 2;
}

// Scenarios other than sentinel -1 also require 200 turns after the start.
constexpr int32_t TurnLimitDeadlineTurn(int32_t limit, int32_t scenario, int32_t startTurn) noexcept
{
    const int32_t cap = TurnLimitCapTurn(limit);
    return scenario == -1 ? cap : std::max(cap, startTurn + 200);
}

constexpr bool TurnLimitWarningDue(int32_t limit, int32_t scenario, int32_t startTurn, int32_t turn) noexcept
{
    return turn == TurnLimitDeadlineTurn(limit, scenario, startTurn) - 5;
}

constexpr bool BeforeTurnLimitYear(int32_t limit, int32_t year) noexcept
{
    return year < kTurnLimitYears[static_cast<size_t>(limit)];
}

// STARTERA at Q+0x1C drives grants of 5, 14 or 24 technologies at 0x82D22A78 (Future grants 1 to 46).
// Freeze for new-game setup only; never apply on load.
constexpr int32_t kStartingEraNative = 0;
constexpr int32_t kStartingEraFuture = 4;

constexpr bool IsValidStartingEra(int32_t era) noexcept
{
    return era <= kStartingEraFuture;
}

// Only scenario sentinel -1 permits a new-game era override.
constexpr bool AppliesStartingEra(int32_t era, int32_t scenario) noexcept
{
    return era != kStartingEraNative && scenario == -1;
}

// Retail STARTYEAR (Q+0x3E) pairs with STARTERA
constexpr int32_t StartingEraYear(int32_t era) noexcept
{
    constexpr int32_t kYears[] = { 0, 1000, 1500, 1800, 2025 };
    return kYears[era];
}

int32_t ParseStartingEra(std::string_view text) noexcept;

void    RestoreStartingEra(int32_t era) noexcept;
int32_t StartingEraForGame() noexcept;

// Network setup writes S32[0x82FFE7C8] = 2 at 0x82D221EC
constexpr int32_t kMultiplayerAiDifficultyNative = 2;
constexpr int32_t kMultiplayerAiDifficultyDeity  = 4;

constexpr bool IsValidMultiplayerAiDifficulty(int32_t difficulty) noexcept
{
    return difficulty >= kMultiplayerAiDifficultyNative && difficulty <= kMultiplayerAiDifficultyDeity;
}

int32_t ParseMultiplayerAiDifficulty(std::string_view text) noexcept;

void    RestoreMultiplayerAiDifficulty(int32_t difficulty) noexcept;
int32_t MultiplayerAiDifficultyForGame() noexcept;

// U32[0x830ED2D8 + 4 * (8 * p + q)] is war 0, peace 1, no contact -1.
constexpr uint32_t kRelationMatrix  = 0x830ED2D8;
constexpr int32_t  kRelationPlayers = 8;
constexpr uint32_t kRelationWar     = 0;

constexpr bool IsPairOf(uint32_t mask, int32_t p, int32_t q) noexcept
{
    return p != q && p >= 0 && q >= 0 && p < kRelationPlayers && q < kRelationPlayers &&
           (mask >> p & 1) != 0 && (mask >> q & 1) != 0;
}

inline uint32_t IntendedCivilizations(int32_t count, int32_t barbarian, uint32_t humans, int32_t maxAi) noexcept
{
    return ai_civilizations::CivilizationMask(count, barbarian) &
           ~ai_civilizations::RemovableMask(count, barbarian, humans, maxAi);
}

// Live means an owned populated city or an occupied, non-dead unit.
inline uint32_t LiveCivilizations(int32_t count, int32_t barbarian, uint32_t cityOwners, uint32_t unitOwners) noexcept
{
    return ai_civilizations::CivilizationMask(count, barbarian) & (cityOwners | unitOwners);
}

inline int32_t ProjectWar(std::span<uint32_t, 64> relation, uint32_t mask) noexcept
{
    int32_t changed = 0;
    for (int32_t p = 0; p < kRelationPlayers; ++p)
    {
        for (int32_t q = 0; q < kRelationPlayers; ++q)
        {
            uint32_t& cell = relation[static_cast<size_t>(kRelationPlayers * p + q)];
            if (IsPairOf(mask, p, q) && cell != kRelationWar)
            {
                cell = kRelationWar;
                ++changed;
            }
        }
    }
    return changed;
}

constexpr bool ForbidsPeace(bool active, uint32_t live, int32_t p, int32_t q) noexcept
{
    return active && IsPairOf(live, p, q);
}

// Includes shared requests at 0x82D03388 and delivered command 39.
constexpr bool ForbidsRelationWrite(bool active, uint32_t live, int32_t p, int32_t q, uint32_t current, uint32_t value) noexcept
{
    return ForbidsPeace(active, live, p, q) && current == kRelationWar && value != kRelationWar;
}

// Freeze on new game; clear on load until sidecar restoration.
void RestoreAlwaysAtWar(bool enabled) noexcept;
bool AlwaysAtWarForGame() noexcept;

// Type 24 part-rocket actors from 0x82D1DED8 use key 256 * owner + slot.
constexpr int32_t SpaceshipActorIndex(int32_t owner, int32_t slot) noexcept
{
    return owner * 256 + slot;
}

constexpr bool MatchesSpaceshipTag(int32_t tag, int32_t owner, int32_t slot) noexcept
{
    return tag == SpaceshipActorIndex(owner, slot);
}

constexpr int32_t kSpaceshipFinishCorridor = 1000;

constexpr bool FinishesSpaceshipEarly(int32_t previousTime) noexcept
{
    return previousTime <= kSpaceshipFinishCorridor;
}

// Domination needs currentTurn - startTurn >= 10 in the native palace-count arm.
constexpr int32_t kNativeDominationMinimumTurns = 10;

// Accepts only the offered choices "10" and "5".
bool ParseDominationMinimumTurns(std::string_view text, int32_t& turns) noexcept;

// Only a game started in this process may shorten it
void NoteNewGame() noexcept;
void NoteGameLoad() noexcept;
bool ShortenedMinimumAllowed() noexcept;
void RestoreDominationMinimum(int32_t turns) noexcept;
// Zero when no practice save restored one.
int32_t RestoredDominationMinimum() noexcept;

int32_t DominationMinimumTurns(std::string_view setting, bool shortenedAllowed) noexcept;

// Rebase the elapsed turns so the native compare against 10 tests the chosen
// minimum. The native value is unchanged at 10.
int32_t RebaseDominationElapsed(int32_t elapsed, int32_t minimumTurns) noexcept;

enum class UnitMovement
{
    Normal,
    Fast,
    Instant,
};

constexpr UnitMovement ParseUnitMovement(std::string_view text) noexcept
{
    return text == "fast" ? UnitMovement::Fast : text == "instant" ? UnitMovement::Instant
                                                                   : UnitMovement::Normal;
}

constexpr int32_t kFastMovementFactor = 3;
// Largest quantum with quantum + quantum / 2 fitting a signed 32-bit word.
constexpr int32_t kMaxMovementQuantum = 1431655765;

constexpr int32_t ScaleMovementQuantum(int32_t quantum) noexcept
{
    return quantum > 0 ? static_cast<int32_t>(std::min<int64_t>(int64_t{ quantum } * kFastMovementFactor, kMaxMovementQuantum)) : quantum;
}

constexpr bool AcceleratesMovingUnit(int32_t unitType) noexcept
{
    return unitType != 24 && unitType != 27;
}

} // namespace rerevved::gameplay_fixes
