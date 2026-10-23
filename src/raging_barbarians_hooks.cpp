#include "raging_barbarians_hooks.h"
#include "raging_barbarians_settings.h"

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(raging_barbarians_trace, false, "ReRevved", "Log barbarian production, aggression, departure decisions and verified unit creations")
    .debug_only();

namespace
{

constexpr uint32_t kBarbarianOwner = 0x82F700B0;
constexpr uint32_t kPresence       = 0x830E7B10;
constexpr uint32_t kMode           = 0x82FFE7C8;
constexpr uint32_t kUnits          = 0x830F2BF0;

std::atomic<uint64_t> overrideHits{ 0 };
std::atomic<uint64_t> aggressionOverrideHits{ 0 };
std::atomic<uint64_t> creations{ 0 };
std::atomic<uint64_t> failures{ 0 };

bool readBytes(uint64_t address, void* out, uint32_t size)
{
    if (!size || address < 0x10000 || address + size > 0xFFFFF000ull)
    {
        return false;
    }
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    const auto first = static_cast<uint32_t>(address);
    const auto last  = first + size - 1;
    auto*      heap  = memory->LookupHeap(first);
    if (!heap || memory->LookupHeap(last) != heap ||
        heap->QueryRangeAccess(first, last) == rex::memory::PageAccess::kNoAccess)
    {
        return false;
    }
    std::memcpy(out, memory->TranslateVirtual<const uint8_t*>(first), size);
    return true;
}

uint16_t u16(const uint8_t* bytes)
{
    return (uint16_t{ bytes[0] } << 8) | bytes[1];
}

int16_t s16(const uint8_t* bytes)
{
    return std::bit_cast<int16_t>(u16(bytes));
}

bool readWord(uint64_t address, uint32_t& value)
{
    uint8_t bytes[4]{};
    if (!readBytes(address, bytes, sizeof(bytes)))
    {
        return false;
    }
    value = (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) |
            (uint32_t{ bytes[2] } << 8) | bytes[3];
    return true;
}

void traceInput(uint32_t base, uint32_t offset, const rerevved::raging_barbarians::Evaluation& evaluation)
{
    uint8_t city[0x3A]{};
    if (!readBytes(uint64_t{ base } + offset, city, sizeof(city)))
    {
        REXLOG_INFO("Raging Barbarians: override_hits={} native_q={} effective_q={} enabled={} city_unreadable=1",
                    overrideHits.load(),
                    evaluation.nativeValue,
                    evaluation.finalValue,
                    evaluation.enabled);
        return;
    }
    const int32_t x             = s16(city + 0x30);
    const int32_t y             = s16(city + 0x32);
    uint8_t       presence      = 0;
    uint32_t      mode          = 0;
    const int64_t cell          = int64_t{ x } * 40 + y;
    const bool    presenceValid = cell >= 0 && cell < 1600 && readBytes(kPresence + cell, &presence, 1);
    const bool    modeValid     = readWord(kMode, mode);
    REXLOG_INFO("Raging Barbarians: override_hits={} native_q={} effective_q={} enabled={} city_offset={} owner={} byte1={} x={} y={} weighted_presence={} presence_valid={} mode={} mode_valid={} production={}",
                overrideHits.load(),
                evaluation.nativeValue,
                evaluation.finalValue,
                evaluation.enabled,
                offset,
                static_cast<int>(std::bit_cast<int8_t>(city[0])),
                static_cast<int>(std::bit_cast<int8_t>(city[1])),
                x,
                y,
                presence,
                presenceValid,
                std::bit_cast<int32_t>(mode),
                modeValid,
                s16(city + 0x36));
}

} // namespace

void FreezeRagingBarbariansForGame()
{
    rerevved::raging_barbarians::FreezeForGame();
}

void ApplyRagingBarbariansFecundity(PPCRegister& fecundity, PPCRegister& cityBase, PPCRegister& cityOffset)
{
    // CityUpdate 0x82D153E4 follows extsh r10,r11 and both native owner guards.
    // Only r10 changes; q+1, weighted presence, mode suppression and credit stay native.
    try
    {
        const auto evaluation = rerevved::raging_barbarians::Evaluate(fecundity.s32);
        if (evaluation.enabled)
        {
            fecundity.s64 = evaluation.finalValue;
        }
        if (REXCVAR_GET(raging_barbarians_trace))
        {
            if (evaluation.enabled)
            {
                ++overrideHits;
            }
            traceInput(cityBase.u32, cityOffset.u32, evaluation);
        }
    }
    catch (...)
    {
        // Diagnostics never escape into the guest update.
    }
}

void ApplyRagingBarbariansAggression(PPCRegister& aggression, PPCRegister& cityOrdinal)
{
    // 0x82CB3F44 follows the native city-owner guard and precedes both extsh reads.
    // Override r11 so the zero/99 arm and native 3-a threshold use the same input.
    const auto evaluation = rerevved::raging_barbarians::Evaluate(aggression.s16);
    if (evaluation.enabled)
    {
        aggression.s64 = evaluation.finalValue;
    }
    if (!REXCVAR_GET(raging_barbarians_trace))
    {
        return;
    }
    try
    {
        if (evaluation.enabled)
        {
            ++aggressionOverrideHits;
        }
        REXLOG_INFO("Raging Barbarians: aggression_override_hits={} city_ordinal={} native_a={} effective_a={} native_R={} effective_R={} enabled={}",
                    aggressionOverrideHits.load(),
                    cityOrdinal.s32,
                    evaluation.nativeValue,
                    evaluation.finalValue,
                    evaluation.nativeValue == 0 ? 99 : 3 - evaluation.nativeValue,
                    evaluation.finalValue == 0 ? 99 : 3 - evaluation.finalValue,
                    evaluation.enabled);
    }
    catch (...)
    {
    }
}

void ObserveRagingBarbariansDeparture(PPCRegister& defenders, PPCRegister& threshold, PPCRegister& unitBase, PPCRegister& unitOffset, PPCRegister& origin)
{
    if (!REXCVAR_GET(raging_barbarians_trace))
    {
        return;
    }
    try
    {
        uint8_t  unit[0x24]{};
        uint32_t barbarian = 0;
        if (!readBytes(uint64_t{ unitBase.u32 } + unitOffset.u32, unit, sizeof(unit)) ||
            !readWord(kBarbarianOwner, barbarian) || std::bit_cast<int8_t>(unit[0]) != std::bit_cast<int32_t>(barbarian))
        {
            return;
        }
        const bool thresholdHold = defenders.s32 <= threshold.s32;
        REXLOG_INFO("Raging Barbarians: departure unit_offset={} owner={} type={} x={} y={} origin={} weighted_defenders={} R={} unit_weight_byte={} threshold_hold={} retain_selected={} enabled={}",
                    unitOffset.u32,
                    std::bit_cast<int32_t>(barbarian),
                    unit[1],
                    s16(unit + 0x1C),
                    s16(unit + 0x1E),
                    origin.s32,
                    defenders.s32,
                    threshold.s32,
                    unit[4],
                    thresholdHold,
                    !thresholdHold && unit[4] == 0,
                    rerevved::raging_barbarians::Evaluate(0).enabled);
    }
    catch (...)
    {
    }
}

void ObserveRagingBarbariansCredit(PPCRegister& credit, PPCRegister& presence, PPCRegister& cityBase, PPCRegister& cityOffset)
{
    if (!REXCVAR_GET(raging_barbarians_trace))
    {
        return;
    }
    try
    {
        uint8_t    city[0x3A]{};
        const bool valid = readBytes(uint64_t{ cityBase.u32 } + cityOffset.u32, city, sizeof(city));
        REXLOG_INFO("Raging Barbarians: city_offset={} native_credit={} weighted_presence={} production_before={} city_valid={}",
                    cityOffset.u32,
                    credit.s32,
                    presence.u32,
                    valid ? s16(city + 0x36) : -1,
                    valid);
    }
    catch (...)
    {
    }
}

void ObserveRagingBarbariansCreation(PPCRegister& result, PPCRegister& owner, PPCRegister& type, PPCRegister& cityBase, PPCRegister& stack)
{
    if (!REXCVAR_GET(raging_barbarians_trace))
    {
        return;
    }
    try
    {
        uint32_t barbarian = 0;
        if (!readWord(kBarbarianOwner, barbarian) || owner.s32 != std::bit_cast<int32_t>(barbarian) || type.s32 < 3 || type.s32 > 5)
        {
            return;
        }
        uint32_t       offset = 0;
        uint8_t        city[0x3A]{};
        const bool     cityValid = readWord(uint64_t{ stack.u32 } + 0x74, offset) &&
                                   readBytes(uint64_t{ cityBase.u32 } + offset, city, sizeof(city));
        uint8_t        unit[0x54]{};
        const bool     validSlot = rerevved::raging_barbarians::IsCreationResultValid(owner.s32, type.s32, result.s32);
        const uint64_t unitIndex = validSlot ? uint64_t{ owner.u32 } * 256 + result.u32 : 0;
        const bool     unitValid = validSlot && readBytes(kUnits + unitIndex * sizeof(unit), unit, sizeof(unit));
        const bool     verified  = cityValid && unitValid && unit[0] == owner.u32 && unit[1] == type.u32 && (unit[0x0C] & 0x80) == 0 &&
                                   city[0] == owner.u32 && s16(unit + 0x1C) == s16(city + 0x30) && s16(unit + 0x1E) == s16(city + 0x32);
        if (verified)
        {
            ++creations;
        }
        if (result.s32 == -1)
        {
            ++failures;
        }
        REXLOG_INFO("Raging Barbarians: override_hits={} creations={} failures={} owner={} type={} slot={} verified={} city_offset={} city_valid={} x={} y={} production_at_return={}",
                    overrideHits.load(),
                    creations.load(),
                    failures.load(),
                    owner.s32,
                    type.s32,
                    result.s32,
                    verified,
                    offset,
                    cityValid,
                    cityValid ? s16(city + 0x30) : -1,
                    cityValid ? s16(city + 0x32) : -1,
                    cityValid ? s16(city + 0x36) : -1);
    }
    catch (...)
    {
    }
}
