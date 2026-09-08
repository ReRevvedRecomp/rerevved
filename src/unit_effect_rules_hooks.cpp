#include "unit_effect_rules_registry.h"

#include <array>
#include <cstdint>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "unit_catalog_api.h"

namespace
{

constexpr uint32_t kPlayerCivilizations = 0x830ECD28;
constexpr int32_t  kPlayerCount         = 6;
constexpr uint32_t kUnitRecordSize      = 0x54;

constexpr std::array<ReRevvedUnitEffectId, 9> kSpecialUpgradeEffects = {
    REREVVED_UNIT_EFFECT_CREATION_BLITZ,
    REREVVED_UNIT_EFFECT_CREATION_INFILTRATION,
    REREVVED_UNIT_EFFECT_CREATION_GUERILLA,
    REREVVED_UNIT_EFFECT_CREATION_LOYALTY,
    REREVVED_UNIT_EFFECT_CREATION_ENGINEER,
    REREVVED_UNIT_EFFECT_CREATION_LEADERSHIP,
    REREVVED_UNIT_EFFECT_CREATION_MARCH,
    REREVVED_UNIT_EFFECT_CREATION_MEDIC,
    REREVVED_UNIT_EFFECT_CREATION_SCOUT,
};

bool isGuestReadableRange(uint32_t address, uint32_t extent)
{
    if (address > UINT32_MAX - extent || address < 0x10000 ||
        address + extent - 1 >= 0xFFFFF000)
    {
        return false;
    }

    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    return heap && memory->LookupHeap(address + extent - 1) == heap &&
           heap->QueryRangeAccess(address, address + extent - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

bool tryReadU8(uint32_t address, uint8_t& value)
{
    if (!isGuestReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    const auto* memory = REX_KERNEL_MEMORY();
    value              = memory->TranslateVirtual<const uint8_t*>(address)[0];
    return true;
}

bool tryWriteU8(uint32_t address, uint8_t value)
{
    if (!isGuestReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    auto* memory                                   = REX_KERNEL_MEMORY();
    memory->TranslateVirtual<uint8_t*>(address)[0] = value;
    return true;
}

bool tryReadU32(uint32_t address, uint32_t& value)
{
    if (!isGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    const auto* source =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    value = (uint32_t{ source[0] } << 24) | (uint32_t{ source[1] } << 16) |
            (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
    return true;
}

bool tryWriteU32(uint32_t address, uint32_t value)
{
    if (!isGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    auto* target = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address);
    target[0]    = static_cast<uint8_t>(value >> 24);
    target[1]    = static_cast<uint8_t>(value >> 16);
    target[2]    = static_cast<uint8_t>(value >> 8);
    target[3]    = static_cast<uint8_t>(value);
    return true;
}

bool tryReadCivilization(int32_t player, ReRevvedCivilizationId& civilization)
{
    if (player < 0 || player >= kPlayerCount ||
        !isGuestReadableRange(kPlayerCivilizations +
                                  static_cast<uint32_t>(player) * 4,
                              sizeof(uint32_t)))
    {
        return false;
    }

    const auto* memory = REX_KERNEL_MEMORY();
    const auto* source = memory->TranslateVirtual<const uint8_t*>(
        kPlayerCivilizations + static_cast<uint32_t>(player) * 4);
    const uint32_t value = (uint32_t{ source[0] } << 24) |
                           (uint32_t{ source[1] } << 16) |
                           (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
    if (value >= REREVVED_CIVILIZATION_COUNT)
    {
        return false;
    }
    civilization = static_cast<ReRevvedCivilizationId>(value);
    return true;
}

} // namespace

void ReRevvedApplyUnitEffectCreationGrants(PPCRegister& player,
                                           PPCRegister& unitTable,
                                           PPCRegister& recordOffset)
{
    if (unitTable.u32 > UINT32_MAX - recordOffset.u32)
    {
        return;
    }
    const uint32_t record = unitTable.u32 + recordOffset.u32;
    if (!isGuestReadableRange(record, kUnitRecordSize))
    {
        return;
    }

    uint8_t runtimeBaseType = 0;
    uint8_t nativeLevel     = 0;
    if (!tryReadU8(record + 0x01, runtimeBaseType) ||
        !tryReadU8(record + 0x05, nativeLevel))
    {
        return;
    }

    ReRevvedCivilizationId civilization = REREVVED_CIVILIZATION_UNKNOWN;
    if (!tryReadCivilization(player.s32, civilization))
    {
        return;
    }

    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, runtimeBaseType, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return;
    }

    ReRevvedUnitEffectEvaluation veteranEvaluation{};
    if (rerevved::unit_effect_rules::TryEvaluate(
            civilization,
            runtimeBaseType,
            identity,
            REREVVED_UNIT_EFFECT_CREATION_VETERAN,
            nativeLevel,
            veteranEvaluation) &&
        (veteranEvaluation.statusFlags &
         REREVVED_UNIT_EFFECT_EVALUATION_GRANTED) != 0 &&
        veteranEvaluation.finalLevel > nativeLevel)
    {
        // The native +0x3C gate, UEA 50 route, and maximum-two saturation
        // remain immediately after this hook.
        tryWriteU8(record + 0x05,
                   static_cast<uint8_t>(veteranEvaluation.finalLevel));
    }

    uint32_t nativeUpgrades = 0;
    if (!tryReadU32(record + 0x10, nativeUpgrades))
    {
        return;
    }

    uint32_t grantedUpgrades = 0;
    for (ReRevvedUnitEffectId effect : kSpecialUpgradeEffects)
    {
        uint32_t mask = 0;
        if (!rerevved::unit_effect_rules::TryGetNativeSpecialUpgradeMask(
                effect, mask))
        {
            continue;
        }

        ReRevvedUnitEffectEvaluation evaluation{};
        if (rerevved::unit_effect_rules::TryEvaluate(
                civilization,
                runtimeBaseType,
                identity,
                effect,
                (nativeUpgrades & mask) != 0 ? 1 : 0,
                evaluation) &&
            (evaluation.statusFlags &
             REREVVED_UNIT_EFFECT_EVALUATION_GRANTED) != 0 &&
            (nativeUpgrades & mask) == 0)
        {
            grantedUpgrades |= mask;
        }
    }

    if (grantedUpgrades == 0 ||
        !tryWriteU32(record + 0x10, nativeUpgrades | grantedUpgrades))
    {
        return;
    }

    uint32_t marchMask = 0;
    if (rerevved::unit_effect_rules::TryGetNativeSpecialUpgradeMask(
            REREVVED_UNIT_EFFECT_CREATION_MARCH, marchMask) &&
        (grantedUpgrades & marchMask) != 0)
    {
        uint8_t movement = 0;
        if (tryReadU8(record + 0x02, movement))
        {
            tryWriteU8(record + 0x02, static_cast<uint8_t>(movement + 3));
        }
    }
}
