#include "ai_economy_settings.h"

#include "ai_civilizations.h"
#include "gameplay_session_settings.h"

#include <bit>
#include <cstdint>

#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace
{

constexpr uint32_t kCities        = 0x8314FE28;
constexpr int32_t  kCityStride    = 0xBC;
constexpr int32_t  kCitySlots     = 128;
constexpr uint32_t kUnitTable     = 0x82F700D8;
constexpr uint32_t kUnitStride    = 0x94;
constexpr uint32_t kUnitCostField = 0x44;
constexpr uint32_t kNetwork       = 0x8314F10B;

uint8_t* guestBytes(uint32_t address, uint32_t size, rex::memory::PageAccess needed) noexcept
{
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory || !size || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
    {
        return nullptr;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap ||
        (static_cast<uint32_t>(heap->QueryRangeAccess(address, address + size - 1)) & static_cast<uint32_t>(needed)) != static_cast<uint32_t>(needed))
    {
        return nullptr;
    }
    return memory->TranslateVirtual<uint8_t*>(address);
}

const uint8_t* readable(uint32_t address, uint32_t size) noexcept
{
    return guestBytes(address, size, rex::memory::PageAccess::kReadOnly);
}

uint8_t* writable(uint32_t address, uint32_t size) noexcept
{
    return guestBytes(address, size, rex::memory::PageAccess::kReadWrite);
}

bool readWord(uint32_t address, uint32_t& value) noexcept
{
    const auto* bytes = readable(address, 4);
    if (!bytes)
    {
        return false;
    }
    value = uint32_t{ bytes[0] } << 24 | uint32_t{ bytes[1] } << 16 | uint32_t{ bytes[2] } << 8 | uint32_t{ bytes[3] };
    return true;
}

bool writeWord(uint32_t address, uint32_t value) noexcept
{
    auto* bytes = writable(address, 4);
    if (!bytes)
    {
        return false;
    }
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
    return true;
}

bool ordinaryAi(int32_t player) noexcept
{
    if (rerevved::ai_economy::NativeCostsRequired())
    {
        return false;
    }
    uint32_t count = 0, barbarian = 0, humans = 0;
    namespace ai = rerevved::ai_civilizations;
    return readWord(ai::kPlayerCountGlobal, count) &&
           readWord(ai::kBarbarianPlayerGlobal, barbarian) &&
           readWord(ai::kHumanMaskGlobal, humans) &&
           ai::IsOrdinaryAi(static_cast<int32_t>(count), static_cast<int32_t>(barbarian), humans, player);
}

bool cityOwner(int32_t offset, int32_t& owner) noexcept
{
    if (offset < 0 || offset % kCityStride != 0 || offset / kCityStride >= kCitySlots)
    {
        return false;
    }
    const auto* bytes = readable(kCities + static_cast<uint32_t>(offset), 1);
    if (!bytes)
    {
        return false;
    }
    owner = *bytes;
    return true;
}

// Scale a completed city contribution held in a register and in the stack word
// the native code saved it to. Both change together or neither does.
void scaleYield(PPCRegister& stack, uint32_t savedOffset, int32_t owner, PPCRegister& value, int32_t bonusPercent) noexcept
{
    const int32_t native = value.s32;
    if (bonusPercent == 0 || !ordinaryAi(owner))
    {
        return;
    }
    const int32_t scaled = rerevved::ai_economy::ScaleYield(native, bonusPercent);
    if (scaled == native || !writeWord(stack.u32 + savedOffset, static_cast<uint32_t>(scaled)))
    {
        return;
    }
    value.u64 = static_cast<uint32_t>(scaled);
}

} // namespace

bool rerevved::ai_economy::NativeCostsRequired() noexcept
{
    const auto* flag = readable(kNetwork, 1);
    return !flag || (*flag != 0 && !gameplay_session::ForNetworkGame());
}

void ApplyAiResearchCostPercent(PPCRegister& technology, PPCRegister& player, PPCRegister& cost)
{
    const auto policy = rerevved::ai_economy::ForGame();
    if (policy.researchBonusPercent == 0 || technology.s32 < 1 || technology.s32 > 47 || cost.s32 <= 0 || !ordinaryAi(player.s32))
    {
        return;
    }
    cost.s64 = rerevved::ai_economy::ScaleCost(cost.s32, policy.researchBonusPercent);
}

void ApplyAiUnitProductionCostPercent(PPCRegister& cityOffset, PPCRegister& unitType, PPCRegister& scalar)
{
    const auto policy = rerevved::ai_economy::ForGame();
    if (policy.productionBonusPercent == 0 || unitType.s32 < 0 || unitType.s32 > 28 || scalar.s32 <= 0)
    {
        return;
    }
    int32_t owner = -1;
    if (!cityOwner(cityOffset.s32, owner) || !ordinaryAi(owner))
    {
        return;
    }
    const auto* factor = readable(kUnitTable + static_cast<uint32_t>(unitType.s32) * kUnitStride + kUnitCostField, 1);
    if (!factor)
    {
        return;
    }
    const int32_t result = rerevved::ai_economy::ScaleUnitScalar(scalar.s32, std::bit_cast<int8_t>(*factor), policy.productionBonusPercent);
    if (result != scalar.s32)
    {
        scalar.s64 = result;
    }
}

void ApplyAiNonUnitProductionCostPercent(PPCRegister& cityOffset, PPCRegister& itemOffset, PPCRegister& cost)
{
    const auto policy    = rerevved::ai_economy::ForGame();
    const bool supported = (itemOffset.s32 >= 0 && itemOffset.s32 <= 22) || (itemOffset.s32 >= 100 && itemOffset.s32 <= 132);
    if (policy.productionBonusPercent == 0 || !supported || cost.s32 <= 0)
    {
        return;
    }
    int32_t owner = -1;
    if (!cityOwner(cityOffset.s32, owner) || !ordinaryAi(owner))
    {
        return;
    }
    cost.s64 = rerevved::ai_economy::ScaleCost(cost.s32, policy.productionBonusPercent);
}

// Evidence: https://github.com/ReRevvedRecomp/rerevved-research/blob/main/manifests/ai-options-boundaries.json
// Native stores r8 to the halfword city cache and keeps the saved word at r1+0x7C.
void ApplyAiGoldBonus(PPCRegister& stack, PPCRegister& owner, PPCRegister& value)
{
    scaleYield(stack, 0x7C, owner.s32, value, rerevved::ai_economy::ForGame().goldBonusPercent);
}

// The owner is the signed word at r1+0x84, not r4.
void ApplyAiFoodBonus(PPCRegister& stack, PPCRegister& value)
{
    const int32_t bonus = rerevved::ai_economy::ForGame().foodBonusPercent;
    uint32_t      owner = 0;
    if (bonus == 0 || !readWord(stack.u32 + 0x84, owner))
    {
        return;
    }
    scaleYield(stack, 0x90, static_cast<int32_t>(owner), value, bonus);
}

void ApplyAiCultureBonus(PPCRegister& stack, PPCRegister& owner, PPCRegister& value)
{
    scaleYield(stack, 0x78, owner.s32, value, rerevved::ai_economy::ForGame().cultureBonusPercent);
}
