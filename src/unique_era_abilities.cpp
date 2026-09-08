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

constexpr std::array<EraAbilityId, 45> kRetailAbilities = {
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

std::shared_mutex                  registryMutex;
std::vector<EraAbilityReplacement> registry;

bool isRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', ERA_ABILITY_RULE_ID_CAPACITY);
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
                ERA_ABILITY_RULE_ID_CAPACITY - length - 1);
}

template <size_t Size>
bool isZeroed(const int32_t (&values)[Size])
{
    return std::all_of(
        std::begin(values), std::end(values), [](int32_t value)
        {
            return value == 0;
        });
}

bool isCellValid(CivilizationId civilization,
                 UnlockEra      unlockEra)
{
    return civilization >= 0 && civilization < CIVILIZATION_COUNT &&
           unlockEra >= UNLOCK_ERA_ANCIENT &&
           unlockEra <= UNLOCK_ERA_MODERN;
}

bool isRetailAbilityValid(EraAbilityId ability)
{
    return std::binary_search(
        kRetailAbilities.begin(), kRetailAbilities.end(), ability);
}

bool isReplacementAbilityValid(EraAbilityId ability)
{
    return isRetailAbilityValid(ability) ||
           ability ==
               ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
}

bool targetMatches(const EraAbilityReplacement& rule,
                   CivilizationId               civilization,
                   UnlockEra                    unlockEra)
{
    return rule.civilization == civilization && rule.unlockEra == unlockEra;
}

bool ruleKeyMatches(const EraAbilityReplacement& left,
                    const EraAbilityReplacement& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const EraAbilityReplacement& left,
                 const EraAbilityReplacement& right)
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
int32_t copyOutput(Record*       out,
                   uint32_t      outSize,
                   const Record& producer,
                   uint32_t      minimumPrefix)
{
    if (!out)
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < minimumPrefix)
    {
        return ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t copySize = std::min<uint32_t>(outSize, sizeof(Record));
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, &producer, copySize);
    return ERA_ABILITIES_OK;
}

uint32_t replacementCount(const EraAbilityReplacement& target)
{
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return targetMatches(
                candidate, target.civilization, target.unlockEra);
        }));
}

} // namespace

bool TryEvaluate(CivilizationId            civilization,
                 UnlockEra                 unlockEra,
                 EraAbilityId              nativeAbility,
                 EraAbilityCellEvaluation& evaluation)
{
    if (!isCellValid(civilization, unlockEra) ||
        !isRetailAbilityValid(nativeAbility))
    {
        return false;
    }

    evaluation = {
        sizeof(EraAbilityCellEvaluation),
        nativeAbility,
        nativeAbility,
        0,
        0,
        {},
    };

    std::shared_lock lock(registryMutex);
    for (const auto& rule : registry)
    {
        if (!targetMatches(rule, civilization, unlockEra))
        {
            continue;
        }
        ++evaluation.replacementCount;
        evaluation.effectiveAbility = rule.replacementAbility;
    }

    if (evaluation.replacementCount == 1)
    {
        evaluation.statusFlags |=
            ERA_ABILITY_EVALUATION_REPLACED;
    }
    else if (evaluation.replacementCount > 1)
    {
        evaluation.statusFlags |=
            ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT;
        evaluation.effectiveAbility = nativeAbility;
    }
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registryMutex);
    registry.clear();
}

} // namespace rerevved::unique_era_abilities

static_assert(sizeof(UnlockEra) == sizeof(int32_t));
static_assert(sizeof(EraAbilityId) == sizeof(int32_t));
static_assert(sizeof(EraAbilityReplacement) == 176);
static_assert(sizeof(EraAbilityRuleInfo) == 180);
static_assert(sizeof(EraAbilityCellQuery) == 40);
static_assert(sizeof(EraAbilityCellEvaluation) == 40);

extern "C" uint32_t EraAbilitiesAbiVersion(void)
{
    return ERA_ABILITIES_ABI_VERSION;
}

extern "C" int32_t RegisterEraAbilityReplacement(
    const EraAbilityReplacement* rule)
{
    using namespace rerevved::unique_era_abilities;
    if (!rule ||
        rule->structSize < sizeof(EraAbilityReplacement) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isCellValid(rule->civilization, rule->unlockEra) ||
        !isReplacementAbilityValid(rule->replacementAbility) ||
        !isZeroed(rule->reserved))
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    EraAbilityReplacement normalized = *rule;
    normalized.structSize            = sizeof(normalized);
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
                       ? ERA_ABILITIES_OK
                       : ERA_ABILITIES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return ERA_ABILITIES_ERR_INTERNAL;
    }
    return ERA_ABILITIES_OK;
}

extern "C" int32_t GetEraAbilityRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unique_era_abilities::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::unique_era_abilities::registry.size());
    return ERA_ABILITIES_OK;
}

extern "C" int32_t GetEraAbilityRule(
    uint32_t            index,
    EraAbilityRuleInfo* out,
    uint32_t            outSize)
{
    using namespace rerevved::unique_era_abilities;
    if (!out)
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    const auto&        rule = registry[index];
    EraAbilityRuleInfo result{};
    result.structSize         = sizeof(result);
    result.civilization       = rule.civilization;
    result.unlockEra          = rule.unlockEra;
    result.replacementAbility = rule.replacementAbility;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    if (replacementCount(rule) > 1)
    {
        result.statusFlags |=
            ERA_ABILITY_RULE_REPLACEMENT_CONFLICT;
    }
    return copyOutput(out, outSize, result, kRuleInfoPrefix);
}

extern "C" int32_t EvaluateEraAbilityCell(
    const EraAbilityCellQuery* query,
    EraAbilityCellEvaluation*  out,
    uint32_t                   outSize)
{
    using namespace rerevved::unique_era_abilities;
    if (!out)
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query ||
        query->structSize < sizeof(EraAbilityCellQuery) ||
        !isZeroed(query->reserved))
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    EraAbilityCellEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->unlockEra,
                     query->nativeAbility,
                     result))
    {
        return ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result, kEvaluationPrefix);
}
