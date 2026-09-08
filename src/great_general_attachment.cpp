#include "great_general_attachment.h"

#include <bit>
#include <cstdint>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace rerevved
{

namespace
{

constexpr uint8_t kGreatGeneralType = 30;

bool isLive(const GreatGeneralUnitState& unit)
{
    return unit.slot != 0xFF && (unit.flags & 0x80000000) == 0;
}

} // namespace

bool TryPlanGreatGeneralCoordinateRepair(
    const GreatGeneralUnitState& carrier,
    const GreatGeneralUnitState& general,
    int16_t&                     repairedX,
    int16_t&                     repairedY)
{
    if (!isLive(carrier) || !isLive(general) ||
        general.type != kGreatGeneralType ||
        carrier.player != general.player ||
        general.carrierLink != carrier.unit ||
        (carrier.x == general.x && carrier.y == general.y))
    {
        return false;
    }

    repairedX = carrier.x;
    repairedY = carrier.y;
    return true;
}

} // namespace rerevved

namespace
{

constexpr uint32_t kUnitTable      = 0x830F2BF0;
constexpr uint32_t kUnitRecordSize = 0x54;
constexpr int32_t  kPlayerCount    = 6;
constexpr int32_t  kUnitsPerPlayer = 256;

bool isGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

bool isGuestReadableRange(uint32_t address, uint32_t extent)
{
    if (extent == 0 || address > UINT32_MAX - extent)
    {
        return false;
    }

    const uint32_t end = address + extent;
    if (!isGuestPointer(address) || !isGuestPointer(end - 1))
    {
        return false;
    }

    auto* memory = REX_KERNEL_MEMORY();
    auto* heap   = memory->LookupHeap(address);
    return heap && memory->LookupHeap(end - 1) == heap &&
           heap->QueryRangeAccess(address, end - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

uint16_t readBigEndianU16(const uint8_t* value)
{
    return (uint16_t{ value[0] } << 8) | uint16_t{ value[1] };
}

uint32_t readBigEndianU32(const uint8_t* value)
{
    return (uint32_t{ value[0] } << 24) | (uint32_t{ value[1] } << 16) |
           (uint32_t{ value[2] } << 8) | uint32_t{ value[3] };
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

bool captureUnit(int32_t                          player,
                 int32_t                          unit,
                 rerevved::GreatGeneralUnitState& state)
{
    uint32_t address = 0;
    if (!tryGetUnitAddress(player, unit, address) ||
        !isGuestReadableRange(address, kUnitRecordSize))
    {
        return false;
    }

    const auto* memory = REX_KERNEL_MEMORY();
    const auto* record = memory->TranslateVirtual<const uint8_t*>(address);
    state              = {
        player,
        unit,
        record[0x00],
        record[0x01],
        readBigEndianU32(record + 0x0C),
        std::bit_cast<int16_t>(readBigEndianU16(record + 0x1C)),
        std::bit_cast<int16_t>(readBigEndianU16(record + 0x1E)),
        std::bit_cast<int16_t>(readBigEndianU16(record + 0x50)),
    };
    return true;
}

void writeCoordinates(int32_t player, int32_t unit, int16_t x, int16_t y)
{
    constexpr uint32_t kCoordinatesOffset = 0x1C;
    uint32_t           address            = 0;
    if (!tryGetUnitAddress(player, unit, address) ||
        !isGuestReadableRange(address + kCoordinatesOffset,
                              sizeof(uint32_t)))
    {
        return;
    }

    const uint16_t xBits       = std::bit_cast<uint16_t>(x);
    const uint16_t yBits       = std::bit_cast<uint16_t>(y);
    auto*          memory      = REX_KERNEL_MEMORY();
    auto*          destination = memory->TranslateVirtual<uint8_t*>(
        address + kCoordinatesOffset);
    destination[0] = static_cast<uint8_t>(xBits >> 8);
    destination[1] = static_cast<uint8_t>(xBits);
    destination[2] = static_cast<uint8_t>(yBits >> 8);
    destination[3] = static_cast<uint8_t>(yBits);
}

void repairPair(const rerevved::GreatGeneralUnitState& carrier,
                const rerevved::GreatGeneralUnitState& general)
{
    int16_t repairedX = 0;
    int16_t repairedY = 0;
    if (!rerevved::TryPlanGreatGeneralCoordinateRepair(
            carrier, general, repairedX, repairedY))
    {
        return;
    }

    writeCoordinates(general.player, general.unit, repairedX, repairedY);
}

void repairPairsForCarrier(int32_t player, int32_t carrierUnit)
{
    rerevved::GreatGeneralUnitState carrier{};
    if (!captureUnit(player, carrierUnit, carrier))
    {
        return;
    }

    for (int32_t generalUnit = 0; generalUnit < kUnitsPerPlayer;
         ++generalUnit)
    {
        rerevved::GreatGeneralUnitState general{};
        if (captureUnit(player, generalUnit, general))
        {
            repairPair(carrier, general);
        }
    }
}

void repairAllPairs()
{
    for (int32_t player = 0; player < kPlayerCount; ++player)
    {
        for (int32_t generalUnit = 0; generalUnit < kUnitsPerPlayer;
             ++generalUnit)
        {
            rerevved::GreatGeneralUnitState general{};
            if (!captureUnit(player, generalUnit, general))
            {
                continue;
            }

            rerevved::GreatGeneralUnitState carrier{};
            if (captureUnit(player, general.carrierLink, carrier))
            {
                repairPair(carrier, general);
            }
        }
    }
}

} // namespace

void ReRevvedFixGreatGeneralBorderCompletion()
{
    repairAllPairs();
}

void ReRevvedFixGreatGeneralPostCombat(PPCRegister& player,
                                       PPCRegister& unit)
{
    repairPairsForCarrier(player.s32, unit.s32);
}
