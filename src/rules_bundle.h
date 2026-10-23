#pragma once

#include "city_founding_unit_rules.h"

#include <building_effect_rules.h>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <nation_select_text.h>
#include <string>
#include <string_view>
#include <terrain_yield_rules.h>
#include <unique_era_abilities.h>
#include <unique_unit_rules.h>
#include <unit_combat_rules.h>
#include <unit_effect_rules.h>
#include <unit_movement_rules.h>
#include <unit_production_cost_rules.h>
#include <variant>
#include <vector>
#include <wonder_effect_rules.h>

namespace rerevved::rules_bundle
{

using NativeRequest = std::variant<TerrainYieldRule, UnitCombatRule, UniqueUnitScalarRule, UnitMovementRule, UnitEffectRule, UnitProductionCostRule, EraAbilityReplacement, NationSelectTextRule, BuildingEffectRuleV2, WonderEffectRuleV2, city_founding_unit::Rule>;

struct IndexedRequest
{
    size_t        bundleIndex;
    NativeRequest request;
};

struct RulesBundle
{
    std::vector<IndexedRequest> rules;
};
enum class Domain
{
    TerrainYieldRule,
    UnitCombatRule,
    UniqueUnitScalarRule,
    UnitMovementRule,
    UnitEffectRule,
    UnitProductionCostRule,
    EraAbilityReplacement,
    NationSelectTextRule,
    BuildingEffectRule,
    WonderEffectRule,
    CityFoundingUnitRule
};

struct PublishResult
{
    bool    published    = false;
    size_t  bundleIndex  = 0;
    Domain  domain       = Domain::TerrainYieldRule;
    int32_t nativeResult = 0;
    char    ruleId[TERRAIN_YIELD_RULE_ID_CAPACITY + 1]{};
};

PublishResult PublishRulesBundle(const RulesBundle& bundle) noexcept;

#ifdef RULES_BUNDLE_TESTING
// Zero fails the next preparation step; negative disables injection.
// Thread-local so concurrent readers and other publishers are unaffected.
void FailPreparationAfterForTests(int64_t successfulSteps) noexcept;
#endif

struct RuleIdentity
{
    std::string_view providerId;
    std::string_view ruleId;
};

struct UnitSelectors
{
    std::string_view civilization;
    std::string_view baseUnitType;
    std::string_view identity;
};

struct TextSelectors
{
    std::string_view surface;
    std::string_view civilization;
    std::string_view unlockEra;
    std::string_view ability;
    std::string_view baseUnitType;
    std::string_view identity;
    std::string_view displayForm;
};

bool IsContractToken(std::string_view kind, std::string_view field, std::string_view value) noexcept;

struct RuleParameter
{
    std::string_view field;
    std::string_view value;
};

int32_t     MapRuleParameters(std::string_view kind, std::initializer_list<RuleParameter> parameters) noexcept;
std::string SupportedRuleParameterValues(std::string_view kind, std::string_view field);

// Builders return native domain result codes and leave output unchanged on failure.
int32_t BuildTerrainYieldRule(RuleIdentity ids, std::string_view terrain, std::string_view component, std::string_view operation, int32_t value, TerrainYieldRule& out) noexcept;
int32_t BuildUnitCombatRule(RuleIdentity ids, UnitSelectors unit, std::string_view terrain, std::string_view property, int32_t percentageDelta, UnitCombatRule& out) noexcept;
int32_t BuildUniqueUnitScalarRule(RuleIdentity ids, UnitSelectors unit, std::string_view property, std::string_view operation, int32_t value, UniqueUnitScalarRule& out) noexcept;
int32_t BuildUnitMovementRule(RuleIdentity ids, UnitSelectors unit, int32_t value, UnitMovementRule& out) noexcept;
int32_t BuildUnitEffectRule(RuleIdentity ids, UnitSelectors unit, std::string_view effect, UnitEffectRule& out) noexcept;
int32_t BuildUnitProductionCostRule(RuleIdentity ids, UnitSelectors unit, int32_t percentageDelta, UnitProductionCostRule& out) noexcept;
int32_t BuildEraTechnologyAbility(RuleIdentity ids, std::string_view civilization, std::string_view unlockEra, std::string_view technology, EraAbilityReplacement& out) noexcept;
int32_t BuildNationSelectTextRule(RuleIdentity ids, TextSelectors selectors, std::string_view text, NationSelectTextRule& out) noexcept;
int32_t BuildCityFoundingUnitRule(RuleIdentity ids, bool enable, city_founding_unit::Rule& out) noexcept;

} // namespace rerevved::rules_bundle
