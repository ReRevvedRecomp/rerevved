#include "unit_combat_rules_registry.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "unit_catalog_api.h"

namespace
{

constexpr uint32_t kUnitTable            = 0x830F2BF0;
constexpr uint32_t kUnitRecordSize       = 0x54;
constexpr int32_t  kPlayerCount          = 6;
constexpr int32_t  kUnitsPerPlayer       = 256;
constexpr uint32_t kTerrainTable         = 0x8312B288;
constexpr uint32_t kAttackerPlayerOffset = 1572;
constexpr uint32_t kAttackerUnitOffset   = 1580;
constexpr uint32_t kDefenderPlayerOffset = 1596;
constexpr uint32_t kDefenderUnitOffset   = 1604;
constexpr uint32_t kAttackTextOffset     = 336;
constexpr uint32_t kDefenseTextOffset    = 592;

bool IsGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

bool IsGuestReadableRange(uint32_t address, uint32_t extent)
{
    if (address > UINT32_MAX - extent)
    {
        return false;
    }

    const uint32_t end = address + extent;
    if (!IsGuestPointer(address) || !IsGuestPointer(end - 1))
    {
        return false;
    }

    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return false;
    }
    auto* heap = memory->LookupHeap(address);
    return heap && memory->LookupHeap(end - 1) == heap &&
           heap->QueryRangeAccess(address, end - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

uint32_t ReadBigEndianU32(const uint8_t* value)
{
    return (uint32_t{ value[0] } << 24) | (uint32_t{ value[1] } << 16) |
           (uint32_t{ value[2] } << 8) | uint32_t{ value[3] };
}

uint16_t ReadBigEndianU16(const uint8_t* value)
{
    return (uint16_t{ value[0] } << 8) | uint16_t{ value[1] };
}

bool TryReadGuestU32(uint32_t address, uint32_t& value)
{
    if (!IsGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    const auto* source =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    value = ReadBigEndianU32(source);
    return true;
}

bool TryReadStackI32(uint32_t stack, uint32_t offset, int32_t& value)
{
    if (stack > UINT32_MAX - offset)
    {
        return false;
    }

    uint32_t bits = 0;
    if (!TryReadGuestU32(stack + offset, bits))
    {
        return false;
    }
    value = std::bit_cast<int32_t>(bits);
    return true;
}

bool TryReadCivilization(int32_t player, ReRevvedCivilizationId& civilization)
{
    if (player < 0 || player >= kPlayerCount)
    {
        return false;
    }

    uint32_t value = 0;
    if (!TryReadGuestU32(0x830ECD28u +
                             static_cast<uint32_t>(player) * sizeof(uint32_t),
                         value) ||
        value >= REREVVED_CIVILIZATION_COUNT)
    {
        return false;
    }
    civilization = static_cast<ReRevvedCivilizationId>(value);
    return true;
}

bool TryGetUnitAddress(int32_t player, int32_t unit, uint32_t& address)
{
    if (player < 0 || player >= kPlayerCount || unit < 0 ||
        unit >= kUnitsPerPlayer)
    {
        return false;
    }

    const uint32_t index =
        static_cast<uint32_t>(player * kUnitsPerPlayer + unit);
    address = kUnitTable + index * kUnitRecordSize;
    return true;
}

bool TryReadUnitCoordinates(int32_t  player,
                            int32_t  unit,
                            int16_t& x,
                            int16_t& y)
{
    uint32_t address = 0;
    if (!TryGetUnitAddress(player, unit, address) ||
        !IsGuestReadableRange(address + 0x1C, sizeof(uint32_t)))
    {
        return false;
    }

    const auto* record =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    x = std::bit_cast<int16_t>(ReadBigEndianU16(record + 0x1C));
    y = std::bit_cast<int16_t>(ReadBigEndianU16(record + 0x1E));
    return true;
}

bool TryReadUnitBaseType(int32_t             player,
                         int32_t             unit,
                         ReRevvedUnitTypeId& base_unit_type)
{
    uint32_t address = 0;
    if (!TryGetUnitAddress(player, unit, address) ||
        !IsGuestReadableRange(address + 0x01, sizeof(uint8_t)))
    {
        return false;
    }

    const uint8_t value = *REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(
        address + 0x01);
    if (value >= REREVVED_UNIT_TYPE_COUNT)
    {
        return false;
    }
    base_unit_type = static_cast<ReRevvedUnitTypeId>(value);
    return true;
}

bool IsForestDefenderTile(int32_t defender_player, int32_t defender_unit)
{
    int16_t x = 0;
    int16_t y = 0;
    if (!TryReadUnitCoordinates(defender_player, defender_unit, x, y) ||
        x < 0 || y < 0)
    {
        return false;
    }

    const uint32_t tile_address =
        kTerrainTable + static_cast<uint32_t>(x) * 40u +
        static_cast<uint32_t>(y);
    if (!IsGuestReadableRange(tile_address, 1))
    {
        return false;
    }

    const uint8_t terrain = *REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(
        tile_address);
    return terrain == 3; // Guest Forest.
}

bool TryResolveIdentity(uint32_t                stack,
                        uint32_t                player_offset,
                        uint32_t                unit_offset,
                        ReRevvedCivilizationId& civilization,
                        ReRevvedUnitTypeId&     unit_type,
                        ReRevvedUnitIdentityId& identity)
{
    int32_t            player         = 0;
    int32_t            unit           = 0;
    ReRevvedUnitTypeId base_unit_type = REREVVED_UNIT_TYPE_UNKNOWN;
    if (!TryReadStackI32(stack, player_offset, player) ||
        !TryReadStackI32(stack, unit_offset, unit) ||
        !TryReadCivilization(player, civilization) ||
        !TryReadUnitBaseType(player, unit, base_unit_type) ||
        !rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, base_unit_type, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return false;
    }

    unit_type = base_unit_type;
    return true;
}

constexpr uint32_t kCombatTextBufferSize = 256;

bool TryAppendForestCombatLine(uint32_t stack,
                               uint32_t buffer_offset,
                               int32_t  percentage_delta)
{
    if (stack > UINT32_MAX - buffer_offset)
    {
        return false;
    }

    const uint32_t buffer_address = stack + buffer_offset;
    if (!IsGuestReadableRange(buffer_address, kCombatTextBufferSize))
    {
        return false;
    }

    auto*    memory = REX_KERNEL_MEMORY();
    auto*    buffer = memory->TranslateVirtual<uint8_t*>(buffer_address);
    uint32_t end    = 0;
    while (end < kCombatTextBufferSize && buffer[end] != 0)
    {
        ++end;
    }
    if (end == kCombatTextBufferSize)
    {
        return false;
    }

    const char* prefix        = "Woodsman ";
    uint32_t    prefix_length = 0;
    while (prefix[prefix_length] != '\0')
    {
        ++prefix_length;
    }

    const int64_t magnitude   = percentage_delta < 0
                                    ? -static_cast<int64_t>(percentage_delta)
                                    : static_cast<int64_t>(percentage_delta);
    uint32_t      digit_count = 1;
    for (int64_t divisor = 10; magnitude >= divisor; divisor *= 10)
    {
        ++digit_count;
    }

    // Prefix, sign, digits, percent sign, newline, and terminal NUL.
    const uint32_t line_size = prefix_length + 1 + digit_count + 3;
    if (end > kCombatTextBufferSize - line_size)
    {
        return false;
    }

    auto*    target   = buffer + end;
    uint32_t position = 0;
    for (; position < prefix_length; ++position)
    {
        target[position] = static_cast<uint8_t>(prefix[position]);
    }

    target[position++] = percentage_delta < 0 ? '-' : '+';
    // Format the decimal digits directly into the guest buffer.
    int64_t remaining = magnitude;
    int64_t divisor   = 1;
    for (uint32_t index = 1; index < digit_count; ++index)
    {
        divisor *= 10;
    }
    for (; divisor != 0; divisor /= 10)
    {
        target[position++] = static_cast<uint8_t>('0' + remaining / divisor);
        remaining %= divisor;
    }

    target[position++] = '%';
    target[position++] = '\n';
    target[position]   = '\0';
    return true;
}

void ApplyCombatRule(uint32_t                   stack,
                     uint32_t                   identity_player_offset,
                     uint32_t                   identity_unit_offset,
                     ReRevvedUnitCombatProperty property,
                     uint32_t                   text_buffer_offset,
                     PPCRegister&               accumulator)
{
    int32_t defender_player = 0;
    int32_t defender_unit   = 0;
    if (!TryReadStackI32(stack, kDefenderPlayerOffset, defender_player) ||
        !TryReadStackI32(stack, kDefenderUnitOffset, defender_unit) ||
        !IsForestDefenderTile(defender_player, defender_unit))
    {
        return;
    }

    ReRevvedCivilizationId civilization = REREVVED_CIVILIZATION_UNKNOWN;
    ReRevvedUnitTypeId     unit_type    = REREVVED_UNIT_TYPE_UNKNOWN;
    ReRevvedUnitIdentityId identity     = REREVVED_UNIT_IDENTITY_BASE;
    if (!TryResolveIdentity(stack,
                            identity_player_offset,
                            identity_unit_offset,
                            civilization,
                            unit_type,
                            identity))
    {
        return;
    }

    ReRevvedUnitCombatEvaluation evaluation{};
    if (!rerevved::unit_combat_rules::TryEvaluate(civilization,
                                                  unit_type,
                                                  identity,
                                                  REREVVED_TERRAIN_FOREST,
                                                  property,
                                                  evaluation) ||
        evaluation.additive_count == 0 ||
        (evaluation.status_flags &
         REREVVED_UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0)
    {
        return;
    }

    const int32_t percentage_delta = evaluation.final_percent -
                                     evaluation.native_percent;
    if (percentage_delta == 0)
    {
        return;
    }

    const int64_t composed = static_cast<int64_t>(accumulator.s32) +
                             percentage_delta;
    if (composed < std::numeric_limits<int32_t>::min() ||
        composed > std::numeric_limits<int32_t>::max())
    {
        return;
    }

    accumulator.s64 = static_cast<int32_t>(composed);
    TryAppendForestCombatLine(stack, text_buffer_offset, percentage_delta);
}

} // namespace

// The code generator emits the native attack call after this callback. The
// callback joins the initialized native percentage accumulator and text list.
void ReRevvedApplyUnitCombatAttackPercent(PPCRegister& stack,
                                          PPCRegister& accumulator)
{
    ApplyCombatRule(stack.u32,
                    kAttackerPlayerOffset,
                    kAttackerUnitOffset,
                    REREVVED_UNIT_COMBAT_ATTACK,
                    kAttackTextOffset,
                    accumulator);
}

// The code generator emits the native defense call after this callback. The
// callback joins the initialized native percentage accumulator and text list.
void ReRevvedApplyUnitCombatDefensePercent(PPCRegister& stack,
                                           PPCRegister& accumulator)
{
    ApplyCombatRule(stack.u32,
                    kDefenderPlayerOffset,
                    kDefenderUnitOffset,
                    REREVVED_UNIT_COMBAT_DEFENSE,
                    kDefenseTextOffset,
                    accumulator);
}
