// Always-on repairs of native selection and admission bugs.

#include "gameplay_fixes.h"

#include <atomic>
#include <cstdint>

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

constexpr uint32_t kResearchLogCap        = 64;
constexpr uint32_t kResearchSummaryPeriod = 256;
constexpr int32_t  kAlphabetTechnology    = 1;

std::atomic<uint32_t> researchLogged{ 0 };
std::atomic<uint32_t> researchSuppressed{ 0 };
std::atomic<int64_t>  researchLastKey{ -1 };

} // namespace

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
