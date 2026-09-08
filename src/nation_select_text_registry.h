#pragma once

#include <nation_select_text.h>

namespace rerevved::nation_select_text
{

bool TryEvaluate(const NationSelectTextQuery& query,
                 NationSelectTextEvaluation&  evaluation);

void ResetForTests();

} // namespace rerevved::nation_select_text
