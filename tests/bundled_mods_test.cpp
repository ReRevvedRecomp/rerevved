#include "building_effect_rules_registry.h"
#include "city_founding_unit_rules.h"
#include "mod_script_rules.h"
#include "nation_select_text_registry.h"
#include "registry_scope.hpp"
#include "rules_fixtures.hpp"
#include "terrain_yield_rules_registry.h"
#include "unique_era_abilities_registry.h"
#include "unique_unit_rules_registry.h"
#include "unit_combat_rules_registry.h"
#include "unit_effect_rules_registry.h"
#include "unit_movement_rules_registry.h"
#include "unit_production_cost_rules_registry.h"
#include "wonder_effect_rules_registry.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <rex/system/mod_catalog.h>
#include <rex/system/mod_script.h>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace rerevved;
using namespace rerevved::tests::rules_fixtures;
using rex::system::DiscoverModCatalog;
using rex::system::kModPluginAbiVersion;
using rex::system::ModDiagnostic;
using rex::system::ModPackage;
using rex::system::ModPackageStatus;
using rex::system::ModPayloadKind;
using rex::system::ModScriptValue;

namespace
{

struct ExpectedMod
{
    std::string_view         id;
    std::vector<std::string> domains;
    size_t                   rules;
};

// scripts/package.py BUNDLED_MODS in catalog order, sorted by folder name.
const std::array<ExpectedMod, 9> expectedMods = { {
    { "cataphracts-defense", { "unique_unit_scalar", "nation_select_text" }, 2 },
    { "chinese-library-gold", { "building_yield" }, 1 },
    { "founding-militia", { "city_founding_unit" }, 1 },
    { "hills-production", { "terrain_yield" }, 1 },
    { "hoplite-loyalty", { "unit_effect", "unit_production_cost", "nation_select_text" }, 3 },
    { "jaguar-woodsman", { "unit_combat", "nation_select_text" }, 3 },
    { "keshik-movement", { "unit_movement", "nation_select_text" }, 2 },
    { "mongol-horseback", { "era_technology_ability", "nation_select_text" }, 2 },
    { "stonehenge-religion", { "wonder_completion_technology" }, 1 },
} };

// Continuous integration assembles the mods before configuring, so a missing
// directory there is a broken job, not an optional input.
bool runningOnGitHubActions()
{
#if defined(_WIN32)
    char*       value  = nullptr;
    std::size_t length = 0;
    std::string text;
    if (_dupenv_s(&value, &length, "GITHUB_ACTIONS") == 0 && value)
        text = value;
    std::free(value);
    return text == "true";
#else
    const char* value = std::getenv("GITHUB_ACTIONS");
    return value && std::string_view(value) == "true";
#endif
}

std::string diagnosticText(const std::vector<ModDiagnostic>& diagnostics)
{
    std::string text;
    for (const auto& diagnostic : diagnostics)
        text += diagnostic.message + "; ";
    return text;
}

// The decoded bundle is a table whose "rules" field is an array of records.
size_t ruleRecords(const ModScriptValue& bundle)
{
    for (const auto& entry : bundle.entries)
        if (entry.key.type == ModScriptValue::Type::kString && entry.key.string == "rules")
            return entry.value.entries.size();
    UNSCOPED_INFO("decoded bundle has no rules field");
    REQUIRE(false);
    return 0;
}

template <typename Count>
uint32_t registeredCount(Count getCount)
{
    uint32_t count = 0;
    UNSCOPED_INFO("registry count");
    REQUIRE(getCount(&count) == 0);
    return count;
}

void expectRegistered(const char* domain, uint32_t actual, uint32_t expected)
{
    UNSCOPED_INFO("registered " << domain << " rules");
    REQUIRE(actual == expected);
}

void expectHillsBonus(int32_t bonus)
{
    for (int32_t native : { 0, 9 })
    {
        TerrainYieldQuery query{};
        query.structSize  = sizeof(query);
        query.terrain     = TERRAIN_HILL;
        query.component   = TERRAIN_YIELD_PRODUCTION;
        query.nativeValue = native;
        TerrainYieldEvaluation result{};
        UNSCOPED_INFO("Hills evaluation");
        REQUIRE(EvaluateTerrainYield(&query, &result, sizeof(result)) == 0);
        UNSCOPED_INFO("Hills native + bonus");
        REQUIRE((result.nativeValue == native && result.finalValue == native + bonus &&
                 result.additiveCount == 1 && result.replacementCount == 0 && result.statusFlags == 0));
    }
}

} // namespace

TEST_CASE("bundled_mods", "[ctest][bundled-mods]")
{
    const std::filesystem::path modsRoot{ REREVVED_BUNDLED_MODS_DIR };
    std::error_code             directoryError;
    if (!std::filesystem::is_directory(modsRoot, directoryError))
    {
        if (runningOnGitHubActions())
        {
            FAIL("Assembled mods are missing at " << modsRoot.string() << "; the workflow must run scripts/assemble_mods.py first");
        }
        SKIP("Run python scripts/assemble_mods.py --mods-checkout <rerevved-mods checkout> to assemble " << modsRoot.string());
    }

    const rerevved::tests::ScopedRegistryReset registryScope;
    const auto                                 host = CreateModScriptRulesHost();
    REQUIRE(host != nullptr);
    const auto catalog = DiscoverModCatalog(modsRoot, REREVVED_TEST_GAME_VERSION, kModPluginAbiVersion, host->contracts());
    UNSCOPED_INFO("catalog diagnostics: " << diagnosticText(catalog.diagnostics));
    REQUIRE(catalog.diagnostics.empty());
    UNSCOPED_INFO("exactly the bundled packages");
    REQUIRE(catalog.packages.size() == expectedMods.size());

    // Match SDK startup: discover each package, run its entry, then prepare and
    // commit the whole loadout at once.
    std::vector<ModScriptValue> bundles;
    bundles.reserve(expectedMods.size());
    for (const auto& expected : expectedMods)
    {
        CAPTURE(expected.id);
        const ModPackage* package = catalog.Find(expected.id);
        REQUIRE(package != nullptr);
        UNSCOPED_INFO("package diagnostics: " << diagnosticText(package->diagnostics));
        REQUIRE((package->status == ModPackageStatus::kReady && package->diagnostics.empty()));
        UNSCOPED_INFO("manifest payload");
        REQUIRE((package->payload_kind == ModPayloadKind::kLua && package->entry == "main.lua" &&
                 package->api == "rerevved.rules" && package->api_version == 1));
        UNSCOPED_INFO("required domains in manifest order");
        REQUIRE(package->required_domains == expected.domains);

        auto run = rex::system::RunModScriptFile(package->mod_root, package->entry);
        UNSCOPED_INFO("SDK script execution: " << rex::system::ToString(run.error.stage) << ": " << run.error.message);
        REQUIRE(run.ok);
        UNSCOPED_INFO("rule record count");
        REQUIRE(ruleRecords(run.value) == expected.rules);

        bundles.push_back(std::move(run.value));
    }

    const auto                                        baseline = snapshot();
    std::vector<rex::system::ModScriptPublishRequest> requests;
    for (size_t i = 0; i < expectedMods.size(); ++i)
    {
        const ModPackage* package = catalog.Find(expectedMods[i].id);
        REQUIRE(package != nullptr);
        requests.push_back({ package->id, package->entry, package->api, package->api_version, package->required_domains, bundles[i] });
    }
    rex::system::ModScriptPublishResult result;
    auto                                prepared = host->PrepareReplacement(requests, result);
    UNSCOPED_INFO("loadout preparation: " << rex::system::ToString(result.stage) << ": " << result.message);
    REQUIRE((prepared != nullptr && result.published));
    prepared->Commit();

    expectRegistered("terrain_yield", registeredCount(GetTerrainYieldRuleCount), 1);
    expectRegistered("unit_combat", registeredCount(GetUnitCombatRuleCount), 2);
    expectRegistered("unique_unit_scalar", registeredCount(GetUniqueUnitScalarRuleCount), 1);
    expectRegistered("unit_movement", registeredCount(GetUnitMovementRuleCount), 1);
    expectRegistered("unit_effect", registeredCount(GetUnitEffectRuleCount), 1);
    expectRegistered("unit_production_cost", registeredCount(GetUnitProductionCostRuleCount), 1);
    expectRegistered("era_technology_ability", registeredCount(GetEraAbilityRuleCount), 1);
    expectRegistered("nation_select_text", registeredCount(GetNationSelectTextRuleCount), 5);
    int32_t amount = 0;
    UNSCOPED_INFO("Building yield rule");
    REQUIRE((building_effect_rules::RuleCount() == 1 && building_effect_rules::TryGetEffectiveAmount(amount) && amount == 1));
    UNSCOPED_INFO("Wonder rule");
    REQUIRE((wonder_effect_rules::RuleCount() == 1 && wonder_effect_rules::IsStonehengeReligionActive()));
    UNSCOPED_INFO("Founding rule");
    REQUIRE((city_founding_unit::RuleCount() == 1 && city_founding_unit::IsEnabled()));
    expectHillsBonus(1);

    // Preparing the same loadout again changes nothing.
    const auto before = snapshot();
    prepared          = host->PrepareReplacement(requests, result);
    UNSCOPED_INFO("replay: " << rex::system::ToString(result.stage) << ": " << result.message);
    REQUIRE((prepared != nullptr && result.published));
    prepared->Commit();
    const bool unchanged = snapshot() == before;
    UNSCOPED_INFO("identical replay idempotence");
    REQUIRE(unchanged);

    // Disabling all Lua mods prepares an empty loadout and removes their rules.
    // No native rule is registered here, so each domain returns to zero.
    prepared = host->PrepareReplacement({}, result);
    UNSCOPED_INFO("unload: " << rex::system::ToString(result.stage) << ": " << result.message);
    REQUIRE((prepared != nullptr && result.published));
    prepared->Commit();
    expectRegistered("terrain_yield", registeredCount(GetTerrainYieldRuleCount), 0);
    expectRegistered("unit_combat", registeredCount(GetUnitCombatRuleCount), 0);
    expectRegistered("unique_unit_scalar", registeredCount(GetUniqueUnitScalarRuleCount), 0);
    expectRegistered("unit_movement", registeredCount(GetUnitMovementRuleCount), 0);
    expectRegistered("unit_effect", registeredCount(GetUnitEffectRuleCount), 0);
    expectRegistered("unit_production_cost", registeredCount(GetUnitProductionCostRuleCount), 0);
    expectRegistered("era_technology_ability", registeredCount(GetEraAbilityRuleCount), 0);
    expectRegistered("nation_select_text", registeredCount(GetNationSelectTextRuleCount), 0);
    UNSCOPED_INFO("Building yield rule removed");
    REQUIRE((building_effect_rules::RuleCount() == 0 && !building_effect_rules::TryGetEffectiveAmount(amount)));
    UNSCOPED_INFO("Wonder rule removed");
    REQUIRE((wonder_effect_rules::RuleCount() == 0 && !wonder_effect_rules::IsStonehengeReligionActive()));
    UNSCOPED_INFO("Founding rule removed");
    REQUIRE((city_founding_unit::RuleCount() == 0 && !city_founding_unit::IsEnabled()));
    UNSCOPED_INFO("unload restores the pre-load registry state");
    REQUIRE(snapshot() == baseline);
}
