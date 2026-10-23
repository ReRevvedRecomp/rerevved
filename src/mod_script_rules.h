#pragma once

#include <memory>
#include <rex/system/mod_script.h>

#include <building_effect_rules.h>
#include <wonder_effect_rules.h>

namespace rerevved
{

std::shared_ptr<rex::system::IModScriptHost> CreateModScriptRulesHost();

#ifdef RULES_BUNDLE_TESTING
// Receives each complete effect request before its support check.
void ObserveEffectRequestsForTests(void (*building)(const BuildingEffectRuleV2&),
                                   void (*wonder)(const WonderEffectRuleV2&)) noexcept;
#endif

} // namespace rerevved
