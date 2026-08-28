#include <unique_unit_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedUniqueUnitRulesAbiVersionFn version_fn =
        ReRevvedUniqueUnitRulesAbiVersion;
    ReRevvedRegisterUniqueUnitScalarRuleFn register_fn =
        ReRevvedRegisterUniqueUnitScalarRule;
    ReRevvedGetUniqueUnitScalarRuleCountFn count_fn =
        ReRevvedGetUniqueUnitScalarRuleCount;
    ReRevvedGetUniqueUnitScalarRuleFn  get_fn = ReRevvedGetUniqueUnitScalarRule;
    ReRevvedEvaluateUniqueUnitScalarFn evaluate_fn =
        ReRevvedEvaluateUniqueUnitScalar;

    ReRevvedUniqueUnitScalarRule       rule       = { 0 };
    ReRevvedUniqueUnitScalarRuleInfo   info       = { 0 };
    ReRevvedUniqueUnitScalarQuery      query      = { 0 };
    ReRevvedUniqueUnitScalarEvaluation evaluation = { 0 };

    if (!version_fn || !register_fn || !count_fn || !get_fn || !evaluate_fn ||
        sizeof(rule) != 176 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 || offsetof(ReRevvedUniqueUnitScalarRule, value) != 152 ||
        offsetof(ReRevvedUniqueUnitScalarRuleInfo, status_flags) != 156 ||
        offsetof(ReRevvedUniqueUnitScalarEvaluation, final_value) != 8)
    {
        return 1;
    }
    return version_fn() == REREVVED_UNIQUE_UNIT_RULES_ABI_VERSION ? 0 : 2;
}
