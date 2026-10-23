#include "mod_script_rules.h"
#include "rules_bundle.h"
#include <algorithm>
#include <array>
#include <building_effect_rules.h>
#include <cmath>
#include <cstring>
#include <utility>
#include <wonder_effect_rules.h>

namespace rerevved
{
namespace
{

using namespace rex::system;
using namespace rules_bundle;
using Value = ModScriptValue;
using Type  = Value::Type;

struct Schema
{
    std::string_view                        kind;
    std::initializer_list<std::string_view> fields;
};

const Schema schemas[] = {
    { "terrain_yield", { "terrain", "component", "operation", "value" } },
    { "unit_combat", { "civilization", "base_unit_type", "identity", "terrain", "property", "percentage_delta" } },
    { "unique_unit_scalar", { "civilization", "base_unit_type", "identity", "property", "operation", "value" } },
    { "unit_movement", { "civilization", "base_unit_type", "identity", "value" } },
    { "unit_effect", { "civilization", "base_unit_type", "identity", "effect" } },
    { "unit_production_cost", { "civilization", "base_unit_type", "identity", "percentage_delta" } },
    { "era_technology_ability", { "civilization", "unlock_era", "technology" } },
    { "nation_select_text", { "surface", "text", "civilization" } },
    { "building_yield", { "civilization", "building", "yield", "amount" } },
    { "wonder_completion_technology", { "wonder", "technology" } },
    { "city_founding_unit", { "unit", "upgrades" } },
};

struct EffectRequest
{
    std::string                                            locator;
    std::variant<BuildingEffectRuleV2, WonderEffectRuleV2> rule;
};

#ifdef RULES_BUNDLE_TESTING
void (*observeBuildingEffect)(const BuildingEffectRuleV2&) = nullptr;
void (*observeWonderEffect)(const WonderEffectRuleV2&)     = nullptr;
#endif

template <typename Rule>
Rule effectRequest(std::string_view provider, std::string_view id, int32_t effect, int32_t amount)
{
    Rule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider.data(), provider.size());
    std::memcpy(rule.ruleId, id.data(), id.size());
    rule.effect = effect;
    rule.amount = amount;
    return rule;
}

const char* typeName(Type type)
{
    switch (type)
    {
        case Type::kBoolean:
            return "boolean";
        case Type::kInteger:
            return "integer";
        case Type::kNumber:
            return "number";
        case Type::kString:
            return "string";
        case Type::kTable:
            return "table";
    }
    return "unknown";
}

[[noreturn]] void fail(const std::string& locator, std::string message, ModScriptStage stage = ModScriptStage::kSchema)
{
    throw ModScriptPublishResult{ false, stage, locator + ": " + message };
}

const Value& field(const Value& table, std::string_view name, const std::string& locator)
{
    for (const auto& entry : table.entries)
    {
        if (entry.key.type == Type::kString && entry.key.string == name)
        {
            return entry.value;
        }
    }
    fail(locator, "field " + std::string(name) + ": required");
}

std::string_view stringField(const Value& table, std::string_view name, const std::string& locator)
{
    const auto& value = field(table, name, locator);
    if (value.type != Type::kString)
    {
        fail(locator, "field " + std::string(name) + ": expected string, got " + typeName(value.type));
    }
    if (value.string.find('\0') != std::string::npos)
    {
        fail(locator, "field " + std::string(name) + ": embedded NUL");
    }
    return value.string;
}

std::string_view upgradeField(const Value& table, const std::string& locator)
{
    const auto& value = field(table, "upgrades", locator);
    if (value.type != Type::kTable || value.entries.size() != 1 ||
        value.entries[0].key.type != Type::kInteger || value.entries[0].key.integer != 1 ||
        value.entries[0].value.type != Type::kString)
    {
        fail(locator, "field upgrades: expected dense array containing one upgrade; supported values: " + SupportedRuleParameterValues("city_founding_unit", "upgrades"));
    }
    return value.entries[0].value.string;
}

int32_t integerField(const Value& table, std::string_view name, const std::string& locator)
{
    const auto& value = field(table, name, locator);
    if (value.type == Type::kInteger)
    {
        if (value.integer >= INT32_MIN && value.integer <= INT32_MAX)
        {
            return static_cast<int32_t>(value.integer);
        }
    }
    else if (value.type == Type::kNumber)
    {
        if (std::isfinite(value.number) && std::trunc(value.number) == value.number &&
            value.number >= INT32_MIN && value.number <= INT32_MAX)
        {
            return static_cast<int32_t>(value.number);
        }
    }
    else
    {
        fail(locator, "field " + std::string(name) + ": expected integer, got " + typeName(value.type));
    }
    fail(locator, "field " + std::string(name) + ": expected finite integral signed 32-bit value");
}

void checkId(std::string_view id, const std::string& locator, std::string_view name)
{
    const auto alnum = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (id.empty() || id.size() > 63 || !alnum(id.front()) ||
        !std::all_of(id.begin(), id.end(), [&](char c)
                     {
                         return alnum(c) || c == '.' || c == '_' || c == '-';
                     }))
    {
        fail(locator, "field " + std::string(name) + ": expected 1..63 ID bytes, lowercase letter or digit first, then [a-z0-9._-]");
    }
}

void checkTable(const Value& table, const std::string& locator)
{
    if (table.type != Type::kTable)
    {
        fail(locator, std::string("expected table, got ") + typeName(table.type));
    }
    for (size_t i = 0; i < table.entries.size(); ++i)
    {
        const auto& key = table.entries[i].key;
        if (key.type != Type::kString || key.string.find('\0') != std::string::npos)
        {
            fail(locator, "expected string field keys without NUL");
        }
        for (size_t j = 0; j < i; ++j)
        {
            if (table.entries[j].key.string == key.string)
            {
                fail(locator, "field " + key.string + ": duplicate field");
            }
        }
    }
}

class RulesHost final : public IModScriptHost
{
public:
    RulesHost()
    {
        for (const auto& schema : schemas)
        {
            contracts_[0].domains.emplace_back(schema.kind);
        }
    }

    std::span<const ModScriptContract> contracts() const override
    {
        return contracts_;
    }

    ModScriptPublishResult Publish(const ModScriptPublishRequest& request) noexcept override
    {
        ModScriptPublishResult fallback;
        fallback.stage = ModScriptStage::kPublish;
        try
        {
            // Prepare the exception diagnostic before any registry commit.
            fallback.message = "host preparation failed; no rules from this bundle published";
            if (request.api != "rerevved.rules" || request.api_version != 1)
            {
                fail("contract", "expected rerevved.rules version 1", ModScriptStage::kSupport);
            }
            const auto& root = request.value;
            checkTable(root, "root");
            for (const auto& entry : root.entries)
            {
                if (entry.key.string != "provider_id" && entry.key.string != "rules")
                {
                    fail("root", "field " + entry.key.string + ": unknown field");
                }
            }
            const auto provider = stringField(root, "provider_id", "root");
            checkId(provider, "root", "provider_id");
            const auto& rules = field(root, "rules", "root");
            if (rules.type != Type::kTable || rules.entries.empty() || rules.entries.size() > 256)
            {
                fail("rules", "expected dense array of 1..256 records");
            }
            RulesBundle bundle;
            bundle.rules.reserve(rules.entries.size());
            std::vector<EffectRequest> effects;
            for (size_t i = 0; i < rules.entries.size(); ++i)
            {
                std::string locator = "rules[" + std::to_string(i + 1) + "]";
                const auto& entry   = rules.entries[i];
                if (entry.key.type != Type::kInteger || entry.key.integer != static_cast<int64_t>(i + 1))
                {
                    fail(locator, "expected dense integer keys 1..n");
                }
                const auto& record = entry.value;
                checkTable(record, locator);
                const auto id = stringField(record, "id", locator);
                checkId(id, locator, "id");
                locator += " " + std::string(id);
                const auto kind   = stringField(record, "kind", locator);
                const auto schema = std::find_if(std::begin(schemas), std::end(schemas), [&](const auto& item)
                                                 {
                                                     return item.kind == kind;
                                                 });
                if (schema == std::end(schemas))
                {
                    fail(locator, "field kind: unsupported kind " + std::string(kind), ModScriptStage::kSupport);
                }
                if (std::find(request.declared_domains.begin(), request.declared_domains.end(), kind) == request.declared_domains.end())
                {
                    fail(locator, "field kind: domain not declared in requires");
                }
                std::initializer_list<std::string_view> textFields;
                if (kind == "nation_select_text")
                {
                    const auto surface = stringField(record, "surface", locator);
                    if (surface == "unique_unit")
                    {
                        textFields = unitTextFields;
                    }
                    else if (surface == "era_ability")
                    {
                        textFields = eraTextFields;
                    }
                    else
                    {
                        fail(locator, "field surface: invalid token");
                    }
                }
                for (const auto& item : record.entries)
                {
                    const auto& name = item.key.string;
                    if (name != "id" && name != "kind" &&
                        std::find(schema->fields.begin(), schema->fields.end(), name) == schema->fields.end() &&
                        std::find(textFields.begin(), textFields.end(), name) == textFields.end())
                    {
                        fail(locator, "field " + name + ": unknown field");
                    }
                }
                const auto validate = [&](std::string_view name)
                {
                    if (name == "value" || name == "percentage_delta" || name == "amount")
                    {
                        integerField(record, name, locator);
                    }
                    else
                    {
                        const auto value = name == "upgrades" ? upgradeField(record, locator) : stringField(record, name, locator);
                        if (name != "text" && !IsContractToken(kind, name, value))
                        {
                            const auto supported = SupportedRuleParameterValues(kind, name);
                            fail(locator, "field " + std::string(name) + (supported.empty() ? ": invalid token" : ": unsupported value; supported values: " + supported));
                        }
                    }
                };
                for (const auto name : schema->fields)
                    validate(name);
                for (const auto name : textFields)
                    validate(name);
                const auto s = [&](std::string_view name)
                {
                    return stringField(record, name, locator);
                };
                const auto n = [&](std::string_view name)
                {
                    return integerField(record, name, locator);
                };
                const auto unit = [&]()
                {
                    return UnitSelectors{ s("civilization"), s("base_unit_type"), s("identity") };
                };
                const RuleIdentity ids{ provider, id };
                NativeRequest      mapped;
                int32_t            status = 0;
                switch (schema - std::begin(schemas))
                {
                    case 0:
                        status = BuildTerrainYieldRule(ids, s("terrain"), s("component"), s("operation"), n("value"), mapped.emplace<TerrainYieldRule>());
                        break;
                    case 1:
                        status = BuildUnitCombatRule(ids, unit(), s("terrain"), s("property"), n("percentage_delta"), mapped.emplace<UnitCombatRule>());
                        break;
                    case 2:
                        status = BuildUniqueUnitScalarRule(ids, unit(), s("property"), s("operation"), n("value"), mapped.emplace<UniqueUnitScalarRule>());
                        break;
                    case 3:
                        status = BuildUnitMovementRule(ids, unit(), n("value"), mapped.emplace<UnitMovementRule>());
                        break;
                    case 4:
                        status = BuildUnitEffectRule(ids, unit(), s("effect"), mapped.emplace<UnitEffectRule>());
                        break;
                    case 5:
                        status = BuildUnitProductionCostRule(ids, unit(), n("percentage_delta"), mapped.emplace<UnitProductionCostRule>());
                        break;
                    case 6:
                        status = BuildEraTechnologyAbility(ids, s("civilization"), s("unlock_era"), s("technology"), mapped.emplace<EraAbilityReplacement>());
                        break;
                    case 7:
                    {
                        const auto text = s("text");
                        if (text.empty() || text.size() > 255 || !std::all_of(text.begin(), text.end(), [](unsigned char c)
                                                                              {
                                                                                  return c >= 32 && c <= 126;
                                                                              }))
                        {
                            fail(locator, "field text: expected 1..255 printable ASCII bytes");
                        }
                        TextSelectors selectors{};
                        selectors.surface      = s("surface");
                        selectors.civilization = s("civilization");
                        if (selectors.surface == "unique_unit")
                        {
                            selectors.baseUnitType = s("base_unit_type");
                            selectors.identity     = s("identity");
                            selectors.displayForm  = s("display_form");
                        }
                        else
                        {
                            selectors.unlockEra = s("unlock_era");
                            selectors.ability   = s("ability");
                        }
                        status = BuildNationSelectTextRule(ids, selectors, text, mapped.emplace<NationSelectTextRule>());
                        break;
                    }
                    case 8:
                    {
                        // Effect records are also reported to the support check; the bundle publishes them with the other rules.
                        const auto effect  = MapRuleParameters(kind, { { "civilization", s("civilization") }, { "building", s("building") }, { "yield", s("yield") } });
                        const auto request = effectRequest<BuildingEffectRuleV2>(provider, id, effect, n("amount"));
                        effects.push_back({ locator, request });
                        mapped.emplace<BuildingEffectRuleV2>(request);
                        break;
                    }
                    case 9:
                    {
                        const auto effect  = MapRuleParameters(kind, { { "wonder", s("wonder") }, { "technology", s("technology") } });
                        const auto request = effectRequest<WonderEffectRuleV2>(provider, id, effect, 0);
                        effects.push_back({ locator, request });
                        mapped.emplace<WonderEffectRuleV2>(request);
                        break;
                    }
                    case 10:
                    {
                        const auto enable = MapRuleParameters(kind, { { "unit", s("unit") }, { "upgrades", upgradeField(record, locator) } });
                        if (enable == 0)
                            fail(locator, "unsupported unit and upgrades combination");
                        status = BuildCityFoundingUnitRule(ids, true, mapped.emplace<city_founding_unit::Rule>());
                        break;
                    }
                }
                if (status != 0)
                {
                    fail(locator, "invalid selector combination or value; native result " + std::to_string(status));
                }
                bundle.rules.push_back({ i + 1, std::move(mapped) });
            }
            // Check each requested domain before any registry publication.
            BuildingEffectSupportV2 buildingSupport{};
            WonderEffectSupportV2   wonderSupport{};
            int32_t                 buildingStatus = 0;
            int32_t                 wonderStatus   = 0;
            const auto              hasBuilding    = std::any_of(effects.begin(), effects.end(), [](const auto& entry)
                                                                 {
                                                     return std::holds_alternative<BuildingEffectRuleV2>(entry.rule);
                                                                 });
            const auto              hasWonder      = std::any_of(effects.begin(), effects.end(), [](const auto& entry)
                                                                 {
                                                   return std::holds_alternative<WonderEffectRuleV2>(entry.rule);
                                                                 });
            if (hasBuilding)
            {
                buildingStatus = GetBuildingEffectSupportV2(&buildingSupport, sizeof(buildingSupport));
            }
            if (hasWonder)
            {
                wonderStatus = GetWonderEffectSupportV2(&wonderSupport, sizeof(wonderSupport));
            }
#ifdef RULES_BUNDLE_TESTING
            for (const auto& entry : effects)
            {
                if (const auto* rule = std::get_if<BuildingEffectRuleV2>(&entry.rule); rule && observeBuildingEffect)
                    observeBuildingEffect(*rule);
                if (const auto* rule = std::get_if<WonderEffectRuleV2>(&entry.rule); rule && observeWonderEffect)
                    observeWonderEffect(*rule);
            }
#endif
            for (const auto& entry : effects)
            {
                const bool        building  = std::holds_alternative<BuildingEffectRuleV2>(entry.rule);
                const auto        status    = building ? buildingStatus : wonderStatus;
                const bool        available = building ? buildingSupport.runtimeAvailable != 0 : wonderSupport.runtimeAvailable != 0;
                const std::string domain    = building ? "Building Effect Rules" : "Wonder Effect Rules";
                if (status != 0)
                {
                    fail(entry.locator, domain + " support query failed; no rules from this bundle published", ModScriptStage::kSupport);
                }
                if (!available)
                {
                    fail(entry.locator, domain + " runtime integration unavailable; no rules from this bundle published", ModScriptStage::kSupport);
                }
            }
            ModScriptPublishResult success;
            success.published = true;
            const auto result = PublishRulesBundle(bundle);
            if (result.published)
            {
                // Only noexcept destruction and moves follow the registry swaps.
                return success;
            }
            std::string reason;
            if (result.nativeResult == -12)
            {
                reason = "conflicting rule identity; ";
            }
            else if (result.nativeResult == -14)
            {
                reason = "integration unavailable; ";
            }
            fail("rules[" + std::to_string(result.bundleIndex) + "] " + result.ruleId,
                 reason + "native result " + std::to_string(result.nativeResult) + "; no rules from this bundle published",
                 ModScriptStage::kPublish);
        }
        catch (ModScriptPublishResult& failure)
        {
            return std::move(failure);
        }
        catch (...)
        {
            return fallback;
        }
    }

private:
    static constexpr std::initializer_list<std::string_view> unitTextFields{ "base_unit_type", "identity", "display_form" };
    static constexpr std::initializer_list<std::string_view> eraTextFields{ "unlock_era", "ability" };
    std::array<ModScriptContract, 1>                         contracts_{ ModScriptContract{ "rerevved.rules", 1, {} } };
};

} // namespace

#ifdef RULES_BUNDLE_TESTING
void ObserveEffectRequestsForTests(void (*building)(const BuildingEffectRuleV2&),
                                   void (*wonder)(const WonderEffectRuleV2&)) noexcept
{
    observeBuildingEffect = building;
    observeWonderEffect   = wonder;
}
#endif

std::shared_ptr<rex::system::IModScriptHost> CreateModScriptRulesHost()
{
    return std::make_shared<RulesHost>();
}

} // namespace rerevved
