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

bool IsGuestReadableRange(uint32_t address, uint32_t extent)
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

bool TryReadU8(uint32_t address, uint8_t& value)
{
    if (!IsGuestReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    const auto* memory = REX_KERNEL_MEMORY();
    value              = memory->TranslateVirtual<const uint8_t*>(address)[0];
    return true;
}

bool TryWriteU8(uint32_t address, uint8_t value)
{
    if (!IsGuestReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    auto* memory                                   = REX_KERNEL_MEMORY();
    memory->TranslateVirtual<uint8_t*>(address)[0] = value;
    return true;
}

bool TryReadU32(uint32_t address, uint32_t& value)
{
    if (!IsGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    const auto* source =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    value = (uint32_t{ source[0] } << 24) | (uint32_t{ source[1] } << 16) |
            (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
    return true;
}

bool TryWriteU32(uint32_t address, uint32_t value)
{
    if (!IsGuestReadableRange(address, sizeof(uint32_t)))
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

bool TryReadCivilization(int32_t player, ReRevvedCivilizationId& civilization)
{
    if (player < 0 || player >= kPlayerCount ||
        !IsGuestReadableRange(kPlayerCivilizations +
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
                                           PPCRegister& unit_table,
                                           PPCRegister& record_offset)
{
    if (unit_table.u32 > UINT32_MAX - record_offset.u32)
    {
        return;
    }
    const uint32_t record = unit_table.u32 + record_offset.u32;
    if (!IsGuestReadableRange(record, kUnitRecordSize))
    {
        return;
    }

    uint8_t runtime_base_type = 0;
    uint8_t native_level      = 0;
    if (!TryReadU8(record + 0x01, runtime_base_type) ||
        !TryReadU8(record + 0x05, native_level))
    {
        return;
    }

    ReRevvedCivilizationId civilization = REREVVED_CIVILIZATION_UNKNOWN;
    if (!TryReadCivilization(player.s32, civilization))
    {
        return;
    }

    ReRevvedUnitIdentityId identity = REREVVED_UNIT_IDENTITY_BASE;
    if (!rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, runtime_base_type, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return;
    }

    ReRevvedUnitEffectEvaluation veteran_evaluation{};
    if (rerevved::unit_effect_rules::TryEvaluate(
            civilization,
            runtime_base_type,
            identity,
            REREVVED_UNIT_EFFECT_CREATION_VETERAN,
            native_level,
            veteran_evaluation) &&
        (veteran_evaluation.status_flags &
         REREVVED_UNIT_EFFECT_EVALUATION_GRANTED) != 0 &&
        veteran_evaluation.final_level > native_level)
    {
        // The native +0x3C gate, UEA 50 route, and maximum-two saturation
        // remain immediately after this hook.
        TryWriteU8(record + 0x05,
                   static_cast<uint8_t>(veteran_evaluation.final_level));
    }

    uint32_t native_upgrades = 0;
    if (!TryReadU32(record + 0x10, native_upgrades))
    {
        return;
    }

    uint32_t granted_upgrades = 0;
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
                runtime_base_type,
                identity,
                effect,
                (native_upgrades & mask) != 0 ? 1 : 0,
                evaluation) &&
            (evaluation.status_flags &
             REREVVED_UNIT_EFFECT_EVALUATION_GRANTED) != 0 &&
            (native_upgrades & mask) == 0)
        {
            granted_upgrades |= mask;
        }
    }

    if (granted_upgrades == 0 ||
        !TryWriteU32(record + 0x10, native_upgrades | granted_upgrades))
    {
        return;
    }

    uint32_t march_mask = 0;
    if (rerevved::unit_effect_rules::TryGetNativeSpecialUpgradeMask(
            REREVVED_UNIT_EFFECT_CREATION_MARCH, march_mask) &&
        (granted_upgrades & march_mask) != 0)
    {
        uint8_t movement = 0;
        if (TryReadU8(record + 0x02, movement))
        {
            TryWriteU8(record + 0x02, static_cast<uint8_t>(movement + 3));
        }
    }
}
