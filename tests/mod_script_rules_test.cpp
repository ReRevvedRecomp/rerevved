#include "allocation_harness.hpp"
#include "building_effect_rules_registry.h"
#include "mod_script_rules.h"
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
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <vector>

using namespace rerevved;
using namespace rerevved::rules_bundle;
using namespace rerevved::tests::rules_fixtures;
namespace allocation = rerevved::tests::allocation;

namespace
{

// Complete default package scripts, executed by the SDK Lua runtime.
const std::array<std::string_view, 8> scripts = {
    R"lua(local production_bonus = 1

return {
    provider_id = "aeshur.hills-production",
    rules = {
        {
            id = "hills-production", kind = "terrain_yield",
            terrain = "hill", component = "production",
            operation = "add", value = production_bonus,
        },
    },
}
)lua",
    R"lua(local forest_bonus = 50
local sign = forest_bonus >= 0 and "+" or ""

return {
    provider_id = "aeshur.jaguar-woodsman",
    rules = {
        {
            id = "jaguar-warrior-forest-attack", kind = "unit_combat",
            civilization = "aztec", base_unit_type = "warrior",
            identity = "jaguar_warrior", terrain = "forest",
            property = "attack", percentage_delta = forest_bonus,
        },
        {
            id = "jaguar-warrior-forest-defense", kind = "unit_combat",
            civilization = "aztec", base_unit_type = "warrior",
            identity = "jaguar_warrior", terrain = "forest",
            property = "defense", percentage_delta = forest_bonus,
        },
        {
            id = "jaguar-woodsman-text", kind = "nation_select_text",
            surface = "unique_unit", civilization = "aztec",
            base_unit_type = "warrior", identity = "jaguar_warrior",
            display_form = "unit",
            text = "Jaguar Warrior - Warrior with " .. sign .. forest_bonus
                .. "% Attack and Defense in Forest",
        },
    },
}
)lua",
    R"lua(local defense_bonus = 1
local sign = defense_bonus >= 0 and "+" or ""

return {
    provider_id = "aeshur.cataphracts-defense",
    rules = {
        {
            id = "cataphract-defense", kind = "unique_unit_scalar",
            civilization = "roman", base_unit_type = "knights",
            identity = "cataphract", property = "base_defense",
            operation = "add", value = defense_bonus,
        },
        {
            id = "cataphract-defense-text", kind = "nation_select_text",
            surface = "unique_unit", civilization = "roman",
            base_unit_type = "knights", identity = "cataphract",
            display_form = "unit",
            text = "Cataphract - Knight with " .. sign .. defense_bonus
                .. " base Defense",
        },
    },
}
)lua",
    R"lua(local movement_bonus = 1
local sign = movement_bonus >= 0 and "+" or ""

return {
    provider_id = "aeshur.keshik-movement",
    rules = {
        {
            id = "keshik-movement", kind = "unit_movement",
            civilization = "mongolian", base_unit_type = "horsemen",
            identity = "keshik", value = movement_bonus,
        },
        {
            id = "keshik-movement-text", kind = "nation_select_text",
            surface = "unique_unit", civilization = "mongolian",
            base_unit_type = "horsemen", identity = "keshik",
            display_form = "unit",
            text = "Keshik - Horseman with " .. sign .. movement_bonus
                .. " movement",
        },
    },
}
)lua",
    R"lua(local production_delta = -30

return {
    provider_id = "aeshur.hoplite-loyalty",
    rules = {
        {
            id = "hoplite-loyalty", kind = "unit_effect",
            civilization = "greek", base_unit_type = "phalanx",
            identity = "hoplite", effect = "creation_loyalty",
        },
        {
            id = "hoplite-production-cost", kind = "unit_production_cost",
            civilization = "greek", base_unit_type = "phalanx",
            identity = "hoplite", percentage_delta = production_delta,
        },
        {
            id = "hoplite-loyalty-text", kind = "nation_select_text",
            surface = "unique_unit", civilization = "greek",
            base_unit_type = "phalanx", identity = "hoplite",
            display_form = "unit",
            text = "Hoplite - Starts with Loyalty; costs 10 Production",
        },
    },
}
)lua",
    R"lua(return {
    provider_id = "aeshur.mongol-horseback",
    rules = {
        {
            id = "mongol-ancient-horseback-riding",
            kind = "era_technology_ability", civilization = "mongolian",
            unlock_era = "ancient",
            technology = "horseback_riding",
        },
        {
            id = "mongol-ancient-horseback-riding-text",
            kind = "nation_select_text", surface = "era_ability",
            civilization = "mongolian", unlock_era = "ancient",
            ability = "knowledge_of_horseback_riding",
            text = "Knowledge of Horseback Riding",
        },
    },
}
)lua",
    R"lua(return {
    provider_id = "aeshur.stonehenge-religion",
    rules = {
        {
            id = "stonehenge-religion", kind = "wonder_completion_technology",
            wonder = "stonehenge", technology = "religion",
        },
    },
}
)lua",
    R"lua(local gold_addition = 1

return {
    provider_id = "aeshur.chinese-library-gold",
    rules = {
        {
            id = "chinese-library-fixed-gold", kind = "building_yield",
            civilization = "china", building = "library", yield = "gold", amount = gold_addition,
        },
    },
}
)lua",
};

using rex::system::ModScriptPublishResult;
using rex::system::ModScriptStage;
using rex::system::ModScriptValue;

const std::array<std::vector<std::string>, 8> domains = { {
    { "terrain_yield" },
    { "unit_combat", "nation_select_text" },
    { "unique_unit_scalar", "nation_select_text" },
    { "unit_movement", "nation_select_text" },
    { "unit_effect", "unit_production_cost", "nation_select_text" },
    { "era_technology_ability", "nation_select_text" },
    { "wonder_completion_technology" },
    { "building_yield" },
} };

auto host = CreateModScriptRulesHost();

ModScriptValue decode(std::string_view source)
{
    auto result = rex::system::RunModScript("main.lua", source);
    UNSCOPED_INFO("SDK script execution: " << rex::system::ToString(result.error.stage) << ": " << result.error.message);
    REQUIRE(result.ok);
    return std::move(result.value);
}

ModScriptPublishResult publish(const ModScriptValue& value, size_t package = 0)
{
    return host->Publish({ "package", "main.lua", "rerevved.rules", 1, domains[package], value });
}

ModScriptPublishResult run(size_t package, std::string_view source)
{
    return publish(decode(source), package);
}

std::string mutate(size_t package, std::string_view edit)
{
    return "local function declarations()\n" + std::string(scripts[package]) +
           "\nend\nlocal bundle = declarations()\n" + std::string(edit) + "\nreturn bundle";
}

void rejected(size_t package, std::string_view edit, ModScriptStage stage = ModScriptStage::kSchema, std::string_view diagnostic = {})
{
    const auto before = snapshot();
    const auto result = run(package, mutate(package, edit));
    if (result.published || result.stage != stage || result.message.find(diagnostic) == std::string::npos)
    {
        UNSCOPED_INFO("wrong rejection or diagnostic; edit: " << edit << "; result: " << rex::system::ToString(result.stage) << ": " << result.message);
        REQUIRE(false);
    }
    UNSCOPED_INFO("rejected script changed a registry");
    REQUIRE(snapshot() == before);
}

RulesBundle packageFixture(size_t package)
{
    const std::array<size_t, 7> offsets{ 0, 1, 4, 6, 8, 11, 13 };
    const auto                  all = fixtures();
    RulesBundle                 result;
    for (size_t i = offsets[package]; i < offsets[package + 1]; ++i)
    {
        result.rules.push_back({ result.rules.size() + 1, all.rules[i].request });
    }
    return result;
}

void parity(size_t package, const RulesBundle& expectedBundle, std::string_view source)
{
    reset();
    UNSCOPED_INFO("native expected publication");
    REQUIRE(PublishRulesBundle(expectedBundle).published);
    const auto native = snapshot();
    reset();
    const auto result = run(package, source);
    UNSCOPED_INFO("Lua publication: " << result.message);
    REQUIRE(result.published);
    UNSCOPED_INFO("complete normalized native declaration/readback parity");
    REQUIRE(snapshot() == native);
}

void evaluateHills(int32_t bonus)
{
    for (int32_t native : { -3, 0, 1, 9, 100 })
    {
        TerrainYieldQuery query{};
        query.structSize  = sizeof(query);
        query.terrain     = TERRAIN_HILL;
        query.component   = TERRAIN_YIELD_PRODUCTION;
        query.nativeValue = native;
        TerrainYieldEvaluation result{};
        UNSCOPED_INFO("Hills evaluation");
        REQUIRE(EvaluateTerrainYield(&query, &result, sizeof(result)) == 0);
        UNSCOPED_INFO("Hills native + bonus parity");
        REQUIRE((result.nativeValue == native && result.finalValue == native + bonus &&
                 result.additiveCount == 1 && result.replacementCount == 0 && result.statusFlags == 0));
    }
}

void TestScripts()
{
    const auto contracts = host->contracts();
    UNSCOPED_INFO("contract descriptor");
    REQUIRE((contracts.size() == 1 && contracts[0].api == "rerevved.rules" && contracts[0].version == 1));
    const std::vector<std::string> implemented{
        "terrain_yield", "unit_combat", "unique_unit_scalar", "unit_movement", "unit_effect", "unit_production_cost", "era_technology_ability", "nation_select_text", "building_yield", "wonder_completion_technology", "city_founding_unit"
    };
    UNSCOPED_INFO("exactly eleven bound kinds");
    REQUIRE(contracts[0].domains == implemented);
    for (size_t package = 0; package < 6; ++package)
    {
        parity(package, packageFixture(package), scripts[package]);
        if (package == 0)
            evaluateHills(1);
        const auto before = snapshot();
        UNSCOPED_INFO("identical replay idempotence");
        REQUIRE((run(package, scripts[package]).published && snapshot() == before));
    }
    reset();
    for (size_t package = 0; package < 6; ++package)
    {
        UNSCOPED_INFO("combined script publication");
        REQUIRE(run(package, scripts[package]).published);
    }
    const auto combined = snapshot();
    reset();
    UNSCOPED_INFO("thirteen native declarations");
    REQUIRE(PublishRulesBundle(fixtures()).published);
    UNSCOPED_INFO("all thirteen declarations coexist");
    REQUIRE(snapshot() == combined);
}

void TestEdits()
{
    struct Edit
    {
        size_t      package;
        const char* from;
        const char* to;
        int32_t     value;
        const char* text;
    };

    const Edit edits[] = {
        { 0, "production_bonus = 1", "production_bonus = 2", 2, "" },
        { 1, "forest_bonus = 50", "forest_bonus = 25", 25, "Jaguar Warrior - Warrior with +25% Attack and Defense in Forest" },
        { 2, "defense_bonus = 1", "defense_bonus = 2", 2, "Cataphract - Knight with +2 base Defense" },
        { 3, "movement_bonus = 1", "movement_bonus = 2", 2, "Keshik - Horseman with +2 movement" },
        { 4, "production_delta = -30", "production_delta = -20", -20, "Hoplite - Starts with Loyalty; costs 12 Production" },
    };
    for (const auto& edit : edits)
    {
        auto expectedBundle = packageFixture(edit.package);
        for (auto& entry : expectedBundle.rules)
        {
            std::visit([&](auto& rule)
                       {
                           using T = std::decay_t<decltype(rule)>;
                           if constexpr (std::is_same_v<T, TerrainYieldRule> || std::is_same_v<T, UniqueUnitScalarRule> || std::is_same_v<T, UnitMovementRule>)
                               rule.value = edit.value;
                           else if constexpr (std::is_same_v<T, UnitCombatRule> || std::is_same_v<T, UnitProductionCostRule>)
                               rule.percentageDelta = edit.value;
                           else if constexpr (std::is_same_v<T, NationSelectTextRule>)
                           {
                               std::memset(rule.text, 0, sizeof(rule.text));
                               std::memcpy(rule.text, edit.text, std::strlen(edit.text));
                           }
                       },
                       entry.request);
        }
        std::string source(scripts[edit.package]);
        source.replace(source.find(edit.from), std::strlen(edit.from), edit.to);
        if (edit.package == 4)
        {
            constexpr std::string_view oldText = "Hoplite - Starts with Loyalty; costs 10 Production";
            source.replace(source.find(oldText), oldText.size(), edit.text);
        }
        parity(edit.package, expectedBundle, source);
        if (edit.package == 0)
            evaluateHills(2);
    }
}

ModScriptValue& ownedField(ModScriptValue& table, std::string_view name)
{
    for (auto& entry : table.entries)
        if (entry.key.string == name)
            return entry.value;
    UNSCOPED_INFO("owned field missing");
    REQUIRE(false);
    return table;
}

void rejectedOwned(size_t package, const ModScriptValue& value)
{
    const auto before = snapshot();
    const auto result = publish(value, package);
    UNSCOPED_INFO("owned schema rejection: " << result.message);
    REQUIRE((!result.published && result.stage == ModScriptStage::kSchema));
    UNSCOPED_INFO("rejected owned value changed a registry");
    REQUIRE(snapshot() == before);
}

void TestSchema()
{
    reset();
    std::set<std::string> schemas;
    for (size_t package = 0; package < 8; ++package)
    {
        auto        root  = decode(scripts[package]);
        const auto& rules = ownedField(root, "rules");
        for (size_t i = 0; i < rules.entries.size(); ++i)
        {
            auto        record = rules.entries[i].value;
            std::string schema = ownedField(record, "kind").string;
            if (schema == "nation_select_text")
            {
                schema += ":" + ownedField(record, "surface").string;
            }
            if (!schemas.insert(schema).second)
            {
                continue;
            }
            CAPTURE(schema);
            auto  changed       = root;
            auto& entries       = ownedField(changed, "rules").entries[i].value.entries;
            auto  extra         = record.entries.front();
            extra.key.string    = "extra";
            extra.value         = {};
            extra.value.type    = ModScriptValue::Type::kBoolean;
            extra.value.boolean = true;
            entries.push_back(extra);
            rejectedOwned(package, changed);
            for (const auto& entry : record.entries)
            {
                CAPTURE(entry.key.string);
                auto  missing = root;
                auto& fields  = ownedField(missing, "rules").entries[i].value.entries;
                std::erase_if(fields, [&](const auto& field)
                              {
                                  return field.key.string == entry.key.string;
                              });
                rejectedOwned(package, missing);
                // The Lua numeric-string case below keeps its exact diagnostic.
                if (package == 0 && entry.key.string == "value")
                {
                    continue;
                }
                auto  wrong = root;
                auto& value = ownedField(ownedField(wrong, "rules").entries[i].value, entry.key.string);
                value       = {};
                if (entry.value.type == ModScriptValue::Type::kString)
                {
                    value.type    = ModScriptValue::Type::kInteger;
                    value.integer = 1;
                }
                else
                {
                    value.type   = ModScriptValue::Type::kString;
                    value.string = "1";
                }
                rejectedOwned(package, wrong);
            }
        }
    }
    const char* rootCases[] = {
        "bundle = 1",
        "bundle.provider_id = nil",
        "bundle.rules = nil",
        "bundle.extra = true",
        "bundle[1] = true",
        "bundle.provider_id = 1",
        "bundle.rules = 1",
        "bundle.rules = {}",
        "local rule = bundle.rules[1]; for i = 2,257 do bundle.rules[i] = rule end",
        "bundle.rules[3] = bundle.rules[1]",
        "bundle.rules.extra = bundle.rules[1]",
        "bundle.rules[0] = bundle.rules[1]",
        "bundle.rules[1] = false",
        "bundle.rules[1][1] = true",
        "bundle['bad\\0key'] = 1",
        "bundle.rules[1]['bad\\0key'] = 1",
    };
    for (const auto edit : rootCases)
        rejected(0, edit);
    rejected(0, "bundle.rules[1].kind = 'unknown'", ModScriptStage::kSupport, "field kind: unsupported");
    const auto                     hills = decode(scripts[0]);
    const std::vector<std::string> wrongDomains{ "unit_combat" };
    auto                           result = host->Publish({ "package", "main.lua", "rerevved.rules", 1, wrongDomains, hills });
    UNSCOPED_INFO("undeclared returned kind");
    REQUIRE((!result.published && result.stage == ModScriptStage::kSchema && result.message.find("not declared") != std::string::npos));
    for (auto request : {
             rex::system::ModScriptPublishRequest{ "package", "main.lua", "other", 1, domains[0], hills },
             rex::system::ModScriptPublishRequest{ "package", "main.lua", "rerevved.rules", 2, domains[0], hills } })
    {
        result = host->Publish(request);
        UNSCOPED_INFO("api/version mismatch");
        REQUIRE((!result.published && result.stage == ModScriptStage::kSupport));
    }
    for (const auto value : { "1.5", "2147483648", "-2147483649", "true", "{}" })
        rejected(0, "bundle.rules[1].value = " + std::string(value));
    rejected(0, "bundle.rules[1].value = '1'", ModScriptStage::kSchema, "rules[1] hills-production: field value: expected integer, got string");
    for (const auto value : { "2147483647", "-2147483648", "2.0", "-2.0" })
    {
        reset();
        UNSCOPED_INFO("int32 endpoints and integral floats accepted");
        REQUIRE(run(0, mutate(0, "bundle.rules[1].value = " + std::string(value))).published);
    }
    for (const auto fieldName : { "bundle.provider_id", "bundle.rules[1].id" })
    {
        for (const auto id : { "", "UPPER", "-start", ".start", "_start", "white space", "a/b", "a:b", "a\\0b", "a\\255" })
            rejected(0, std::string(fieldName) + " = '" + id + "'");
        rejected(0, std::string(fieldName) + " = '" + std::string(64, 'a') + "'");
        reset();
        UNSCOPED_INFO("63-byte ID with permitted charset");
        REQUIRE(run(0, mutate(0, std::string(fieldName) + " = '0._-" + std::string(59, 'a') + "'")).published);
    }
    for (const auto text : { "", "a\\0b", "a\\nb", "a\\127", "a\\255" })
        rejected(1, "bundle.rules[3].text = '" + std::string(text) + "'");
    rejected(1, "bundle.rules[3].text = '" + std::string(256, 'a') + "'");
    for (size_t length : { 1u, 255u })
    {
        reset();
        UNSCOPED_INFO("text length boundaries");
        REQUIRE(run(1, mutate(1, "bundle.rules[3].text = '" + std::string(length, '~') + "'")).published);
    }
    rejected(1, "bundle.rules[3].unlock_era = 'ancient'");
    rejected(1, "bundle.rules[3].ability = 'knowledge_of_horseback_riding'");
    rejected(1, "bundle.rules[3].display_form = nil");
    rejected(1, "bundle.rules[3].display_form = 'army'");
    rejected(1, "bundle.rules[3].surface = 'leader'");
    for (const auto fieldName : { "base_unit_type", "identity", "display_form" })
        rejected(5, "bundle.rules[2]." + std::string(fieldName) + " = 'unused'");
    for (const auto fieldName : { "civilization", "unlock_era", "ability" })
        rejected(5, "bundle.rules[2]." + std::string(fieldName) + " = nil");
    const std::array<std::vector<std::string>, 6> tokens = { {
        { "terrain", "component", "operation" },
        { "civilization", "base_unit_type", "identity", "terrain", "property" },
        { "civilization", "base_unit_type", "identity", "property", "operation" },
        { "civilization", "base_unit_type", "identity" },
        { "civilization", "base_unit_type", "identity", "effect" },
        { "civilization", "unlock_era", "technology" },
    } };
    for (size_t package = 0; package < tokens.size(); ++package)
        for (const auto& name : tokens[package])
            rejected(package, "bundle.rules[1]." + name + " = 'unreviewed'");
    rejected(1, "bundle.rules[1].civilization = 'roman'");
    rejected(1, "bundle.rules[1].terrain = 'hill'");
    rejected(5, "bundle.rules[2].ability = 'unreviewed'");
    reset();
    UNSCOPED_INFO("256 dense records accepted and deduplicated");
    REQUIRE(run(0, mutate(0, "local rule = bundle.rules[1]; for i = 2,256 do bundle.rules[i] = rule end")).published);
}

void TestParameterizedKinds()
{
    reset();
    REQUIRE(run(0, scripts[0]).published);
    rejected(5, "bundle.rules[1].technology = 'writing'", ModScriptStage::kSchema, "field technology: unsupported value; supported values: horseback_riding");
    rejected(6, "bundle.rules[1].technology = 'horseback_riding'", ModScriptStage::kSchema, "field technology: unsupported value; supported values: religion");
    rejected(7, "bundle.rules[1].civilization = 'roman'", ModScriptStage::kSchema, "field civilization: unsupported value; supported values: china");
    rejected(5, "bundle.rules[1].kind = 'era_ability_replacement'", ModScriptStage::kSupport, "field kind: unsupported kind era_ability_replacement");
    rejected(6, "bundle.rules[1].kind = 'wonder_effect'", ModScriptStage::kSupport, "field kind: unsupported kind wonder_effect");
    rejected(7, "bundle.rules[1].kind = 'building_effect'", ModScriptStage::kSupport, "field kind: unsupported kind building_effect");
    rejected(5, "bundle.rules[1].replacement_ability = 'knowledge_of_horseback_riding'", ModScriptStage::kSchema, "field replacement_ability: unknown field");
    rejected(6, "bundle.rules[1].amount = 0", ModScriptStage::kSchema, "field amount: unknown field");
    rejected(7, "bundle.rules[1].effect = 'chinese_library_fixed_gold'", ModScriptStage::kSchema, "field effect: unknown field");
    rejected(7, "bundle.rules[1].amount = 1.5", ModScriptStage::kSchema, "field amount:");
}

void TestBuildingEffectPublication()
{
    reset();
    UNSCOPED_INFO("Building-only bundle publishes");
    REQUIRE(run(7, scripts[7]).published);
    int32_t amount = 0;
    UNSCOPED_INFO("Building rule retained with its amount");
    REQUIRE((building_effect_rules::RuleCount() == 1 && building_effect_rules::TryGetEffectiveAmount(amount) && amount == 1));
    const building_effect_rules::CacheQuery query{ 1000, 0x40001000u, building_effect_rules::kModeCacheOnly, 3, 3, CIVILIZATION_CHINESE, true, false, 10, 0, 0, 0 };
    const auto                              decision = building_effect_rules::EvaluateCachePhase(query);
    UNSCOPED_INFO("published Building rule is evaluated");
    REQUIRE((decision.applyCache && decision.newCache == 11));
    const auto published = snapshot();
    UNSCOPED_INFO("identical Building replay is idempotent");
    REQUIRE((run(7, scripts[7]).published && snapshot() == published));

    // A different amount for the same identity conflicts across bundles.
    const std::string conflict = "rules[1] chinese-library-fixed-gold: conflicting rule identity; native result -12; no rules from this bundle published";
    UNSCOPED_INFO("exact conflict diagnostic");
    REQUIRE(run(7, mutate(7, "bundle.rules[1].amount = 2")).message == conflict);
    rejected(7, "bundle.rules[1].amount = 2", ModScriptStage::kPublish, "conflicting rule identity");
    rejected(7, "bundle.rules[1].amount = 0", ModScriptStage::kPublish, "native result -12");

    // Distinct identities add; an out-of-range sum is rejected with nothing retained.
    UNSCOPED_INFO("distinct identity composes");
    REQUIRE((run(7, mutate(7, "bundle.rules[1].id = 'second-gold'; bundle.rules[1].amount = -3")).published &&
             building_effect_rules::TryGetEffectiveAmount(amount) && amount == -2 && building_effect_rules::RuleCount() == 2));
    const auto composed = snapshot();
    rejected(7, "bundle.rules[1].id = 'tiny-gold'; bundle.rules[1].amount = -2147483647", ModScriptStage::kPublish, "native result -10");
    UNSCOPED_INFO("sum overflow retained nothing");
    REQUIRE(snapshot() == composed);

    // Building records publish atomically with the bundle's other domains.
    const std::vector<std::string> mixedDomains{ "terrain_yield", "building_yield", "wonder_completion_technology" };
    const std::string              terrain = "bundle.rules[2] = { id='mixed-terrain', kind='terrain_yield', terrain='hill', component='production', operation='add', value=2 }";
    const auto                     mixed   = [&](const std::string& edit)
    {
        return host->Publish({ "package", "main.lua", "rerevved.rules", 1, mixedDomains, decode(mutate(7, edit)) });
    };
    auto result = mixed(terrain + "; bundle.rules[1].id = 'third-gold'; bundle.rules[1].amount = 5");
    UNSCOPED_INFO("Building and terrain records publish together");
    REQUIRE((result.published && building_effect_rules::TryGetEffectiveAmount(amount) && amount == 3));
    evaluateHills(2);
    const auto both = snapshot();

    std::string otherTerrain = terrain;
    otherTerrain.replace(otherTerrain.find("mixed-terrain"), 13, "other-terrain");
    result = mixed(otherTerrain + "; bundle.rules[1].amount = 2");
    UNSCOPED_INFO("later Building conflict fails the bundle");
    REQUIRE((!result.published && result.stage == ModScriptStage::kPublish && result.message.find("conflicting rule identity") != std::string::npos));
    UNSCOPED_INFO("a Building conflict published no terrain record");
    REQUIRE(snapshot() == both);

    const std::string wonder = "bundle.rules[2] = { id='mixed-wonder', kind='wonder_completion_technology', wonder='stonehenge', technology='writing' }";
    result                   = mixed(wonder + "; bundle.rules[1].id = 'fourth-gold'");
    UNSCOPED_INFO("an unsupported technology rejects the whole bundle");
    REQUIRE((!result.published && result.stage == ModScriptStage::kSchema &&
             result.message == "rules[2] mixed-wonder: field technology: unsupported value; supported values: religion"));
    UNSCOPED_INFO("an unsupported technology published no Building record");
    REQUIRE(snapshot() == both);

    // A failure anywhere leaves the registry as it was.
    for (int64_t checkpoint = 0; checkpoint < 200; ++checkpoint)
    {
        {
            const tests::ScopedPreparationFailure failAt(checkpoint);
            result = mixed(otherTerrain + "; bundle.rules[1].id = 'fifth-gold'; bundle.rules[1].amount = 1");
        }
        if (result.published)
        {
            UNSCOPED_INFO("preparation sweep eventually publishes");
            REQUIRE((building_effect_rules::TryGetEffectiveAmount(amount) && amount == 4));
            return;
        }
        UNSCOPED_INFO("preparation failure left a partial publication");
        REQUIRE((result.stage == ModScriptStage::kPublish && snapshot() == both));
    }
    UNSCOPED_INFO("Building preparation sweep never succeeded");
    REQUIRE(false);
}

std::vector<BuildingEffectRuleV2> observedBuilding;
std::vector<WonderEffectRuleV2>   observedWonder;

template <typename Rule>
Rule expectedEffect(const char* provider, const char* id, int32_t effect, int32_t amount)
{
    Rule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider));
    std::memcpy(rule.ruleId, id, std::strlen(id));
    rule.effect = effect;
    rule.amount = amount;
    return rule;
}

void TestEffectRequests()
{
    reset();
    observedBuilding.clear();
    observedWonder.clear();
    rerevved::ObserveEffectRequestsForTests([](const BuildingEffectRuleV2& rule)
                                            {
                                                observedBuilding.push_back(rule);
                                            },
                                            [](const WonderEffectRuleV2& rule)
                                            {
                                                observedWonder.push_back(rule);
                                            });
    run(6, scripts[6]);
    run(7, scripts[7]);
    run(7, mutate(7, "bundle.rules[1].amount = 0"));
    rerevved::ObserveEffectRequestsForTests(nullptr, nullptr);
    const auto                 wonder     = expectedEffect<WonderEffectRuleV2>("aeshur.stonehenge-religion", "stonehenge-religion", WONDER_EFFECT_STONEHENGE_RELIGION, 0);
    const BuildingEffectRuleV2 building[] = {
        expectedEffect<BuildingEffectRuleV2>("aeshur.chinese-library-gold", "chinese-library-fixed-gold", BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD, 1),
        expectedEffect<BuildingEffectRuleV2>("aeshur.chinese-library-gold", "chinese-library-fixed-gold", BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD, 0),
    };
    UNSCOPED_INFO("each effect request reaches its support check");
    REQUIRE((observedWonder.size() == 1 && observedBuilding.size() == 2));
    UNSCOPED_INFO("complete Stonehenge V2 request");
    REQUIRE(std::memcmp(&observedWonder[0], &wonder, sizeof(WonderEffectRuleV2)) == 0);
    for (size_t i = 0; i < 2; ++i)
    {
        UNSCOPED_INFO("complete Library V2 request");
        REQUIRE(std::memcmp(&observedBuilding[i], &building[i], sizeof(BuildingEffectRuleV2)) == 0);
    }
}

void TestWonderEffectPublication()
{
    reset();
    UNSCOPED_INFO("Religion grant bundle publishes");
    REQUIRE(run(6, scripts[6]).published);
    UNSCOPED_INFO("Religion grant retained");
    REQUIRE((wonder_effect_rules::RuleCount() == 1 && wonder_effect_rules::IsStonehengeReligionActive()));
    const auto published = snapshot();
    UNSCOPED_INFO("identical Religion replay is idempotent");
    REQUIRE((run(6, scripts[6]).published && snapshot() == published));

    // Wonder records publish atomically with the bundle's other domains.
    const std::vector<std::string> mixedDomains{ "terrain_yield", "building_yield", "wonder_completion_technology" };
    const auto                     mixed = [&](const std::string& edit)
    {
        return host->Publish({ "package", "main.lua", "rerevved.rules", 1, mixedDomains, decode(mutate(6, edit)) });
    };
    const std::string terrain = "bundle.rules[2] = { id='mixed-terrain', kind='terrain_yield', terrain='hill', component='production', operation='add', value=2 }";
    const std::string gold    = "bundle.rules[3] = { id='mixed-gold', kind='building_yield', civilization='china', building='library', yield='gold', amount=1 }";
    auto              result  = mixed("bundle.rules[1].id = 'second-religion'; " + terrain + "; " + gold);
    int32_t           amount  = 0;
    UNSCOPED_INFO("Religion, terrain and Building records publish together");
    REQUIRE((result.published && wonder_effect_rules::RuleCount() == 2 && building_effect_rules::TryGetEffectiveAmount(amount) && amount == 1));
    evaluateHills(2);
    const auto all = snapshot();

    std::string otherTerrain = terrain;
    otherTerrain.replace(otherTerrain.find("mixed-terrain"), 13, "other-terrain");
    std::string otherGold = gold;
    otherGold.replace(otherGold.find("mixed-gold"), 10, "other-gold");
    result = mixed("bundle.rules[1].technology = 'writing'; " + otherTerrain + "; " + otherGold);
    UNSCOPED_INFO("an unsupported technology rejects the whole bundle");
    REQUIRE((!result.published && result.stage == ModScriptStage::kSchema &&
             result.message == "rules[1] stonehenge-religion: field technology: unsupported value; supported values: religion"));
    UNSCOPED_INFO("an unsupported technology published no terrain or Building record");
    REQUIRE(snapshot() == all);

    // A failure anywhere leaves the registries as they were.
    for (int64_t checkpoint = 0; checkpoint < 200; ++checkpoint)
    {
        {
            const tests::ScopedPreparationFailure failAt(checkpoint);
            result = mixed("bundle.rules[1].id = 'fourth-religion'; " + otherTerrain);
        }
        if (result.published)
        {
            UNSCOPED_INFO("preparation sweep eventually publishes");
            REQUIRE(wonder_effect_rules::RuleCount() == 3);
            return;
        }
        UNSCOPED_INFO("preparation failure left a partial publication");
        REQUIRE((result.stage == ModScriptStage::kPublish && snapshot() == all));
    }
    UNSCOPED_INFO("Wonder preparation sweep never succeeded");
    REQUIRE(false);
}

void TestCityFoundingUnit()
{
    reset();
    UNSCOPED_INFO("Lua founding rule defaults off");
    REQUIRE(!city_founding_unit::IsEnabled());
    const std::vector<std::string> declared{ "terrain_yield", "city_founding_unit" };
    const auto                     publishSource = [&](const std::string& source)
    {
        return host->Publish({ "package", "main.lua", "rerevved.rules", 1, declared, decode(source) });
    };
    const std::string source = "return { provider_id='test.founding', rules={ { id='militia', kind='city_founding_unit', unit='militia', upgrades={'loyalty'} } } }";
    UNSCOPED_INFO("Lua enables city founding unit with Loyalty");
    REQUIRE((publishSource(source).published && city_founding_unit::IsEnabled()));
    const auto before = snapshot();
    UNSCOPED_INFO("Lua founding replay is idempotent");
    REQUIRE((publishSource(source).published && snapshot() == before));
    const auto rejectEdit = [&](const std::string& edit, ModScriptStage stage, std::string_view diagnostic)
    {
        const auto result = publishSource("local function declarations()\n" + source + "\nend\nlocal bundle = declarations()\n" + edit + "\nreturn bundle");
        UNSCOPED_INFO("invalid founding bundle left all registries intact: " << result.message);
        REQUIRE((!result.published && result.stage == stage && result.message.find(diagnostic) != std::string::npos && snapshot() == before));
    };
    rejectEdit("bundle.provider_id = 'other.founding'; bundle.rules[2] = { id='hill', kind='terrain_yield', terrain='hill', component='production', operation='add', value=1 }",
               ModScriptStage::kPublish,
               "conflicting rule identity");
    rejectEdit("bundle.rules[1].upgrades = {'veteran'}", ModScriptStage::kSchema, "field upgrades: unsupported value; supported values: loyalty");
    rejectEdit("bundle.rules[1].upgrades = {}", ModScriptStage::kSchema, "field upgrades: expected dense array containing one upgrade; supported values: loyalty");
    rejectEdit("bundle.rules[1].upgrades = {[2]='loyalty'}", ModScriptStage::kSchema, "field upgrades:");
    rejectEdit("bundle.rules[1].unit = 'warrior'", ModScriptStage::kSchema, "field unit: unsupported value; supported values: militia");
    rejectEdit("bundle.rules[1].kind = 'founding_militia'", ModScriptStage::kSupport, "field kind: unsupported kind founding_militia");
    rejectEdit("bundle.rules[1].enable = false", ModScriptStage::kSchema, "field enable: unknown field");
    const std::vector<std::string> undeclared{ "terrain_yield" };
    const auto                     missing = host->Publish({ "package", "main.lua", "rerevved.rules", 1, undeclared, decode(source) });
    UNSCOPED_INFO("founding kind must be declared");
    REQUIRE((!missing.published && missing.stage == ModScriptStage::kSchema && snapshot() == before));
}

void TestOwnedValues()
{
    reset();
    const auto before = snapshot();
    // The SDK rejects these while decoding; the host also defends its public owned-value boundary.
    for (double number : { std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
    {
        auto  value        = decode(scripts[0]);
        auto& numberValue  = ownedField(ownedField(value, "rules").entries[0].value, "value");
        numberValue.type   = ModScriptValue::Type::kNumber;
        numberValue.number = number;
        const auto result  = publish(value);
        UNSCOPED_INFO("nonfinite owned number");
        REQUIRE((!result.published && result.stage == ModScriptStage::kSchema));
    }
    auto  value = decode(scripts[0]);
    auto& key   = ownedField(value, "rules").entries[0].key;
    key.type    = ModScriptValue::Type::kNumber;
    key.number  = 1.5;
    auto result = publish(value);
    UNSCOPED_INFO("noninteger owned key");
    REQUIRE((!result.published && result.stage == ModScriptStage::kSchema));
    value = decode(scripts[0]);
    value.entries.push_back(value.entries.front());
    result = publish(value);
    UNSCOPED_INFO("duplicate owned field");
    REQUIRE((!result.published && result.stage == ModScriptStage::kSchema));
    const auto decoded = rex::system::RunModScript("main.lua", mutate(0, "bundle.rules[1.5] = bundle.rules[1]"));
    UNSCOPED_INFO("SDK noninteger key rejection");
    REQUIRE((!decoded.ok && decoded.error.stage == ModScriptStage::kDecode));
    UNSCOPED_INFO("owned validation published nothing");
    REQUIRE(snapshot() == before);
}

void TestHostAllocation()
{
    for (size_t package : { 0u, 1u, 4u })
    {
        reset();
        UNSCOPED_INFO("allocation earlier package");
        REQUIRE(run(5, scripts[5]).published);
        const auto before    = snapshot();
        const auto value     = decode(scripts[package]);
        bool       succeeded = false;
        for (int64_t allocation = 0; allocation < 1000; ++allocation)
        {
            const auto result = [&]
            {
                allocation::ScopedCountdown failAt(allocation);
                return publish(value, package);
            }();
            if (result.published)
            {
                succeeded = true;
                break;
            }
            UNSCOPED_INFO("host allocation failure stage");
            REQUIRE(result.stage == ModScriptStage::kPublish);
            UNSCOPED_INFO("allocation failure after partial or complete commit");
            REQUIRE(snapshot() == before);
        }
        UNSCOPED_INFO("host allocation sweep never succeeded");
        REQUIRE(succeeded);
        const auto after = snapshot();
        UNSCOPED_INFO("successful host allocation sweep did not publish");
        REQUIRE(after != before);
        UNSCOPED_INFO("earlier rules survive host allocation sweep");
        REQUIRE((run(5, scripts[5]).published && snapshot() == after));
    }
}

void TestAtomicity()
{
    reset();
    UNSCOPED_INFO("earlier Hills bundle");
    REQUIRE(run(0, scripts[0]).published);
    const auto earlier = snapshot();
    for (size_t package : { 1u, 4u })
    {
        rejected(package, "bundle.rules[3].text = 1", ModScriptStage::kSchema, "rules[3]");
        rejected(package, "bundle.rules[2].identity = 'invalid'", ModScriptStage::kSchema, "rules[2]");
        rejected(package, "bundle.rules[4] = {}; for k,v in pairs(bundle.rules[3]) do bundle.rules[4][k] = v end; bundle.rules[4].text = 'conflict'", ModScriptStage::kPublish, "native result -12");
        UNSCOPED_INFO("later invalid/conflicting bundle retained earlier rules");
        REQUIRE(snapshot() == earlier);
        bool reachedSuccess = false;
        for (int64_t checkpoint = 0; checkpoint < 200; ++checkpoint)
        {
            const auto result = [&]
            {
                const tests::ScopedPreparationFailure failAt(checkpoint);
                return run(package, scripts[package]);
            }();
            if (result.published)
            {
                reachedSuccess = true;
                break;
            }
            UNSCOPED_INFO("allocation failure publication diagnostic");
            REQUIRE((result.stage == ModScriptStage::kPublish && result.message.find("native result -13") != std::string::npos));
            UNSCOPED_INFO("allocation failure partially published");
            REQUIRE(snapshot() == earlier);
        }
        UNSCOPED_INFO("all allocation preparation checkpoints exercised");
        REQUIRE(reachedSuccess);
        const auto accepted = snapshot();
        rejected(package, "bundle.rules[3].text = 'conflicting replay'", ModScriptStage::kPublish, "rules[3]");
        UNSCOPED_INFO("conflicting replay preserved accepted bundle");
        REQUIRE(snapshot() == accepted);
        reset();
        UNSCOPED_INFO("restore earlier bundle");
        REQUIRE(run(0, scripts[0]).published);
    }
    UNSCOPED_INFO("valid later package survives earlier failures");
    REQUIRE(run(5, scripts[5]).published);
}

} // namespace

TEST_CASE("mod_script_rules", "[ctest][mod-script-rules]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestScripts();
    TestEdits();
    TestSchema();
    TestOwnedValues();
    TestParameterizedKinds();
    TestEffectRequests();
    TestBuildingEffectPublication();
    TestWonderEffectPublication();
    TestCityFoundingUnit();
    TestHostAllocation();
    TestAtomicity();
}
