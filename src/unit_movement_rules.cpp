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

std::shared_mutex registry_mutex;
std::vector<ReRevvedUnitMovementRule> registry;

bool IsRuleIdValid(const char* value)
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

void NormalizeRuleId(char* value)
{
    const size_t length = std::strlen(value);
    std::memset(value + length + 1,
                0,
                REREVVED_UNIT_MOVEMENT_RULE_ID_CAPACITY - length - 1);
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

bool IsCivilizationValid(ReRevvedCivilizationId civilization)
{
    return civilization >= 0 && civilization < REREVVED_CIVILIZATION_COUNT;
}

bool IsUnitTypeValid(ReRevvedUnitTypeId unit_type)
{
    return unit_type >= 0 && unit_type < REREVVED_UNIT_TYPE_COUNT;
}

bool IsTargetValid(ReRevvedCivilizationId civilization,
                   ReRevvedUnitTypeId base_unit_type,
                   ReRevvedUnitIdentityId identity)
{
    if (!IsCivilizationValid(civilization) || !IsUnitTypeValid(base_unit_type) ||
        identity == REREVVED_UNIT_IDENTITY_BASE ||
        identity < REREVVED_UNIT_IDENTITY_BASE ||
        identity >= REREVVED_UNIT_IDENTITY_COUNT)
    {
        return false;
    }

    ReRevvedUnitIdentityId resolved = REREVVED_UNIT_IDENTITY_BASE;
    return unit_catalog::TryResolveUnitIdentity(
               civilization, base_unit_type, resolved) &&
           resolved == identity;
}

bool TargetMatches(const ReRevvedUnitMovementRule& rule,
                   ReRevvedCivilizationId civilization,
                   ReRevvedUnitTypeId base_unit_type,
                   ReRevvedUnitIdentityId identity)
{
    return rule.civilization == civilization &&
           rule.base_unit_type == base_unit_type && rule.identity == identity;
}

bool RuleKeyMatches(const ReRevvedUnitMovementRule& left,
                    const ReRevvedUnitMovementRule& right)
{
    return std::strcmp(left.provider_id, right.provider_id) == 0 &&
           std::strcmp(left.rule_id, right.rule_id) == 0;
}

bool RuleKeyLess(const ReRevvedUnitMovementRule& left,
                 const ReRevvedUnitMovementRule& right)
{
    const int provider_order = std::strcmp(left.provider_id, right.provider_id);
    return provider_order < 0 ||
           (provider_order == 0 &&
            std::strcmp(left.rule_id, right.rule_id) < 0);
}

template <typename Record>
void ClearOutput(Record* out, uint32_t out_size)
{
    if (out)
    {
        std::memset(out, 0, std::min<uint32_t>(out_size, sizeof(Record)));
    }
}

template <typename Record>
int32_t CopyOutput(Record* out, uint32_t out_size, const Record& producer)
{
    uint32_t copy_size = std::min<uint32_t>(out_size, sizeof(Record));
    copy_size -= copy_size % sizeof(uint32_t);
    std::memcpy(out, &producer, copy_size);
    return REREVVED_UNIT_MOVEMENT_RULES_OK;
}

} // namespace

bool TryEvaluate(ReRevvedCivilizationId civilization,
                 ReRevvedUnitTypeId base_unit_type,
                 ReRevvedUnitIdentityId identity,
                 int32_t native_value,
                 ReRevvedUnitMovementEvaluation& evaluation)
{
    if (!IsTargetValid(civilization, base_unit_type, identity))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedUnitMovementEvaluation),
        native_value,
        native_value,
        0,
        0,
        {},
    };

    std::shared_lock lock(registry_mutex);
    int64_t additive_sum   = 0;
    bool additive_overflow = false;
    for (const auto& rule : registry)
    {
        if (!TargetMatches(rule, civilization, base_unit_type, identity))
        {
            continue;
        }

        ++evaluation.additive_count;
        if ((rule.value > 0 &&
             additive_sum > std::numeric_limits<int64_t>::max() - rule.value) ||
            (rule.value < 0 &&
             additive_sum < std::numeric_limits<int64_t>::min() - rule.value))
        {
            additive_overflow = true;
        }
        else
        {
            additive_sum += rule.value;
        }
    }

    const int64_t composed = static_cast<int64_t>(native_value) + additive_sum;
    if (additive_overflow ||
        composed < std::numeric_limits<int32_t>::min() ||
        composed > std::numeric_limits<int32_t>::max())
    {
        evaluation.status_flags |= REREVVED_UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW;
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
    if (!rule || rule->struct_size < sizeof(ReRevvedUnitMovementRule) ||
        !IsRuleIdValid(rule->provider_id) || !IsRuleIdValid(rule->rule_id) ||
        !IsTargetValid(rule->civilization, rule->base_unit_type, rule->identity) ||
        !IsZeroed(rule->reserved))
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitMovementRule normalized = *rule;
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
                       ? REREVVED_UNIT_MOVEMENT_RULES_OK
                       : REREVVED_UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), RuleKeyLess);
    }
    catch (...)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INTERNAL;
    }
    return REREVVED_UNIT_MOVEMENT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUnitMovementRuleCount(uint32_t* out_count)
{
    if (!out_count)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unit_movement_rules::registry_mutex);
    *out_count = static_cast<uint32_t>(
        rerevved::unit_movement_rules::registry.size());
    return REREVVED_UNIT_MOVEMENT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUnitMovementRule(
    uint32_t index,
    ReRevvedUnitMovementRuleInfo* out,
    uint32_t out_size)
{
    using namespace rerevved::unit_movement_rules;
    if (!out)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kRuleInfoPrefix)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registry_mutex);
    if (index >= registry.size())
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto& rule = registry[index];
    ReRevvedUnitMovementRuleInfo result{};
    result.struct_size    = sizeof(result);
    result.civilization   = rule.civilization;
    result.base_unit_type = rule.base_unit_type;
    result.identity       = rule.identity;
    result.value          = rule.value;
    std::memcpy(result.provider_id, rule.provider_id, sizeof(result.provider_id));
    std::memcpy(result.rule_id, rule.rule_id, sizeof(result.rule_id));
    return CopyOutput(out, out_size, result);
}

extern "C" int32_t ReRevvedEvaluateUnitMovement(
    const ReRevvedUnitMovementQuery* query,
    ReRevvedUnitMovementEvaluation* out,
    uint32_t out_size)
{
    using namespace rerevved::unit_movement_rules;
    if (!out)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kEvaluationPrefix)
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->struct_size < sizeof(ReRevvedUnitMovementQuery) ||
        !IsZeroed(query->reserved))
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitMovementEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->base_unit_type,
                     query->identity,
                     query->native_value,
                     result))
    {
        return REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT;
    }
    return CopyOutput(out, out_size, result);
}
