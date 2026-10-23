#include "rules_fixtures.hpp"
#include "building_effect_rules_registry.h"
#include "nation_select_text_registry.h"
#include "rules_bundle.h"
#include "terrain_yield_rules_registry.h"
#include "unique_era_abilities_registry.h"
#include "unique_unit_rules_registry.h"
#include "unit_combat_rules_registry.h"
#include "unit_effect_rules_registry.h"
#include "unit_movement_rules_registry.h"
#include "unit_production_cost_rules_registry.h"
#include "wonder_effect_rules_registry.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>

using namespace rerevved;
using namespace rerevved::rules_bundle;

namespace rerevved::tests::rules_fixtures
{
namespace
{

template <typename T>
T expected(const char* provider, const char* id)
{
    T rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, id, std::strlen(id) + 1);
    return rule;
}

} // namespace

RulesBundle fixtures()
{
    // Expected fields copied from the native consumers, revision ff2a3af.
    RulesBundle bundle;
    {
        auto native      = expected<TerrainYieldRule>("aeshur.hills-production", "hills-production");
        native.terrain   = TERRAIN_HILL;
        native.component = TERRAIN_YIELD_PRODUCTION;
        native.operation = TERRAIN_YIELD_ADD;
        native.value     = 1;
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    }
    for (bool defense : { false, true })
    {
        auto native            = expected<UnitCombatRule>("aeshur.jaguar-woodsman",
                                                          defense ? "jaguar-warrior-forest-defense" : "jaguar-warrior-forest-attack");
        native.civilization    = CIVILIZATION_AZTEC;
        native.baseUnitType    = UNIT_TYPE_WARRIOR;
        native.identity        = UNIT_IDENTITY_JAGUAR_WARRIOR;
        native.terrain         = TERRAIN_FOREST;
        native.property        = defense ? UNIT_COMBAT_DEFENSE : UNIT_COMBAT_ATTACK;
        native.percentageDelta = 50;
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    }
    auto unitText = [&](const char* provider, const char* id, const char* text, int32_t civilization, int32_t baseUnitType, int32_t identity)
    {
        auto native         = expected<NationSelectTextRule>(provider, id);
        native.surface      = NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT;
        native.civilization = civilization;
        native.unlockEra    = NATION_SELECT_TEXT_SELECTOR_UNUSED;
        native.ability      = 0;
        native.baseUnitType = baseUnitType;
        native.identity     = identity;
        native.displayForm  = UNIT_DISPLAY_FORM_UNIT;
        std::memcpy(native.text, text, std::strlen(text) + 1);
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    };
    unitText("aeshur.jaguar-woodsman", "jaguar-woodsman-text", "Jaguar Warrior - Warrior with +50% Attack and Defense in Forest", CIVILIZATION_AZTEC, UNIT_TYPE_WARRIOR, UNIT_IDENTITY_JAGUAR_WARRIOR);
    {
        auto native         = expected<UniqueUnitScalarRule>("aeshur.cataphracts-defense", "cataphract-defense");
        native.civilization = CIVILIZATION_ROMAN;
        native.baseUnitType = UNIT_TYPE_KNIGHTS;
        native.identity     = UNIT_IDENTITY_CATAPHRACT;
        native.property     = UNIQUE_UNIT_SCALAR_BASE_DEFENSE;
        native.operation    = UNIQUE_UNIT_SCALAR_ADD;
        native.value        = 1;
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    }
    unitText("aeshur.cataphracts-defense", "cataphract-defense-text", "Cataphract - Knight with +1 base Defense", CIVILIZATION_ROMAN, UNIT_TYPE_KNIGHTS, UNIT_IDENTITY_CATAPHRACT);
    {
        auto native         = expected<UnitMovementRule>("aeshur.keshik-movement", "keshik-movement");
        native.civilization = CIVILIZATION_MONGOLIAN;
        native.baseUnitType = UNIT_TYPE_HORSEMEN;
        native.identity     = UNIT_IDENTITY_KESHIK;
        native.value        = 1;
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    }
    unitText("aeshur.keshik-movement", "keshik-movement-text", "Keshik - Horseman with +1 movement", CIVILIZATION_MONGOLIAN, UNIT_TYPE_HORSEMEN, UNIT_IDENTITY_KESHIK);
    {
        auto native         = expected<UnitEffectRule>("aeshur.hoplite-loyalty", "hoplite-loyalty");
        native.civilization = CIVILIZATION_GREEK;
        native.baseUnitType = UNIT_TYPE_PHALANX;
        native.identity     = UNIT_IDENTITY_HOPLITE;
        native.effect       = UNIT_EFFECT_CREATION_LOYALTY;
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    }
    {
        auto native            = expected<UnitProductionCostRule>("aeshur.hoplite-loyalty", "hoplite-production-cost");
        native.civilization    = CIVILIZATION_GREEK;
        native.baseUnitType    = UNIT_TYPE_PHALANX;
        native.identity        = UNIT_IDENTITY_HOPLITE;
        native.percentageDelta = -30;
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    }
    unitText("aeshur.hoplite-loyalty", "hoplite-loyalty-text", "Hoplite - Starts with Loyalty; costs 10 Production", CIVILIZATION_GREEK, UNIT_TYPE_PHALANX, UNIT_IDENTITY_HOPLITE);
    {
        auto native               = expected<EraAbilityReplacement>("aeshur.mongol-horseback", "mongol-ancient-horseback-riding");
        native.civilization       = CIVILIZATION_MONGOLIAN;
        native.unlockEra          = UNLOCK_ERA_ANCIENT;
        native.replacementAbility = ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    }
    {
        auto native           = expected<NationSelectTextRule>("aeshur.mongol-horseback", "mongol-ancient-horseback-riding-text");
        native.surface        = NATION_SELECT_TEXT_SURFACE_ERA_ABILITY;
        native.civilization   = CIVILIZATION_MONGOLIAN;
        native.unlockEra      = UNLOCK_ERA_ANCIENT;
        native.ability        = ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
        native.baseUnitType   = NATION_SELECT_TEXT_SELECTOR_UNUSED;
        native.identity       = NATION_SELECT_TEXT_SELECTOR_UNUSED;
        native.displayForm    = NATION_SELECT_TEXT_SELECTOR_UNUSED;
        constexpr char text[] = "Knowledge of Horseback Riding";
        std::memcpy(native.text, text, sizeof(text));
        bundle.rules.push_back({ bundle.rules.size() + 1, native });
    }
    return bundle;
}

void reset()
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
}

template <typename Info, typename Count, typename Get>
void appendSnapshot(std::string& bytes, Count count, Get get)
{
    uint32_t size = 0;
    UNSCOPED_INFO("snapshot count");
    REQUIRE(count(&size) == 0);
    bytes.append(reinterpret_cast<const char*>(&size), sizeof(size));
    for (uint32_t i = 0; i < size; ++i)
    {
        Info info{};
        UNSCOPED_INFO("snapshot entry");
        REQUIRE(get(i, &info, sizeof(info)) == 0);
        bytes.append(reinterpret_cast<const char*>(&info), sizeof(info));
    }
}

std::string snapshot()
{
    std::string bytes;
    appendSnapshot<TerrainYieldRuleInfo>(bytes, GetTerrainYieldRuleCount, GetTerrainYieldRule);
    appendSnapshot<UnitCombatRuleInfo>(bytes, GetUnitCombatRuleCount, GetUnitCombatRule);
    appendSnapshot<UniqueUnitScalarRuleInfo>(bytes, GetUniqueUnitScalarRuleCount, GetUniqueUnitScalarRule);
    appendSnapshot<UnitMovementRuleInfo>(bytes, GetUnitMovementRuleCount, GetUnitMovementRule);
    appendSnapshot<UnitEffectRuleInfo>(bytes, GetUnitEffectRuleCount, GetUnitEffectRule);
    appendSnapshot<UnitProductionCostRuleInfo>(bytes, GetUnitProductionCostRuleCount, GetUnitProductionCostRule);
    appendSnapshot<EraAbilityRuleInfo>(bytes, GetEraAbilityRuleCount, GetEraAbilityRule);
    appendSnapshot<NationSelectTextRuleInfo>(bytes, GetNationSelectTextRuleCount, GetNationSelectTextRule);
    const uint32_t count   = static_cast<uint32_t>(building_effect_rules::RuleCount());
    int32_t        amount  = 0;
    const bool     present = building_effect_rules::TryGetEffectiveAmount(amount);
    bytes.append(reinterpret_cast<const char*>(&count), sizeof(count));
    bytes.append(reinterpret_cast<const char*>(&present), sizeof(present));
    bytes.append(reinterpret_cast<const char*>(&amount), sizeof(amount));
    const uint32_t wonderCount  = static_cast<uint32_t>(wonder_effect_rules::RuleCount());
    const bool     wonderActive = wonder_effect_rules::IsStonehengeReligionActive();
    bytes.append(reinterpret_cast<const char*>(&wonderCount), sizeof(wonderCount));
    bytes.append(reinterpret_cast<const char*>(&wonderActive), sizeof(wonderActive));
    const auto militiaCount   = city_founding_unit::RuleCount();
    const bool militiaEnabled = city_founding_unit::IsEnabled();
    bytes.append(reinterpret_cast<const char*>(&militiaCount), sizeof(militiaCount));
    bytes.append(reinterpret_cast<const char*>(&militiaEnabled), sizeof(militiaEnabled));
    return bytes;
}

} // namespace rerevved::tests::rules_fixtures
