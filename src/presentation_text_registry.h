#pragma once

#include <presentation_text.h>

namespace rerevved::presentation_text
{

bool TryEvaluate(const ReRevvedPresentationTextQuery& query,
                 ReRevvedPresentationTextEvaluation&  evaluation);

void ResetForTests();

} // namespace rerevved::presentation_text
