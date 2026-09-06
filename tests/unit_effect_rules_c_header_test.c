#include <unit_effect_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedUnitEffectRulesAbiVersionFn version_fn =
        ReRevvedUnitEffectRulesAbiVersion;
    ReRevvedRegisterUnitEffectRuleFn register_fn = ReRevvedRegisterUnitEffectRule;
    ReRevvedGetUnitEffectRuleCountFn count_fn    = ReRevvedGetUnitEffectRuleCount;
    ReRevvedGetUnitEffectRuleFn      get_fn      = ReRevvedGetUnitEffectRule;
    ReRevvedEvaluateUnitEffectFn     evaluate_fn = ReRevvedEvaluateUnitEffect;

    ReRevvedUnitEffectRule       rule       = { 0 };
    ReRevvedUnitEffectRuleInfo   info       = { 0 };
    ReRevvedUnitEffectQuery      query      = { 0 };
    ReRevvedUnitEffectEvaluation evaluation = { 0 };

    if (!version_fn || !register_fn || !count_fn || !get_fn || !evaluate_fn ||
        sizeof(rule) != 168 || sizeof(info) != 192 || sizeof(query) != 44 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedUnitEffectRule, civilization) != 132 ||
        offsetof(ReRevvedUnitEffectRule, identity) != 140 ||
        offsetof(ReRevvedUnitEffectRule, effect) != 144 ||
        offsetof(ReRevvedUnitEffectRuleInfo, identity) != 140 ||
        offsetof(ReRevvedUnitEffectRuleInfo, status_flags) != 148 ||
        offsetof(ReRevvedUnitEffectQuery, identity) != 12 ||
        offsetof(ReRevvedUnitEffectQuery, native_level) != 20 ||
        offsetof(ReRevvedUnitEffectEvaluation, final_level) != 8 ||
        REREVVED_UNIT_EFFECT_CREATION_VETERAN != 1)
    {
        return 1;
    }
    return version_fn() == REREVVED_UNIT_EFFECT_RULES_ABI_VERSION ? 0 : 2;
}
