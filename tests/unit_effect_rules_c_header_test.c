#include <unit_effect_rules.h>

#include <stddef.h>

int main(void)
{
    UnitEffectRulesAbiVersionFn versionFn =
        UnitEffectRulesAbiVersion;
    RegisterUnitEffectRuleFn registerFn = RegisterUnitEffectRule;
    GetUnitEffectRuleCountFn countFn    = GetUnitEffectRuleCount;
    GetUnitEffectRuleFn      getFn      = GetUnitEffectRule;
    EvaluateUnitEffectFn     evaluateFn = EvaluateUnitEffect;

    UnitEffectRule       rule       = { 0 };
    UnitEffectRuleInfo   info       = { 0 };
    UnitEffectQuery      query      = { 0 };
    UnitEffectEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 44 ||
        sizeof(evaluation) != 40 ||
        offsetof(UnitEffectRule, civilization) != 132 ||
        offsetof(UnitEffectRule, identity) != 140 ||
        offsetof(UnitEffectRule, effect) != 144 ||
        offsetof(UnitEffectRuleInfo, identity) != 140 ||
        offsetof(UnitEffectRuleInfo, statusFlags) != 148 ||
        offsetof(UnitEffectQuery, identity) != 12 ||
        offsetof(UnitEffectQuery, nativeLevel) != 20 ||
        offsetof(UnitEffectEvaluation, finalLevel) != 8 ||
        UNIT_EFFECT_CREATION_VETERAN != 1 ||
        UNIT_EFFECT_CREATION_GUERILLA != 2 ||
        UNIT_EFFECT_CREATION_BLITZ != 3 ||
        UNIT_EFFECT_CREATION_INFILTRATION != 4 ||
        UNIT_EFFECT_CREATION_LOYALTY != 5 ||
        UNIT_EFFECT_CREATION_ENGINEER != 6 ||
        UNIT_EFFECT_CREATION_LEADERSHIP != 7 ||
        UNIT_EFFECT_CREATION_MARCH != 8 ||
        UNIT_EFFECT_CREATION_MEDIC != 9 ||
        UNIT_EFFECT_CREATION_SCOUT != 10)
    {
        return 1;
    }
    return versionFn() == UNIT_EFFECT_RULES_ABI_VERSION ? 0 : 2;
}
