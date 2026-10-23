#pragma once

// Returns every process-wide rules registry to its initial state when a test
// case starts and again when it ends, however it ends. Each case used to own a
// fresh process; this keeps one case's registrations from reaching the next.

#include "building_effect_rules_registry.h"
#include "city_founding_unit_rules.h"
#include "main_menu_logo_asset.h"
#include "nation_select_text_registry.h"
#include "terrain_yield_rules_registry.h"
#include "unique_era_abilities_registry.h"
#include "unique_unit_rules_registry.h"
#include "unit_combat_rules_registry.h"
#include "unit_effect_rules_registry.h"
#include "unit_movement_rules_registry.h"
#include "unit_production_cost_rules_registry.h"
#include "wonder_effect_rules_registry.h"

namespace rerevved::tests
{

inline void ResetAllRegistries()
{
    terrain_yield_rules::ResetForTests();
    unit_combat_rules::ResetForTests();
    unique_unit_rules::ResetForTests();
    unit_movement_rules::ResetForTests();
    unit_effect_rules::ResetForTests();
    unit_production_cost_rules::ResetForTests();
    unique_era_abilities::ResetForTests();
    nation_select_text::ResetForTests();
    building_effect_rules::ResetForTests();
    wonder_effect_rules::ResetForTests();
    city_founding_unit::ResetForTests();
    main_menu_logo::ResetForTests();
}

class ScopedRegistryReset
{
public:
    ScopedRegistryReset()
    {
        ResetAllRegistries();
    }

    ~ScopedRegistryReset()
    {
        ResetAllRegistries();
    }

    ScopedRegistryReset(const ScopedRegistryReset&)            = delete;
    ScopedRegistryReset& operator=(const ScopedRegistryReset&) = delete;
};

} // namespace rerevved::tests
