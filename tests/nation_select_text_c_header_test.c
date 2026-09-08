#include <nation_select_text.h>

#include <stddef.h>

int main(void)
{
    ReRevvedNationSelectTextAbiVersionFn versionFn =
        ReRevvedNationSelectTextAbiVersion;
    ReRevvedRegisterNationSelectTextRuleFn registerFn =
        ReRevvedRegisterNationSelectTextRule;
    ReRevvedGetNationSelectTextRuleCountFn countFn =
        ReRevvedGetNationSelectTextRuleCount;
    ReRevvedGetNationSelectTextRuleFn  getFn = ReRevvedGetNationSelectTextRule;
    ReRevvedEvaluateNationSelectTextFn evaluateFn =
        ReRevvedEvaluateNationSelectText;

    ReRevvedNationSelectTextRule       rule       = { 0 };
    ReRevvedNationSelectTextRuleInfo   info       = { 0 };
    ReRevvedNationSelectTextQuery      query      = { 0 };
    ReRevvedNationSelectTextEvaluation evaluation = { 0 };
    if (!versionFn || !registerFn || !countFn || !getFn || !evaluateFn ||
        sizeof(rule) != 448 || sizeof(info) != 452 || sizeof(query) != 64 ||
        sizeof(evaluation) != 300 ||
        offsetof(ReRevvedNationSelectTextRule, text) != 160 ||
        offsetof(ReRevvedNationSelectTextRuleInfo, statusFlags) != 416 ||
        REREVVED_NATION_SELECT_TEXT_ABI_VERSION != 1u)
    {
        return 1;
    }
    return versionFn() == REREVVED_NATION_SELECT_TEXT_ABI_VERSION ? 0 : 2;
}
