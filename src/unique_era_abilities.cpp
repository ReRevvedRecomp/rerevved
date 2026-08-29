#include "unique_era_abilities_registry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace rerevved::unique_era_abilities
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 148;
constexpr uint32_t kEvaluationPrefix = 20;

constexpr std::array<ReRevvedUniqueEraAbilityId, 45> kRetailAbilities = {
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    12,
    13,
    14,
    16,
    17,
    18,
    19,
    20,
    23,
    24,
    25,
    26,
    27,
    28,
    30,
    32,
    34,
    35,
    36,
    38,
    40,
    41,
    42,
    43,
    46,
    47,
    48,
    50,
    51,
    55,
    56,
    58,
    59,
    60,
    61,
};

std::shared_mutex                                registry_mutex;
std::vector<ReRevvedUniqueEraAbilityReplacement> registry;

bool IsRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', REREVVED_UNIQUE_ERA_ABILITY_RULE_ID_CAPACITY);
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
                REREVVED_UNIQUE_ERA_ABILITY_RULE_ID_CAPACITY - length - 1);
}

template <size_t Size>
bool IsZeroed(const int32_t (&values)[Size])
{
    return std::all_of(
        std::begin(values), std::end(values), [](int32_t value)
        {
            return value == 0;
        });
}

bool IsCellValid(ReRevvedCivilizationId     civilization,
                 ReRevvedUniqueEraUnlockEra unlock_era)
{
    return civilization >= 0 && civilization < REREVVED_CIVILIZATION_COUNT &&
           unlock_era >= REREVVED_UNIQUE_ERA_ANCIENT &&
           unlock_era <= REREVVED_UNIQUE_ERA_MODERN;
}

bool IsRetailAbilityValid(ReRevvedUniqueEraAbilityId ability)
{
    return std::binary_search(
        kRetailAbilities.begin(), kRetailAbilities.end(), ability);
}

bool IsReplacementAbilityValid(ReRevvedUniqueEraAbilityId ability)
{
    return IsRetailAbilityValid(ability) ||
           ability ==
               REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
}

bool TargetMatches(const ReRevvedUniqueEraAbilityReplacement& rule,
                   ReRevvedCivilizationId                     civilization,
                   ReRevvedUniqueEraUnlockEra                 unlock_era)
{
    return rule.civilization == civilization && rule.unlock_era == unlock_era;
}

bool RuleKeyMatches(const ReRevvedUniqueEraAbilityReplacement& left,
                    const ReRevvedUniqueEraAbilityReplacement& right)
{
    return std::strcmp(left.provider_id, right.provider_id) == 0 &&
           std::strcmp(left.rule_id, right.rule_id) == 0;
}

bool RuleKeyLess(const ReRevvedUniqueEraAbilityReplacement& left,
                 const ReRevvedUniqueEraAbilityReplacement& right)
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
int32_t CopyOutput(Record*       out,
                   uint32_t      out_size,
                   const Record& producer,
                   uint32_t      minimum_prefix)
{
    if (!out)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < minimum_prefix)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t copy_size = std::min<uint32_t>(out_size, sizeof(Record));
    copy_size -= copy_size % sizeof(uint32_t);
    std::memcpy(out, &producer, copy_size);
    return REREVVED_UNIQUE_ERA_ABILITIES_OK;
}

uint32_t ReplacementCount(const ReRevvedUniqueEraAbilityReplacement& target)
{
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return TargetMatches(
                candidate, target.civilization, target.unlock_era);
        }));
}

} // namespace

bool TryEvaluate(ReRevvedCivilizationId                  civilization,
                 ReRevvedUniqueEraUnlockEra              unlock_era,
                 ReRevvedUniqueEraAbilityId              native_ability,
                 ReRevvedUniqueEraAbilityCellEvaluation& evaluation)
{
    if (!IsCellValid(civilization, unlock_era) ||
        !IsRetailAbilityValid(native_ability))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedUniqueEraAbilityCellEvaluation),
        native_ability,
        native_ability,
        0,
        0,
        {},
    };

    std::shared_lock lock(registry_mutex);
    for (const auto& rule : registry)
    {
        if (!TargetMatches(rule, civilization, unlock_era))
        {
            continue;
        }
        ++evaluation.replacement_count;
        evaluation.effective_ability = rule.replacement_ability;
    }

    if (evaluation.replacement_count == 1)
    {
        evaluation.status_flags |=
            REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACED;
    }
    else if (evaluation.replacement_count > 1)
    {
        evaluation.status_flags |=
            REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT;
        evaluation.effective_ability = native_ability;
    }
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registry_mutex);
    registry.clear();
}

} // namespace rerevved::unique_era_abilities

static_assert(sizeof(ReRevvedUniqueEraUnlockEra) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUniqueEraAbilityId) == sizeof(int32_t));
static_assert(sizeof(ReRevvedUniqueEraAbilityReplacement) == 176);
static_assert(sizeof(ReRevvedUniqueEraAbilityRuleInfo) == 180);
static_assert(sizeof(ReRevvedUniqueEraAbilityCellQuery) == 40);
static_assert(sizeof(ReRevvedUniqueEraAbilityCellEvaluation) == 40);

extern "C" uint32_t ReRevvedUniqueEraAbilitiesAbiVersion(void)
{
    return REREVVED_UNIQUE_ERA_ABILITIES_ABI_VERSION;
}

extern "C" int32_t ReRevvedRegisterUniqueEraAbilityReplacement(
    const ReRevvedUniqueEraAbilityReplacement* rule)
{
    using namespace rerevved::unique_era_abilities;
    if (!rule ||
        rule->struct_size < sizeof(ReRevvedUniqueEraAbilityReplacement) ||
        !IsRuleIdValid(rule->provider_id) || !IsRuleIdValid(rule->rule_id) ||
        !IsCellValid(rule->civilization, rule->unlock_era) ||
        !IsReplacementAbilityValid(rule->replacement_ability) ||
        !IsZeroed(rule->reserved))
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUniqueEraAbilityReplacement normalized = *rule;
    normalized.struct_size                         = sizeof(normalized);
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
                       ? REREVVED_UNIQUE_ERA_ABILITIES_OK
                       : REREVVED_UNIQUE_ERA_ABILITIES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), RuleKeyLess);
    }
    catch (...)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INTERNAL;
    }
    return REREVVED_UNIQUE_ERA_ABILITIES_OK;
}

extern "C" int32_t ReRevvedGetUniqueEraAbilityRuleCount(uint32_t* out_count)
{
    if (!out_count)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unique_era_abilities::registry_mutex);
    *out_count = static_cast<uint32_t>(
        rerevved::unique_era_abilities::registry.size());
    return REREVVED_UNIQUE_ERA_ABILITIES_OK;
}

extern "C" int32_t ReRevvedGetUniqueEraAbilityRule(
    uint32_t                          index,
    ReRevvedUniqueEraAbilityRuleInfo* out,
    uint32_t                          out_size)
{
    using namespace rerevved::unique_era_abilities;
    if (!out)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kRuleInfoPrefix)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registry_mutex);
    if (index >= registry.size())
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    const auto&                      rule = registry[index];
    ReRevvedUniqueEraAbilityRuleInfo result{};
    result.struct_size         = sizeof(result);
    result.civilization        = rule.civilization;
    result.unlock_era          = rule.unlock_era;
    result.replacement_ability = rule.replacement_ability;
    std::memcpy(result.provider_id, rule.provider_id, sizeof(result.provider_id));
    std::memcpy(result.rule_id, rule.rule_id, sizeof(result.rule_id));
    if (ReplacementCount(rule) > 1)
    {
        result.status_flags |=
            REREVVED_UNIQUE_ERA_ABILITY_RULE_REPLACEMENT_CONFLICT;
    }
    return CopyOutput(out, out_size, result, kRuleInfoPrefix);
}

extern "C" int32_t ReRevvedEvaluateUniqueEraAbilityCell(
    const ReRevvedUniqueEraAbilityCellQuery* query,
    ReRevvedUniqueEraAbilityCellEvaluation*  out,
    uint32_t                                 out_size)
{
    using namespace rerevved::unique_era_abilities;
    if (!out)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kEvaluationPrefix)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query ||
        query->struct_size < sizeof(ReRevvedUniqueEraAbilityCellQuery) ||
        !IsZeroed(query->reserved))
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUniqueEraAbilityCellEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->unlock_era,
                     query->native_ability,
                     result))
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    return CopyOutput(out, out_size, result, kEvaluationPrefix);
}
