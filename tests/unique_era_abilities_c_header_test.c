#include <unique_era_abilities.h>

#include <stddef.h>

int main(void)
{
    if (REREVVED_UNIQUE_ERA_ABILITIES_ABI_VERSION != 2u ||
        REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING != 0x10000)
    {
        return 1;
    }
    ReRevvedUniqueEraAbilitiesAbiVersionFn version_fn =
        ReRevvedUniqueEraAbilitiesAbiVersion;
    ReRevvedRegisterUniqueEraAbilityReplacementFn register_fn =
        ReRevvedRegisterUniqueEraAbilityReplacement;
    ReRevvedGetUniqueEraAbilityRuleCountFn count_fn =
        ReRevvedGetUniqueEraAbilityRuleCount;
    ReRevvedGetUniqueEraAbilityRuleFn get_fn =
        ReRevvedGetUniqueEraAbilityRule;
    ReRevvedEvaluateUniqueEraAbilityCellFn evaluate_fn =
        ReRevvedEvaluateUniqueEraAbilityCell;

    ReRevvedUniqueEraAbilityReplacement    rule       = { 0 };
    ReRevvedUniqueEraAbilityRuleInfo       info       = { 0 };
    ReRevvedUniqueEraAbilityCellQuery      query      = { 0 };
    ReRevvedUniqueEraAbilityCellEvaluation evaluation = { 0 };

    if (version_fn() != REREVVED_UNIQUE_ERA_ABILITIES_ABI_VERSION ||
        sizeof(rule) != 176 || sizeof(info) != 180 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 ||
        offsetof(ReRevvedUniqueEraAbilityReplacement, replacement_ability) !=
            140 ||
        offsetof(ReRevvedUniqueEraAbilityRuleInfo, status_flags) != 144 ||
        offsetof(ReRevvedUniqueEraAbilityCellEvaluation, effective_ability) !=
            8)
    {
        return 1;
    }

    (void)register_fn;
    (void)count_fn;
    (void)get_fn;
    (void)evaluate_fn;
    return 0;
}
