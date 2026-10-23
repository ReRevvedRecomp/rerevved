#pragma once

#include "rules_bundle.h"
#include <string>

namespace rerevved::tests::rules_fixtures
{

rules_bundle::RulesBundle fixtures();
void                      reset();
std::string               snapshot();

} // namespace rerevved::tests::rules_fixtures
