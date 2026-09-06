#include <presentation_text.h>

#include <stddef.h>

int main(void)
{
    ReRevvedPresentationTextAbiVersionFn version_fn =
        ReRevvedPresentationTextAbiVersion;
    ReRevvedRegisterPresentationTextRuleFn register_fn =
        ReRevvedRegisterPresentationTextRule;
    ReRevvedGetPresentationTextRuleCountFn count_fn =
        ReRevvedGetPresentationTextRuleCount;
    ReRevvedGetPresentationTextRuleFn  get_fn = ReRevvedGetPresentationTextRule;
    ReRevvedEvaluatePresentationTextFn evaluate_fn =
        ReRevvedEvaluatePresentationText;

    ReRevvedPresentationTextRule       rule       = { 0 };
    ReRevvedPresentationTextRuleInfo   info       = { 0 };
    ReRevvedPresentationTextQuery      query      = { 0 };
    ReRevvedPresentationTextEvaluation evaluation = { 0 };
    if (!version_fn || !register_fn || !count_fn || !get_fn || !evaluate_fn ||
        sizeof(rule) != 448 || sizeof(info) != 452 || sizeof(query) != 64 ||
        sizeof(evaluation) != 300 ||
        offsetof(ReRevvedPresentationTextRule, text) != 160 ||
        offsetof(ReRevvedPresentationTextRuleInfo, status_flags) != 416 ||
        REREVVED_PRESENTATION_TEXT_ABI_VERSION != 1u)
    {
        return 1;
    }
    return version_fn() == REREVVED_PRESENTATION_TEXT_ABI_VERSION ? 0 : 2;
}
