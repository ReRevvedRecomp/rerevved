#include "rules_bundle.h"
#include "building_effect_rules_registry.h"
#include "nation_select_text_registry.h"
#include "terrain_yield_rules_registry.h"
#include "unique_era_abilities_registry.h"
#include "unique_unit_rules_registry.h"
#include "unit_combat_rules_registry.h"
#include "unit_effect_rules_registry.h"
#include "unit_movement_rules_registry.h"
#include "unit_production_cost_rules_registry.h"
#include "wonder_effect_rules_registry.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <tuple>
#include <type_traits>

namespace rerevved::rules_bundle
{
namespace
{

#ifdef RULES_BUNDLE_TESTING
thread_local int64_t preparationSteps = -1;
#endif
void preparationCheckpoint()
{
#ifdef RULES_BUNDLE_TESTING
    if (preparationSteps == 0)
    {
        throw std::bad_alloc();
    }
    if (preparationSteps > 0)
    {
        --preparationSteps;
    }
#endif
}

template <typename T>
struct Registry;

template <>
struct Registry<TerrainYieldRule>
{
    static constexpr Domain  domain        = Domain::TerrainYieldRule;
    static constexpr int32_t internalError = TERRAIN_YIELD_RULES_ERR_INTERNAL;
    static constexpr auto    normalize     = terrain_yield_rules::NormalizeRequest;
    static constexpr auto    lock          = terrain_yield_rules::AcquirePublicationLock;
    static constexpr auto    build         = terrain_yield_rules::BuildCandidate;
    static constexpr auto    commit        = terrain_yield_rules::CommitCandidate;
};

template <>
struct Registry<UnitCombatRule>
{
    static constexpr Domain  domain        = Domain::UnitCombatRule;
    static constexpr int32_t internalError = UNIT_COMBAT_RULES_ERR_INTERNAL;
    static constexpr auto    normalize     = unit_combat_rules::NormalizeRequest;
    static constexpr auto    lock          = unit_combat_rules::AcquirePublicationLock;
    static constexpr auto    build         = unit_combat_rules::BuildCandidate;
    static constexpr auto    commit        = unit_combat_rules::CommitCandidate;
};

template <>
struct Registry<UniqueUnitScalarRule>
{
    static constexpr Domain  domain        = Domain::UniqueUnitScalarRule;
    static constexpr int32_t internalError = UNIQUE_UNIT_RULES_ERR_INTERNAL;
    static constexpr auto    normalize     = unique_unit_rules::NormalizeRequest;
    static constexpr auto    lock          = unique_unit_rules::AcquirePublicationLock;
    static constexpr auto    build         = unique_unit_rules::BuildCandidate;
    static constexpr auto    commit        = unique_unit_rules::CommitCandidate;
};

template <>
struct Registry<UnitMovementRule>
{
    static constexpr Domain  domain        = Domain::UnitMovementRule;
    static constexpr int32_t internalError = UNIT_MOVEMENT_RULES_ERR_INTERNAL;
    static constexpr auto    normalize     = unit_movement_rules::NormalizeRequest;
    static constexpr auto    lock          = unit_movement_rules::AcquirePublicationLock;
    static constexpr auto    build         = unit_movement_rules::BuildCandidate;
    static constexpr auto    commit        = unit_movement_rules::CommitCandidate;
};

template <>
struct Registry<UnitEffectRule>
{
    static constexpr Domain  domain        = Domain::UnitEffectRule;
    static constexpr int32_t internalError = UNIT_EFFECT_RULES_ERR_INTERNAL;
    static constexpr auto    normalize     = unit_effect_rules::NormalizeRequest;
    static constexpr auto    lock          = unit_effect_rules::AcquirePublicationLock;
    static constexpr auto    build         = unit_effect_rules::BuildCandidate;
    static constexpr auto    commit        = unit_effect_rules::CommitCandidate;
};

template <>
struct Registry<UnitProductionCostRule>
{
    static constexpr Domain  domain        = Domain::UnitProductionCostRule;
    static constexpr int32_t internalError = UNIT_PRODUCTION_COST_RULES_ERR_INTERNAL;
    static constexpr auto    normalize     = unit_production_cost_rules::NormalizeRequest;
    static constexpr auto    lock          = unit_production_cost_rules::AcquirePublicationLock;
    static constexpr auto    build         = unit_production_cost_rules::BuildCandidate;
    static constexpr auto    commit        = unit_production_cost_rules::CommitCandidate;
};

template <>
struct Registry<EraAbilityReplacement>
{
    static constexpr Domain  domain        = Domain::EraAbilityReplacement;
    static constexpr int32_t internalError = ERA_ABILITIES_ERR_INTERNAL;
    static constexpr auto    normalize     = unique_era_abilities::NormalizeRequest;
    static constexpr auto    lock          = unique_era_abilities::AcquirePublicationLock;
    static constexpr auto    build         = unique_era_abilities::BuildCandidate;
    static constexpr auto    commit        = unique_era_abilities::CommitCandidate;
};

template <>
struct Registry<NationSelectTextRule>
{
    static constexpr Domain  domain        = Domain::NationSelectTextRule;
    static constexpr int32_t internalError = NATION_SELECT_TEXT_ERR_INTERNAL;
    static constexpr auto    normalize     = nation_select_text::NormalizeRequest;
    static constexpr auto    lock          = nation_select_text::AcquirePublicationLock;
    static constexpr auto    build         = nation_select_text::BuildCandidate;
    static constexpr auto    commit        = nation_select_text::CommitCandidate;
};

template <>
struct Registry<BuildingEffectRuleV2>
{
    static constexpr Domain  domain        = Domain::BuildingEffectRule;
    static constexpr int32_t internalError = BUILDING_EFFECT_RULES_ERR_INTERNAL;
    static constexpr auto    normalize     = building_effect_rules::NormalizeRequest;
    static constexpr auto    lock          = building_effect_rules::AcquirePublicationLock;
    static constexpr auto    build         = building_effect_rules::BuildCandidate;
    static constexpr auto    commit        = building_effect_rules::CommitCandidate;
};

template <>
struct Registry<WonderEffectRuleV2>
{
    static constexpr Domain  domain        = Domain::WonderEffectRule;
    static constexpr int32_t internalError = WONDER_EFFECT_RULES_ERR_INTERNAL;
    static constexpr auto    normalize     = wonder_effect_rules::NormalizeRequest;
    static constexpr auto    lock          = wonder_effect_rules::AcquirePublicationLock;
    static constexpr auto    build         = wonder_effect_rules::BuildCandidate;
    static constexpr auto    commit        = wonder_effect_rules::CommitCandidate;
};

template <>
struct Registry<city_founding_unit::Rule>
{
    static constexpr Domain  domain        = Domain::CityFoundingUnitRule;
    static constexpr int32_t internalError = city_founding_unit::kInternalError;
    static constexpr auto    normalize     = city_founding_unit::NormalizeRequest;
    static constexpr auto    lock          = city_founding_unit::AcquirePublicationLock;
    static constexpr auto    build         = city_founding_unit::BuildCandidate;
    static constexpr auto    commit        = city_founding_unit::CommitCandidate;
};

template <typename T>
struct Stage
{
    std::vector<T>                      requests;
    std::vector<const IndexedRequest*>  origins;
    std::vector<T>                      candidate;
    std::unique_lock<std::shared_mutex> lock;
};

void locate(PublishResult& result, const IndexedRequest& input) noexcept
{
    result.bundleIndex = input.bundleIndex;
    std::visit([&](const auto& rule)
               {
                   using T = std::decay_t<decltype(rule)>;
                   static_assert(sizeof(rule.ruleId) < sizeof(result.ruleId));
                   result.domain       = Registry<T>::domain;
                   result.nativeResult = Registry<T>::internalError;
                   std::memset(result.ruleId, 0, sizeof(result.ruleId));
                   std::memcpy(result.ruleId, rule.ruleId, sizeof(rule.ruleId));
               },
               input.request);
}

template <typename T>
void acquire(Stage<T>& stage, PublishResult& result)
{
    if (stage.requests.empty())
    {
        return;
    }
    locate(result, *stage.origins.front());
    stage.lock = Registry<T>::lock();
}

template <typename T>
bool prepare(Stage<T>& stage, PublishResult& result)
{
    if (stage.requests.empty())
    {
        return true;
    }
    size_t  offset = 0;
    int32_t status = Registry<T>::internalError;
    try
    {
        status = Registry<T>::build(stage.requests, stage.candidate, offset, preparationCheckpoint);
    }
    catch (...)
    {
        locate(result, *stage.origins[offset]);
        return false;
    }
    if (status != 0)
    {
        locate(result, *stage.origins[offset]);
        result.nativeResult = status;
        return false;
    }
    return true;
}

template <typename T>
void commit(Stage<T>& stage) noexcept
{
    if (!stage.requests.empty())
    {
        Registry<T>::commit(stage.candidate);
    }
}

struct Token
{
    std::string_view name;
    int32_t          value;
};

template <size_t N>
bool mapToken(std::string_view name, const Token (&table)[N], int32_t& out) noexcept
{
    for (const auto& token : table)
    {
        if (token.name == name)
        {
            out = token.value;
            return true;
        }
    }
    return false;
}

// Contract tokens use public constants; native consumer fixtures verify every mapping.
constexpr Token terrains[]      = { { "hill", TERRAIN_HILL }, { "forest", TERRAIN_FOREST } };
constexpr Token civilizations[] = {
    { "aztec", CIVILIZATION_AZTEC }, { "roman", CIVILIZATION_ROMAN }, { "greek", CIVILIZATION_GREEK }, { "mongolian", CIVILIZATION_MONGOLIAN }
};
constexpr Token unitTypes[] = {
    { "warrior", UNIT_TYPE_WARRIOR }, { "knights", UNIT_TYPE_KNIGHTS }, { "horsemen", UNIT_TYPE_HORSEMEN }, { "phalanx", UNIT_TYPE_PHALANX }
};
constexpr Token identities[] = {
    { "jaguar_warrior", UNIT_IDENTITY_JAGUAR_WARRIOR }, { "cataphract", UNIT_IDENTITY_CATAPHRACT }, { "keshik", UNIT_IDENTITY_KESHIK }, { "hoplite", UNIT_IDENTITY_HOPLITE }
};
constexpr Token eras[]              = { { "ancient", UNLOCK_ERA_ANCIENT } };
constexpr Token abilities[]         = { { "knowledge_of_horseback_riding", ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING } };
constexpr Token components[]        = { { "production", TERRAIN_YIELD_PRODUCTION } };
constexpr Token terrainOperations[] = { { "add", TERRAIN_YIELD_ADD } };
constexpr Token scalarOperations[]  = { { "add", UNIQUE_UNIT_SCALAR_ADD } };
constexpr Token combatProperties[]  = { { "attack", UNIT_COMBAT_ATTACK }, { "defense", UNIT_COMBAT_DEFENSE } };
constexpr Token scalarProperties[]  = { { "base_defense", UNIQUE_UNIT_SCALAR_BASE_DEFENSE } };
constexpr Token effects[]           = { { "creation_loyalty", UNIT_EFFECT_CREATION_LOYALTY } };
constexpr Token surfaces[]          = {
    { "unique_unit", NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT },
    { "era_ability", NATION_SELECT_TEXT_SURFACE_ERA_ABILITY }
};
constexpr Token displayForms[] = { { "unit", UNIT_DISPLAY_FORM_UNIT } };

struct ParameterMapping
{
    std::string_view                     kind;
    std::initializer_list<RuleParameter> parameters;
    int32_t                              nativeEffect;
};

constexpr ParameterMapping parameterMappings[] = {
    { "city_founding_unit", { { "unit", "militia" }, { "upgrades", "loyalty" } }, 1 },
    { "building_yield", { { "civilization", "china" }, { "building", "library" }, { "yield", "gold" } }, BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD },
    { "wonder_completion_technology", { { "wonder", "stonehenge" }, { "technology", "religion" } }, WONDER_EFFECT_STONEHENGE_RELIGION },
    { "era_technology_ability", { { "technology", "horseback_riding" } }, ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING },
};

template <size_t N>
bool copyString(std::string_view input, char (&out)[N]) noexcept
{
    if (input.empty() || input.size() >= N || input.find('\0') != std::string_view::npos)
    {
        return false;
    }
    std::memcpy(out, input.data(), input.size());
    return true;
}

template <typename T>
bool initialize(RuleIdentity ids, T& rule) noexcept
{
    rule.structSize = sizeof(rule);
    return copyString(ids.providerId, rule.providerId) && copyString(ids.ruleId, rule.ruleId);
}

template <typename T>
bool mapUnit(UnitSelectors unit, T& rule) noexcept
{
    return mapToken(unit.civilization, civilizations, rule.civilization) &&
           mapToken(unit.baseUnitType, unitTypes, rule.baseUnitType) &&
           mapToken(unit.identity, identities, rule.identity);
}

} // namespace

#ifdef RULES_BUNDLE_TESTING
void FailPreparationAfterForTests(int64_t successfulSteps) noexcept
{
    preparationSteps = successfulSteps;
}
#endif

PublishResult PublishRulesBundle(const RulesBundle& bundle) noexcept
{
    PublishResult result{};
    try
    {
        // Registry acquisition order is terrain, combat, scalar, movement,
        // effect, production, era, text, building, wonder, then Militia.
        std::tuple<Stage<TerrainYieldRule>, Stage<UnitCombatRule>, Stage<UniqueUnitScalarRule>, Stage<UnitMovementRule>, Stage<UnitEffectRule>, Stage<UnitProductionCostRule>, Stage<EraAbilityReplacement>, Stage<NationSelectTextRule>, Stage<BuildingEffectRuleV2>, Stage<WonderEffectRuleV2>, Stage<city_founding_unit::Rule>> stages;
        for (const auto& input : bundle.rules)
        {
            locate(result, input);
            bool valid = std::visit([&](const auto& rule)
                                    {
                                        using T = std::decay_t<decltype(rule)>;
                                        T normalized{};
                                        result.nativeResult = Registry<T>::normalize(&rule, normalized);
                                        if (result.nativeResult != 0)
                                        {
                                            return false;
                                        }
                                        result.nativeResult = Registry<T>::internalError;
                                        auto& stage         = std::get<Stage<T>>(stages);
                                        preparationCheckpoint();
                                        stage.requests.push_back(normalized);
                                        preparationCheckpoint();
                                        stage.origins.push_back(&input);
                                        return true;
                                    },
                                    input.request);
            if (!valid)
            {
                return result;
            }
        }
        std::apply([&](auto&... stage)
                   {
                       (acquire(stage, result), ...);
                   },
                   stages);
        const bool ready = std::apply([&](auto&... stage)
                                      {
                                          return (prepare(stage, result) && ...);
                                      },
                                      stages);
        if (!ready)
        {
            return result;
        }
        // From the first swap to return: no allocation, validation or throwing work.
        std::apply([](auto&... stage) noexcept
                   {
                       (commit(stage), ...);
                   },
                   stages);
        return { true };
    }
    catch (...)
    {
        return result;
    }
}

int32_t BuildCityFoundingUnitRule(RuleIdentity ids, bool enable, city_founding_unit::Rule& out) noexcept
{
    city_founding_unit::Rule rule{};
    if (!initialize(ids, rule))
    {
        return city_founding_unit::kInvalidArgument;
    }
    rule.enable = enable ? 1 : 0;
    return city_founding_unit::NormalizeRequest(&rule, out);
}

bool IsContractToken(std::string_view kind, std::string_view field, std::string_view value) noexcept
{
    bool parameterField = false;
    for (const auto& mapping : parameterMappings)
    {
        if (mapping.kind != kind)
            continue;
        for (const auto& parameter : mapping.parameters)
        {
            if (parameter.field == field)
            {
                parameterField = true;
                if (parameter.value == value)
                    return true;
            }
        }
    }
    if (parameterField)
        return false;
    int32_t mapped = 0;
    if (field == "civilization")
        return mapToken(value, civilizations, mapped);
    if (field == "base_unit_type")
        return mapToken(value, unitTypes, mapped);
    if (field == "identity")
        return mapToken(value, identities, mapped);
    if (field == "terrain")
        return mapToken(value, terrains, mapped);
    if (field == "component")
        return mapToken(value, components, mapped);
    if (field == "operation")
        return kind == "terrain_yield" ? mapToken(value, terrainOperations, mapped) : mapToken(value, scalarOperations, mapped);
    if (field == "property")
        return kind == "unit_combat" ? mapToken(value, combatProperties, mapped) : mapToken(value, scalarProperties, mapped);
    if (field == "effect")
        return mapToken(value, effects, mapped);
    if (field == "unlock_era")
        return mapToken(value, eras, mapped);
    if (field == "ability")
        return mapToken(value, abilities, mapped);
    if (field == "surface")
        return mapToken(value, surfaces, mapped);
    if (field == "display_form")
        return mapToken(value, displayForms, mapped);
    return false;
}

int32_t MapRuleParameters(std::string_view kind, std::initializer_list<RuleParameter> parameters) noexcept
{
    for (const auto& mapping : parameterMappings)
    {
        if (mapping.kind == kind && mapping.parameters.size() == parameters.size() &&
            std::all_of(mapping.parameters.begin(), mapping.parameters.end(), [&](const auto& supported)
                        {
                            return std::any_of(parameters.begin(), parameters.end(), [&](const auto& parameter)
                                               {
                                                   return parameter.field == supported.field && parameter.value == supported.value;
                                               });
                        }))
        {
            return mapping.nativeEffect;
        }
    }
    return 0;
}

std::string SupportedRuleParameterValues(std::string_view kind, std::string_view field)
{
    std::string values;
    const auto  append = [&](std::string_view value)
    {
        if (!values.empty())
            values += ", ";
        values += value;
    };
    for (const auto& mapping : parameterMappings)
    {
        if (mapping.kind == kind)
        {
            for (const auto& parameter : mapping.parameters)
            {
                if (parameter.field == field)
                    append(parameter.value);
            }
        }
    }
    if (values.empty() && kind == "era_technology_ability")
    {
        if (field == "civilization")
        {
            for (const auto& token : civilizations)
                append(token.name);
        }
        else if (field == "unlock_era")
        {
            for (const auto& token : eras)
                append(token.name);
        }
    }
    return values;
}

int32_t BuildTerrainYieldRule(RuleIdentity ids, std::string_view terrain, std::string_view component, std::string_view operation, int32_t value, TerrainYieldRule& out) noexcept
{
    TerrainYieldRule rule{};
    if (!initialize(ids, rule) || !mapToken(terrain, terrains, rule.terrain) ||
        !mapToken(component, components, rule.component) ||
        !mapToken(operation, terrainOperations, rule.operation))
    {
        return TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }
    rule.value = value;
    return terrain_yield_rules::NormalizeRequest(&rule, out);
}

int32_t BuildUnitCombatRule(RuleIdentity ids, UnitSelectors unit, std::string_view terrain, std::string_view property, int32_t percentageDelta, UnitCombatRule& out) noexcept
{
    UnitCombatRule rule{};
    if (!initialize(ids, rule) || !mapUnit(unit, rule) ||
        !mapToken(terrain, terrains, rule.terrain) ||
        !mapToken(property, combatProperties, rule.property))
    {
        return UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT;
    }
    rule.percentageDelta = percentageDelta;
    return unit_combat_rules::NormalizeRequest(&rule, out);
}

int32_t BuildUniqueUnitScalarRule(RuleIdentity ids, UnitSelectors unit, std::string_view property, std::string_view operation, int32_t value, UniqueUnitScalarRule& out) noexcept
{
    UniqueUnitScalarRule rule{};
    if (!initialize(ids, rule) || !mapUnit(unit, rule) ||
        !mapToken(property, scalarProperties, rule.property) ||
        !mapToken(operation, scalarOperations, rule.operation))
    {
        return UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    rule.value = value;
    return unique_unit_rules::NormalizeRequest(&rule, out);
}

int32_t BuildUnitMovementRule(RuleIdentity ids, UnitSelectors unit, int32_t value, UnitMovementRule& out) noexcept
{
    UnitMovementRule rule{};
    if (!initialize(ids, rule) || !mapUnit(unit, rule))
    {
        return UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    rule.value = value;
    return unit_movement_rules::NormalizeRequest(&rule, out);
}

int32_t BuildUnitEffectRule(RuleIdentity ids, UnitSelectors unit, std::string_view effect, UnitEffectRule& out) noexcept
{
    UnitEffectRule rule{};
    if (!initialize(ids, rule) || !mapUnit(unit, rule) ||
        !mapToken(effect, effects, rule.effect))
    {
        return UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    return unit_effect_rules::NormalizeRequest(&rule, out);
}

int32_t BuildUnitProductionCostRule(RuleIdentity ids, UnitSelectors unit, int32_t percentageDelta, UnitProductionCostRule& out) noexcept
{
    UnitProductionCostRule rule{};
    if (!initialize(ids, rule) || !mapUnit(unit, rule))
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT;
    }
    rule.percentageDelta = percentageDelta;
    return unit_production_cost_rules::NormalizeRequest(&rule, out);
}

int32_t BuildEraTechnologyAbility(RuleIdentity ids, std::string_view civilization, std::string_view unlockEra, std::string_view technology, EraAbilityReplacement& out) noexcept
{
    EraAbilityReplacement rule{};
    rule.replacementAbility = MapRuleParameters("era_technology_ability", { { "technology", technology } });
    if (!initialize(ids, rule) || !mapToken(civilization, civilizations, rule.civilization) ||
        !mapToken(unlockEra, eras, rule.unlockEra) ||
        rule.replacementAbility == 0)
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    return unique_era_abilities::NormalizeRequest(&rule, out);
}

int32_t BuildNationSelectTextRule(RuleIdentity ids, TextSelectors selectors, std::string_view text, NationSelectTextRule& out) noexcept
{
    NationSelectTextRule rule{};
    if (!initialize(ids, rule) || !copyString(text, rule.text) ||
        !mapToken(selectors.surface, surfaces, rule.surface) ||
        !mapToken(selectors.civilization, civilizations, rule.civilization))
    {
        return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
    }
    if (rule.surface == NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT)
    {
        if (!selectors.unlockEra.empty() || !selectors.ability.empty() ||
            !mapUnit(UnitSelectors{ selectors.civilization, selectors.baseUnitType, selectors.identity }, rule) ||
            !mapToken(selectors.displayForm, displayForms, rule.displayForm))
        {
            return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
        }
        rule.unlockEra = NATION_SELECT_TEXT_SELECTOR_UNUSED;
        rule.ability   = 0;
    }
    else
    {
        if (!selectors.baseUnitType.empty() || !selectors.identity.empty() ||
            !selectors.displayForm.empty() ||
            !mapToken(selectors.unlockEra, eras, rule.unlockEra) ||
            !mapToken(selectors.ability, abilities, rule.ability))
        {
            return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
        }
        rule.baseUnitType = NATION_SELECT_TEXT_SELECTOR_UNUSED;
        rule.identity     = NATION_SELECT_TEXT_SELECTOR_UNUSED;
        rule.displayForm  = NATION_SELECT_TEXT_SELECTOR_UNUSED;
    }
    return nation_select_text::NormalizeRequest(&rule, out);
}

} // namespace rerevved::rules_bundle
