#include <unit_effect_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedUnitEffectRulesAbiVersionFn versionFn =
        ReRevvedUnitEffectRulesAbiVersion;
    ReRevvedRegisterUnitEffectRuleFn registerFn = ReRevvedRegisterUnitEffectRule;
    ReRevvedGetUnitEffectRuleCountFn countFn    = ReRevvedGetUnitEffectRuleCount;
    ReRevvedGetUnitEffectRuleFn      getFn      = ReRevvedGetUnitEffectRule;
    ReRevvedEvaluateUnitEffectFn     evaluateFn = ReRevvedEvaluateUnitEffect;

    ReRevvedUnitEffectRule       rule       = { 0 };
    ReRevvedUnitEffectRuleInfo   info       = { 0 };
    ReRevvedUnitEffectQuery      query      = { 0 };
    ReRevvedUnitEffectEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 44 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedUnitEffectRule, civilization) != 132 ||
        offsetof(ReRevvedUnitEffectRule, identity) != 140 ||
        offsetof(ReRevvedUnitEffectRule, effect) != 144 ||
        offsetof(ReRevvedUnitEffectRuleInfo, identity) != 140 ||
        offsetof(ReRevvedUnitEffectRuleInfo, statusFlags) != 148 ||
        offsetof(ReRevvedUnitEffectQuery, identity) != 12 ||
        offsetof(ReRevvedUnitEffectQuery, nativeLevel) != 20 ||
        offsetof(ReRevvedUnitEffectEvaluation, finalLevel) != 8 ||
        REREVVED_UNIT_EFFECT_CREATION_VETERAN != 1 ||
        REREVVED_UNIT_EFFECT_CREATION_GUERILLA != 2 ||
        REREVVED_UNIT_EFFECT_CREATION_BLITZ != 3 ||
        REREVVED_UNIT_EFFECT_CREATION_INFILTRATION != 4 ||
        REREVVED_UNIT_EFFECT_CREATION_LOYALTY != 5 ||
        REREVVED_UNIT_EFFECT_CREATION_ENGINEER != 6 ||
        REREVVED_UNIT_EFFECT_CREATION_LEADERSHIP != 7 ||
        REREVVED_UNIT_EFFECT_CREATION_MARCH != 8 ||
        REREVVED_UNIT_EFFECT_CREATION_MEDIC != 9 ||
        REREVVED_UNIT_EFFECT_CREATION_SCOUT != 10)
    {
        return 1;
    }
    return versionFn() == REREVVED_UNIT_EFFECT_RULES_ABI_VERSION ? 0 : 2;
}
