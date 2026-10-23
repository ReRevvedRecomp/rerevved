#include "building_effect_rules_registry.h"
#include "nation_select_text_registry.h"
#include "preparation_failure.hpp"
#include "registry_scope.hpp"
#include "rules_bundle.h"
#include "rules_fixtures.hpp"
#include "terrain_yield_rules_registry.h"
#include "unique_era_abilities_registry.h"
#include "unique_unit_rules_registry.h"
#include "unit_combat_rules_registry.h"
#include "unit_effect_rules_registry.h"
#include "unit_movement_rules_registry.h"
#include "unit_production_cost_rules_registry.h"
#include "wonder_effect_rules_registry.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

using namespace rerevved;
using namespace rerevved::rules_bundle;
using namespace rerevved::tests::rules_fixtures;

namespace
{

void checkFixtureBuilders()
{
    const auto bundle = fixtures();
    UNSCOPED_INFO("thirteen non-Building/Wonder declarations");
    REQUIRE(bundle.rules.size() == 13);
    size_t index = 0;
    {
        const auto&      native = std::get<TerrainYieldRule>(bundle.rules[index++].request);
        TerrainYieldRule built{};
        UNSCOPED_INFO("terrain builder");
        REQUIRE(BuildTerrainYieldRule({ native.providerId, native.ruleId },
                                      "hill",
                                      "production",
                                      "add",
                                      1,
                                      built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    }
    for (bool defense : { false, true })
    {
        const auto&    native = std::get<UnitCombatRule>(bundle.rules[index++].request);
        UnitCombatRule built{};
        UNSCOPED_INFO("combat builder");
        REQUIRE(BuildUnitCombatRule({ native.providerId, native.ruleId },
                                    { "aztec", "warrior", "jaguar_warrior" },
                                    "forest",
                                    defense ? "defense" : "attack",
                                    50,
                                    built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    }
    auto unitText = [&](UnitSelectors tokens)
    {
        const auto&          native = std::get<NationSelectTextRule>(bundle.rules[index++].request);
        NationSelectTextRule built{};
        UNSCOPED_INFO("unit text builder");
        REQUIRE(BuildNationSelectTextRule({ native.providerId, native.ruleId },
                                          { "unique_unit", tokens.civilization, {}, {}, tokens.baseUnitType, tokens.identity, "unit" },
                                          native.text,
                                          built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    };
    unitText({ "aztec", "warrior", "jaguar_warrior" });
    {
        const auto&          native = std::get<UniqueUnitScalarRule>(bundle.rules[index++].request);
        UniqueUnitScalarRule built{};
        UNSCOPED_INFO("scalar builder");
        REQUIRE(BuildUniqueUnitScalarRule({ native.providerId, native.ruleId },
                                          { "roman", "knights", "cataphract" },
                                          "base_defense",
                                          "add",
                                          1,
                                          built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    }
    unitText({ "roman", "knights", "cataphract" });
    {
        const auto&      native = std::get<UnitMovementRule>(bundle.rules[index++].request);
        UnitMovementRule built{};
        UNSCOPED_INFO("movement builder");
        REQUIRE(BuildUnitMovementRule({ native.providerId, native.ruleId },
                                      { "mongolian", "horsemen", "keshik" },
                                      1,
                                      built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    }
    unitText({ "mongolian", "horsemen", "keshik" });
    {
        const auto&    native = std::get<UnitEffectRule>(bundle.rules[index++].request);
        UnitEffectRule built{};
        UNSCOPED_INFO("effect builder");
        REQUIRE(BuildUnitEffectRule({ native.providerId, native.ruleId },
                                    { "greek", "phalanx", "hoplite" },
                                    "creation_loyalty",
                                    built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    }
    {
        const auto&            native = std::get<UnitProductionCostRule>(bundle.rules[index++].request);
        UnitProductionCostRule built{};
        UNSCOPED_INFO("cost builder");
        REQUIRE(BuildUnitProductionCostRule({ native.providerId, native.ruleId },
                                            { "greek", "phalanx", "hoplite" },
                                            -30,
                                            built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    }
    unitText({ "greek", "phalanx", "hoplite" });
    {
        const auto&           native = std::get<EraAbilityReplacement>(bundle.rules[index++].request);
        EraAbilityReplacement built{};
        UNSCOPED_INFO("era builder");
        REQUIRE(BuildEraTechnologyAbility({ native.providerId, native.ruleId },
                                          "mongolian",
                                          "ancient",
                                          "horseback_riding",
                                          built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    }
    {
        const auto&          native = std::get<NationSelectTextRule>(bundle.rules[index++].request);
        NationSelectTextRule built{};
        UNSCOPED_INFO("era text builder");
        REQUIRE(BuildNationSelectTextRule({ native.providerId, native.ruleId },
                                          { "era_ability", "mongolian", "ancient", "knowledge_of_horseback_riding", {}, {}, {} },
                                          native.text,
                                          built) == 0);
        UNSCOPED_INFO("complete native request mismatch");
        REQUIRE(std::memcmp(&built, &native, sizeof(built)) == 0);
    }
}

void TestParity()
{
    auto bundle = fixtures();
    for (const auto& entry : bundle.rules)
    {
        std::visit([](const auto& rule)
                   {
                       using T = std::decay_t<decltype(rule)>;
                       if constexpr (std::is_same_v<T, TerrainYieldRule>)
                       {
                           T normalized{};
                           UNSCOPED_INFO("normalize fixture");
                           REQUIRE(terrain_yield_rules::NormalizeRequest(&rule, normalized) == 0);
                           UNSCOPED_INFO("normalized fixture parity");
                           REQUIRE(std::memcmp(&rule, &normalized, sizeof(T)) == 0);
                           UNSCOPED_INFO("native registration");
                           REQUIRE(RegisterTerrainYieldRule(&rule) == 0);
                       }
                       else if constexpr (std::is_same_v<T, UnitCombatRule>)
                       {
                           T normalized{};
                           UNSCOPED_INFO("normalize fixture");
                           REQUIRE(unit_combat_rules::NormalizeRequest(&rule, normalized) == 0);
                           UNSCOPED_INFO("normalized fixture parity");
                           REQUIRE(std::memcmp(&rule, &normalized, sizeof(T)) == 0);
                           UNSCOPED_INFO("native registration");
                           REQUIRE(RegisterUnitCombatRule(&rule) == 0);
                       }
                       else if constexpr (std::is_same_v<T, UniqueUnitScalarRule>)
                       {
                           T normalized{};
                           UNSCOPED_INFO("normalize fixture");
                           REQUIRE(unique_unit_rules::NormalizeRequest(&rule, normalized) == 0);
                           UNSCOPED_INFO("normalized fixture parity");
                           REQUIRE(std::memcmp(&rule, &normalized, sizeof(T)) == 0);
                           UNSCOPED_INFO("native registration");
                           REQUIRE(RegisterUniqueUnitScalarRule(&rule) == 0);
                       }
                       else if constexpr (std::is_same_v<T, UnitMovementRule>)
                       {
                           T normalized{};
                           UNSCOPED_INFO("normalize fixture");
                           REQUIRE(unit_movement_rules::NormalizeRequest(&rule, normalized) == 0);
                           UNSCOPED_INFO("normalized fixture parity");
                           REQUIRE(std::memcmp(&rule, &normalized, sizeof(T)) == 0);
                           UNSCOPED_INFO("native registration");
                           REQUIRE(RegisterUnitMovementRule(&rule) == 0);
                       }
                       else if constexpr (std::is_same_v<T, UnitEffectRule>)
                       {
                           T normalized{};
                           UNSCOPED_INFO("normalize fixture");
                           REQUIRE(unit_effect_rules::NormalizeRequest(&rule, normalized) == 0);
                           UNSCOPED_INFO("normalized fixture parity");
                           REQUIRE(std::memcmp(&rule, &normalized, sizeof(T)) == 0);
                           UNSCOPED_INFO("native registration");
                           REQUIRE(RegisterUnitEffectRule(&rule) == 0);
                       }
                       else if constexpr (std::is_same_v<T, UnitProductionCostRule>)
                       {
                           T normalized{};
                           UNSCOPED_INFO("normalize fixture");
                           REQUIRE(unit_production_cost_rules::NormalizeRequest(&rule, normalized) == 0);
                           UNSCOPED_INFO("normalized fixture parity");
                           REQUIRE(std::memcmp(&rule, &normalized, sizeof(T)) == 0);
                           UNSCOPED_INFO("native registration");
                           REQUIRE(RegisterUnitProductionCostRule(&rule) == 0);
                       }
                       else if constexpr (std::is_same_v<T, EraAbilityReplacement>)
                       {
                           T normalized{};
                           UNSCOPED_INFO("normalize fixture");
                           REQUIRE(unique_era_abilities::NormalizeRequest(&rule, normalized) == 0);
                           UNSCOPED_INFO("normalized fixture parity");
                           REQUIRE(std::memcmp(&rule, &normalized, sizeof(T)) == 0);
                           UNSCOPED_INFO("native registration");
                           REQUIRE(RegisterEraAbilityReplacement(&rule) == 0);
                       }
                       else if constexpr (std::is_same_v<T, NationSelectTextRule>)
                       {
                           T normalized{};
                           UNSCOPED_INFO("normalize fixture");
                           REQUIRE(nation_select_text::NormalizeRequest(&rule, normalized) == 0);
                           UNSCOPED_INFO("normalized fixture parity");
                           REQUIRE(std::memcmp(&rule, &normalized, sizeof(T)) == 0);
                           UNSCOPED_INFO("native registration");
                           REQUIRE(RegisterNationSelectTextRule(&rule) == 0);
                       }
                   },
                   entry.request);
    }
    const auto native = snapshot();
    reset();
    UNSCOPED_INFO("bundle publication");
    REQUIRE(PublishRulesBundle(bundle).published);
    UNSCOPED_INFO("batch differs from single-rule registrations");
    REQUIRE(snapshot() == native);
}

void checkFailure(const RulesBundle& bundle, size_t index, Domain domain, int32_t code, const std::string& before)
{
    const auto result = PublishRulesBundle(bundle);
    UNSCOPED_INFO("expected rejection");
    REQUIRE(!result.published);
    UNSCOPED_INFO("wrong original bundle index");
    REQUIRE(result.bundleIndex == index);
    UNSCOPED_INFO("wrong domain/result");
    REQUIRE((result.domain == domain && result.nativeResult == code));
    const auto& input = *std::find_if(bundle.rules.begin(), bundle.rules.end(), [&](const auto& entry)
                                      {
                                          return entry.bundleIndex == index;
                                      });
    std::visit([&](const auto& rule)
               {
                   UNSCOPED_INFO("wrong failing rule ID");
                   REQUIRE(std::strcmp(result.ruleId, rule.ruleId) == 0);
               },
               input.request);
    UNSCOPED_INFO("failed bundle changed a registry");
    REQUIRE(snapshot() == before);
}

void TestRollback()
{
    auto        bundle = fixtures();
    RulesBundle earlier{ { bundle.rules.front() } };
    UNSCOPED_INFO("earlier bundle");
    REQUIRE(PublishRulesBundle(earlier).published);
    const auto before                                                         = snapshot();
    auto       invalid                                                        = bundle;
    std::get<NationSelectTextRule>(invalid.rules.back().request).baseUnitType = UNIT_TYPE_WARRIOR;
    checkFailure(invalid, 13, Domain::NationSelectTextRule, NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT, before);
    invalid                                                            = bundle;
    std::get<NationSelectTextRule>(invalid.rules[3].request).unlockEra = UNLOCK_ERA_ANCIENT;
    checkFailure(invalid, 4, Domain::NationSelectTextRule, NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT, before);

    for (size_t target : { size_t{ 3 }, size_t{ 9 } })
    {
        reset();
        UNSCOPED_INFO("earlier bundle conflict case");
        REQUIRE(PublishRulesBundle(earlier).published);
        auto existing = bundle.rules[target];
        std::visit([](auto& rule)
                   {
                       using T = std::decay_t<decltype(rule)>;
                       if constexpr (std::is_same_v<T, NationSelectTextRule>)
                       {
                           rule.text[0] = 'X';
                       }
                       if constexpr (std::is_same_v<T, UnitProductionCostRule>)
                       {
                           rule.percentageDelta = -20;
                       }
                   },
                   existing.request);
        UNSCOPED_INFO("conflict seed");
        REQUIRE(PublishRulesBundle({ { existing } }).published);
        const auto seeded = snapshot();
        checkFailure(bundle, target + 1, target == 3 ? Domain::NationSelectTextRule : Domain::UnitProductionCostRule, target == 3 ? int32_t{ NATION_SELECT_TEXT_ERR_DUPLICATE_RULE_ID } : int32_t{ UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID }, seeded);
        auto internal        = bundle;
        existing.bundleIndex = 71;
        internal.rules.push_back(existing);
        reset();
        const auto empty = snapshot();
        checkFailure(internal, 71, target == 3 ? Domain::NationSelectTextRule : Domain::UnitProductionCostRule, target == 3 ? int32_t{ NATION_SELECT_TEXT_ERR_DUPLICATE_RULE_ID } : int32_t{ UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID }, empty);
    }
    reset();
    UNSCOPED_INFO("earlier before invalid");
    REQUIRE(PublishRulesBundle(earlier).published);
    UNSCOPED_INFO("invalid later bundle");
    REQUIRE(!PublishRulesBundle(invalid).published);
    UNSCOPED_INFO("valid bundle after failure");
    REQUIRE(PublishRulesBundle(bundle).published);
}

void dirtyTails(IndexedRequest& entry)
{
    std::visit([](auto& rule)
               {
                   rule.structSize += 16;
                   std::memset(rule.providerId + std::strlen(rule.providerId) + 1, 'x', sizeof(rule.providerId) - std::strlen(rule.providerId) - 1);
                   std::memset(rule.ruleId + std::strlen(rule.ruleId) + 1, 'y', sizeof(rule.ruleId) - std::strlen(rule.ruleId) - 1);
                   if constexpr (std::is_same_v<std::decay_t<decltype(rule)>, NationSelectTextRule>)
                   {
                       std::memset(rule.text + std::strlen(rule.text) + 1, 'z', sizeof(rule.text) - std::strlen(rule.text) - 1);
                   }
               },
               entry.request);
}

void TestReplay()
{
    auto bundle    = fixtures();
    auto duplicate = bundle;
    for (auto entry : bundle.rules)
    {
        dirtyTails(entry);
        entry.bundleIndex += 100;
        duplicate.rules.push_back(entry);
    }
    UNSCOPED_INFO("normalized in-bundle duplicate");
    REQUIRE(PublishRulesBundle(duplicate).published);
    const auto before = snapshot();
    UNSCOPED_INFO("identical replay");
    REQUIRE(PublishRulesBundle(bundle).published);
    UNSCOPED_INFO("replay added entries");
    REQUIRE(snapshot() == before);
    std::reverse(duplicate.rules.begin(), duplicate.rules.end());
    UNSCOPED_INFO("reversed replay");
    REQUIRE(PublishRulesBundle(duplicate).published);
    UNSCOPED_INFO("reversed replay changed payloads");
    REQUIRE(snapshot() == before);
}

RulesBundle renamed(RulesBundle bundle, const std::string& provider)
{
    for (auto& entry : bundle.rules)
    {
        std::visit([&](auto& rule)
                   {
                       std::memset(rule.providerId, 0, sizeof(rule.providerId));
                       UNSCOPED_INFO("provider test length");
                       REQUIRE(provider.size() < sizeof(rule.providerId));
                       std::memcpy(rule.providerId, provider.data(), provider.size());
                   },
                   entry.request);
    }
    return bundle;
}

void TestAllocation()
{
    auto base = fixtures();
    UNSCOPED_INFO("allocation baseline");
    REQUIRE(PublishRulesBundle(base).published);
    const auto before   = snapshot();
    auto       bundle   = renamed(base, "allocation");
    size_t     failures = 0;
    for (int64_t step = 0; step < 1000; ++step)
    {
        const auto result = [&]
        {
            const tests::ScopedPreparationFailure failAt(step);
            return PublishRulesBundle(bundle);
        }();
        if (result.published)
        {
            UNSCOPED_INFO("unexpected preparation checkpoint coverage");
            REQUIRE(failures == 55);
            UNSCOPED_INFO("successful allocation trial did not publish");
            REQUIRE(snapshot() != before);
            UNSCOPED_INFO("earlier bundle survived");
            REQUIRE(PublishRulesBundle(base).published);
            return;
        }
        ++failures;
        UNSCOPED_INFO("allocation locator");
        REQUIRE((result.bundleIndex >= 1 && result.bundleIndex <= 13));
        std::visit([&](const auto& rule)
                   {
                       UNSCOPED_INFO("allocation rule locator");
                       REQUIRE(std::strcmp(result.ruleId, rule.ruleId) == 0);
                   },
                   bundle.rules[result.bundleIndex - 1].request);
        UNSCOPED_INFO("allocation native internal error");
        REQUIRE(result.nativeResult == -13);
        UNSCOPED_INFO("allocation failure changed registries");
        REQUIRE(snapshot() == before);
    }
    UNSCOPED_INFO("allocation loop never succeeded");
    REQUIRE(false);
}

BuildingEffectRuleV2 libraryRule(const char* provider, const char* id, int32_t amount)
{
    BuildingEffectRuleV2 rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider));
    std::memcpy(rule.ruleId, id, std::strlen(id));
    rule.effect = BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD;
    rule.amount = amount;
    return rule;
}

void TestBuildingEffect()
{
    reset();
    auto bundle = fixtures();
    bundle.rules.push_back({ 14, libraryRule("aeshur.library", "gold", 2) });
    UNSCOPED_INFO("Building effect publishes with the other domains");
    REQUIRE(PublishRulesBundle(bundle).published);
    int32_t amount = 0;
    UNSCOPED_INFO("Building effect retained");
    REQUIRE((building_effect_rules::RuleCount() == 1 && building_effect_rules::TryGetEffectiveAmount(amount) && amount == 2));
    const auto published = snapshot();
    UNSCOPED_INFO("identical replay is idempotent");
    REQUIRE((PublishRulesBundle(bundle).published && snapshot() == published));

    auto dirty = bundle.rules.back();
    dirtyTails(dirty);
    UNSCOPED_INFO("dirty ID tails normalize to the retained identity");
    REQUIRE((PublishRulesBundle({ { dirty } }).published && snapshot() == published));

    TerrainYieldRule extraTerrain{};
    UNSCOPED_INFO("extra terrain fixture");
    REQUIRE(BuildTerrainYieldRule({ "aeshur.extra", "hill" }, "hill", "production", "add", 3, extraTerrain) == 0);
    for (const auto& [conflicting, code] : { std::pair<IndexedRequest, int32_t>{ { 2, libraryRule("aeshur.library", "gold", 3) }, BUILDING_EFFECT_RULES_ERR_CONFLICT },
                                             std::pair<IndexedRequest, int32_t>{ { 2, libraryRule("aeshur.other", "gold", std::numeric_limits<int32_t>::max()) }, BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT } })
    {
        RulesBundle failing{ { { 1, extraTerrain }, conflicting } };
        checkFailure(failing, 2, Domain::BuildingEffectRule, code, published);
    }

    RulesBundle composed{ { { 1, libraryRule("aeshur.other", "gold", -5) }, { 2, libraryRule("aeshur.third", "gold", 7) } } };
    UNSCOPED_INFO("distinct identities compose");
    REQUIRE(PublishRulesBundle(composed).published);
    UNSCOPED_INFO("additive composition");
    REQUIRE((building_effect_rules::RuleCount() == 3 && building_effect_rules::TryGetEffectiveAmount(amount) && amount == 4));

    reset();
    UNSCOPED_INFO("earlier Building effect");
    REQUIRE(PublishRulesBundle({ { { 1, libraryRule("aeshur.library", "gold", 2) } } }).published);
    const auto  before = snapshot();
    RulesBundle atomic{ { { 1, extraTerrain }, { 2, libraryRule("aeshur.fresh", "gold", 1) } } };
    for (int64_t step = 0; step < 200; ++step)
    {
        const auto result = [&]
        {
            const tests::ScopedPreparationFailure failAt(step);
            return PublishRulesBundle(atomic);
        }();
        if (result.published)
        {
            UNSCOPED_INFO("allocation sweep eventually publishes both domains");
            REQUIRE((snapshot() != before && building_effect_rules::TryGetEffectiveAmount(amount) && amount == 3));
            return;
        }
        UNSCOPED_INFO("allocation failure result");
        REQUIRE(result.nativeResult == BUILDING_EFFECT_RULES_ERR_INTERNAL);
        UNSCOPED_INFO("allocation failure left a partial publication");
        REQUIRE(snapshot() == before);
    }
    UNSCOPED_INFO("Building effect allocation sweep never succeeded");
    REQUIRE(false);
}

WonderEffectRuleV2 wonderRule(const char* provider, const char* id, int32_t effect, int32_t amount)
{
    WonderEffectRuleV2 rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider));
    std::memcpy(rule.ruleId, id, std::strlen(id));
    rule.effect = effect;
    rule.amount = amount;
    return rule;
}

void TestWonderEffect()
{
    reset();
    auto bundle = fixtures();
    bundle.rules.push_back({ 14, libraryRule("aeshur.library", "gold", 2) });
    bundle.rules.push_back({ 15, wonderRule("aeshur.stonehenge", "religion", WONDER_EFFECT_STONEHENGE_RELIGION, 0) });
    UNSCOPED_INFO("Wonder effect publishes with the other domains");
    REQUIRE(PublishRulesBundle(bundle).published);
    UNSCOPED_INFO("Wonder and Building effects retained");
    REQUIRE((wonder_effect_rules::RuleCount() == 1 && wonder_effect_rules::IsStonehengeReligionActive() && building_effect_rules::RuleCount() == 1));
    const auto published = snapshot();
    UNSCOPED_INFO("identical replay is idempotent");
    REQUIRE((PublishRulesBundle(bundle).published && snapshot() == published));

    auto dirty = bundle.rules.back();
    dirtyTails(dirty);
    UNSCOPED_INFO("dirty ID tails normalize to the retained identity");
    REQUIRE((PublishRulesBundle({ { dirty } }).published && snapshot() == published));

    TerrainYieldRule extraTerrain{};
    UNSCOPED_INFO("extra terrain fixture");
    REQUIRE(BuildTerrainYieldRule({ "aeshur.extra", "hill" }, "hill", "production", "add", 3, extraTerrain) == 0);
    const std::pair<WonderEffectRuleV2, int32_t> failures[] = {
        { wonderRule("aeshur.stonehenge", "religion-culture", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE, 5), WONDER_EFFECT_RULES_ERR_UNAVAILABLE },
        { wonderRule("aeshur.stonehenge", "religion", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE, 0), WONDER_EFFECT_RULES_ERR_UNAVAILABLE },
        { wonderRule("aeshur.stonehenge", "religion", WONDER_EFFECT_STONEHENGE_RELIGION, 1), WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT },
        { wonderRule("aeshur.fresh", "religion", 3, 0), WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT },
    };
    for (const auto& [failing, code] : failures)
    {
        RulesBundle mixed{ { { 1, extraTerrain }, { 2, libraryRule("aeshur.fresh", "gold", 1) }, { 3, failing } } };
        checkFailure(mixed, 3, Domain::WonderEffectRule, code, published);
    }

    RulesBundle composed{ { { 1, wonderRule("aeshur.other", "religion", WONDER_EFFECT_STONEHENGE_RELIGION, 0) } } };
    UNSCOPED_INFO("distinct identities are retained");
    REQUIRE((PublishRulesBundle(composed).published && wonder_effect_rules::RuleCount() == 2));

    reset();
    UNSCOPED_INFO("earlier Building effect");
    REQUIRE(PublishRulesBundle({ { { 1, libraryRule("aeshur.library", "gold", 2) } } }).published);
    const auto  before = snapshot();
    RulesBundle atomic{ { { 1, extraTerrain }, { 2, wonderRule("aeshur.fresh", "religion", WONDER_EFFECT_STONEHENGE_RELIGION, 0) } } };
    for (int64_t step = 0; step < 200; ++step)
    {
        const auto result = [&]
        {
            const tests::ScopedPreparationFailure failAt(step);
            return PublishRulesBundle(atomic);
        }();
        if (result.published)
        {
            UNSCOPED_INFO("allocation sweep eventually publishes both domains");
            REQUIRE((snapshot() != before && wonder_effect_rules::RuleCount() == 1));
            return;
        }
        UNSCOPED_INFO("allocation failure result");
        REQUIRE(result.nativeResult == WONDER_EFFECT_RULES_ERR_INTERNAL);
        UNSCOPED_INFO("allocation failure left a partial publication");
        REQUIRE(snapshot() == before);
    }
    UNSCOPED_INFO("Wonder effect allocation sweep never succeeded");
    REQUIRE(false);
}

void TestBuilders()
{
    checkFixtureBuilders();
    auto             bundle = fixtures();
    TerrainYieldRule terrain{};
    UNSCOPED_INFO("int32 min");
    REQUIRE((BuildTerrainYieldRule({ "provider", "rule" }, "hill", "production", "add", std::numeric_limits<int32_t>::min(), terrain) == 0 &&
             terrain.value == std::numeric_limits<int32_t>::min()));
    UNSCOPED_INFO("int32 max");
    REQUIRE((BuildTerrainYieldRule({ "provider", "rule" }, "hill", "production", "add", std::numeric_limits<int32_t>::max(), terrain) == 0 &&
             terrain.value == std::numeric_limits<int32_t>::max()));
    const auto original = terrain;
    for (std::string_view token : { "Hill", " hill", "3", "plains", "" })
    {
        UNSCOPED_INFO("unknown token");
        REQUIRE(BuildTerrainYieldRule({ "provider", "rule" }, token, "production", "add", 1, terrain) == TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
        UNSCOPED_INFO("failed builder changed output");
        REQUIRE(std::memcmp(&terrain, &original, sizeof(terrain)) == 0);
    }
    const std::string longest(63, 'a');
    const std::string oversized(64, 'a');
    const std::string nul("a\0b", 3);
    UNSCOPED_INFO("maximum IDs");
    REQUIRE(BuildTerrainYieldRule({ longest, longest }, "hill", "production", "add", 2, terrain) == 0);
    for (std::string_view id : { std::string_view(oversized), std::string_view(nul), std::string_view("Bad"), std::string_view("") })
    {
        UNSCOPED_INFO("invalid provider ID");
        REQUIRE(BuildTerrainYieldRule({ id, "rule" }, "hill", "production", "add", 1, terrain) != 0);
        UNSCOPED_INFO("invalid rule ID");
        REQUIRE(BuildTerrainYieldRule({ "provider", id }, "hill", "production", "add", 1, terrain) != 0);
    }
    UnitCombatRule combat{};
    UNSCOPED_INFO("editable combat");
    REQUIRE((BuildUnitCombatRule({ "p", "r" }, { "aztec", "warrior", "jaguar_warrior" }, "forest", "attack", 25, combat) == 0 && combat.percentageDelta == 25));
    UNSCOPED_INFO("mismatched selectors");
    REQUIRE(BuildUnitCombatRule({ "p", "r" }, { "roman", "warrior", "jaguar_warrior" }, "forest", "attack", 25, combat) != 0);
    UNSCOPED_INFO("wrong property domain");
    REQUIRE(BuildUnitCombatRule({ "p", "r" }, { "aztec", "warrior", "jaguar_warrior" }, "forest", "base_defense", 25, combat) != 0);
    NationSelectTextRule text{};
    const TextSelectors  unit{ "unique_unit", "greek", {}, {}, "phalanx", "hoplite", "unit" };
    UNSCOPED_INFO("maximum text");
    REQUIRE(BuildNationSelectTextRule({ "p", "r" }, unit, std::string(255, 'x'), text) == 0);
    for (const auto& invalid : { std::string(256, 'x'), nul, std::string("line\nbreak"), std::string() })
    {
        UNSCOPED_INFO("invalid text");
        REQUIRE(BuildNationSelectTextRule({ "p", "r" }, unit, invalid, text) != 0);
    }
    auto wrong      = unit;
    wrong.unlockEra = "ancient";
    UNSCOPED_INFO("cross-surface fields");
    REQUIRE(BuildNationSelectTextRule({ "p", "r" }, wrong, "text", text) != 0);
    wrong          = { "era_ability", "mongolian", "ancient", "knowledge_of_horseback_riding", {}, {}, {} };
    wrong.identity = "keshik";
    UNSCOPED_INFO("era unit fields");
    REQUIRE(BuildNationSelectTextRule({ "p", "r" }, wrong, "text", text) != 0);
    wrong.identity  = {};
    wrong.unlockEra = "modern";
    UNSCOPED_INFO("unreviewed era");
    REQUIRE(BuildNationSelectTextRule({ "p", "r" }, wrong, "text", text) != 0);
    wrong             = unit;
    wrong.displayForm = "army";
    UNSCOPED_INFO("unreviewed display form");
    REQUIRE(BuildNationSelectTextRule({ "p", "r" }, wrong, "text", text) != 0);

    city_founding_unit::Rule militia;
    std::memset(&militia, 0xFF, sizeof(militia));
    UNSCOPED_INFO("founding builder initializes request");
    REQUIRE((BuildCityFoundingUnitRule({ "test.founding", "militia" }, true, militia) == 0 && militia.structSize == sizeof(militia)));
    for (const int32_t reserved : militia.reserved)
    {
        UNSCOPED_INFO("founding builder clears reserved fields");
        REQUIRE(reserved == 0);
    }
    bundle.rules.push_back({ bundle.rules.size() + 1, militia });

    // Every bundle domain must reject the same malformed native request as Register*.
    for (size_t i = 0; i < bundle.rules.size(); ++i)
    {
        auto invalid = bundle;
        std::visit([](auto& rule)
                   {
                       rule.reserved[0] = 1;
                   },
                   invalid.rules[i].request);
        const auto before = snapshot();
        const auto result = PublishRulesBundle(invalid);
        UNSCOPED_INFO("reserved-field validation");
        REQUIRE((!result.published && result.bundleIndex == i + 1));
        UNSCOPED_INFO("validation partially published");
        REQUIRE(snapshot() == before);
    }
}

void TestReaders()
{
    const auto                                  base   = fixtures();
    const std::array<int32_t (*)(uint32_t*), 8> counts = {
        GetTerrainYieldRuleCount, GetUnitCombatRuleCount, GetUniqueUnitScalarRuleCount, GetUnitMovementRuleCount, GetUnitEffectRuleCount, GetUnitProductionCostRuleCount, GetEraAbilityRuleCount, GetNationSelectTextRuleCount
    };
    const std::array<uint32_t, 8> perBundle    = { 1, 2, 1, 1, 1, 1, 1, 5 };
    std::atomic<bool>             stop         = false;
    std::atomic<uint64_t>         observations = 0;
    std::atomic<uint32_t>         ready        = 0;
    // Catch2 assertions stay on this thread; a reader records the first problem.
    std::atomic<int> readerProblem = 0;

    enum ReaderProblem : int
    {
        kNone,
        kCountFailed,
        kPartialRegistry,
        kEvaluationFailed,
        kPartialEvaluation,
    };

    auto note = [&](ReaderProblem problem)
    {
        int expectedProblem = kNone;
        readerProblem.compare_exchange_strong(expectedProblem, problem);
    };
    auto reader = [&]
    {
        ++ready;
        while (!stop.load())
        {
            for (size_t i = 0; i < counts.size(); ++i)
            {
                uint32_t count = 0;
                if (counts[i](&count) != 0)
                {
                    note(kCountFailed);
                }
                else if (count % (perBundle[i] * 3) != 0)
                {
                    note(kPartialRegistry);
                }
            }
            UnitCombatEvaluation evaluation{};
            if (!unit_combat_rules::TryEvaluate(CIVILIZATION_AZTEC, UNIT_TYPE_WARRIOR, UNIT_IDENTITY_JAGUAR_WARRIOR, TERRAIN_FOREST, UNIT_COMBAT_ATTACK, evaluation))
            {
                note(kEvaluationFailed);
            }
            else if (evaluation.additiveCount % 3 != 0)
            {
                note(kPartialEvaluation);
            }
            ++observations;
        }
    };
    std::thread first(reader);
    std::thread second(reader);

    // Joins the readers however this test ends, so a failed REQUIRE cannot
    // leave a joinable std::thread behind.
    struct JoinReaders
    {
        std::atomic<bool>& stop;
        std::thread&       first;
        std::thread&       second;

        ~JoinReaders()
        {
            stop = true;
            if (first.joinable())
            {
                first.join();
            }
            if (second.joinable())
            {
                second.join();
            }
        }
    } joinReaders{ stop, first, second };

    while (ready.load() != 2 || observations.load() == 0)
    {
        std::this_thread::yield();
    }
    for (size_t round = 0; round < 40; ++round)
    {
        RulesBundle bundle;
        for (size_t copy = 0; copy < 3; ++copy)
        {
            auto next = renamed(base, "reader-" + std::to_string(round) + "-" + std::to_string(copy));
            bundle.rules.insert(bundle.rules.end(), next.rules.begin(), next.rules.end());
        }
        UNSCOPED_INFO("concurrent publication");
        REQUIRE(PublishRulesBundle(bundle).published);
    }
    stop = true;
    first.join();
    second.join();
    UNSCOPED_INFO("concurrent reader problem " << readerProblem.load() << " (1 count call, 2 partial registry, 3 evaluation, 4 partial evaluation)");
    CHECK(readerProblem.load() == kNone);
    UNSCOPED_INFO("readers did not run");
    REQUIRE(observations.load() > 0);
    for (size_t i = 0; i < counts.size(); ++i)
    {
        uint32_t count = 0;
        UNSCOPED_INFO("final reader count");
        REQUIRE((counts[i](&count) == 0 && count == perBundle[i] * 120));
    }
}

void TestCityFoundingUnit()
{
    reset();
    const auto existing = fixtures();
    UNSCOPED_INFO("existing kinds before city founding unit");
    REQUIRE(PublishRulesBundle(existing).published);
    const auto               before = snapshot();
    city_founding_unit::Rule militia{};
    UNSCOPED_INFO("founding builder");
    REQUIRE(BuildCityFoundingUnitRule({ "test.founding", "militia" }, true, militia) == 0);
    auto mixed = renamed(existing, "founding.bundle");
    mixed.rules.push_back({ 77, militia });
    bool succeeded = false;
    for (int64_t step = 0; step < 200; ++step)
    {
        const auto result = [&]
        {
            const tests::ScopedPreparationFailure failAt(step);
            return PublishRulesBundle(mixed);
        }();
        if (result.published)
        {
            succeeded = true;
            break;
        }
        UNSCOPED_INFO("Militia preparation failure rolled back every kind");
        REQUIRE((snapshot() == before && !city_founding_unit::IsEnabled()));
    }
    UNSCOPED_INFO("mixed founding bundle eventually publishes");
    REQUIRE((succeeded && city_founding_unit::IsEnabled() && city_founding_unit::RuleCount() == 1));
    const auto accepted = snapshot();
    auto       replay   = mixed;
    dirtyTails(replay.rules.back());
    std::reverse(replay.rules.begin(), replay.rules.end());
    UNSCOPED_INFO("normalized reversed Militia replay preserves all kinds");
    REQUIRE((PublishRulesBundle(replay).published && snapshot() == accepted));
    auto conflict = renamed(existing, "failed.bundle");
    auto other    = militia;
    std::strcpy(other.providerId, "other.founding");
    conflict.rules.push_back({ 78, other });
    checkFailure(conflict, 78, Domain::CityFoundingUnitRule, city_founding_unit::kConflict, accepted);
    other        = militia;
    other.enable = 0;
    checkFailure({ { { 79, other } } }, 79, Domain::CityFoundingUnitRule, city_founding_unit::kConflict, accepted);
    reset();
    auto internal = mixed;
    internal.rules.push_back({ 80, other });
    checkFailure(internal, 80, Domain::CityFoundingUnitRule, city_founding_unit::kConflict, snapshot());
    UNSCOPED_INFO("in-bundle conflict stayed default off");
    REQUIRE(!city_founding_unit::IsEnabled());
    UNSCOPED_INFO("explicit disabled founding rule");
    REQUIRE((PublishRulesBundle({ { { 1, other } } }).published && !city_founding_unit::IsEnabled()));
}

} // namespace

TEST_CASE("rules_bundle_parity", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestParity();
}

TEST_CASE("rules_bundle_builders", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestBuilders();
}

TEST_CASE("rules_bundle_rollback", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestRollback();
}

TEST_CASE("rules_bundle_replay", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestReplay();
}

TEST_CASE("rules_bundle_allocation", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestAllocation();
}

TEST_CASE("rules_bundle_readers", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestReaders();
}

TEST_CASE("rules_bundle_buildingeffect", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestBuildingEffect();
}

TEST_CASE("rules_bundle_wondereffect", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestWonderEffect();
}

TEST_CASE("rules_bundle_city_founding_unit", "[ctest][rules-bundle]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestCityFoundingUnit();
}
