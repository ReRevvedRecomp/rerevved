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

std::shared_mutex                                registryMutex;
std::vector<ReRevvedUniqueEraAbilityReplacement> registry;

bool isRuleIdValid(const char* value)
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

void normalizeRuleId(char* value)
{
    const size_t length = std::strlen(value);
    std::memset(value + length + 1,
                0,
                REREVVED_UNIQUE_ERA_ABILITY_RULE_ID_CAPACITY - length - 1);
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

bool isCellValid(ReRevvedCivilizationId     civilization,
                 ReRevvedUniqueEraUnlockEra unlockEra)
{
    return civilization >= 0 && civilization < REREVVED_CIVILIZATION_COUNT &&
           unlockEra >= REREVVED_UNIQUE_ERA_ANCIENT &&
           unlockEra <= REREVVED_UNIQUE_ERA_MODERN;
}

bool isRetailAbilityValid(ReRevvedUniqueEraAbilityId ability)
{
    return std::binary_search(
        kRetailAbilities.begin(), kRetailAbilities.end(), ability);
}

bool isReplacementAbilityValid(ReRevvedUniqueEraAbilityId ability)
{
    return isRetailAbilityValid(ability) ||
           ability ==
               REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
}

bool targetMatches(const ReRevvedUniqueEraAbilityReplacement& rule,
                   ReRevvedCivilizationId                     civilization,
                   ReRevvedUniqueEraUnlockEra                 unlockEra)
{
    return rule.civilization == civilization && rule.unlockEra == unlockEra;
}

bool ruleKeyMatches(const ReRevvedUniqueEraAbilityReplacement& left,
                    const ReRevvedUniqueEraAbilityReplacement& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const ReRevvedUniqueEraAbilityReplacement& left,
                 const ReRevvedUniqueEraAbilityReplacement& right)
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
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < minimumPrefix)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t copySize = std::min<uint32_t>(outSize, sizeof(Record));
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, &producer, copySize);
    return REREVVED_UNIQUE_ERA_ABILITIES_OK;
}

uint32_t replacementCount(const ReRevvedUniqueEraAbilityReplacement& target)
{
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return targetMatches(
                candidate, target.civilization, target.unlockEra);
        }));
}

} // namespace

bool TryEvaluate(ReRevvedCivilizationId                  civilization,
                 ReRevvedUniqueEraUnlockEra              unlockEra,
                 ReRevvedUniqueEraAbilityId              nativeAbility,
                 ReRevvedUniqueEraAbilityCellEvaluation& evaluation)
{
    if (!isCellValid(civilization, unlockEra) ||
        !isRetailAbilityValid(nativeAbility))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedUniqueEraAbilityCellEvaluation),
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
            REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACED;
    }
    else if (evaluation.replacementCount > 1)
    {
        evaluation.statusFlags |=
            REREVVED_UNIQUE_ERA_ABILITY_EVALUATION_REPLACEMENT_CONFLICT;
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
        rule->structSize < sizeof(ReRevvedUniqueEraAbilityReplacement) ||
        !isRuleIdValid(rule->providerId) || !isRuleIdValid(rule->ruleId) ||
        !isCellValid(rule->civilization, rule->unlockEra) ||
        !isReplacementAbilityValid(rule->replacementAbility) ||
        !isZeroed(rule->reserved))
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUniqueEraAbilityReplacement normalized = *rule;
    normalized.structSize                          = sizeof(normalized);
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
                       ? REREVVED_UNIQUE_ERA_ABILITIES_OK
                       : REREVVED_UNIQUE_ERA_ABILITIES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INTERNAL;
    }
    return REREVVED_UNIQUE_ERA_ABILITIES_OK;
}

extern "C" int32_t ReRevvedGetUniqueEraAbilityRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unique_era_abilities::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::unique_era_abilities::registry.size());
    return REREVVED_UNIQUE_ERA_ABILITIES_OK;
}

extern "C" int32_t ReRevvedGetUniqueEraAbilityRule(
    uint32_t                          index,
    ReRevvedUniqueEraAbilityRuleInfo* out,
    uint32_t                          outSize)
{
    using namespace rerevved::unique_era_abilities;
    if (!out)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    const auto&                      rule = registry[index];
    ReRevvedUniqueEraAbilityRuleInfo result{};
    result.structSize         = sizeof(result);
    result.civilization       = rule.civilization;
    result.unlockEra          = rule.unlockEra;
    result.replacementAbility = rule.replacementAbility;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    if (replacementCount(rule) > 1)
    {
        result.statusFlags |=
            REREVVED_UNIQUE_ERA_ABILITY_RULE_REPLACEMENT_CONFLICT;
    }
    return copyOutput(out, outSize, result, kRuleInfoPrefix);
}

extern "C" int32_t ReRevvedEvaluateUniqueEraAbilityCell(
    const ReRevvedUniqueEraAbilityCellQuery* query,
    ReRevvedUniqueEraAbilityCellEvaluation*  out,
    uint32_t                                 outSize)
{
    using namespace rerevved::unique_era_abilities;
    if (!out)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query ||
        query->structSize < sizeof(ReRevvedUniqueEraAbilityCellQuery) ||
        !isZeroed(query->reserved))
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUniqueEraAbilityCellEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->unlockEra,
                     query->nativeAbility,
                     result))
    {
        return REREVVED_UNIQUE_ERA_ABILITIES_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result, kEvaluationPrefix);
}
