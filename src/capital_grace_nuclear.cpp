#include "capital_grace_nuclear.h"

#include <bit>
#include <stdexcept>
#include <utility>

namespace rerevved::capital_grace
{
namespace
{

void require(bool value)
{
    if (!value)
    {
        throw std::runtime_error("capital grace: unresolved special harm binding");
    }
}

City city(const SpecialMemory& memory, int32_t ordinal)
{
    const auto address = CityAddress(ordinal);
    require(address.has_value());
    memory.Range(*address, 188);
    return { memory.Byte(*address), memory.Byte(*address + 3), memory.Word(*address + 16), { std::bit_cast<int16_t>(memory.Half(*address + 48)), std::bit_cast<int16_t>(memory.Half(*address + 50)) } };
}

bool population(const SpecialMemory& memory, int32_t index, int32_t barbarian)
{
    require(index >= -1 && index <= 1600);
    if (!(memory.Half(static_cast<uint32_t>(int64_t{ 0x8312A608 } + 2 * index)) & 1))
    {
        return false;
    }
    const auto target = city(memory, memory.Byte(static_cast<uint32_t>(int64_t{ 0x83128D08 } + index)));
    require(Player(target.owner) || (target.owner == -1 && target.population <= 0));
    return ProtectedCity(target, barbarian);
}

} // namespace

uint32_t SpecialMemory::Word(uint32_t address) const
{
    uint8_t bytes[4];
    require(read && read(context, address, bytes, 4));
    return (uint32_t{ bytes[0] } << 24) | (uint32_t{ bytes[1] } << 16) | (uint32_t{ bytes[2] } << 8) | bytes[3];
}

int32_t SpecialMemory::Signed(uint32_t address) const
{
    return std::bit_cast<int32_t>(Word(address));
}

uint16_t SpecialMemory::Half(uint32_t address) const
{
    uint8_t bytes[2];
    require(read && read(context, address, bytes, 2));
    return (uint16_t{ bytes[0] } << 8) | bytes[1];
}

int32_t SpecialMemory::Byte(uint32_t address) const
{
    uint8_t value;
    require(read && read(context, address, &value, 1));
    return std::bit_cast<int8_t>(value);
}

void SpecialMemory::Range(uint32_t address, uint32_t size) const
{
    // Bounded chunks avoid allocating a buffer from untrusted guest lengths.
    uint8_t bytes[64];
    require(size && uint64_t{ address } + size <= 0xFFFFF000ull);
    while (size)
    {
        const auto count = size < sizeof(bytes) ? size : static_cast<uint32_t>(sizeof(bytes));
        require(read && read(context, address, bytes, count));
        address += count;
        size -= count;
    }
}

std::optional<NuclearVisit> DecodeNuclearVisit(int32_t packed, int32_t width, int32_t height, int32_t dx, int32_t dy) noexcept
{
    const Point center{ packed % 40, packed / 40 };
    if (width < 1 || width > 40 || height < 1 || height > 40 || center.x < 0 || center.x >= width || center.y < 0 || center.y >= height || dx < -1 || dx > 1 || dy < -1 || dy > 1)
    {
        return std::nullopt;
    }
    const Point point{ (center.x + width + dx) % width, center.y + dy };
    return NuclearVisit{ point, 40 * point.x + point.y };
}

bool NuclearVictim(const SpecialMemory& memory, int32_t bank, int32_t slot, uint32_t flagsAddress, Point visit, int32_t barbarian)
{
    require(bank >= -1 && bank <= 7 && slot >= 0 && slot < 256);
    const auto address = static_cast<uint32_t>(int64_t{ kUnits } + 84 * (256 * bank + slot));
    require(flagsAddress == address + 12);
    memory.Range(address, 84);
    // Native scans bank -1 in alternate mode. The stored owner is independent.
    if (memory.Byte(address) == -1 || (memory.Word(address + 12) & 0x80000000u))
    {
        return false;
    }
    const Point position{ std::bit_cast<int16_t>(memory.Half(address + 28)), std::bit_cast<int16_t>(memory.Half(address + 30)) };
    require(position == visit);
    const auto width  = memory.Signed(0x82F77364);
    const auto height = memory.Signed(0x82F77448);
    require(Adjacent(visit, width, height, 0, 0).has_value());
    const auto ordinal = memory.Byte(0x83128D08 + 40 * visit.x + visit.y);
    if (ordinal == -1)
    {
        require(!(memory.Half(0x8312A608 + 2 * (40 * visit.x + visit.y)) & 1));
        return false;
    }
    const auto target = city(memory, ordinal);
    require(target.position == visit && (Player(target.owner) || (target.owner == -1 && target.population <= 0)));
    return ProtectedCity(target, barbarian, visit);
}

bool NuclearPopulation(const SpecialMemory& memory, uint32_t offset, uint32_t populationBase, Point visit, int32_t barbarian)
{
    require(populationBase == kCities + 3 && offset % 188 == 0 && offset / 188 <= 127);
    const int64_t index  = int64_t{ 40 } * visit.x + visit.y;
    const auto    width  = memory.Signed(0x82F77364);
    const auto    height = memory.Signed(0x82F77448);
    require(width >= 1 && width <= 40 && height >= 1 && height <= 40 && visit.x >= 0 && visit.x < width && visit.y >= -1 && visit.y <= height && index >= -1 && index <= 1600);
    // This store is already selected. Do not re-read the tile flag or city plane
    // to excuse the actual destination, including polar aliases.
    const auto target = city(memory, static_cast<int32_t>(offset / 188));
    require(Player(target.owner) || (target.owner == -1 && target.population <= 0));
    return ProtectedCity(target, barbarian);
}

bool NuclearAdmission(const SpecialMemory& memory, const SpecialInput& input)
{
    // Exact selected mutations, epilogues and projectile-consumption bridge.
    constexpr std::pair<uint32_t, uint32_t> profile[] = {
        { 0x82CF7498, 0x3A06A608 },
        { 0x82CF7938, 0x480034B9 },
        { 0x82CF76A4, 0x7D3F51AE },
        { 0x82CF77F0, 0x81410114 },
        { 0x82CF7BF0, 0x7CFF59AE },
        { 0x82CF7D08, 0x38A00000 },
        { 0x82CF78CC, 0x38210100 },
        { 0x82CF78D0, 0x4BB003D0 },
        { 0x82CF7D34, 0x3D40830F },
        { 0x82CF7D68, 0x38210100 },
        { 0x82CF7B7C, 0x38210100 },
        { 0x82CF75D0, 0x38210100 },
        { 0x82CDF1C8, 0x2F14FFFF },
        { 0x82CDF26C, 0x81790000 },
        { 0x82CDF1C4, 0x480231CD },
    };
    for (const auto [address, expected] : profile)
    {
        require(memory.Word(address) == expected);
    }
    const auto width     = memory.Signed(0x82F77364);
    const auto height    = memory.Signed(0x82F77448);
    const bool alternate = memory.Word(0x8314F1AC) != 0;
    const auto packed    = std::bit_cast<int32_t>(input.words[3]);
    for (int32_t direction = -1; direction < 8; ++direction)
    {
        const auto dx    = direction < 0 ? 0 : memory.Signed(0x82F7A0B8 + 4 * direction);
        const auto dy    = direction < 0 ? 0 : memory.Signed(0x82F7A168 + 4 * direction);
        const auto visit = DecodeNuclearVisit(packed, width, height, dx, dy);
        require(visit.has_value());
        if (population(memory, visit->index, input.barbarian))
        {
            return false;
        }
        const auto bank = memory.Byte(static_cast<uint32_t>(int64_t{ 0x830E6850 } + visit->index));
        if (!alternate && bank == -1)
        {
            continue;
        }
        require(bank >= -1 && bank <= 7);
        for (int32_t slot = 0; slot < 256; ++slot)
        {
            const auto address = static_cast<uint32_t>(int64_t{ kUnits } + 84 * (256 * bank + slot));
            memory.Range(address, 84);
            if (memory.Byte(address) == -1 || (memory.Word(address + 12) & 0x80000000u))
            {
                continue;
            }
            const Point position{ std::bit_cast<int16_t>(memory.Half(address + 28)), std::bit_cast<int16_t>(memory.Half(address + 30)) };
            if (position == visit->point && NuclearVictim(memory, bank, slot, address + 12, visit->point, input.barbarian))
            {
                return false;
            }
        }
    }
    return true;
}

void NuclearInvocations::BeginDispatch(uint32_t frame) noexcept
{
    for (auto& token : tokens_)
    {
        if (token.parent == frame)
        {
            token = {};
        }
    }
    for (auto& invocation : frames_)
    {
        if (invocation.parent == frame)
        {
            invocation = {};
        }
    }
}

bool NuclearInvocations::Begin(Invocation invocation) noexcept
{
    for (auto& existing : frames_)
    {
        if (existing.frame == invocation.frame)
        {
            existing = {};
        }
    }
    for (auto& existing : frames_)
    {
        if (!existing.frame)
        {
            existing = invocation;
            return true;
        }
    }
    return false;
}

NuclearInvocations::Invocation* NuclearInvocations::Find(uint32_t frame, bool alternate) noexcept
{
    for (auto& invocation : frames_)
    {
        if (invocation.frame == frame && invocation.alternate == alternate)
        {
            return &invocation;
        }
    }
    return nullptr;
}

void NuclearInvocations::End(uint32_t frame, bool consumption) noexcept
{
    for (auto& invocation : frames_)
    {
        if (invocation.frame != frame)
        {
            continue;
        }
        if (consumption && invocation.refused && !invocation.alternate && invocation.returnAddress == 0x82CDF1AC)
        {
            for (auto& token : tokens_)
            {
                if (!token.frame)
                {
                    token = invocation;
                    break;
                }
            }
        }
        invocation = {};
    }
}

bool NuclearInvocations::Consume(uint32_t frame, int32_t player, int32_t slot) noexcept
{
    bool matched = false;
    for (auto& token : tokens_)
    {
        if (token.parent != frame)
        {
            continue;
        }
        matched = token.refused && token.player == player && token.slot == slot && token.returnAddress == 0x82CDF1AC;
        token   = {};
    }
    return matched;
}

size_t NuclearInvocations::Occupied() const noexcept
{
    size_t count = 0;
    for (const auto& frame : frames_)
    {
        count += frame.frame != 0;
    }
    for (const auto& token : tokens_)
    {
        count += token.frame != 0;
    }
    return count;
}

SpecialDecision AdmitSpecial(const SpecialMemory& source, const SpecialInput& input) noexcept
{
    // CDF340 pumps messages before CDF36C cleanup and D7E910 reloads its
    // bindings. Admission cannot certify those later lifecycle collections.
    if (input.route == SpecialRoute::Type24)
    {
        return SpecialDecision::RefuseUnsupported;
    }
    try
    {
        return NuclearAdmission(source, input) ? SpecialDecision::Native : SpecialDecision::RefuseProtected;
    }
    catch (...)
    {
        return SpecialDecision::RefuseUnsupported;
    }
}

} // namespace rerevved::capital_grace
