#pragma once

// Guest layout and selection for the ai_civilizations hooks in
// ai_civilizations_hooks.cpp, kept free of guest memory so it can be tested
// on the host.

#include <bit>
#include <cstdint>

namespace rerevved::ai_civilizations
{

// Return address of the statistics recount call (bl 0x82D11AD8 at 0x82D2530C)
// in new-game init sub_82D25170, reached only after setup succeeded.
constexpr uint32_t kNewGameRecountReturn = 0x82D25310;

// Return address of the leader message call (bl 0x82CC85D0 at 0x82D0C3A0) in
// the elimination sub_82D0C1E0, which passes the eliminated player in r3.
constexpr uint32_t kFarewellReturn = 0x82D0C3A4;

// Setup stores the slot count and the barbarian player at 0x82D22918 and
// 0x82D2291C; bit p of the human mask marks a human player.
constexpr uint32_t kPlayerCountGlobal     = 0x82F700AC;
constexpr uint32_t kBarbarianPlayerGlobal = 0x82F700B0;
constexpr uint32_t kHumanMaskGlobal       = 0x8312E608;
constexpr uint32_t kNetworkGameByte       = 0x8314F10B;
// Session object whose local player sub_82D8A3A0 returns.
constexpr uint32_t kSessionGlobal = 0x8314EFE0;

constexpr int32_t kPlayerCount     = 6;
constexpr int32_t kBarbarianPlayer = 5;
// Civilization slots 0..4; the barbarian slot 5 is never removed.
constexpr uint32_t kCivilizationSlots = 0x1F;

// Unit records as sub_82D0C1E0 walks them: 256 records of 0x54 bytes per
// player. Byte +0 is the owner (0xFF when free), byte +1 the unit type and bit
// 31 of word +0x0C marks a killed unit.
constexpr uint32_t kUnitTable        = 0x830F2BF0;
constexpr uint32_t kUnitsPerPlayer   = 256;
constexpr uint32_t kUnitRecordSize   = 0x54;
constexpr uint32_t kPlayerUnitStride = kUnitsPerPlayer * kUnitRecordSize;
constexpr uint32_t kUnitTypeOffset   = 0x01;
constexpr uint32_t kUnitFlagsOffset  = 0x0C;
constexpr uint32_t kUnitKilledFlag   = 0x80000000u;

inline uint32_t UnitRecord(uint32_t player, uint32_t unit)
{
    return kUnitTable + player * kPlayerUnitStride + unit * kUnitRecordSize;
}

// The live test of the kill loop in sub_82D0C1E0 (0x82D0C254..0x82D0C268).
inline bool IsLiveUnit(uint8_t owner, uint32_t flags)
{
    return owner != 0xFF && (flags & kUnitKilledFlag) == 0;
}

// An ai_civilizations value of this or more keeps every AI civilization.
constexpr int32_t kAllAiCivilizations = 4;

// For the ordinary six-slot layout with a human, keep the lowest maxAi AI slots
// so peers agree; one human retains at least one AI. Other layouts return no
// removals.
inline uint32_t RemovableMask(int32_t count, int32_t barbarian, uint32_t humanMask, int32_t maxAi)
{
    const uint32_t humans = humanMask & kCivilizationSlots;
    if (count != kPlayerCount || barbarian != kBarbarianPlayer || humans == 0)
    {
        return 0;
    }
    int32_t keep = maxAi < 0 ? 0 : maxAi;
    if (std::popcount(humans) == 1 && keep < 1)
    {
        keep = 1;
    }
    uint32_t removable = 0;
    for (int32_t slot = 0; slot < kBarbarianPlayer; ++slot)
    {
        if ((humans >> slot & 1) != 0)
        {
            continue;
        }
        if (keep > 0)
        {
            --keep;
            continue;
        }
        removable |= 1u << slot;
    }
    return removable;
}

// FNV-1a over the killed (player, unit, type) list, logged so two peers can
// compare what each removed.
class KillDigest
{
public:
    void Add(uint32_t player, uint32_t unit, uint8_t type)
    {
        Mix(static_cast<uint8_t>(player));
        Mix(static_cast<uint8_t>(unit));
        Mix(type);
        ++count_;
    }

    uint32_t Count() const
    {
        return count_;
    }

    uint32_t Value() const
    {
        return value_;
    }

private:
    void Mix(uint8_t byte)
    {
        value_ = (value_ ^ byte) * 16777619u;
    }

    uint32_t count_ = 0;
    uint32_t value_ = 2166136261u;
};

// Whether the option removed player in the current game. Defined in
// ai_civilizations_hooks.cpp; the leader message hooks hide its farewell.
bool IsRemovedCivilization(uint32_t player);

} // namespace rerevved::ai_civilizations
