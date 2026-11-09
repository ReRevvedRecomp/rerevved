#include "resync_record.h"

#include <algorithm>
#include <bit>
#include <vector>

namespace rerevved::resync_record
{
namespace
{

constexpr uint32_t kPlayerCount     = 0x82F700AC;
constexpr uint32_t kUnits           = 0x830F2BF0;
constexpr uint32_t kUnitStride      = 0x54;
constexpr uint32_t kUnitsPerPlayer  = 256;
constexpr uint32_t kReserve         = 0x830ED544;
constexpr uint32_t kCultureStrength = 0x830ED4E4;
constexpr uint32_t kDerivedCulture  = 0x830ED4C4;
constexpr uint32_t kTally           = 0x830EDD08;
constexpr uint32_t kStrategy        = 0x830E5250;
constexpr uint32_t kStrategyStride  = 0x80;
constexpr uint32_t kSelectedTech    = 0x830EDD48;
constexpr uint32_t kProgress        = 0x830EDD68;
constexpr uint32_t kProgressStride  = 48;
constexpr uint32_t kCities          = 0x8314FE28;
constexpr uint32_t kCityStride      = 0xBC;
constexpr uint32_t kCityCount       = kMaxCities;
constexpr uint32_t kWonders         = 0x830E5148;
constexpr uint32_t kWonderSlots     = 33;

uint32_t be32(const uint8_t* bytes) noexcept
{
    return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | bytes[3];
}

// Sign-extended, as the low word of the native 64-bit adds.
uint32_t s8(const uint8_t* bytes) noexcept
{
    return static_cast<uint32_t>(static_cast<int32_t>(std::bit_cast<int8_t>(bytes[0])));
}

uint32_t s16(const uint8_t* bytes) noexcept
{
    const auto half = static_cast<uint16_t>((uint16_t{ bytes[0] } << 8) | bytes[1]);
    return static_cast<uint32_t>(static_cast<int32_t>(std::bit_cast<int16_t>(half)));
}

bool readWord(const GuestReader& memory, uint32_t address, uint32_t& value)
{
    uint8_t bytes[4];
    if (!memory.Read(address, 4, bytes))
    {
        return false;
    }
    value = be32(bytes);
    return true;
}

} // namespace

std::optional<Ledger> ComputeLedger(const GuestReader& memory)
{
    uint32_t countWord = 0;
    if (!readWord(memory, kPlayerCount, countWord))
    {
        return std::nullopt;
    }
    const auto count = static_cast<int32_t>(countWord);
    if (count > static_cast<int32_t>(kMaxPlayers))
    {
        return std::nullopt;
    }
    Ledger ledger;
    ledger.playerCount = count > 0 ? static_cast<uint32_t>(count) : 0;

    std::vector<uint8_t> block(size_t{ kUnitStride } * kUnitsPerPlayer);
    for (uint32_t player = 0; player < ledger.playerCount; ++player)
    {
        auto& fields = ledger.players[player];
        if (!memory.Read(kUnits + player * kUnitStride * kUnitsPerPlayer, static_cast<uint32_t>(block.size()), block.data()))
        {
            return std::nullopt;
        }
        for (uint32_t index = 0; index < kUnitsPerPlayer; ++index)
        {
            const uint8_t* unit = block.data() + size_t{ index } * kUnitStride;
            if (unit[0] == 0xFF || (be32(unit + 0xC) & 0x80000000u) != 0)
            {
                continue;
            }
            fields.units += s8(unit + 1) * index + (s16(unit + 0x1E) << 1) + s16(unit + 0x52) + s16(unit + 0x1C) + s8(unit + 5);
        }

        uint32_t tech = 0;
        if (!readWord(memory, kReserve + 4 * player, fields.reserve) ||
            !readWord(memory, kCultureStrength + 4 * player, fields.cultureStrength) ||
            !readWord(memory, kDerivedCulture + 4 * player, fields.derivedCulture) ||
            !readWord(memory, kTally + 4 * player, fields.tally) ||
            !readWord(memory, kStrategy + kStrategyStride * player, fields.strategy) ||
            !readWord(memory, kSelectedTech + 4 * player, tech))
        {
            return std::nullopt;
        }
        if (static_cast<int32_t>(tech) != -1 &&
            !readWord(memory, kProgress + ((tech + kProgressStride * player) << 2), fields.progress))
        {
            return std::nullopt;
        }
        ledger.sum += fields.units + fields.reserve + fields.cultureStrength + fields.derivedCulture + fields.tally + fields.strategy + fields.progress;
    }

    std::vector<uint8_t> cities(size_t{ kCityStride } * kCityCount);
    if (!memory.Read(kCities, static_cast<uint32_t>(cities.size()), cities.data()))
    {
        return std::nullopt;
    }
    for (uint32_t index = 0; index < kCityCount; ++index)
    {
        const uint8_t* city       = cities.data() + size_t{ index } * kCityStride;
        const uint32_t owner      = s8(city);
        const uint32_t population = s8(city + 3);
        if (owner == 0xFFFFFFFFu || static_cast<int32_t>(population) <= 0)
        {
            continue;
        }
        const uint32_t contribution = (s16(city + 0x38) << 1) + s16(city + 0x34) + s16(city + 0x36) + be32(city + 0x10) + s16(city + 0x3C) + population + owner;
        ledger.cities += contribution;
        ledger.cityFields[ledger.cityCount++] = { index,
                                                  static_cast<int32_t>(owner),
                                                  static_cast<int32_t>(population),
                                                  be32(city + 0x10),
                                                  static_cast<int32_t>(s16(city + 0x34)),
                                                  static_cast<int32_t>(s16(city + 0x36)),
                                                  static_cast<int32_t>(s16(city + 0x38)),
                                                  static_cast<int32_t>(s16(city + 0x3C)) };
        // The owner byte is signed; negative owners compare high and fall outside the range.
        if (owner < ledger.playerCount)
        {
            ledger.players[owner].cities += contribution;
        }
    }

    // The scalar adds the word at +4 of each 8-byte slot.
    std::vector<uint8_t> wonders(size_t{ kWonderSlots } * 8);
    if (!memory.Read(kWonders, static_cast<uint32_t>(wonders.size()), wonders.data()))
    {
        return std::nullopt;
    }
    for (uint32_t slot = 0; slot < kWonderSlots; ++slot)
    {
        ledger.wonders += be32(wonders.data() + size_t{ slot } * 8 + 4);
    }

    ledger.sum += ledger.cities + ledger.wonders;
    return ledger;
}

uint32_t OverridesDigest(const resource_identity_overrides::Snapshot& snapshot)
{
    uint32_t hash = 2166136261u;
    for (const uint8_t byte : resource_identity_overrides::Serialize(snapshot))
    {
        hash = (hash ^ byte) * 16777619u;
    }
    return hash;
}

bool LedgerDedupe::Admit(uint32_t turn, uint32_t caller, uint32_t scalar)
{
    std::lock_guard lock(mutex_);
    if (!started_ || turn != turn_)
    {
        started_ = true;
        turn_    = turn;
        count_   = 0;
    }
    const auto end = entries_.begin() + static_cast<std::ptrdiff_t>(count_);
    if (std::any_of(entries_.begin(), end, [&](const Entry& entry)
                    {
                        return entry.caller == caller && entry.scalar == scalar;
                    }) ||
        count_ == kCapacity)
    {
        return false;
    }
    entries_[count_++] = { caller, scalar };
    return true;
}

} // namespace rerevved::resync_record
