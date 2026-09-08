#include "unit_combat_rules_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "unit_catalog_api.h"

namespace rerevved::unit_combat_rules
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 160;
constexpr uint32_t kEvaluationPrefix = 20;
constexpr int32_t  kNativePercent    = 100;

std::shared_mutex                   registryMutex;
std::vector<ReRevvedUnitCombatRule> registry;

bool isRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', REREVVED_UNIT_COMBAT_RULE_ID_CAPACITY);
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
                REREVVED_UNIT_COMBAT_RULE_ID_CAPACITY - length - 1);
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

bool isCivilizationValid(ReRevvedCivilizationId civilization)
{
    return civilization >= 0 && civilization < REREVVED_CIVILIZATION_COUNT;
}

bool isUnitTypeValid(ReRevvedUnitTypeId unitType)
{
    return unitType >= 0 && unitType < REREVVED_UNIT_TYPE_COUNT;
}

bool isTerrainValid(ReRevvedTerrainId terrain)
{
    // ABI 1 deliberately exposes only the recovered Forest combat terrain.
    return terrain == REREVVED_TERRAIN_FOREST;
}

bool isPropertyValid(ReRevvedUnitCombatProperty property)
{
    return property == REREVVED_UNIT_COMBAT_ATTACK ||
           property == REREVVED_UNIT_COMBAT_DEFENSE;
}

bool isTargetValid(ReRevvedCivilizationId civilization,
                   ReRevvedUnitTypeId     baseUnitType,
                   ReRevvedUnitIdentityId identity)
{
    if (!isCivilizationValid(civilization) || !isUnitTypeValid(baseUnitType) ||
        identity == REREVVED_UNIT_IDENTITY_BASE ||
        identity < REREVVED_UNIT_IDENTITY_BASE ||
        identity >= REREVVED_UNIT_IDENTITY_COUNT)
    {
        return false;
    }

    ReRevvedUnitIdentityId resolved = REREVVED_UNIT_IDENTITY_BASE;
    return unit_catalog::TryResolveUnitIdentity(
               civilization, baseUnitType, resolved) &&
           resolved == identity;
}

bool targetMatches(const ReRevvedUnitCombatRule& rule,
                   ReRevvedCivilizationId        civilization,
                   ReRevvedUnitTypeId            baseUnitType,
                   ReRevvedUnitIdentityId        identity,
                   ReRevvedTerrainId             terrain,
                   ReRevvedUnitCombatProperty    property)
{
    return rule.civilization == civilization &&
           rule.baseUnitType == baseUnitType && rule.identity == identity &&
           rule.terrain == terrain && rule.property == property;
}

bool ruleKeyMatches(const ReRevvedUnitCombatRule& left,
                    const ReRevvedUnitCombatRule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const ReRevvedUnitCombatRule& left,
                 const ReRevvedUnitCombatRule& right)
{
    const int providerOrder = std::strcmp(left.providerId, right.providerId);
    return providerOrder < 0 ||
           (providerOrder == 0 &&
            std::strcmp(left.ruleId, right.ruleId) < 0);
}

template <typename Record>
void clearOutput(Record* out, uint32_t outSize)
{
    std::memset(out, 0, std::min<uint32_t>(outSize, sizeof(Record)));
}

template <typename Record>
int32_t copyOutput(Record* out, uint32_t outSize, const Record& producer)
{
    uint32_t copySize = std::min<uint32_t>(outSize, sizeof(Record));
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, &producer, copySize);
    return REREVVED_UNIT_COMBAT_RULES_OK;
}

} // namespace

bool TryEvaluate(ReRevvedCivilizationId        civilization,
                 ReRevvedUnitTypeId            baseUnitType,
                 ReRevvedUnitIdentityId        identity,
                 ReRevvedTerrainId             terrain,
                 ReRevvedUnitCombatProperty    property,
                 ReRevvedUnitCombatEvaluation& evaluation)
{
    if (!isTargetValid(civilization, baseUnitType, identity) ||
        !isTerrainValid(terrain) || !isPropertyValid(property))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedUnitCombatEvaluation),
        kNativePercent,
        kNativePercent,
        0,
        0,
        {},
    };

    std::shared_lock lock(registryMutex);
    int64_t          additiveSum      = 0;
    bool             additiveOverflow = false;
    for (const auto& rule : registry)
    {
        if (!targetMatches(rule,
                           civilization,
                           baseUnitType,
                           identity,
                           terrain,
                           property))
        {
            continue;
        }

        ++evaluation.additiveCount;
        if ((rule.percentageDelta > 0 &&
             additiveSum > std::numeric_limits<int64_t>::max() -
                               rule.percentageDelta) ||
            (rule.percentageDelta < 0 &&
             additiveSum < std::numeric_limits<int64_t>::min() -
                               rule.percentageDelta))
        {
            additiveOverflow = true;
        }
        else
        {
            additiveSum += rule.percentageDelta;
        }
    }

    const int64_t composed = static_cast<int64_t>(kNativePercent) +
                             additiveSum;
    if (additiveOverflow || composed <= 0 ||
        composed > std::numeric_limits<int32_t>::max())
    {
        evaluation.statusFlags |=
            REREVVED_UNIT_COMBAT_EVALUATION_OUT_OF_RANGE;
        return true;
    }

    evaluation.finalPercent = static_cast<int32_t>(composed);
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registryMutex);
    registry.clear();
}

} // namespace rerevved::unit_combat_rules

static_assert(sizeof(ReRevvedUnitCombatRule) == 168);
static_assert(sizeof(ReRevvedUnitCombatRuleInfo) == 192);
static_assert(sizeof(ReRevvedUnitCombatQuery) == 40);
static_assert(sizeof(ReRevvedUnitCombatEvaluation) == 40);

extern "C" uint32_t ReRevvedUnitCombatRulesAbiVersion(void)
{
    return REREVVED_UNIT_COMBAT_RULES_ABI_VERSION;
}

extern "C" int32_t ReRevvedRegisterUnitCombatRule(
    const ReRevvedUnitCombatRule* rule)
{
    using namespace rerevved::unit_combat_rules;
    if (!rule || rule->structSize < sizeof(ReRevvedUnitCombatRule) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isTargetValid(rule->civilization, rule->baseUnitType, rule->identity) ||
        !isTerrainValid(rule->terrain) || !isPropertyValid(rule->property) ||
        !isZeroed(rule->reserved))
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitCombatRule normalized = *rule;
    normalized.structSize             = sizeof(normalized);
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
                       ? REREVVED_UNIT_COMBAT_RULES_OK
                       : REREVVED_UNIT_COMBAT_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_INTERNAL;
    }
    return REREVVED_UNIT_COMBAT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUnitCombatRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unit_combat_rules::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::unit_combat_rules::registry.size());
    return REREVVED_UNIT_COMBAT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUnitCombatRule(
    uint32_t                    index,
    ReRevvedUnitCombatRuleInfo* out,
    uint32_t                    outSize)
{
    using namespace rerevved::unit_combat_rules;
    if (!out)
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto&                rule = registry[index];
    ReRevvedUnitCombatRuleInfo result{};
    result.structSize      = sizeof(result);
    result.civilization    = rule.civilization;
    result.baseUnitType    = rule.baseUnitType;
    result.identity        = rule.identity;
    result.terrain         = rule.terrain;
    result.property        = rule.property;
    result.percentageDelta = rule.percentageDelta;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    return copyOutput(out, outSize, result);
}

extern "C" int32_t ReRevvedEvaluateUnitCombat(
    const ReRevvedUnitCombatQuery* query,
    ReRevvedUnitCombatEvaluation*  out,
    uint32_t                       outSize)
{
    using namespace rerevved::unit_combat_rules;
    if (!out)
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->structSize < sizeof(ReRevvedUnitCombatQuery) ||
        !isZeroed(query->reserved))
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitCombatEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->baseUnitType,
                     query->identity,
                     query->terrain,
                     query->property,
                     result))
    {
        return REREVVED_UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result);
}
