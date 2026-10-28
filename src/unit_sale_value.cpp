#include "unit_sale_value.h"

#include <cstddef>

namespace rerevved::unit_sale
{
namespace
{

uint32_t word(const uint8_t* bytes)
{
    return uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | bytes[3];
}

int ownMultiplier(const Record& record)
{
    return record.marker ? 3 : 1;
}

Decision capture(Memory& memory, const Seam& seam, Snapshot& out)
{
    if (seam.unitBase != kUnitBase || seam.definitionBase != kDefinitionBase || seam.deadMask != 0x80000000 ||
        seam.globalBase != kGlobalBase || seam.frame < 0x10000 || (seam.frame & 15) || seam.frame > 0xFFFFF000 - 0x94 ||
        seam.originalPlayer < 0 || seam.originalPlayer >= 8)
    {
        return Decision::Identity;
    }
    std::array<uint8_t, 8> keys{};
    std::array<uint8_t, 4> saved{};
    if (!memory.Read(0x8312B8E8, keys.data(), keys.size()) || !memory.Read(seam.frame + 0x90, saved.data(), saved.size()))
    {
        return Decision::Unreadable;
    }
    out.player = word(keys.data());
    out.slot   = word(keys.data() + 4);
    out.frame  = seam.frame;
    if (out.player >= 8 || out.slot >= 256 || out.player != seam.originalPlayer || out.slot != word(saved.data()))
    {
        return Decision::Identity;
    }
    std::array<uint8_t, 84 * 256> units{};
    std::array<uint8_t, 148 * 38> definitions{};
    if (!memory.Read(kUnitBase + 84 * 256 * out.player, units.data(), units.size()) ||
        !memory.Read(kDefinitionBase, definitions.data(), definitions.size()))
    {
        return Decision::Unreadable;
    }
    for (size_t i = 0; i < out.records.size(); ++i)
    {
        const auto* bytes = units.data() + 84 * i;
        out.records[i]    = { bytes[0], static_cast<int8_t>(bytes[1]), bytes[4], word(bytes + 0xC), static_cast<int16_t>(uint16_t{ bytes[0x26] } << 8 | bytes[0x27]) };
    }
    for (size_t i = 0; i < out.definitions.size(); ++i)
    {
        const auto* bytes  = definitions.data() + 148 * i;
        out.definitions[i] = { static_cast<int8_t>(bytes[0x44]), word(bytes + 0x50) };
    }
    const auto& carrier = out.records[out.slot];
    if (carrier.owner == 255 || (carrier.flags & 0x80000000) || carrier.type < 0 || carrier.type >= 38)
    {
        return Decision::Unsupported;
    }
    const auto& definition = out.definitions[carrier.type];
    out.multiplier         = ownMultiplier(carrier);
    out.sum                = carrier.type == 7 ? 0 : definition.factor;
    out.weighted           = out.sum * out.multiplier;
    if (definition.flags & 0x10)
    {
        for (const auto& record : out.records)
        {
            // Native collection 0x82D2B3D0/B51C tests these fields, not
            // owner equality, rank, location or the General attachment link.
            if (record.owner == 255 || (record.flags & 0x80000000) || record.link != static_cast<int32_t>(out.slot) || record.type == 7)
            {
                continue;
            }
            if (record.type < 0 || record.type >= 38 || (out.definitions[record.type].flags & 0x10))
            {
                return Decision::Unsupported;
            }
            const int factor = out.definitions[record.type].factor;
            out.sum += factor;
            out.weighted += factor * ownMultiplier(record);
        }
    }
    return Decision::Captured;
}

bool previewIdentity(const Seam& seam, const Snapshot& snapshot, bool beginning)
{
    const auto type = snapshot.records[snapshot.slot].type;
    return seam.slot == snapshot.slot && seam.type == type &&
           (!beginning || (seam.offset == 84 * ((snapshot.player << 8) + snapshot.slot) &&
                           seam.definitionOffset == 148u * type && seam.definitionFlags == snapshot.definitions[type].flags));
}

uint64_t nativeAmount(int32_t product)
{
    // 0x82D2B450..B458 and B5D0..B5D8: signed word multiply,
    // masked word shift, then full-register add. Products are bounded here.
    return static_cast<uint64_t>(static_cast<int64_t>(product)) + static_cast<uint32_t>(static_cast<uint32_t>(product) << 2);
}

} // namespace

Decision Collection::Begin(Memory& memory, Phase nextPhase, const Seam& seam) noexcept
{
    if (busy || active)
    {
        poisoned = true;
        return Decision::Reentrant;
    }
    busy            = true;
    active          = true;
    poisoned        = false;
    phase           = nextPhase;
    Decision result = Decision::Exception;
    try
    {
        Snapshot next{};
        result = capture(memory, seam, next);
        if (result == Decision::Captured &&
            (nextPhase == Phase::Preview ? !previewIdentity(seam, next, true) : seam.slot != next.slot))
        {
            result = Decision::Identity;
        }
        if (poisoned)
        {
            result = Decision::Reentrant;
        }
        if (result == Decision::Captured)
        {
            snapshot = next;
        }
    }
    catch (...)
    {
        result = Decision::Exception;
    }
    active = result == Decision::Captured;
    busy   = false;
    return result;
}

Decision Collection::Apply(Memory& memory, Phase wantedPhase, const Seam& seam, uint64_t& amount) noexcept
{
    if (busy)
    {
        poisoned = true;
        return Decision::Reentrant;
    }
    if (!active)
    {
        return Decision::Missing;
    }
    if (poisoned || phase != wantedPhase || snapshot.frame != seam.frame)
    {
        active = false;
        return poisoned ? Decision::Reentrant : Decision::Identity;
    }
    busy            = true;
    Decision result = Decision::Exception;
    try
    {
        Snapshot current{};
        result = capture(memory, seam, current);
        if (result == Decision::Captured && current != snapshot)
        {
            result = Decision::Changed;
        }
        if (result == Decision::Captured && wantedPhase == Phase::Preview && !previewIdentity(seam, current, false))
        {
            result = Decision::Identity;
        }
        const int64_t h = wantedPhase == Phase::Preview ? 1 : seam.h;
        if (result == Decision::Captured && (seam.nativeSum != snapshot.sum || (h != 1 && h != 2) ||
                                             (wantedPhase == Phase::Preview && seam.multiplier != snapshot.multiplier) ||
                                             amount != nativeAmount(static_cast<int32_t>(h * snapshot.multiplier * snapshot.sum))))
        {
            result = Decision::NativeValue;
        }
        if (poisoned)
        {
            result = Decision::Reentrant;
        }
        if (result == Decision::Captured)
        {
            amount = static_cast<uint32_t>(5 * h * snapshot.weighted);
            result = Decision::Applied;
        }
    }
    catch (...)
    {
        result = Decision::Exception;
    }
    active = false;
    busy   = false;
    return result;
}

} // namespace rerevved::unit_sale
