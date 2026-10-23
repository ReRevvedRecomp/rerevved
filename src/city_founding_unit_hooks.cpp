#include "city_founding_unit_hooks.h"
#include "city_founding_unit_rules.h"

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(city_founding_unit_trace, false, "ReRevved", "Log city founding unit gates, creator results and Loyalty writes")
    .debug_only();

REX_EXTERN(sub_82D11750);

namespace
{

bool accessible(uint32_t address, uint32_t size, bool write)
{
    if (!size || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
    {
        return false;
    }
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap)
    {
        return false;
    }
    const auto access = heap->QueryRangeAccess(address, address + size - 1);
    return write ? (access == rex::memory::PageAccess::kReadWrite || access == rex::memory::PageAccess::kExecuteReadWrite) : access != rex::memory::PageAccess::kNoAccess;
}

class GuestWorld final : public rerevved::city_founding_unit::World
{
public:
    GuestWorld(PPCRegister stack, PPCRegister threadPointer, PPCFPSCRRegister fpscr)
    : stack_(stack)
    , threadPointer_(threadPointer)
    , fpscr_(fpscr)
    {
    }

    bool Read(uint32_t address, void* out, uint32_t size) override
    {
        if (!accessible(address, size, false))
        {
            return false;
        }
        std::memcpy(out, REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address), size);
        return true;
    }

    bool Write(uint32_t address, const void* bytes, uint32_t size) override
    {
        if (!accessible(address, size, true))
        {
            return false;
        }
        std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address), bytes, size);
        return true;
    }

    int32_t Create(int32_t owner, int32_t type, int32_t x, int32_t y) override
    {
        // A private ABI linkage/home area keeps creator argument spills below
        // the live CityCreate frame. All continuation registers stay untouched.
        if (stack_.u32 < 0x10070 || !accessible(stack_.u32 - 0x70, 0x70, true))
        {
            return -1;
        }
        PPCContext call{};
        call.r1.u64 = stack_.u32 - 0x70;
        call.r13    = threadPointer_;
        call.fpscr  = fpscr_;
        call.lr     = 0x82D1A314;
        call.r3.s64 = owner;
        call.r4.s64 = type;
        call.r5.s64 = x;
        call.r6.s64 = y;
        // Xenon stwu frames use a big-endian 32-bit back chain.
        const uint8_t backChain[] = { static_cast<uint8_t>(stack_.u32 >> 24),
                                      static_cast<uint8_t>(stack_.u32 >> 16),
                                      static_cast<uint8_t>(stack_.u32 >> 8),
                                      static_cast<uint8_t>(stack_.u32) };
        if (!Write(call.r1.u32, backChain, sizeof(backChain)))
        {
            return -1;
        }

        struct HostFloatScope
        {
            uint32_t saved = PPCFPSCRRegister::Platform::getcsr();

            ~HostFloatScope()
            {
                PPCFPSCRRegister::Platform::setcsr(saved);
            }
        } floatScope;

        sub_82D11750(call, REX_KERNEL_MEMORY()->virtual_membase());
        return call.r3.s32;
    }

private:
    PPCRegister      stack_;
    PPCRegister      threadPointer_;
    PPCFPSCRRegister fpscr_;
};

const char* decisionName(rerevved::city_founding_unit::Decision decision)
{
    using rerevved::city_founding_unit::Decision;
    switch (decision)
    {
        case Decision::Disabled:
            return "disabled";
        case Decision::InvalidFrame:
            return "invalid_frame";
        case Decision::OriginMismatch:
            return "origin_mismatch";
        case Decision::InvalidOwner:
            return "invalid_owner";
        case Decision::InvalidCity:
            return "invalid_city";
        case Decision::Reentry:
            return "reentry";
        case Decision::Admitted:
            return "admitted";
    }
    return "unknown";
}

} // namespace

void ApplyCityFoundingUnit(PPCRegister& stack, PPCRegister& citySlot, PPCRegister& owner, PPCRegister& threadPointer, PPCFPSCRRegister& fpscr)
{
    try
    {
        GuestWorld world(stack, threadPointer, fpscr);
        const auto result = rerevved::city_founding_unit::Apply(world, { stack.u32, citySlot.s32, owner.s32 });
        if (REXCVAR_GET(city_founding_unit_trace))
        {
            REXLOG_INFO("City founding unit: gate={} frame={:08X} origin={:08X} city={} owner={} x={} y={} creator_called={} creator_result={} retained_unit={} loyalty_write={} upgrades_before={:08X} upgrades_after={:08X}",
                        decisionName(result.decision),
                        stack.u32,
                        result.origin,
                        citySlot.s32,
                        owner.s32,
                        result.x,
                        result.y,
                        result.creatorCalled,
                        result.unitSlot,
                        result.retainedUnit,
                        result.loyaltyWritten,
                        result.upgradesBefore,
                        result.upgradesAfter);
        }
        if (result.creatorCalled && !result.loyaltyWritten)
        {
            REXLOG_WARN("City founding unit: grant failed city={} owner={} creator_result={} retained_unit={}; no retry",
                        citySlot.s32,
                        owner.s32,
                        result.unitSlot,
                        result.retainedUnit);
        }
    }
    catch (...)
    {
        REXLOG_WARN("City founding unit: grant interrupted city={} owner={}; no retry", citySlot.s32, owner.s32);
    }
}
