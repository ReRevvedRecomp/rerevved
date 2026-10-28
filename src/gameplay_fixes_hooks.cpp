#include "gameplay_fixes.h"
#include "gameplay_session_settings.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace
{

bool readGuestU32(uint32_t address, uint32_t& value)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || address < 0x10000 || address > 0xFFFFF000u)
    {
        return false;
    }
    const auto* p = memory->TranslateVirtual<const uint8_t*>(address);
    value         = uint32_t{ p[0] } << 24 | uint32_t{ p[1] } << 16 | uint32_t{ p[2] } << 8 | uint32_t{ p[3] };
    return true;
}

bool readGuestU8(uint32_t address, uint8_t& value)
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory || address < 0x10000 || address > 0xFFFFF000u)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || heap->QueryRangeAccess(address, address) == rex::memory::PageAccess::kNoAccess)
    {
        return false;
    }
    value = *memory->TranslateVirtual<const uint8_t*>(address);
    return true;
}

constexpr uint32_t kResearchLogCap        = 64;
constexpr uint32_t kResearchSummaryPeriod = 256;
constexpr int32_t  kAlphabetTechnology    = 1;

std::atomic<uint32_t> researchLogged{ 0 };
std::atomic<uint32_t> researchSuppressed{ 0 };
std::atomic<int64_t>  researchLastKey{ -1 };

std::atomic<int64_t> dominationLastKey{ -1 };

constexpr int32_t kDominationPlayerSlots = 8;

std::array<std::atomic<int64_t>, kDominationPlayerSlots> dominationCapitalsLastKey{ { -1, -1, -1, -1, -1, -1, -1, -1 } };

} // namespace

// Before the population hurdle at 0x82D0A158. Earlier native gates require an
// owned populated city without the building and a score above the current best.
bool AdmitFirstDiscoveryTempleCity(PPCRegister& buildingIndex)
{
    return rerevved::gameplay_fixes::BypassTemplePopulationThreshold(buildingIndex.s32);
}

// Before the join branch at 0x82D09BD0 (bne to the next building at 0x82D0A2E4).
// r9 is the acquiring technology and r18 the normalized building index. The
// native loop skips the building before scoring, delivery or recipient save.
bool SuppressLiteracyCourthouseReward(PPCRegister& technology, PPCRegister& buildingIndex)
{
    return rerevved::gameplay_fixes::SuppressLiteracyCourthouseReward(technology.s32, buildingIndex.s32);
}

// Before the resolver-equality branch at 0x82D0A254. r15 is the saved recipient
// city, r16 the city table (owner byte at record+0) and r29 the acquiring player.
// Jumping to 0x82D0A280 is the native mismatch path: no caption format call, with
// the owner classifier and frame+0xA4 left to the guest.
bool SuppressStaleFirstDiscoveryCaption(PPCRegister& savedCity, PPCRegister& cityTable, PPCRegister& acquirer)
{
    namespace gf = rerevved::gameplay_fixes;
    if (savedCity.s32 < 0 || savedCity.s32 >= gf::kFirstDiscoveryCityCount)
    {
        return true;
    }
    uint8_t        owner   = 0;
    const uint32_t address = cityTable.u32 + static_cast<uint32_t>(savedCity.s32) * gf::kFirstDiscoveryCityStride;
    if (!readGuestU8(address, owner))
    {
        return true;
    }
    return gf::SuppressStaleFirstDiscoveryCaption(savedCity.s32, static_cast<int8_t>(owner), acquirer.s32);
}

// At 0x82D09588 the native first-ownership/phase flag is already established.
// Skip only its source-6 clear. Never manufacture a discovery flag.
bool PreserveEgyptianIrrigationReward(
    PPCRegister& player, PPCRegister& technology, PPCRegister& source, PPCRegister& phase)
{
    if (player.s32 < 0 || player.s32 >= 8 || technology.s32 != 11 || source.s32 != 6 ||
        (phase.s32 != 1 && phase.s32 != 2))
    {
        return false;
    }
    constexpr uint32_t kCivilizations = 0x830ECD28;
    uint32_t           civilization   = 0;
    auto*              memory         = REX_KERNEL_MEMORY();
    const uint32_t     address        = kCivilizations + player.u32 * 4;
    auto*              heap           = memory ? memory->LookupHeap(address) : nullptr;
    if (!heap || heap->QueryRangeAccess(address, address + 3) == rex::memory::PageAccess::kNoAccess ||
        !readGuestU32(address, civilization))
    {
        return false;
    }
    return rerevved::gameplay_fixes::PreserveEgyptianIrrigationDiscovery(
        technology.s32, source.s32, phase.s32, static_cast<int32_t>(civilization));
}

// Before destination indexing at 0x82CF7090 and 0x82CF7274. -1 has no
// destination slot. Configured failure tails preserve native bookkeeping.
bool SkipFailedSpyUnitCreation(PPCRegister& returnedSlot)
{
    return returnedSlot.s32 == -1;
}

// Before add r24,r11,r10 at 0x82CED34C; r10 = r18 + 4 begins the nonlocal research
// scoring loop at ordinal 1. r11 is the per-player list offset (player * 400),
// r26 the retained count N+1.
void FixAiResearchScanStart(PPCRegister& r10, PPCRegister& r11, PPCRegister& r18, PPCRegister& r26)
{
    r10.u64 = rerevved::gameplay_fixes::AiResearchScanStart(r18.u32);

    const uint32_t count = rerevved::gameplay_fixes::AiResearchScanCount(r26.s32);
    uint32_t       first = 0;
    if (!readGuestU32(r11.u32 + r18.u32, first))
    {
        return;
    }
    const int64_t key = static_cast<int64_t>(count) << 32 | first;
    if (researchLastKey.exchange(key, std::memory_order_relaxed) == key)
    {
        return;
    }
    if (researchLogged.load(std::memory_order_relaxed) >= kResearchLogCap)
    {
        const uint32_t suppressed = researchSuppressed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (suppressed % kResearchSummaryPeriod != 0)
        {
            return;
        }
        REXLOG_INFO("GameplayFix: research scan suppressed={}", suppressed);
        return;
    }
    researchLogged.fetch_add(1, std::memory_order_relaxed);
    REXLOG_INFO(
        "GameplayFix: research scan base={:08X} count={} first={} alphabet={}",
        r11.u32 + r18.u32,
        count,
        static_cast<int32_t>(first),
        static_cast<int32_t>(first) == kAlphabetTechnology ? 1 : 0);
}

// Before cmpwi cr6,r31,0 at 0x82D2A084: admit any launch dialog result but -1.
bool AdmitSpaceshipArrivalYear(PPCRegister& r31)
{
    const int32_t result   = r31.s32;
    const bool    admitted = rerevved::gameplay_fixes::AdmitsSpaceshipArrivalYear(result);
    REXLOG_INFO(
        "GameplayFix: spaceship launch year={} admitted={} old={}",
        result,
        admitted ? 1 : 0,
        result > 0 ? 1 : 0);
    return admitted;
}

// Before cmpwi cr6,r9,10 at 0x82D11264; r9 is the elapsed turn count and is
// dead after the compare.
void ApplyDominationMinimumTurns(PPCRegister& r9)
{
    const auto    host    = rerevved::gameplay_session::ForNetworkGame();
    const int32_t minimum = host && rerevved::gameplay_fixes::ShortenedMinimumAllowed()
                                ? host->dominationMinimumTurns
                                : rerevved::gameplay_fixes::DominationMinimumTurns(
                                      rex::cvar::Query<std::string>("domination_min_turns"), rerevved::gameplay_fixes::ShortenedMinimumAllowed());
    const int32_t elapsed = r9.s32;
    r9.s32                = rerevved::gameplay_fixes::RebaseDominationElapsed(elapsed, minimum);

    const int32_t native        = rerevved::gameplay_fixes::kNativeDominationMinimumTurns;
    const bool    nativeAllowed = elapsed >= native;
    const bool    allowed       = r9.s32 >= native;
    if (minimum != native || allowed != nativeAllowed)
    {
        const int64_t key = static_cast<int64_t>(elapsed) << 32 | static_cast<uint32_t>(minimum);
        if (dominationLastKey.exchange(key, std::memory_order_relaxed) != key)
        {
            REXLOG_INFO(
                "GameplayFix: domination elapsed={} minimum={} native_allowed={} allowed={}",
                elapsed,
                minimum,
                nativeAllowed ? 1 : 0,
                allowed ? 1 : 0);
        }
    }
}

// Before cmpw cr6,r9,r11 at 0x82D11250. r27 is the player, r9 the owned original
// capitals, r10 the scenario QUICKWIN value and r11 the required count (total minus
// r10). Logs without changing registers.
void LogDominationCapitals(PPCRegister& r27, PPCRegister& r9, PPCRegister& r10, PPCRegister& r11)
{
    const int32_t player = r27.s32;
    if (player < 0 || player >= kDominationPlayerSlots)
    {
        return;
    }
    const int32_t owned    = r9.s32;
    const int32_t required = r11.s32;
    const int32_t total    = required + r10.s32;
    const int64_t key      = static_cast<int64_t>(static_cast<uint16_t>(owned)) << 32 |
                             static_cast<int64_t>(static_cast<uint16_t>(required)) << 16 | static_cast<uint16_t>(total);
    if (dominationCapitalsLastKey[player].exchange(key, std::memory_order_relaxed) != key)
    {
        REXLOG_INFO(
            "GameplayFix: domination capitals player={} owned={} required={} total={}", player, owned, required, total);
    }
}

REX_EXTERN(__imp__sub_82CF0CB0);

// ActiveCivilizationBonusLookup: r3 bonus ID, r4 player, r5 cumulative mode. It
// returns the active flag in r3. The native lookup runs once and only r3 may
// change after it, so its table and the Unique Era Ability cell hook stay in force.
REX_HOOK_RAW(sub_82CF0CB0)
{
    const int32_t  ability        = ctx.r3.s32;
    const int32_t  mode           = ctx.r5.s32;
    const uint64_t returnAddress  = ctx.lr;
    const uint32_t growthBaseline = ctx.r31.u32;
    __imp__sub_82CF0CB0(ctx, base);
    if (rerevved::gameplay_fixes::SuppressBonusLookupResult(ability, mode, returnAddress, growthBaseline))
    {
        ctx.r3.u64 = 0;
    }
}
