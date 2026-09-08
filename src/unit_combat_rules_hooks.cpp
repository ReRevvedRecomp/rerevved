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

bool isGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

bool isGuestReadableRange(uint32_t address, uint32_t extent)
{
    if (address > UINT32_MAX - extent)
    {
        return false;
    }

    const uint32_t end = address + extent;
    if (!isGuestPointer(address) || !isGuestPointer(end - 1))
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

uint32_t readBigEndianU32(const uint8_t* value)
{
    return (uint32_t{ value[0] } << 24) | (uint32_t{ value[1] } << 16) |
           (uint32_t{ value[2] } << 8) | uint32_t{ value[3] };
}

uint16_t readBigEndianU16(const uint8_t* value)
{
    return (uint16_t{ value[0] } << 8) | uint16_t{ value[1] };
}

bool tryReadGuestU32(uint32_t address, uint32_t& value)
{
    if (!isGuestReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    const auto* source =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    value = readBigEndianU32(source);
    return true;
}

bool tryReadStackI32(uint32_t stack, uint32_t offset, int32_t& value)
{
    if (stack > UINT32_MAX - offset)
    {
        return false;
    }

    uint32_t bits = 0;
    if (!tryReadGuestU32(stack + offset, bits))
    {
        return false;
    }
    value = std::bit_cast<int32_t>(bits);
    return true;
}

bool tryReadCivilization(int32_t player, ReRevvedCivilizationId& civilization)
{
    if (player < 0 || player >= kPlayerCount)
    {
        return false;
    }

    uint32_t value = 0;
    if (!tryReadGuestU32(0x830ECD28u +
                             static_cast<uint32_t>(player) * sizeof(uint32_t),
                         value) ||
        value >= REREVVED_CIVILIZATION_COUNT)
    {
        return false;
    }
    civilization = static_cast<ReRevvedCivilizationId>(value);
    return true;
}

bool tryGetUnitAddress(int32_t player, int32_t unit, uint32_t& address)
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

bool tryReadUnitCoordinates(int32_t  player,
                            int32_t  unit,
                            int16_t& x,
                            int16_t& y)
{
    uint32_t address = 0;
    if (!tryGetUnitAddress(player, unit, address) ||
        !isGuestReadableRange(address + 0x1C, sizeof(uint32_t)))
    {
        return false;
    }

    const auto* record =
        REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
    x = std::bit_cast<int16_t>(readBigEndianU16(record + 0x1C));
    y = std::bit_cast<int16_t>(readBigEndianU16(record + 0x1E));
    return true;
}

bool tryReadUnitBaseType(int32_t             player,
                         int32_t             unit,
                         ReRevvedUnitTypeId& baseUnitType)
{
    uint32_t address = 0;
    if (!tryGetUnitAddress(player, unit, address) ||
        !isGuestReadableRange(address + 0x01, sizeof(uint8_t)))
    {
        return false;
    }

    const uint8_t value = *REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(
        address + 0x01);
    if (value >= REREVVED_UNIT_TYPE_COUNT)
    {
        return false;
    }
    baseUnitType = static_cast<ReRevvedUnitTypeId>(value);
    return true;
}

bool isForestDefenderTile(int32_t defenderPlayer, int32_t defenderUnit)
{
    int16_t x = 0;
    int16_t y = 0;
    if (!tryReadUnitCoordinates(defenderPlayer, defenderUnit, x, y) ||
        x < 0 || y < 0)
    {
        return false;
    }

    const uint32_t tileAddress =
        kTerrainTable + static_cast<uint32_t>(x) * 40u +
        static_cast<uint32_t>(y);
    if (!isGuestReadableRange(tileAddress, 1))
    {
        return false;
    }

    const uint8_t terrain = *REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(
        tileAddress);
    return terrain == 3; // Guest Forest.
}

bool tryResolveIdentity(uint32_t                stack,
                        uint32_t                playerOffset,
                        uint32_t                unitOffset,
                        ReRevvedCivilizationId& civilization,
                        ReRevvedUnitTypeId&     unitType,
                        ReRevvedUnitIdentityId& identity)
{
    int32_t            player       = 0;
    int32_t            unit         = 0;
    ReRevvedUnitTypeId baseUnitType = REREVVED_UNIT_TYPE_UNKNOWN;
    if (!tryReadStackI32(stack, playerOffset, player) ||
        !tryReadStackI32(stack, unitOffset, unit) ||
        !tryReadCivilization(player, civilization) ||
        !tryReadUnitBaseType(player, unit, baseUnitType) ||
        !rerevved::unit_catalog::TryResolveUnitIdentity(
            civilization, baseUnitType, identity) ||
        identity == REREVVED_UNIT_IDENTITY_BASE)
    {
        return false;
    }

    unitType = baseUnitType;
    return true;
}

constexpr uint32_t kCombatTextBufferSize = 256;

bool tryAppendForestCombatLine(uint32_t stack,
                               uint32_t bufferOffset,
                               int32_t  percentageDelta)
{
    if (stack > UINT32_MAX - bufferOffset)
    {
        return false;
    }

    const uint32_t bufferAddress = stack + bufferOffset;
    if (!isGuestReadableRange(bufferAddress, kCombatTextBufferSize))
    {
        return false;
    }

    auto*    memory = REX_KERNEL_MEMORY();
    auto*    buffer = memory->TranslateVirtual<uint8_t*>(bufferAddress);
    uint32_t end    = 0;
    while (end < kCombatTextBufferSize && buffer[end] != 0)
    {
        ++end;
    }
    if (end == kCombatTextBufferSize)
    {
        return false;
    }

    const char* prefix       = "Woodsman ";
    uint32_t    prefixLength = 0;
    while (prefix[prefixLength] != '\0')
    {
        ++prefixLength;
    }

    const int64_t magnitude  = percentageDelta < 0
                                   ? -static_cast<int64_t>(percentageDelta)
                                   : static_cast<int64_t>(percentageDelta);
    uint32_t      digitCount = 1;
    for (int64_t divisor = 10; magnitude >= divisor; divisor *= 10)
    {
        ++digitCount;
    }

    // Prefix, sign, digits, percent sign, newline, and terminal NUL.
    const uint32_t lineSize = prefixLength + 1 + digitCount + 3;
    if (end > kCombatTextBufferSize - lineSize)
    {
        return false;
    }

    auto*    target   = buffer + end;
    uint32_t position = 0;
    for (; position < prefixLength; ++position)
    {
        target[position] = static_cast<uint8_t>(prefix[position]);
    }

    target[position++] = percentageDelta < 0 ? '-' : '+';
    // Format the decimal digits directly into the guest buffer.
    int64_t remaining = magnitude;
    int64_t divisor   = 1;
    for (uint32_t index = 1; index < digitCount; ++index)
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

void applyCombatRule(uint32_t                   stack,
                     uint32_t                   identityPlayerOffset,
                     uint32_t                   identityUnitOffset,
                     ReRevvedUnitCombatProperty property,
                     uint32_t                   textBufferOffset,
                     PPCRegister&               accumulator)
{
    int32_t defenderPlayer = 0;
    int32_t defenderUnit   = 0;
    if (!tryReadStackI32(stack, kDefenderPlayerOffset, defenderPlayer) ||
        !tryReadStackI32(stack, kDefenderUnitOffset, defenderUnit) ||
        !isForestDefenderTile(defenderPlayer, defenderUnit))
    {
        return;
    }

    ReRevvedCivilizationId civilization = REREVVED_CIVILIZATION_UNKNOWN;
    ReRevvedUnitTypeId     unitType     = REREVVED_UNIT_TYPE_UNKNOWN;
    ReRevvedUnitIdentityId identity     = REREVVED_UNIT_IDENTITY_BASE;
    if (!tryResolveIdentity(stack,
                            identityPlayerOffset,
                            identityUnitOffset,
                            civilization,
                            unitType,
                            identity))
    {
        return;
    }

    ReRevvedUnitCombatEvaluation evaluation{};
    if (!rerevved::unit_combat_rules::TryEvaluate(civilization,
                                                  unitType,
                                                  identity,
                                                  REREVVED_TERRAIN_FOREST,
                                                  property,
                                                  evaluation) ||
        evaluation.additiveCount == 0 ||
        (evaluation.statusFlags &
         REREVVED_UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0)
    {
        return;
    }

    const int32_t percentageDelta = evaluation.finalPercent -
                                    evaluation.nativePercent;
    if (percentageDelta == 0)
    {
        return;
    }

    const int64_t composed = static_cast<int64_t>(accumulator.s32) +
                             percentageDelta;
    if (composed < std::numeric_limits<int32_t>::min() ||
        composed > std::numeric_limits<int32_t>::max())
    {
        return;
    }

    accumulator.s64 = static_cast<int32_t>(composed);
    tryAppendForestCombatLine(stack, textBufferOffset, percentageDelta);
}

} // namespace

// The code generator emits the native attack call after this callback. The
// callback joins the initialized native percentage accumulator and text list.
void ReRevvedApplyUnitCombatAttackPercent(PPCRegister& stack,
                                          PPCRegister& accumulator)
{
    applyCombatRule(stack.u32,
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
    applyCombatRule(stack.u32,
                    kDefenderPlayerOffset,
                    kDefenderUnitOffset,
                    REREVVED_UNIT_COMBAT_DEFENSE,
                    kDefenseTextOffset,
                    accumulator);
}
