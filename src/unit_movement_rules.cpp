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

std::shared_mutex             registryMutex;
std::vector<UnitMovementRule> registry;

bool isRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', UNIT_MOVEMENT_RULE_ID_CAPACITY);
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
                UNIT_MOVEMENT_RULE_ID_CAPACITY - length - 1);
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

bool targetMatches(const UnitMovementRule& rule,
                   CivilizationId          civilization,
                   UnitTypeId              baseUnitType,
                   UnitIdentityId          identity)
{
    return rule.civilization == civilization &&
           rule.baseUnitType == baseUnitType && rule.identity == identity;
}

bool ruleKeyMatches(const UnitMovementRule& left,
                    const UnitMovementRule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const UnitMovementRule& left,
                 const UnitMovementRule& right)
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
    return UNIT_MOVEMENT_RULES_OK;
}

} // namespace

bool TryEvaluate(CivilizationId          civilization,
                 UnitTypeId              baseUnitType,
                 UnitIdentityId          identity,
                 int32_t                 nativeValue,
                 UnitMovementEvaluation& evaluation)
{
    if (!isTargetValid(civilization, baseUnitType, identity))
    {
        return false;
    }

    evaluation = {
        sizeof(UnitMovementEvaluation),
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
        evaluation.statusFlags |= UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW;
        return true;
    }

    evaluation.finalValue = static_cast<int32_t>(composed);
    return true;
}

int32_t NormalizeRequest(const UnitMovementRule* rule, UnitMovementRule& normalized) noexcept
{
    if (!rule || rule->structSize < sizeof(UnitMovementRule) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isTargetValid(rule->civilization, rule->baseUnitType, rule->identity) ||
        !isZeroed(rule->reserved))
    {
        return UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    normalized            = *rule;
    normalized.structSize = sizeof(normalized);
    normalizeRuleId(normalized.providerId);
    normalizeRuleId(normalized.ruleId);

    return UNIT_MOVEMENT_RULES_OK;
}

std::unique_lock<std::shared_mutex> AcquirePublicationLock()
{
    return std::unique_lock(registryMutex);
}

int32_t BuildCandidate(std::span<const UnitMovementRule> requests,
                       std::vector<UnitMovementRule>&    candidate,
                       size_t&                           failingOffset,
                       void (*checkpoint)(),
                       std::span<const UnitMovementRule> removed,
                       bool                              exclusive)
{
    failingOffset = 0;
    if (checkpoint)
    {
        checkpoint();
    }
    candidate = registry;
    // Only the preceding Lua transaction's exact owned records may be removed.
    std::erase_if(candidate, [&](const auto& existing)
                  {
                      return std::any_of(removed.begin(), removed.end(), [&](const auto& owned)
                                         {
                                             return std::memcmp(&existing, &owned, sizeof(owned)) == 0;
                                         });
                  });
    const size_t retainedCount = candidate.size();
    for (size_t i = 0; i < requests.size(); ++i)
    {
        failingOffset          = i;
        const auto& normalized = requests[i];
        const auto  duplicate  = std::find_if(candidate.begin(), candidate.end(), [&](const auto& existing)
                                              {
                                                return ruleKeyMatches(existing, normalized);
                                              });
        if (duplicate != candidate.end())
        {
            if ((exclusive && static_cast<size_t>(duplicate - candidate.begin()) < retainedCount) || std::memcmp(&*duplicate, &normalized, sizeof(normalized)) != 0)
            {
                return UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID;
            }
            continue;
        }
        if (checkpoint)
        {
            checkpoint();
        }
        candidate.push_back(normalized);
    }
    if (checkpoint)
    {
        checkpoint();
    }
    std::sort(candidate.begin(), candidate.end(), ruleKeyLess);
    return UNIT_MOVEMENT_RULES_OK;
}

void CommitCandidate(std::vector<UnitMovementRule>& candidate) noexcept
{
    registry.swap(candidate);
}

void ResetForTests()
{
    std::unique_lock lock(registryMutex);
    registry.clear();
}

} // namespace rerevved::unit_movement_rules

static_assert(sizeof(UnitMovementRule) == 168);
static_assert(sizeof(UnitMovementRuleInfo) == 192);
static_assert(sizeof(UnitMovementQuery) == 40);
static_assert(sizeof(UnitMovementEvaluation) == 40);

extern "C" uint32_t UnitMovementRulesAbiVersion(void)
{
    return UNIT_MOVEMENT_RULES_ABI_VERSION;
}

extern "C" int32_t RegisterUnitMovementRule(
    const UnitMovementRule* rule)
{
    using namespace rerevved::unit_movement_rules;
    UnitMovementRule normalized{};
    const int32_t    validation = NormalizeRequest(rule, normalized);
    if (validation != UNIT_MOVEMENT_RULES_OK)
    {
        return validation;
    }

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
                       ? UNIT_MOVEMENT_RULES_OK
                       : UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return UNIT_MOVEMENT_RULES_ERR_INTERNAL;
    }
    return UNIT_MOVEMENT_RULES_OK;
}

extern "C" int32_t GetUnitMovementRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unit_movement_rules::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::unit_movement_rules::registry.size());
    return UNIT_MOVEMENT_RULES_OK;
}

extern "C" int32_t GetUnitMovementRule(
    uint32_t              index,
    UnitMovementRuleInfo* out,
    uint32_t              outSize)
{
    using namespace rerevved::unit_movement_rules;
    if (!out)
    {
        return UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto&          rule = registry[index];
    UnitMovementRuleInfo result{};
    result.structSize   = sizeof(result);
    result.civilization = rule.civilization;
    result.baseUnitType = rule.baseUnitType;
    result.identity     = rule.identity;
    result.value        = rule.value;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    return copyOutput(out, outSize, result);
}

extern "C" int32_t EvaluateUnitMovement(
    const UnitMovementQuery* query,
    UnitMovementEvaluation*  out,
    uint32_t                 outSize)
{
    using namespace rerevved::unit_movement_rules;
    if (!out)
    {
        return UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->structSize < sizeof(UnitMovementQuery) ||
        !isZeroed(query->reserved))
    {
        return UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    UnitMovementEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->baseUnitType,
                     query->identity,
                     query->nativeValue,
                     result))
    {
        return UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result);
}
