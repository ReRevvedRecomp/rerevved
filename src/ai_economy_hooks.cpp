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

const uint8_t* readable(uint32_t address, uint32_t size) noexcept
{
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory || !size || address < 0x10000 || uint64_t{ address } + size > 0xFFFFF000ull)
    {
        return nullptr;
    }
    auto* heap = memory->LookupHeap(address);
    if (!heap || memory->LookupHeap(address + size - 1) != heap ||
        (static_cast<uint32_t>(heap->QueryRangeAccess(address, address + size - 1)) & static_cast<uint32_t>(rex::memory::PageAccess::kReadOnly)) == 0)
    {
        return nullptr;
    }
    return memory->TranslateVirtual<const uint8_t*>(address);
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

} // namespace

bool rerevved::ai_economy::NativeCostsRequired() noexcept
{
    const auto* flag = readable(kNetwork, 1);
    return !flag || (*flag != 0 && !gameplay_session::ForNetworkGame());
}

void ApplyAiResearchCostPercent(PPCRegister& technology, PPCRegister& player, PPCRegister& cost)
{
    const auto policy = rerevved::ai_economy::ForGame();
    if (policy.researchCostPercent == 100 || technology.s32 < 1 || technology.s32 > 47 || cost.s32 <= 0 || !ordinaryAi(player.s32))
    {
        return;
    }
    cost.s64 = rerevved::ai_economy::ScaleCost(cost.s32, policy.researchCostPercent);
}

void ApplyAiUnitProductionCostPercent(PPCRegister& cityOffset, PPCRegister& unitType, PPCRegister& scalar)
{
    const auto policy = rerevved::ai_economy::ForGame();
    if (policy.productionCostPercent == 100 || unitType.s32 < 0 || unitType.s32 > 28 || scalar.s32 <= 0)
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
    const int32_t result = rerevved::ai_economy::ScaleUnitScalar(scalar.s32, std::bit_cast<int8_t>(*factor), policy.productionCostPercent);
    if (result != scalar.s32)
    {
        scalar.s64 = result;
    }
}

void ApplyAiNonUnitProductionCostPercent(PPCRegister& cityOffset, PPCRegister& itemOffset, PPCRegister& cost)
{
    const auto policy    = rerevved::ai_economy::ForGame();
    const bool supported = (itemOffset.s32 >= 0 && itemOffset.s32 <= 22) || (itemOffset.s32 >= 100 && itemOffset.s32 <= 132);
    if (policy.productionCostPercent == 100 || !supported || cost.s32 <= 0)
    {
        return;
    }
    int32_t owner = -1;
    if (!cityOwner(cityOffset.s32, owner) || !ordinaryAi(owner))
    {
        return;
    }
    cost.s64 = rerevved::ai_economy::ScaleCost(cost.s32, policy.productionCostPercent);
}
