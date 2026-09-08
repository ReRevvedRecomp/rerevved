#include <unique_unit_rules.h>

#include <stddef.h>

int main(void)
{
    ReRevvedUniqueUnitRulesAbiVersionFn versionFn =
        ReRevvedUniqueUnitRulesAbiVersion;
    ReRevvedRegisterUniqueUnitScalarRuleFn registerFn =
        ReRevvedRegisterUniqueUnitScalarRule;
    ReRevvedGetUniqueUnitScalarRuleCountFn countFn =
        ReRevvedGetUniqueUnitScalarRuleCount;
    ReRevvedGetUniqueUnitScalarRuleFn  getFn = ReRevvedGetUniqueUnitScalarRule;
    ReRevvedEvaluateUniqueUnitScalarFn evaluateFn =
        ReRevvedEvaluateUniqueUnitScalar;

    ReRevvedUniqueUnitScalarRule       rule       = { 0 };
    ReRevvedUniqueUnitScalarRuleInfo   info       = { 0 };
    ReRevvedUniqueUnitScalarQuery      query      = { 0 };
    ReRevvedUniqueUnitScalarEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 176 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 || offsetof(ReRevvedUniqueUnitScalarRule, value) != 152 ||
        offsetof(ReRevvedUniqueUnitScalarRuleInfo, statusFlags) != 156 ||
        offsetof(ReRevvedUniqueUnitScalarEvaluation, finalValue) != 8)
    {
        return 1;
    }
    return versionFn() == REREVVED_UNIQUE_UNIT_RULES_ABI_VERSION ? 0 : 2;
}
