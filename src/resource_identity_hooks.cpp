#include "resource_identity_overrides.h"
#include "resource_identity_trace.h"
#include "start_resources_runtime.h"

#include <algorithm>
#include <bit>
#include <cstdint>

#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REX_EXTERN(__imp__sub_82CF1558);

REX_HOOK_RAW(sub_82CF1558)
{
    rerevved::resource_identity_trace::ReaderScope trace(ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64);
    __imp__sub_82CF1558(ctx, base);
    trace.Returned(ctx.r3.s32);
}

REX_EXTERN(__imp__sub_82CF1CE8);

REX_HOOK_RAW(sub_82CF1CE8)
{
    namespace traceApi = rerevved::resource_identity_trace;
    traceApi::YieldScope trace(traceApi::YieldOwner::Food, ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64);
    __imp__sub_82CF1CE8(ctx, base);
    trace.Returned(ctx.r3.s32);
}

REX_EXTERN(__imp__sub_82CF1AF0);

REX_HOOK_RAW(sub_82CF1AF0)
{
    namespace traceApi = rerevved::resource_identity_trace;
    traceApi::YieldScope trace(traceApi::YieldOwner::Production, ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64);
    __imp__sub_82CF1AF0(ctx, base);
    trace.Returned(ctx.r3.s32);
}

REX_EXTERN(__imp__sub_82CF17C8);

REX_HOOK_RAW(sub_82CF17C8)
{
    namespace traceApi = rerevved::resource_identity_trace;
    traceApi::YieldScope trace(traceApi::YieldOwner::Trade, ctx.lr, ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64);
    __imp__sub_82CF17C8(ctx, base);
    trace.Returned(ctx.r3.s32);
}

REX_EXTERN(__imp__sub_82D13978);

REX_HOOK_RAW(sub_82D13978)
{
    rerevved::resource_identity_trace::CityScope trace(ctx.lr, ctx.r3.u64, ctx.r4.u64);
    __imp__sub_82D13978(ctx, base);
}

void TraceTerrainInput(PPCRegister& terrain, PPCRegister& specialFlag, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1D78, "terrain_input_and_special_flag", 0, terrain.u64, specialFlag.u64, player.u64);
}

void TraceTerrainNative(PPCRegister& terrain, PPCRegister& nativeBase, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1D98, "terrain_native_base", static_cast<uint64_t>(nativeBase.s8), terrain.u64, nativeBase.u64, player.u64);
}

void TraceTerrainEffective(PPCRegister& total, PPCRegister& terrain, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1DA0, "terrain_effective_base", total.u64, terrain.u64, 0, player.u64);
}

void TracePlainsBonus(PPCRegister& total, PPCRegister& flags, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1DC0, "plains_city_flags_add_2", total.u64, flags.u64, 2, player.u64);
}

void TraceSeaBonus(PPCRegister& total, PPCRegister& flags, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1DE8, "sea_city_flags_add_1", total.u64, flags.u64, 1, player.u64);
}

void TraceTerrainBonuses(PPCRegister& total, PPCRegister& terrain, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1DEC, "after_terrain_bonuses", total.u64, terrain.u64, 0, player.u64);
}

void TraceTerrainAbility(PPCRegister& total, PPCRegister& ability, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1E28, "terrain_ability_argument", total.u64, ability.u64, 0, player.u64);
}

void TraceTerrainAbilityResult(PPCRegister& total, PPCRegister& result, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1E2C, "terrain_ability_result_add_1_if_nonzero", total.u64, result.u64, 1, player.u64);
}

void TraceAbilityTotal(PPCRegister& total, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1E38, "after_terrain_ability", total.u64, 0, 0, player.u64);
}

void TraceTileBits(PPCRegister& total, PPCRegister& count, PPCRegister& bits, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1EB0, "tile_bit_count_and_mask", total.u64, count.u64, bits.u64, player.u64);
}

void TraceTileBonusAbility(PPCRegister& total, PPCRegister& result, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1EC8, "tile_bonus_ability_17_result", total.u64, result.u64, 17, player.u64);
}

void TraceTileBonusPlayerMask(PPCRegister& total, PPCRegister& gate, PPCRegister& mask, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1EF4, "tile_bonus_player_gate_and_mask", total.u64, gate.u64, mask.u64, player.u64);
}

void TraceTileBonus(PPCRegister& total, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1EFC, "tile_bonus_add_1", total.u64, 1, 0, player.u64);
}

void TraceFoodIdentity(PPCRegister& total, PPCRegister& ordinal, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1F10, "identity_result", total.u64, ordinal.u64, 0, player.u64);
}

void TraceResourceClass(PPCRegister& total, PPCRegister& result, PPCRegister& ordinal, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1F20, "resource_class_result", total.u64, result.u64, ordinal.u64, player.u64);
}

void TraceResourceFoodCall(PPCRegister& total, PPCRegister& ordinal, PPCRegister& argument, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1F30, "82CF0630_ordinal_argument", total.u64, ordinal.u64, argument.u64, player.u64);
}

void TraceResourceFoodResult(PPCRegister& total, PPCRegister& contribution, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1F34, "82CF0630_contribution", total.u64, contribution.u64, 0, player.u64);
}

void TracePlayerModifier(PPCRegister& total, PPCRegister& modifier, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1F54, "player_modifier_before_division", total.u64, modifier.u64, 0, player.u64);
}

void TracePlayerModifierAdd(PPCRegister& total, PPCRegister& contribution, PPCRegister& numerator, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1F5C, "player_modifier_add", total.u64, contribution.u64, numerator.u64, player.u64);
}

void TraceFoodFinal(PPCRegister& total, PPCRegister& player) noexcept
{
    rerevved::resource_identity_trace::FoodStep(0x82CF1F60, "final_food", total.u64, 0, 0, player.u64);
}

void TraceCityWorkTotals(PPCRegister& stack) noexcept
{
    rerevved::resource_identity_trace::CityTotals(stack.u64);
}

void TraceSettlersFood(PPCRegister& a, PPCRegister& b, PPCRegister& food, PPCRegister& manager) noexcept
{
    rerevved::resource_identity_trace::OverlayFood(a.u64, b.u64, food.u64, manager.u64);
}

void TraceSettlersFoodCache(PPCRegister& a, PPCRegister& b, PPCRegister& icon, PPCRegister& food, PPCRegister& manager) noexcept
{
    rerevved::resource_identity_trace::OverlayCache(a.u64, b.u64, icon.u64, food.u64, manager.u64);
}

void ApplyResourceIdentityOverride(PPCRegister& tileIndex, PPCRegister& coordinateSum, PPCRegister& terrainBase)
{
    namespace overrides = rerevved::resource_identity_overrides;
    namespace trace     = rerevved::resource_identity_trace;
    const auto snapshot = overrides::Current();
    // Callers such as city work-tile totals pass coordinates with stale high
    // words; the native reader indexes with the low word, so match on it too.
    if (snapshot->empty())
    {
        trace::Decision(*snapshot, tileIndex.u64, coordinateSum.u64, coordinateSum.u64, terrainBase.u64, "no snapshot");
        return;
    }
    const auto found = std::lower_bound(snapshot->begin(), snapshot->end(), tileIndex.u32, [](overrides::Record record, uint32_t index)
                                        {
                                            return record.tileIndex < index;
                                        });
    if (found == snapshot->end() || found->tileIndex != tileIndex.u32)
    {
        trace::Decision(*snapshot, tileIndex.u64, coordinateSum.u64, coordinateSum.u64, terrainBase.u64, "no record");
        return;
    }
    const uint64_t address = uint64_t{ terrainBase.u32 } + tileIndex.u32;
    if (address < 0x10000 || address >= 0xFFFFF000ull)
    {
        trace::Decision(*snapshot, tileIndex.u64, coordinateSum.u64, coordinateSum.u64, terrainBase.u64, "bad address");
        return;
    }
    auto* memory = REX_KERNEL_MEMORY();
    auto* heap   = memory ? memory->LookupHeap(static_cast<uint32_t>(address)) : nullptr;
    if (!heap || heap->QueryRangeAccess(static_cast<uint32_t>(address), static_cast<uint32_t>(address)) == rex::memory::PageAccess::kNoAccess)
    {
        trace::Decision(*snapshot, tileIndex.u64, coordinateSum.u64, coordinateSum.u64, terrainBase.u64, "no access");
        return;
    }
    const auto terrain = std::bit_cast<int8_t>(*memory->TranslateVirtual<const uint8_t*>(static_cast<uint32_t>(address)));
    const auto before  = coordinateSum.u64;
    // Only full r8 changes. Presence, forbidden flags and player gates stay native.
    coordinateSum.u64     = overrides::CoordinateSum(*snapshot, tileIndex.u32, terrain, coordinateSum.u64);
    const bool compatible = terrain >= 0 && terrain <= 6 &&
                            std::ranges::find(overrides::kIdentities[terrain], static_cast<int>(found->ordinal)) != std::end(overrides::kIdentities[terrain]);
    trace::Decision(*snapshot, tileIndex.u64, before, coordinateSum.u64, terrainBase.u64, compatible ? "applied" : "terrain mismatch");
}

bool SkipBalancedIndiaStartingResource()
{
    return rerevved::start_resources::SuppressIndiaStartingResource();
}
