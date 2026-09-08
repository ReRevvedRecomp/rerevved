#include <nation_select_text.h>

#include <stddef.h>

int main(void)
{
    NationSelectTextAbiVersionFn versionFn =
        NationSelectTextAbiVersion;
    RegisterNationSelectTextRuleFn registerFn =
        RegisterNationSelectTextRule;
    GetNationSelectTextRuleCountFn countFn =
        GetNationSelectTextRuleCount;
    GetNationSelectTextRuleFn  getFn = GetNationSelectTextRule;
    EvaluateNationSelectTextFn evaluateFn =
        EvaluateNationSelectText;

    NationSelectTextRule       rule       = { 0 };
    NationSelectTextRuleInfo   info       = { 0 };
    NationSelectTextQuery      query      = { 0 };
    NationSelectTextEvaluation evaluation = { 0 };
    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 448 || sizeof(info) != 452 || sizeof(query) != 64 ||
        sizeof(evaluation) != 300 ||
        offsetof(NationSelectTextRule, text) != 160 ||
        offsetof(NationSelectTextRuleInfo, statusFlags) != 416 ||
        NATION_SELECT_TEXT_ABI_VERSION != 2u)
    {
        return 1;
    }
    return versionFn() == NATION_SELECT_TEXT_ABI_VERSION ? 0 : 2;
}
