#include "unit_production_cost_rules_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "unit_catalog_api.h"

namespace rerevved::unit_production_cost_rules
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 152;
constexpr uint32_t kEvaluationPrefix = 20;
constexpr int32_t  kNativePercent    = 100;

std::shared_mutex                   registryMutex;
std::vector<UnitProductionCostRule> registry;

bool isRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', UNIT_PRODUCTION_COST_RULE_ID_CAPACITY);
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
                UNIT_PRODUCTION_COST_RULE_ID_CAPACITY - length - 1);
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

bool targetMatches(const UnitProductionCostRule& rule,
                   CivilizationId                civilization,
                   UnitTypeId                    baseUnitType,
                   UnitIdentityId                identity)
{
    return rule.civilization == civilization &&
           rule.baseUnitType == baseUnitType && rule.identity == identity;
}

bool ruleKeyMatches(const UnitProductionCostRule& left,
                    const UnitProductionCostRule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const UnitProductionCostRule& left,
                 const UnitProductionCostRule& right)
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
    return UNIT_PRODUCTION_COST_RULES_OK;
}

} // namespace

bool TryEvaluate(CivilizationId                civilization,
                 UnitTypeId                    baseUnitType,
                 UnitIdentityId                identity,
                 UnitProductionCostEvaluation& evaluation)
{
    if (!isTargetValid(civilization, baseUnitType, identity))
    {
        return false;
    }

    evaluation = {
        sizeof(UnitProductionCostEvaluation),
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
        if (!targetMatches(rule, civilization, baseUnitType, identity))
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

    const int64_t composed = static_cast<int64_t>(kNativePercent) + additiveSum;
    if (additiveOverflow ||
        composed <= 0 ||
        composed > std::numeric_limits<int32_t>::max())
    {
        evaluation.statusFlags |=
            UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE;
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

} // namespace rerevved::unit_production_cost_rules

static_assert(sizeof(UnitProductionCostRule) == 168);
static_assert(sizeof(UnitProductionCostRuleInfo) == 192);
static_assert(sizeof(UnitProductionCostQuery) == 40);
static_assert(sizeof(UnitProductionCostEvaluation) == 40);

extern "C" uint32_t UnitProductionCostRulesAbiVersion(void)
{
    return UNIT_PRODUCTION_COST_RULES_ABI_VERSION;
}

extern "C" int32_t RegisterUnitProductionCostRule(
    const UnitProductionCostRule* rule)
{
    using namespace rerevved::unit_production_cost_rules;
    if (!rule || rule->structSize < sizeof(UnitProductionCostRule) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isTargetValid(rule->civilization, rule->baseUnitType, rule->identity) ||
        !isZeroed(rule->reserved))
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT;
    }

    UnitProductionCostRule normalized = *rule;
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
                       ? UNIT_PRODUCTION_COST_RULES_OK
                       : UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INTERNAL;
    }
    return UNIT_PRODUCTION_COST_RULES_OK;
}

extern "C" int32_t GetUnitProductionCostRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unit_production_cost_rules::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::unit_production_cost_rules::registry.size());
    return UNIT_PRODUCTION_COST_RULES_OK;
}

extern "C" int32_t GetUnitProductionCostRule(
    uint32_t                    index,
    UnitProductionCostRuleInfo* out,
    uint32_t                    outSize)
{
    using namespace rerevved::unit_production_cost_rules;
    if (!out)
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto&                rule = registry[index];
    UnitProductionCostRuleInfo result{};
    result.structSize      = sizeof(result);
    result.civilization    = rule.civilization;
    result.baseUnitType    = rule.baseUnitType;
    result.identity        = rule.identity;
    result.percentageDelta = rule.percentageDelta;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    return copyOutput(out, outSize, result);
}

extern "C" int32_t EvaluateUnitProductionCost(
    const UnitProductionCostQuery* query,
    UnitProductionCostEvaluation*  out,
    uint32_t                       outSize)
{
    using namespace rerevved::unit_production_cost_rules;
    if (!out)
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->structSize < sizeof(UnitProductionCostQuery) ||
        !isZeroed(query->reserved))
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT;
    }

    UnitProductionCostEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->baseUnitType,
                     query->identity,
                     result))
    {
        return UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result);
}
