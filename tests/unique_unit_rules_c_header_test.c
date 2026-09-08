#include <unique_unit_rules.h>

#include <stddef.h>

int main(void)
{
    UniqueUnitRulesAbiVersionFn versionFn =
        UniqueUnitRulesAbiVersion;
    RegisterUniqueUnitScalarRuleFn registerFn =
        RegisterUniqueUnitScalarRule;
    GetUniqueUnitScalarRuleCountFn countFn =
        GetUniqueUnitScalarRuleCount;
    GetUniqueUnitScalarRuleFn  getFn = GetUniqueUnitScalarRule;
    EvaluateUniqueUnitScalarFn evaluateFn =
        EvaluateUniqueUnitScalar;

    UniqueUnitScalarRule       rule       = { 0 };
    UniqueUnitScalarRuleInfo   info       = { 0 };
    UniqueUnitScalarQuery      query      = { 0 };
    UniqueUnitScalarEvaluation evaluation = { 0 };

    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 176 || sizeof(info) != 192 || sizeof(query) != 40 ||
        sizeof(evaluation) != 40 || offsetof(UniqueUnitScalarRule, value) != 152 ||
        offsetof(UniqueUnitScalarRuleInfo, statusFlags) != 156 ||
        offsetof(UniqueUnitScalarEvaluation, finalValue) != 8)
    {
        return 1;
    }
    return versionFn() == UNIQUE_UNIT_RULES_ABI_VERSION ? 0 : 2;
}
