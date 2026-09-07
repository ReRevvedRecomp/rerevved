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

std::shared_mutex registry_mutex;
std::vector<ReRevvedTerrainYieldRule> registry;

bool IsTerrainValid(ReRevvedTerrainId terrain)
{
    return terrain >= REREVVED_TERRAIN_SEA &&
           terrain < REREVVED_TERRAIN_COUNT;
}

bool IsComponentValid(ReRevvedTerrainYieldComponent component)
{
    return component == REREVVED_TERRAIN_YIELD_FOOD ||
           component == REREVVED_TERRAIN_YIELD_PRODUCTION ||
           component == REREVVED_TERRAIN_YIELD_TRADE;
}

bool IsOperationValid(ReRevvedTerrainYieldOperation operation)
{
    return operation == REREVVED_TERRAIN_YIELD_REPLACE ||
           operation == REREVVED_TERRAIN_YIELD_ADD;
}

bool IsRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', REREVVED_TERRAIN_YIELD_RULE_ID_CAPACITY);
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

void NormalizeRuleId(char* value)
{
    const size_t length = std::strlen(value);
    std::memset(value + length + 1,
                0,
                REREVVED_TERRAIN_YIELD_RULE_ID_CAPACITY - length - 1);
}

template <size_t Size>
bool IsZeroed(const int32_t (&values)[Size])
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

bool TargetMatches(const ReRevvedTerrainYieldRule& rule,
                   ReRevvedTerrainId terrain,
                   ReRevvedTerrainYieldComponent component)
{
    return rule.terrain == terrain && rule.component == component;
}

bool RuleKeyMatches(const ReRevvedTerrainYieldRule& left,
                    const ReRevvedTerrainYieldRule& right)
{
    return std::strcmp(left.provider_id, right.provider_id) == 0 &&
           std::strcmp(left.rule_id, right.rule_id) == 0;
}

bool RuleKeyLess(const ReRevvedTerrainYieldRule& left,
                 const ReRevvedTerrainYieldRule& right)
{
    const int provider_order = std::strcmp(left.provider_id, right.provider_id);
    return provider_order < 0 ||
           (provider_order == 0 &&
            std::strcmp(left.rule_id, right.rule_id) < 0);
}

template <typename Record>
void ClearOutput(Record* out, uint32_t out_size)
{
    std::memset(out, 0, std::min<uint32_t>(out_size, sizeof(Record)));
}

template <typename Record>
int32_t CopyOutput(Record* out,
                   uint32_t out_size,
                   const Record& producer)
{
    uint32_t copy_size = std::min<uint32_t>(out_size, sizeof(Record));
    copy_size -= copy_size % sizeof(uint32_t);
    std::memcpy(out, &producer, copy_size);
    return REREVVED_TERRAIN_YIELD_RULES_OK;
}

uint32_t ReplacementCount(const ReRevvedTerrainYieldRule& target)
{
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return candidate.operation == REREVVED_TERRAIN_YIELD_REPLACE &&
                   TargetMatches(candidate, target.terrain, target.component);
        }));
}

} // namespace

bool TryMapGuestTerrain(int32_t guest_terrain, ReRevvedTerrainId& terrain)
{
    ReRevvedTerrainId mapped = REREVVED_TERRAIN_UNKNOWN;
    switch (guest_terrain)
    {
        case 0:
            mapped = REREVVED_TERRAIN_SEA;
            break;
        case 2:
            mapped = REREVVED_TERRAIN_PLAINS;
            break;
        case 3:
            mapped = REREVVED_TERRAIN_FOREST;
            break;
        case 4:
            mapped = REREVVED_TERRAIN_HILL;
            break;
        case 5:
            mapped = REREVVED_TERRAIN_DESERT;
            break;
        case 6:
            mapped = REREVVED_TERRAIN_MOUNTAIN;
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

bool TryEvaluate(ReRevvedTerrainId terrain,
                 ReRevvedTerrainYieldComponent component,
                 int32_t native_value,
                 ReRevvedTerrainYieldEvaluation& evaluation)
{
    if (!IsTerrainValid(terrain) || !IsComponentValid(component))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedTerrainYieldEvaluation),
        native_value,
        native_value,
        0,
        0,
        0,
        {},
    };

    std::shared_lock lock(registry_mutex);
    int64_t additive_sum   = 0;
    int32_t replacement    = native_value;
    bool additive_overflow = false;
    for (const auto& rule : registry)
    {
        if (!TargetMatches(rule, terrain, component))
        {
            continue;
        }
        if (rule.operation == REREVVED_TERRAIN_YIELD_REPLACE)
        {
            ++evaluation.replacement_count;
            replacement = rule.value;
        }
        else
        {
            ++evaluation.additive_count;
            if (!additive_overflow)
            {
                int64_t next_sum = 0;
                if (!TryAddChecked(additive_sum, rule.value, next_sum))
                {
                    additive_overflow = true;
                }
                else
                {
                    additive_sum = next_sum;
                }
            }
        }
    }

    if (evaluation.replacement_count > 1)
    {
        evaluation.status_flags |=
            REREVVED_TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT;
        replacement = native_value;
    }

    int64_t composed = 0;
    if (additive_overflow ||
        !TryAddChecked(static_cast<int64_t>(replacement), additive_sum, composed) ||
        composed < std::numeric_limits<int32_t>::min() ||
        composed > std::numeric_limits<int32_t>::max())
    {
        evaluation.status_flags |= REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW;
        return true;
    }

    evaluation.final_value = static_cast<int32_t>(composed);
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registry_mutex);
    registry.clear();
}

} // namespace rerevved::terrain_yield_rules

static_assert(sizeof(ReRevvedTerrainYieldComponent) == sizeof(int32_t));
static_assert(sizeof(ReRevvedTerrainYieldOperation) == sizeof(int32_t));
static_assert(sizeof(ReRevvedTerrainYieldRule) == 168);
static_assert(sizeof(ReRevvedTerrainYieldRuleInfo) == 192);
static_assert(sizeof(ReRevvedTerrainYieldQuery) == 40);
static_assert(sizeof(ReRevvedTerrainYieldEvaluation) == 40);

extern "C" uint32_t ReRevvedTerrainYieldRulesAbiVersion(void)
{
    return REREVVED_TERRAIN_YIELD_RULES_ABI_VERSION;
}

extern "C" int32_t ReRevvedRegisterTerrainYieldRule(
    const ReRevvedTerrainYieldRule* rule)
{
    using namespace rerevved::terrain_yield_rules;
    if (!rule || rule->struct_size < sizeof(ReRevvedTerrainYieldRule) ||
        !IsRuleIdValid(rule->provider_id) || !IsRuleIdValid(rule->rule_id) ||
        !IsTerrainValid(rule->terrain) || !IsComponentValid(rule->component) ||
        !IsOperationValid(rule->operation) || !IsZeroed(rule->reserved))
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedTerrainYieldRule normalized = *rule;
    normalized.struct_size              = sizeof(normalized);
    NormalizeRuleId(normalized.provider_id);
    NormalizeRuleId(normalized.rule_id);

    try
    {
        std::unique_lock lock(registry_mutex);
        const auto duplicate = std::find_if(
            registry.begin(), registry.end(), [&](const auto& candidate)
            {
                return RuleKeyMatches(candidate, normalized);
            });
        if (duplicate != registry.end())
        {
            return std::memcmp(&*duplicate, &normalized, sizeof(normalized)) == 0
                       ? REREVVED_TERRAIN_YIELD_RULES_OK
                       : REREVVED_TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), RuleKeyLess);
    }
    catch (...)
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_INTERNAL;
    }
    return REREVVED_TERRAIN_YIELD_RULES_OK;
}

extern "C" int32_t ReRevvedGetTerrainYieldRuleCount(uint32_t* out_count)
{
    if (!out_count)
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::terrain_yield_rules::registry_mutex);
    *out_count = static_cast<uint32_t>(
        rerevved::terrain_yield_rules::registry.size());
    return REREVVED_TERRAIN_YIELD_RULES_OK;
}

extern "C" int32_t ReRevvedGetTerrainYieldRule(
    uint32_t index,
    ReRevvedTerrainYieldRuleInfo* out,
    uint32_t out_size)
{
    using namespace rerevved::terrain_yield_rules;
    if (!out)
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kRuleInfoPrefix)
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registry_mutex);
    if (index >= registry.size())
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto& rule = registry[index];
    ReRevvedTerrainYieldRuleInfo result{};
    result.struct_size = sizeof(result);
    result.terrain     = rule.terrain;
    result.component   = rule.component;
    result.operation   = rule.operation;
    result.value       = rule.value;
    std::memcpy(result.provider_id, rule.provider_id, sizeof(result.provider_id));
    std::memcpy(result.rule_id, rule.rule_id, sizeof(result.rule_id));
    if (rule.operation == REREVVED_TERRAIN_YIELD_REPLACE &&
        ReplacementCount(rule) > 1)
    {
        result.status_flags |=
            REREVVED_TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT;
    }
    return CopyOutput(out, out_size, result);
}

extern "C" int32_t ReRevvedEvaluateTerrainYield(
    const ReRevvedTerrainYieldQuery* query,
    ReRevvedTerrainYieldEvaluation* out,
    uint32_t out_size)
{
    using namespace rerevved::terrain_yield_rules;
    if (!out)
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kEvaluationPrefix)
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->struct_size < sizeof(ReRevvedTerrainYieldQuery) ||
        !IsZeroed(query->reserved))
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedTerrainYieldEvaluation result{};
    if (!TryEvaluate(
            query->terrain, query->component, query->native_value, result))
    {
        return REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT;
    }
    return CopyOutput(out, out_size, result);
}
