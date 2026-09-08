#include "unit_movement_rules_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "unit_catalog_api.h"

namespace rerevved::unit_movement_rules
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 152;
constexpr uint32_t kEvaluationPrefix = 20;

std::shared_mutex                     registryMutex;
std::vector<ReRevvedUnitMovementRule> registry;

bool isRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', REREVVED_UNIT_MOVEMENT_RULE_ID_CAPACITY);
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
                REREVVED_UNIT_MOVEMENT_RULE_ID_CAPACITY - length - 1);
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

bool targetMatches(const ReRevvedUnitMovementRule& rule,
                   ReRevvedCivilizationId          civilization,
                   ReRevvedUnitTypeId              baseUnitType,
                   ReRevvedUnitIdentityId          identity)
{
    return rule.civilization == civilization &&
           rule.baseUnitType == baseUnitType && rule.identity == identity;
}

bool ruleKeyMatches(const ReRevvedUnitMovementRule& left,
                    const ReRevvedUnitMovementRule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const ReRevvedUnitMovementRule& left,
                 const ReRevvedUnitMovementRule& right)
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
    return REREVVED_UNIT_MOVEMENT_RULES_OK;
}

} // namespace

bool TryEvaluate(ReRevvedCivilizationId          civilization,
                 ReRevvedUnitTypeId              baseUnitType,
                 ReRevvedUnitIdentityId          identity,
                 int32_t                         nativeValue,
                 ReRevvedUnitMovementEvaluation& evaluation)
{
    if (!isTargetValid(civilization, baseUnitType, identity))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedUnitMovementEvaluation),
        nativeValue,
        nativeValue,
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
        if ((rule.value > 0 &&
             additiveSum > std::numeric_limits<int64_t>::max() - rule.value) ||
            (rule.value < 0 &&
             additiveSum < std::numeric_limits<int64_t>::min() - rule.value))
        {
            additiveOverflow = true;
        }
        else
        {
            additiveSum += rule.value;
        }
    }

    const int64_t composed = static_cast<int64_t>(nativeValue) + additiveSum;
    if (additiveOverflow ||
        composed < std::numeric_limits<int32_t>::min() ||
        composed > std::numeric_limits<int32_t>::max())
    {
        evaluation.statusFlags |= REREVVED_UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW;
        return true;
    }

    evaluation.finalValue = static_cast<int32_t>(composed);
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registryMutex);
    registry.clear();
}

} // namespace rerevved::unit_movement_rules

static_assert(sizeof(ReRevvedUnitMovementRule) == 168);
static_assert(sizeof(ReRevvedUnitMovementRuleInfo) == 192);
static_assert(sizeof(ReRevvedUnitMovementQuery) == 40);
static_assert(sizeof(ReRevvedUnitMovementEvaluation) == 40);

extern "C" uint32_t ReRevvedUnitMovementRulesAbiVersion(void)
{
    return REREVVED_UNIT_MOVEMENT_RULES_ABI_VERSION;
}

extern "C" int32_t ReRevvedRegisterUnitMovementRule(
    const ReRevvedUnitMovementRule* rule)
{
    using namespace rerevved::unit_movement_rules;
    if (!rule || rule->structSize < sizeof(ReRevvedUnitMovementRule) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isTargetValid(rule->civilization, rule->baseUnitType, rule->identity) ||
        !isZeroed(rule->reserved))
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitMovementRule normalized = *rule;
    normalized.structSize               = sizeof(normalized);
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
                       ? REREVVED_UNIT_MOVEMENT_RULES_OK
                       : REREVVED_UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INTERNAL;
    }
    return REREVVED_UNIT_MOVEMENT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUnitMovementRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unit_movement_rules::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::unit_movement_rules::registry.size());
    return REREVVED_UNIT_MOVEMENT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUnitMovementRule(
    uint32_t                      index,
    ReRevvedUnitMovementRuleInfo* out,
    uint32_t                      outSize)
{
    using namespace rerevved::unit_movement_rules;
    if (!out)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto&                  rule = registry[index];
    ReRevvedUnitMovementRuleInfo result{};
    result.structSize   = sizeof(result);
    result.civilization = rule.civilization;
    result.baseUnitType = rule.baseUnitType;
    result.identity     = rule.identity;
    result.value        = rule.value;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    return copyOutput(out, outSize, result);
}

extern "C" int32_t ReRevvedEvaluateUnitMovement(
    const ReRevvedUnitMovementQuery* query,
    ReRevvedUnitMovementEvaluation*  out,
    uint32_t                         outSize)
{
    using namespace rerevved::unit_movement_rules;
    if (!out)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->structSize < sizeof(ReRevvedUnitMovementQuery) ||
        !isZeroed(query->reserved))
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitMovementEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->baseUnitType,
                     query->identity,
                     query->nativeValue,
                     result))
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result);
}
