#include <unique_era_abilities.h>

#include <stddef.h>

int main(void)
{
    if (REREVVED_UNIQUE_ERA_ABILITIES_ABI_VERSION != 2u ||
        REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING != 0x10000)
    {
        return 1;
    }
    ReRevvedUniqueEraAbilitiesAbiVersionFn versionFn =
        ReRevvedUniqueEraAbilitiesAbiVersion;
    ReRevvedRegisterUniqueEraAbilityReplacementFn registerFn =
        ReRevvedRegisterUniqueEraAbilityReplacement;
    ReRevvedGetUniqueEraAbilityRuleCountFn countFn =
        ReRevvedGetUniqueEraAbilityRuleCount;
    ReRevvedGetUniqueEraAbilityRuleFn getFn =
        ReRevvedGetUniqueEraAbilityRule;
    ReRevvedEvaluateUniqueEraAbilityCellFn evaluateFn =
        ReRevvedEvaluateUniqueEraAbilityCell;

    ReRevvedUniqueEraAbilityReplacement    rule       = { 0 };
    ReRevvedUniqueEraAbilityRuleInfo       info       = { 0 };
    ReRevvedUniqueEraAbilityCellQuery      query      = { 0 };
    ReRevvedUniqueEraAbilityCellEvaluation evaluation = { 0 };

    if (versionFn() != REREVVED_UNIQUE_ERA_ABILITIES_ABI_VERSION ||
        sizeof(rule) != 176 || sizeof(info) != 180 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedUniqueEraAbilityReplacement, replacementAbility) !=
            140 ||
        offsetof(ReRevvedUniqueEraAbilityRuleInfo, statusFlags) != 144 ||
        offsetof(ReRevvedUniqueEraAbilityCellEvaluation, effectiveAbility) !=
            8)
    {
        return 1;
    }

    (void)registerFn;
    (void)countFn;
    (void)getFn;
    (void)evaluateFn;
    return 0;
}
