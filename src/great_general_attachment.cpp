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

bool IsLive(const GreatGeneralUnitState& unit)
{
    return unit.slot != 0xFF && (unit.flags & 0x80000000) == 0;
}

} // namespace

bool TryPlanGreatGeneralCoordinateRepair(
    const GreatGeneralUnitState& carrier,
    const GreatGeneralUnitState& general,
    int16_t&                     repaired_x,
    int16_t&                     repaired_y)
{
    if (!IsLive(carrier) || !IsLive(general) ||
        general.type != kGreatGeneralType ||
        carrier.player != general.player ||
        general.carrier_link != carrier.unit ||
        (carrier.x == general.x && carrier.y == general.y))
    {
        return false;
    }

    repaired_x = carrier.x;
    repaired_y = carrier.y;
    return true;
}

} // namespace rerevved

namespace
{

constexpr uint32_t kUnitTable      = 0x830F2BF0;
constexpr uint32_t kUnitRecordSize = 0x54;
constexpr int32_t  kPlayerCount    = 6;
constexpr int32_t  kUnitsPerPlayer = 256;

bool IsGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

bool IsGuestReadableRange(uint32_t address, uint32_t extent)
{
    if (extent == 0 || address > UINT32_MAX - extent)
    {
        return false;
    }

    const uint32_t end = address + extent;
    if (!IsGuestPointer(address) || !IsGuestPointer(end - 1))
    {
        return false;
    }

    auto* memory = REX_KERNEL_MEMORY();
    auto* heap   = memory->LookupHeap(address);
    return heap && memory->LookupHeap(end - 1) == heap &&
           heap->QueryRangeAccess(address, end - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

uint16_t ReadBigEndianU16(const uint8_t* value)
{
    return (uint16_t{ value[0] } << 8) | uint16_t{ value[1] };
}

uint32_t ReadBigEndianU32(const uint8_t* value)
{
    return (uint32_t{ value[0] } << 24) | (uint32_t{ value[1] } << 16) |
           (uint32_t{ value[2] } << 8) | uint32_t{ value[3] };
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

bool CaptureUnit(int32_t                          player,
                 int32_t                          unit,
                 rerevved::GreatGeneralUnitState& state)
{
    uint32_t address = 0;
    if (!TryGetUnitAddress(player, unit, address) ||
        !IsGuestReadableRange(address, kUnitRecordSize))
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
        ReadBigEndianU32(record + 0x0C),
        std::bit_cast<int16_t>(ReadBigEndianU16(record + 0x1C)),
        std::bit_cast<int16_t>(ReadBigEndianU16(record + 0x1E)),
        std::bit_cast<int16_t>(ReadBigEndianU16(record + 0x50)),
    };
    return true;
}

void WriteCoordinates(int32_t player, int32_t unit, int16_t x, int16_t y)
{
    constexpr uint32_t kCoordinatesOffset = 0x1C;
    uint32_t           address            = 0;
    if (!TryGetUnitAddress(player, unit, address) ||
        !IsGuestReadableRange(address + kCoordinatesOffset,
                              sizeof(uint32_t)))
    {
        return;
    }

    const uint16_t x_bits      = std::bit_cast<uint16_t>(x);
    const uint16_t y_bits      = std::bit_cast<uint16_t>(y);
    auto*          memory      = REX_KERNEL_MEMORY();
    auto*          destination = memory->TranslateVirtual<uint8_t*>(
        address + kCoordinatesOffset);
    destination[0] = static_cast<uint8_t>(x_bits >> 8);
    destination[1] = static_cast<uint8_t>(x_bits);
    destination[2] = static_cast<uint8_t>(y_bits >> 8);
    destination[3] = static_cast<uint8_t>(y_bits);
}

void RepairPair(const rerevved::GreatGeneralUnitState& carrier,
                const rerevved::GreatGeneralUnitState& general)
{
    int16_t repaired_x = 0;
    int16_t repaired_y = 0;
    if (!rerevved::TryPlanGreatGeneralCoordinateRepair(
            carrier, general, repaired_x, repaired_y))
    {
        return;
    }

    WriteCoordinates(general.player, general.unit, repaired_x, repaired_y);
}

void RepairPairsForCarrier(int32_t player, int32_t carrier_unit)
{
    rerevved::GreatGeneralUnitState carrier{};
    if (!CaptureUnit(player, carrier_unit, carrier))
    {
        return;
    }

    for (int32_t general_unit = 0; general_unit < kUnitsPerPlayer;
         ++general_unit)
    {
        rerevved::GreatGeneralUnitState general{};
        if (CaptureUnit(player, general_unit, general))
        {
            RepairPair(carrier, general);
        }
    }
}

void RepairAllPairs()
{
    for (int32_t player = 0; player < kPlayerCount; ++player)
    {
        for (int32_t general_unit = 0; general_unit < kUnitsPerPlayer;
             ++general_unit)
        {
            rerevved::GreatGeneralUnitState general{};
            if (!CaptureUnit(player, general_unit, general))
            {
                continue;
            }

            rerevved::GreatGeneralUnitState carrier{};
            if (CaptureUnit(player, general.carrier_link, carrier))
            {
                RepairPair(carrier, general);
            }
        }
    }
}

} // namespace

void ReRevvedFixGreatGeneralBorderCompletion()
{
    RepairAllPairs();
}

void ReRevvedFixGreatGeneralPostCombat(PPCRegister& player,
                                       PPCRegister& unit)
{
    RepairPairsForCarrier(player.s32, unit.s32);
}
