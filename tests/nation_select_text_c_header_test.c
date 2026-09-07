#include <nation_select_text.h>

#include <stddef.h>

int main(void)
{
    ReRevvedNationSelectTextAbiVersionFn version_fn =
        ReRevvedNationSelectTextAbiVersion;
    ReRevvedRegisterNationSelectTextRuleFn register_fn =
        ReRevvedRegisterNationSelectTextRule;
    ReRevvedGetNationSelectTextRuleCountFn count_fn =
        ReRevvedGetNationSelectTextRuleCount;
    ReRevvedGetNationSelectTextRuleFn  get_fn = ReRevvedGetNationSelectTextRule;
    ReRevvedEvaluateNationSelectTextFn evaluate_fn =
        ReRevvedEvaluateNationSelectText;

    ReRevvedNationSelectTextRule       rule       = { 0 };
    ReRevvedNationSelectTextRuleInfo   info       = { 0 };
    ReRevvedNationSelectTextQuery      query      = { 0 };
    ReRevvedNationSelectTextEvaluation evaluation = { 0 };
    if (!version_fn || !register_fn || !count_fn || !get_fn || !evaluate_fn ||
        sizeof(rule) != 448 || sizeof(info) != 452 || sizeof(query) != 64 ||
        sizeof(evaluation) != 300 ||
        offsetof(ReRevvedNationSelectTextRule, text) != 160 ||
        offsetof(ReRevvedNationSelectTextRuleInfo, status_flags) != 416 ||
        REREVVED_NATION_SELECT_TEXT_ABI_VERSION != 1u)
    {
        return 1;
    }
    return version_fn() == REREVVED_NATION_SELECT_TEXT_ABI_VERSION ? 0 : 2;
}
