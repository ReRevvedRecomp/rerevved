#include <building_effect_rules.h>
#include <wonder_effect_rules.h>

#include <stddef.h>

_Static_assert(sizeof(BuildingEffectRule) == 160, "Building rule ABI");
_Static_assert(sizeof(WonderEffectRule) == 160, "Wonder rule ABI");
_Static_assert(sizeof(BuildingEffectSupport) == 32, "Building support ABI");
_Static_assert(sizeof(WonderEffectSupport) == 32, "Wonder support ABI");
_Static_assert(sizeof(BuildingEffectPreviewQuery) == 32, "Building query ABI");
_Static_assert(sizeof(WonderEffectPreviewQuery) == 32, "Wonder query ABI");
_Static_assert(sizeof(BuildingEffectPreview) == 32, "Building preview ABI");
_Static_assert(sizeof(WonderEffectPreview) == 32, "Wonder preview ABI");
_Static_assert(BUILDING_EFFECT_RULES_ERR_CONFLICT == -12, "Building conflict code");
_Static_assert(WONDER_EFFECT_RULES_ERR_CONFLICT == -12, "Wonder conflict code");
_Static_assert(WONDER_EFFECT_RULES_ERR_INTERNAL == -13, "Wonder internal code");
_Static_assert(WONDER_EFFECT_RULES_ERR_UNAVAILABLE == -14, "Wonder unavailable code");
_Static_assert(WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE == 1, "Culture replacement ID");
_Static_assert(WONDER_EFFECT_STONEHENGE_RELIGION == 2, "Religion grant ID");
_Static_assert(offsetof(BuildingEffectRule, effect) == 132, "Building selector ABI");
_Static_assert(offsetof(WonderEffectRule, effect) == 132, "Wonder selector ABI");
_Static_assert(offsetof(BuildingEffectPreview, statusFlags) == 20, "Building status ABI");
_Static_assert(offsetof(WonderEffectPreview, statusFlags) == 20, "Wonder status ABI");

_Static_assert(sizeof(BuildingEffectRuleV2) == 160, "BuildingEffectRuleV2 size");
_Static_assert(offsetof(BuildingEffectRuleV2, structSize) == 0, "BuildingEffectRuleV2 structSize");
_Static_assert(offsetof(BuildingEffectRuleV2, providerId) == 4, "BuildingEffectRuleV2 providerId");
_Static_assert(offsetof(BuildingEffectRuleV2, ruleId) == 68, "BuildingEffectRuleV2 ruleId");
_Static_assert(offsetof(BuildingEffectRuleV2, effect) == 132, "BuildingEffectRuleV2 effect");
_Static_assert(offsetof(BuildingEffectRuleV2, amount) == 136, "BuildingEffectRuleV2 amount");
_Static_assert(offsetof(BuildingEffectRuleV2, reserved) == 140, "BuildingEffectRuleV2 reserved");
_Static_assert(sizeof(BuildingEffectSupportV2) == 32, "BuildingEffectSupportV2 size");
_Static_assert(offsetof(BuildingEffectSupportV2, structSize) == 0, "BuildingEffectSupportV2 structSize");
_Static_assert(offsetof(BuildingEffectSupportV2, runtimeAvailable) == 4, "BuildingEffectSupportV2 runtimeAvailable");
_Static_assert(offsetof(BuildingEffectSupportV2, minAmount) == 8, "BuildingEffectSupportV2 minAmount");
_Static_assert(offsetof(BuildingEffectSupportV2, maxAmount) == 12, "BuildingEffectSupportV2 maxAmount");
_Static_assert(offsetof(BuildingEffectSupportV2, reserved) == 16, "BuildingEffectSupportV2 reserved");
_Static_assert(sizeof(WonderEffectRuleV2) == 160, "WonderEffectRuleV2 size");
_Static_assert(offsetof(WonderEffectRuleV2, structSize) == 0, "WonderEffectRuleV2 structSize");
_Static_assert(offsetof(WonderEffectRuleV2, providerId) == 4, "WonderEffectRuleV2 providerId");
_Static_assert(offsetof(WonderEffectRuleV2, ruleId) == 68, "WonderEffectRuleV2 ruleId");
_Static_assert(offsetof(WonderEffectRuleV2, effect) == 132, "WonderEffectRuleV2 effect");
_Static_assert(offsetof(WonderEffectRuleV2, amount) == 136, "WonderEffectRuleV2 amount");
_Static_assert(offsetof(WonderEffectRuleV2, reserved) == 140, "WonderEffectRuleV2 reserved");
_Static_assert(sizeof(WonderEffectSupportV2) == 32, "WonderEffectSupportV2 size");
_Static_assert(offsetof(WonderEffectSupportV2, structSize) == 0, "WonderEffectSupportV2 structSize");
_Static_assert(offsetof(WonderEffectSupportV2, runtimeAvailable) == 4, "WonderEffectSupportV2 runtimeAvailable");
_Static_assert(offsetof(WonderEffectSupportV2, minAmount) == 8, "WonderEffectSupportV2 minAmount");
_Static_assert(offsetof(WonderEffectSupportV2, maxAmount) == 12, "WonderEffectSupportV2 maxAmount");
_Static_assert(offsetof(WonderEffectSupportV2, reserved) == 16, "WonderEffectSupportV2 reserved");

int rerevved_building_wonder_effect_rules_c_header_check(void)
{
    BuildingEffectRulesV2AbiVersionFn buildingVersionV2  = BuildingEffectRulesV2AbiVersion;
    WonderEffectRulesV2AbiVersionFn   wonderVersionV2    = WonderEffectRulesV2AbiVersion;
    GetBuildingEffectSupportV2Fn      buildingSupportV2  = GetBuildingEffectSupportV2;
    GetWonderEffectSupportV2Fn        wonderSupportV2    = GetWonderEffectSupportV2;
    RegisterBuildingEffectRuleV2Fn    buildingRegisterV2 = RegisterBuildingEffectRuleV2;
    RegisterWonderEffectRuleV2Fn      wonderRegisterV2   = RegisterWonderEffectRuleV2;
    PreviewBuildingEffectV2Fn         buildingPreviewV2  = PreviewBuildingEffectV2;
    PreviewWonderEffectV2Fn           wonderPreviewV2    = PreviewWonderEffectV2;
    BuildingEffectSupportV2           buildingV2         = { 0 };
    WonderEffectSupportV2             wonderV2           = { 0 };
    if (buildingVersionV2() != 2 || wonderVersionV2() != 2 ||
        buildingSupportV2(&buildingV2, sizeof(buildingV2)) != 0 ||
        wonderSupportV2(&wonderV2, sizeof(wonderV2)) != 0 ||
        buildingV2.runtimeAvailable != 1 || wonderV2.runtimeAvailable != 1 ||
        buildingV2.minAmount != INT32_MIN || buildingV2.maxAmount != INT32_MAX ||
        wonderV2.minAmount != 0 || wonderV2.maxAmount != 0 ||
        buildingRegisterV2(0) != BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT ||
        wonderRegisterV2(0) != WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT ||
        buildingPreviewV2(0, 0, 0, 0) != BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT ||
        wonderPreviewV2(0, 0, 0, 0) != WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT)
    {
        return 3;
    }
    BuildingEffectRulesAbiVersionFn buildingVersion  = BuildingEffectRulesAbiVersion;
    WonderEffectRulesAbiVersionFn   wonderVersion    = WonderEffectRulesAbiVersion;
    GetBuildingEffectSupportFn      buildingSupport  = GetBuildingEffectSupport;
    GetWonderEffectSupportFn        wonderSupport    = GetWonderEffectSupport;
    RegisterBuildingEffectRuleFn    buildingRegister = RegisterBuildingEffectRule;
    RegisterWonderEffectRuleFn      wonderRegister   = RegisterWonderEffectRule;
    PreviewBuildingEffectFn         buildingPreview  = PreviewBuildingEffect;
    PreviewWonderEffectFn           wonderPreview    = PreviewWonderEffect;
    BuildingEffectSupport           building         = { 0 };
    WonderEffectSupport             wonder           = { 0 };
    if (!buildingRegister || !wonderRegister || !buildingPreview || !wonderPreview ||
        buildingVersion() != BUILDING_EFFECT_RULES_ABI_VERSION ||
        wonderVersion() != WONDER_EFFECT_RULES_ABI_VERSION ||
        buildingSupport(&building, sizeof(building)) != BUILDING_EFFECT_RULES_OK ||
        wonderSupport(&wonder, sizeof(wonder)) != WONDER_EFFECT_RULES_OK)
    {
        return 1;
    }
    return building.runtimeAvailable != 1 || wonder.runtimeAvailable != 1 ? 2 : 0;
}
