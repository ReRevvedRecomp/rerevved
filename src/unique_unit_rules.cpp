#include "unique_unit_rules_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "unit_catalog_api.h"

namespace rerevved::unique_unit_rules
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 160;
constexpr uint32_t kEvaluationPrefix = 24;

std::shared_mutex                         registryMutex;
std::vector<ReRevvedUniqueUnitScalarRule> registry;

bool isPropertyValid(ReRevvedUniqueUnitScalarProperty property)
{
    return property == REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK ||
           property == REREVVED_UNIQUE_UNIT_SCALAR_BASE_DEFENSE;
}

bool isOperationValid(ReRevvedUniqueUnitScalarOperation operation)
{
    return operation == REREVVED_UNIQUE_UNIT_SCALAR_REPLACE ||
           operation == REREVVED_UNIQUE_UNIT_SCALAR_ADD;
}

bool isRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', REREVVED_UNIQUE_UNIT_RULE_ID_CAPACITY);
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
                REREVVED_UNIQUE_UNIT_RULE_ID_CAPACITY - length - 1);
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

bool targetMatches(const ReRevvedUniqueUnitScalarRule& rule,
                   ReRevvedCivilizationId              civilization,
                   ReRevvedUnitTypeId                  baseUnitType,
                   ReRevvedUnitIdentityId              identity,
                   ReRevvedUniqueUnitScalarProperty    property)
{
    return rule.civilization == civilization &&
           rule.baseUnitType == baseUnitType && rule.identity == identity &&
           rule.property == property;
}

bool ruleKeyMatches(const ReRevvedUniqueUnitScalarRule& left,
                    const ReRevvedUniqueUnitScalarRule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const ReRevvedUniqueUnitScalarRule& left,
                 const ReRevvedUniqueUnitScalarRule& right)
{
    const int providerOrder = std::strcmp(left.providerId, right.providerId);
    return providerOrder < 0 ||
           (providerOrder == 0 &&
            std::strcmp(left.ruleId, right.ruleId) < 0);
}

bool isTargetValid(ReRevvedCivilizationId civilization,
                   ReRevvedUnitTypeId     baseUnitType,
                   ReRevvedUnitIdentityId identity)
{
    ReRevvedUnitIdentityId resolved = REREVVED_UNIT_IDENTITY_BASE;
    return identity != REREVVED_UNIT_IDENTITY_BASE &&
           unit_catalog::TryResolveUnitIdentity(
               civilization, baseUnitType, resolved) &&
           resolved == identity;
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
int32_t copyOutput(Record*       out,
                   uint32_t      outSize,
                   const Record& producer,
                   uint32_t      minimumPrefix)
{
    if (!out)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < minimumPrefix)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t copySize = std::min<uint32_t>(outSize, sizeof(Record));
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, &producer, copySize);
    return REREVVED_UNIQUE_UNIT_RULES_OK;
}

uint32_t replacementCount(const ReRevvedUniqueUnitScalarRule& target)
{
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return candidate.operation == REREVVED_UNIQUE_UNIT_SCALAR_REPLACE &&
                   targetMatches(candidate,
                                 target.civilization,
                                 target.baseUnitType,
                                 target.identity,
                                 target.property);
        }));
}

} // namespace

bool TryEvaluate(ReRevvedCivilizationId              civilization,
                 ReRevvedUnitTypeId                  baseUnitType,
                 ReRevvedUnitIdentityId              identity,
                 ReRevvedUniqueUnitScalarProperty    property,
                 int32_t                             nativeValue,
                 ReRevvedUniqueUnitScalarEvaluation& evaluation)
{
    if (!isTargetValid(civilization, baseUnitType, identity) ||
        !isPropertyValid(property))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedUniqueUnitScalarEvaluation),
        nativeValue,
        nativeValue,
        0,
        0,
        0,
        {},
    };

    std::shared_lock lock(registryMutex);
    int64_t          additiveSum      = 0;
    int32_t          replacement      = nativeValue;
    bool             additiveOverflow = false;
    for (const auto& rule : registry)
    {
        if (!targetMatches(
                rule, civilization, baseUnitType, identity, property))
        {
            continue;
        }
        if (rule.operation == REREVVED_UNIQUE_UNIT_SCALAR_REPLACE)
        {
            ++evaluation.replacementCount;
            replacement = rule.value;
        }
        else
        {
            ++evaluation.additiveCount;
            if ((rule.value > 0 &&
                 additiveSum >
                     std::numeric_limits<int64_t>::max() - rule.value) ||
                (rule.value < 0 &&
                 additiveSum <
                     std::numeric_limits<int64_t>::min() - rule.value))
            {
                additiveOverflow = true;
            }
            else
            {
                additiveSum += rule.value;
            }
        }
    }

    if (evaluation.replacementCount > 1)
    {
        evaluation.statusFlags |=
            REREVVED_UNIQUE_UNIT_EVALUATION_REPLACEMENT_CONFLICT;
        replacement = nativeValue;
    }

    const int64_t composed = static_cast<int64_t>(replacement) + additiveSum;
    if (additiveOverflow ||
        composed < std::numeric_limits<int32_t>::min() ||
        composed > std::numeric_limits<int32_t>::max())
    {
        evaluation.statusFlags |= REREVVED_UNIQUE_UNIT_EVALUATION_OVERFLOW;
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

} // namespace rerevved::unique_unit_rules

static_assert(sizeof(ReRevvedUniqueUnitScalarProperty) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUniqueUnitScalarOperation) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUniqueUnitScalarRule) == 176);
static_assert(sizeof(ReRevvedUniqueUnitScalarRuleInfo) == 192);
static_assert(sizeof(ReRevvedUniqueUnitScalarQuery) == 40);
static_assert(sizeof(ReRevvedUniqueUnitScalarEvaluation) == 40);

extern "C" uint32_t ReRevvedUniqueUnitRulesAbiVersion(void)
{
    return REREVVED_UNIQUE_UNIT_RULES_ABI_VERSION;
}

extern "C" int32_t ReRevvedRegisterUniqueUnitScalarRule(
    const ReRevvedUniqueUnitScalarRule* rule)
{
    using namespace rerevved::unique_unit_rules;
    if (!rule || rule->structSize < sizeof(ReRevvedUniqueUnitScalarRule) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isTargetValid(
            rule->civilization, rule->baseUnitType, rule->identity) ||
        !isPropertyValid(rule->property) ||
        !isOperationValid(rule->operation) || !isZeroed(rule->reserved))
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUniqueUnitScalarRule normalized = *rule;
    normalized.structSize                   = sizeof(normalized);
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
                       ? REREVVED_UNIQUE_UNIT_RULES_OK
                       : REREVVED_UNIQUE_UNIT_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INTERNAL;
    }
    return REREVVED_UNIQUE_UNIT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUniqueUnitScalarRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unique_unit_rules::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::unique_unit_rules::registry.size());
    return REREVVED_UNIQUE_UNIT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUniqueUnitScalarRule(
    uint32_t                          index,
    ReRevvedUniqueUnitScalarRuleInfo* out,
    uint32_t                          outSize)
{
    using namespace rerevved::unique_unit_rules;
    if (!out)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto&                      rule = registry[index];
    ReRevvedUniqueUnitScalarRuleInfo result{};
    result.structSize   = sizeof(result);
    result.civilization = rule.civilization;
    result.baseUnitType = rule.baseUnitType;
    result.identity     = rule.identity;
    result.property     = rule.property;
    result.operation    = rule.operation;
    result.value        = rule.value;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    if (rule.operation == REREVVED_UNIQUE_UNIT_SCALAR_REPLACE &&
        replacementCount(rule) > 1)
    {
        result.statusFlags |=
            REREVVED_UNIQUE_UNIT_RULE_REPLACEMENT_CONFLICT;
    }
    return copyOutput(out, outSize, result, kRuleInfoPrefix);
}

extern "C" int32_t ReRevvedEvaluateUniqueUnitScalar(
    const ReRevvedUniqueUnitScalarQuery* query,
    ReRevvedUniqueUnitScalarEvaluation*  out,
    uint32_t                             outSize)
{
    using namespace rerevved::unique_unit_rules;
    if (!out)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->structSize < sizeof(ReRevvedUniqueUnitScalarQuery) ||
        !isZeroed(query->reserved))
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUniqueUnitScalarEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->baseUnitType,
                     query->identity,
                     query->property,
                     query->nativeValue,
                     result))
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result, kEvaluationPrefix);
}
