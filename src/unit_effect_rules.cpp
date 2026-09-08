#include "unit_effect_rules_registry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "unit_catalog_api.h"

namespace rerevved::unit_effect_rules
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 152;
constexpr uint32_t kEvaluationPrefix = 20;
constexpr int32_t  kVeteranLevel     = 2;

struct NativeSpecialUpgradeMapping
{
    UnitEffectId effect;
    uint32_t     mask;
};

constexpr std::array<NativeSpecialUpgradeMapping, 9>
    kNativeSpecialUpgradeMappings = { {
        { UNIT_EFFECT_CREATION_GUERILLA, 1u << 2 },
        { UNIT_EFFECT_CREATION_BLITZ, 1u << 0 },
        { UNIT_EFFECT_CREATION_INFILTRATION, 1u << 1 },
        { UNIT_EFFECT_CREATION_LOYALTY, 1u << 3 },
        { UNIT_EFFECT_CREATION_ENGINEER, 1u << 4 },
        { UNIT_EFFECT_CREATION_LEADERSHIP, 1u << 5 },
        { UNIT_EFFECT_CREATION_MARCH, 1u << 6 },
        { UNIT_EFFECT_CREATION_MEDIC, 1u << 7 },
        { UNIT_EFFECT_CREATION_SCOUT, 1u << 8 },
    } };

std::shared_mutex           registryMutex;
std::vector<UnitEffectRule> registry;

bool isRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', UNIT_EFFECT_RULE_ID_CAPACITY);
    if (!terminator || value[0] == '\0')
    {
        return false;
    }

    for (const char* current = value; *current != '\0'; ++current)
    {
        const char c = *current;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
        {
            return false;
        }
    }
    const char first = value[0];
    return (first >= 'a' && first <= 'z') ||
           (first >= '0' && first <= '9');
}

void normalizeRuleId(char* value)
{
    const size_t length = std::strlen(value);
    std::memset(value + length + 1,
                0,
                UNIT_EFFECT_RULE_ID_CAPACITY - length - 1);
}

template <size_t Size>
bool isZeroed(const int32_t (&values)[Size])
{
    for (int32_t value : values)
    {
        if (value != 0)
        {
            return false;
        }
    }
    return true;
}

bool isCivilizationValid(CivilizationId civilization)
{
    return civilization >= 0 && civilization < CIVILIZATION_COUNT;
}

bool isUnitTypeValid(UnitTypeId unitType)
{
    return unitType >= 0 && unitType < UNIT_TYPE_COUNT;
}

bool isEffectValid(UnitEffectId effect)
{
    if (effect == UNIT_EFFECT_CREATION_VETERAN)
    {
        return true;
    }

    return std::any_of(
        kNativeSpecialUpgradeMappings.begin(),
        kNativeSpecialUpgradeMappings.end(),
        [effect](const NativeSpecialUpgradeMapping& mapping)
        {
            return mapping.effect == effect;
        });
}

bool isTargetValid(CivilizationId civilization,
                   UnitTypeId     baseUnitType,
                   UnitIdentityId identity)
{
    if (!isCivilizationValid(civilization) || !isUnitTypeValid(baseUnitType) ||
        identity == UNIT_IDENTITY_BASE ||
        identity < UNIT_IDENTITY_BASE ||
        identity >= UNIT_IDENTITY_COUNT)
    {
        return false;
    }

    UnitIdentityId resolved = UNIT_IDENTITY_BASE;
    return unit_catalog::TryResolveUnitIdentity(
               civilization, baseUnitType, resolved) &&
           resolved == identity;
}

bool targetMatches(const UnitEffectRule& rule,
                   CivilizationId        civilization,
                   UnitTypeId            baseUnitType,
                   UnitIdentityId        identity,
                   UnitEffectId          effect)
{
    return rule.civilization == civilization &&
           rule.baseUnitType == baseUnitType && rule.identity == identity &&
           rule.effect == effect;
}

bool ruleKeyMatches(const UnitEffectRule& left,
                    const UnitEffectRule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const UnitEffectRule& left,
                 const UnitEffectRule& right)
{
    const int providerOrder = std::strcmp(left.providerId, right.providerId);
    return providerOrder < 0 ||
           (providerOrder == 0 &&
            std::strcmp(left.ruleId, right.ruleId) < 0);
}

template <typename Record>
void clearOutput(Record* out, uint32_t outSize)
{
    if (out)
    {
        std::memset(out, 0, std::min<uint32_t>(outSize, sizeof(Record)));
    }
}

template <typename Record>
int32_t copyOutput(Record* out, uint32_t outSize, const Record& producer)
{
    uint32_t copySize = std::min<uint32_t>(outSize, sizeof(Record));
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, &producer, copySize);
    return UNIT_EFFECT_RULES_OK;
}

} // namespace

bool TryGetNativeSpecialUpgradeMask(UnitEffectId effect,
                                    uint32_t&    mask)
{
    const auto mapping = std::find_if(
        kNativeSpecialUpgradeMappings.begin(),
        kNativeSpecialUpgradeMappings.end(),
        [effect](const NativeSpecialUpgradeMapping& candidate)
        {
            return candidate.effect == effect;
        });
    if (mapping == kNativeSpecialUpgradeMappings.end())
    {
        return false;
    }

    mask = mapping->mask;
    return true;
}

bool TryEvaluate(CivilizationId        civilization,
                 UnitTypeId            baseUnitType,
                 UnitIdentityId        identity,
                 UnitEffectId          effect,
                 int32_t               nativeLevel,
                 UnitEffectEvaluation& evaluation)
{
    if (!isTargetValid(civilization, baseUnitType, identity) ||
        !isEffectValid(effect))
    {
        return false;
    }

    evaluation = {
        sizeof(UnitEffectEvaluation),
        nativeLevel,
        nativeLevel,
        0,
        0,
        {},
    };

    std::shared_lock lock(registryMutex);
    for (const auto& rule : registry)
    {
        if (targetMatches(rule,
                          civilization,
                          baseUnitType,
                          identity,
                          effect))
        {
            ++evaluation.grantCount;
        }
    }

    if (evaluation.grantCount != 0)
    {
        evaluation.statusFlags |= UNIT_EFFECT_EVALUATION_GRANTED;
        if (effect == UNIT_EFFECT_CREATION_VETERAN &&
            evaluation.finalLevel < kVeteranLevel)
        {
            evaluation.finalLevel = kVeteranLevel;
        }
    }
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registryMutex);
    registry.clear();
}

} // namespace rerevved::unit_effect_rules

static_assert(sizeof(UnitEffectRule) == 168);
static_assert(sizeof(UnitEffectRuleInfo) == 192);
static_assert(sizeof(UnitEffectQuery) == 44);
static_assert(sizeof(UnitEffectEvaluation) == 40);

extern "C" uint32_t UnitEffectRulesAbiVersion(void)
{
    return UNIT_EFFECT_RULES_ABI_VERSION;
}

extern "C" int32_t RegisterUnitEffectRule(
    const UnitEffectRule* rule)
{
    using namespace rerevved::unit_effect_rules;
    if (!rule || rule->structSize < sizeof(UnitEffectRule) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isTargetValid(rule->civilization,
                       rule->baseUnitType,
                       rule->identity) ||
        !isEffectValid(rule->effect) || !isZeroed(rule->reserved))
    {
        return UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    UnitEffectRule normalized = *rule;
    normalized.structSize     = sizeof(normalized);
    normalizeRuleId(normalized.providerId);
    normalizeRuleId(normalized.ruleId);

    try
    {
        std::unique_lock lock(registryMutex);
        const auto       duplicate = std::find_if(
            registry.begin(), registry.end(), [&](const auto& candidate)
            {
                return ruleKeyMatches(candidate, normalized);
            });
        if (duplicate != registry.end())
        {
            return std::memcmp(&*duplicate, &normalized, sizeof(normalized)) == 0
                       ? UNIT_EFFECT_RULES_OK
                       : UNIT_EFFECT_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return UNIT_EFFECT_RULES_ERR_INTERNAL;
    }
    return UNIT_EFFECT_RULES_OK;
}

extern "C" int32_t GetUnitEffectRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unit_effect_rules::registryMutex);
    *outCount =
        static_cast<uint32_t>(rerevved::unit_effect_rules::registry.size());
    return UNIT_EFFECT_RULES_OK;
}

extern "C" int32_t GetUnitEffectRule(
    uint32_t            index,
    UnitEffectRuleInfo* out,
    uint32_t            outSize)
{
    using namespace rerevved::unit_effect_rules;
    if (!out)
    {
        return UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto&        rule = registry[index];
    UnitEffectRuleInfo result{};
    result.structSize   = sizeof(result);
    result.civilization = rule.civilization;
    result.baseUnitType = rule.baseUnitType;
    result.identity     = rule.identity;
    result.effect       = rule.effect;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    return copyOutput(out, outSize, result);
}

extern "C" int32_t EvaluateUnitEffect(
    const UnitEffectQuery* query,
    UnitEffectEvaluation*  out,
    uint32_t               outSize)
{
    using namespace rerevved::unit_effect_rules;
    if (!out)
    {
        return UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->structSize < sizeof(UnitEffectQuery) ||
        !isZeroed(query->reserved))
    {
        return UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    UnitEffectEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->baseUnitType,
                     query->identity,
                     query->effect,
                     query->nativeLevel,
                     result))
    {
        return UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result);
}
