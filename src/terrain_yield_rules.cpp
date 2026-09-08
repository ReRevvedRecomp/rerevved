#include "terrain_yield_rules_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace rerevved::terrain_yield_rules
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 152;
constexpr uint32_t kEvaluationPrefix = 24;

std::shared_mutex             registryMutex;
std::vector<TerrainYieldRule> registry;

bool isTerrainValid(TerrainId terrain)
{
    return terrain >= TERRAIN_SEA &&
           terrain < TERRAIN_COUNT;
}

bool isComponentValid(TerrainYieldComponent component)
{
    return component == TERRAIN_YIELD_FOOD ||
           component == TERRAIN_YIELD_PRODUCTION ||
           component == TERRAIN_YIELD_TRADE;
}

bool isOperationValid(TerrainYieldOperation operation)
{
    return operation == TERRAIN_YIELD_REPLACE ||
           operation == TERRAIN_YIELD_ADD;
}

bool isRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', TERRAIN_YIELD_RULE_ID_CAPACITY);
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
                TERRAIN_YIELD_RULE_ID_CAPACITY - length - 1);
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

bool targetMatches(const TerrainYieldRule& rule,
                   TerrainId               terrain,
                   TerrainYieldComponent   component)
{
    return rule.terrain == terrain && rule.component == component;
}

bool ruleKeyMatches(const TerrainYieldRule& left,
                    const TerrainYieldRule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const TerrainYieldRule& left,
                 const TerrainYieldRule& right)
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
int32_t copyOutput(Record*       out,
                   uint32_t      outSize,
                   const Record& producer)
{
    uint32_t copySize = std::min<uint32_t>(outSize, sizeof(Record));
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, &producer, copySize);
    return TERRAIN_YIELD_RULES_OK;
}

uint32_t replacementCount(const TerrainYieldRule& target)
{
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return candidate.operation == TERRAIN_YIELD_REPLACE &&
                   targetMatches(candidate, target.terrain, target.component);
        }));
}

} // namespace

bool TryMapGuestTerrain(int32_t guestTerrain, TerrainId& terrain)
{
    TerrainId mapped = TERRAIN_UNKNOWN;
    switch (guestTerrain)
    {
        case 0:
            mapped = TERRAIN_SEA;
            break;
        case 2:
            mapped = TERRAIN_PLAINS;
            break;
        case 3:
            mapped = TERRAIN_FOREST;
            break;
        case 4:
            mapped = TERRAIN_HILL;
            break;
        case 5:
            mapped = TERRAIN_DESERT;
            break;
        case 6:
            mapped = TERRAIN_MOUNTAIN;
            break;
        default:
            return false;
    }
    terrain = mapped;
    return true;
}

bool TryAddChecked(int64_t accumulator, int64_t value, int64_t& result)
{
    if ((value > 0 && accumulator >
                          std::numeric_limits<int64_t>::max() - value) ||
        (value < 0 && accumulator <
                          std::numeric_limits<int64_t>::min() - value))
    {
        return false;
    }
    result = accumulator + value;
    return true;
}

bool TryEvaluate(TerrainId               terrain,
                 TerrainYieldComponent   component,
                 int32_t                 nativeValue,
                 TerrainYieldEvaluation& evaluation)
{
    if (!isTerrainValid(terrain) || !isComponentValid(component))
    {
        return false;
    }

    evaluation = {
        sizeof(TerrainYieldEvaluation),
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
        if (!targetMatches(rule, terrain, component))
        {
            continue;
        }
        if (rule.operation == TERRAIN_YIELD_REPLACE)
        {
            ++evaluation.replacementCount;
            replacement = rule.value;
        }
        else
        {
            ++evaluation.additiveCount;
            if (!additiveOverflow)
            {
                int64_t nextSum = 0;
                if (!TryAddChecked(additiveSum, rule.value, nextSum))
                {
                    additiveOverflow = true;
                }
                else
                {
                    additiveSum = nextSum;
                }
            }
        }
    }

    if (evaluation.replacementCount > 1)
    {
        evaluation.statusFlags |=
            TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT;
        replacement = nativeValue;
    }

    int64_t composed = 0;
    if (additiveOverflow ||
        !TryAddChecked(static_cast<int64_t>(replacement), additiveSum, composed) ||
        composed < std::numeric_limits<int32_t>::min() ||
        composed > std::numeric_limits<int32_t>::max())
    {
        evaluation.statusFlags |= TERRAIN_YIELD_EVALUATION_OVERFLOW;
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

} // namespace rerevved::terrain_yield_rules

static_assert(sizeof(TerrainYieldComponent) == sizeof(int32_t));
static_assert(sizeof(TerrainYieldOperation) == sizeof(int32_t));
static_assert(sizeof(TerrainYieldRule) == 168);
static_assert(sizeof(TerrainYieldRuleInfo) == 192);
static_assert(sizeof(TerrainYieldQuery) == 40);
static_assert(sizeof(TerrainYieldEvaluation) == 40);

extern "C" uint32_t TerrainYieldRulesAbiVersion(void)
{
    return TERRAIN_YIELD_RULES_ABI_VERSION;
}

extern "C" int32_t RegisterTerrainYieldRule(
    const TerrainYieldRule* rule)
{
    using namespace rerevved::terrain_yield_rules;
    if (!rule || rule->structSize < sizeof(TerrainYieldRule) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isTerrainValid(rule->terrain) || !isComponentValid(rule->component) ||
        !isOperationValid(rule->operation) || !isZeroed(rule->reserved))
    {
        return TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }

    TerrainYieldRule normalized = *rule;
    normalized.structSize       = sizeof(normalized);
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
                       ? TERRAIN_YIELD_RULES_OK
                       : TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return TERRAIN_YIELD_RULES_ERR_INTERNAL;
    }
    return TERRAIN_YIELD_RULES_OK;
}

extern "C" int32_t GetTerrainYieldRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::terrain_yield_rules::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::terrain_yield_rules::registry.size());
    return TERRAIN_YIELD_RULES_OK;
}

extern "C" int32_t GetTerrainYieldRule(
    uint32_t              index,
    TerrainYieldRuleInfo* out,
    uint32_t              outSize)
{
    using namespace rerevved::terrain_yield_rules;
    if (!out)
    {
        return TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto&          rule = registry[index];
    TerrainYieldRuleInfo result{};
    result.structSize = sizeof(result);
    result.terrain    = rule.terrain;
    result.component  = rule.component;
    result.operation  = rule.operation;
    result.value      = rule.value;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    if (rule.operation == TERRAIN_YIELD_REPLACE &&
        replacementCount(rule) > 1)
    {
        result.statusFlags |=
            TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT;
    }
    return copyOutput(out, outSize, result);
}

extern "C" int32_t EvaluateTerrainYield(
    const TerrainYieldQuery* query,
    TerrainYieldEvaluation*  out,
    uint32_t                 outSize)
{
    using namespace rerevved::terrain_yield_rules;
    if (!out)
    {
        return TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->structSize < sizeof(TerrainYieldQuery) ||
        !isZeroed(query->reserved))
    {
        return TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }

    TerrainYieldEvaluation result{};
    if (!TryEvaluate(
            query->terrain, query->component, query->nativeValue, result))
    {
        return TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result);
}
