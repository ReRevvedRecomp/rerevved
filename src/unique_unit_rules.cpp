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

std::shared_mutex                         registry_mutex;
std::vector<ReRevvedUniqueUnitScalarRule> registry;

bool IsPropertyValid(ReRevvedUniqueUnitScalarProperty property)
{
    return property == REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK ||
           property == REREVVED_UNIQUE_UNIT_SCALAR_BASE_DEFENSE;
}

bool IsOperationValid(ReRevvedUniqueUnitScalarOperation operation)
{
    return operation == REREVVED_UNIQUE_UNIT_SCALAR_REPLACE ||
           operation == REREVVED_UNIQUE_UNIT_SCALAR_ADD;
}

bool IsRuleIdValid(const char* value)
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

void NormalizeRuleId(char* value)
{
    const size_t length = std::strlen(value);
    std::memset(value + length + 1,
                0,
                REREVVED_UNIQUE_UNIT_RULE_ID_CAPACITY - length - 1);
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

bool TargetMatches(const ReRevvedUniqueUnitScalarRule& rule,
                   ReRevvedCivilizationId              civilization,
                   ReRevvedUnitTypeId                  base_unit_type,
                   ReRevvedUnitIdentityId              identity,
                   ReRevvedUniqueUnitScalarProperty    property)
{
    return rule.civilization == civilization &&
           rule.base_unit_type == base_unit_type && rule.identity == identity &&
           rule.property == property;
}

bool RuleKeyMatches(const ReRevvedUniqueUnitScalarRule& left,
                    const ReRevvedUniqueUnitScalarRule& right)
{
    return std::strcmp(left.provider_id, right.provider_id) == 0 &&
           std::strcmp(left.rule_id, right.rule_id) == 0;
}

bool RuleKeyLess(const ReRevvedUniqueUnitScalarRule& left,
                 const ReRevvedUniqueUnitScalarRule& right)
{
    const int provider_order = std::strcmp(left.provider_id, right.provider_id);
    return provider_order < 0 ||
           (provider_order == 0 &&
            std::strcmp(left.rule_id, right.rule_id) < 0);
}

bool IsTargetValid(ReRevvedCivilizationId civilization,
                   ReRevvedUnitTypeId     base_unit_type,
                   ReRevvedUnitIdentityId identity)
{
    ReRevvedUnitIdentityId resolved = REREVVED_UNIT_IDENTITY_BASE;
    return identity != REREVVED_UNIT_IDENTITY_BASE &&
           unit_catalog::TryResolveUnitIdentity(
               civilization, base_unit_type, resolved) &&
           resolved == identity;
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
int32_t CopyOutput(Record*       out,
                   uint32_t      out_size,
                   const Record& producer,
                   uint32_t      minimum_prefix)
{
    if (!out)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < minimum_prefix)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t copy_size = std::min<uint32_t>(out_size, sizeof(Record));
    copy_size -= copy_size % sizeof(uint32_t);
    std::memcpy(out, &producer, copy_size);
    return REREVVED_UNIQUE_UNIT_RULES_OK;
}

uint32_t ReplacementCount(const ReRevvedUniqueUnitScalarRule& target)
{
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return candidate.operation == REREVVED_UNIQUE_UNIT_SCALAR_REPLACE &&
                   TargetMatches(candidate,
                                 target.civilization,
                                 target.base_unit_type,
                                 target.identity,
                                 target.property);
        }));
}

} // namespace

bool TryEvaluate(ReRevvedCivilizationId              civilization,
                 ReRevvedUnitTypeId                  base_unit_type,
                 ReRevvedUnitIdentityId              identity,
                 ReRevvedUniqueUnitScalarProperty    property,
                 int32_t                             native_value,
                 ReRevvedUniqueUnitScalarEvaluation& evaluation)
{
    if (!IsTargetValid(civilization, base_unit_type, identity) ||
        !IsPropertyValid(property))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedUniqueUnitScalarEvaluation),
        native_value,
        native_value,
        0,
        0,
        0,
        {},
    };

    std::shared_lock lock(registry_mutex);
    int64_t          additive_sum      = 0;
    int32_t          replacement       = native_value;
    bool             additive_overflow = false;
    for (const auto& rule : registry)
    {
        if (!TargetMatches(
                rule, civilization, base_unit_type, identity, property))
        {
            continue;
        }
        if (rule.operation == REREVVED_UNIQUE_UNIT_SCALAR_REPLACE)
        {
            ++evaluation.replacement_count;
            replacement = rule.value;
        }
        else
        {
            ++evaluation.additive_count;
            if ((rule.value > 0 &&
                 additive_sum >
                     std::numeric_limits<int64_t>::max() - rule.value) ||
                (rule.value < 0 &&
                 additive_sum <
                     std::numeric_limits<int64_t>::min() - rule.value))
            {
                additive_overflow = true;
            }
            else
            {
                additive_sum += rule.value;
            }
        }
    }

    if (evaluation.replacement_count > 1)
    {
        evaluation.status_flags |=
            REREVVED_UNIQUE_UNIT_EVALUATION_REPLACEMENT_CONFLICT;
        replacement = native_value;
    }

    const int64_t composed = static_cast<int64_t>(replacement) + additive_sum;
    if (additive_overflow ||
        composed < std::numeric_limits<int32_t>::min() ||
        composed > std::numeric_limits<int32_t>::max())
    {
        evaluation.status_flags |= REREVVED_UNIQUE_UNIT_EVALUATION_OVERFLOW;
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
    if (!rule || rule->struct_size < sizeof(ReRevvedUniqueUnitScalarRule) ||
        !IsRuleIdValid(rule->provider_id) || !IsRuleIdValid(rule->rule_id) ||
        !IsTargetValid(
            rule->civilization, rule->base_unit_type, rule->identity) ||
        !IsPropertyValid(rule->property) ||
        !IsOperationValid(rule->operation) || !IsZeroed(rule->reserved))
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUniqueUnitScalarRule normalized = *rule;
    normalized.struct_size                  = sizeof(normalized);
    NormalizeRuleId(normalized.provider_id);
    NormalizeRuleId(normalized.rule_id);

    try
    {
        std::unique_lock lock(registry_mutex);
        const auto       duplicate = std::find_if(
            registry.begin(), registry.end(), [&](const auto& candidate)
            {
                return RuleKeyMatches(candidate, normalized);
            });
        if (duplicate != registry.end())
        {
            return std::memcmp(&*duplicate, &normalized, sizeof(normalized)) == 0
                       ? REREVVED_UNIQUE_UNIT_RULES_OK
                       : REREVVED_UNIQUE_UNIT_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), RuleKeyLess);
    }
    catch (...)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INTERNAL;
    }
    return REREVVED_UNIQUE_UNIT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUniqueUnitScalarRuleCount(uint32_t* out_count)
{
    if (!out_count)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unique_unit_rules::registry_mutex);
    *out_count = static_cast<uint32_t>(
        rerevved::unique_unit_rules::registry.size());
    return REREVVED_UNIQUE_UNIT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUniqueUnitScalarRule(
    uint32_t                          index,
    ReRevvedUniqueUnitScalarRuleInfo* out,
    uint32_t                          out_size)
{
    using namespace rerevved::unique_unit_rules;
    if (!out)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kRuleInfoPrefix)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registry_mutex);
    if (index >= registry.size())
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto&                      rule = registry[index];
    ReRevvedUniqueUnitScalarRuleInfo result{};
    result.struct_size    = sizeof(result);
    result.civilization   = rule.civilization;
    result.base_unit_type = rule.base_unit_type;
    result.identity       = rule.identity;
    result.property       = rule.property;
    result.operation      = rule.operation;
    result.value          = rule.value;
    std::memcpy(result.provider_id, rule.provider_id, sizeof(result.provider_id));
    std::memcpy(result.rule_id, rule.rule_id, sizeof(result.rule_id));
    if (rule.operation == REREVVED_UNIQUE_UNIT_SCALAR_REPLACE &&
        ReplacementCount(rule) > 1)
    {
        result.status_flags |=
            REREVVED_UNIQUE_UNIT_RULE_REPLACEMENT_CONFLICT;
    }
    return CopyOutput(out, out_size, result, kRuleInfoPrefix);
}

extern "C" int32_t ReRevvedEvaluateUniqueUnitScalar(
    const ReRevvedUniqueUnitScalarQuery* query,
    ReRevvedUniqueUnitScalarEvaluation*  out,
    uint32_t                             out_size)
{
    using namespace rerevved::unique_unit_rules;
    if (!out)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kEvaluationPrefix)
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->struct_size < sizeof(ReRevvedUniqueUnitScalarQuery) ||
        !IsZeroed(query->reserved))
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUniqueUnitScalarEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->base_unit_type,
                     query->identity,
                     query->property,
                     query->native_value,
                     result))
    {
        return REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT;
    }
    return CopyOutput(out, out_size, result, kEvaluationPrefix);
}
