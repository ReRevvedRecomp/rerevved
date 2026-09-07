#include "unit_effect_rules_registry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "unit_catalog_api.h"

namespace rerevved::unit_effect_rules
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 152;
constexpr uint32_t kEvaluationPrefix = 20;
constexpr int32_t kVeteranLevel      = 2;

struct NativeSpecialUpgradeMapping
{
    ReRevvedUnitEffectId effect;
    uint32_t mask;
};

constexpr std::array<NativeSpecialUpgradeMapping, 9>
    kNativeSpecialUpgradeMappings = { {
        { REREVVED_UNIT_EFFECT_CREATION_GUERILLA, 1u << 2 },
        { REREVVED_UNIT_EFFECT_CREATION_BLITZ, 1u << 0 },
        { REREVVED_UNIT_EFFECT_CREATION_INFILTRATION, 1u << 1 },
        { REREVVED_UNIT_EFFECT_CREATION_LOYALTY, 1u << 3 },
        { REREVVED_UNIT_EFFECT_CREATION_ENGINEER, 1u << 4 },
        { REREVVED_UNIT_EFFECT_CREATION_LEADERSHIP, 1u << 5 },
        { REREVVED_UNIT_EFFECT_CREATION_MARCH, 1u << 6 },
        { REREVVED_UNIT_EFFECT_CREATION_MEDIC, 1u << 7 },
        { REREVVED_UNIT_EFFECT_CREATION_SCOUT, 1u << 8 },
    } };

std::shared_mutex registry_mutex;
std::vector<ReRevvedUnitEffectRule> registry;

bool IsRuleIdValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', REREVVED_UNIT_EFFECT_RULE_ID_CAPACITY);
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
                REREVVED_UNIT_EFFECT_RULE_ID_CAPACITY - length - 1);
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

bool IsEffectValid(ReRevvedUnitEffectId effect)
{
    if (effect == REREVVED_UNIT_EFFECT_CREATION_VETERAN)
    {
        return true;
    }

    return std::any_of(
        kNativeSpecialUpgradeMappings.begin(),
        kNativeSpecialUpgradeMappings.end(),
        [effect](const NativeSpecialUpgradeMapping& mapping)
        {
            return mapping.effect == effect;
        });
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

bool TargetMatches(const ReRevvedUnitEffectRule& rule,
                   ReRevvedCivilizationId civilization,
                   ReRevvedUnitTypeId base_unit_type,
                   ReRevvedUnitIdentityId identity,
                   ReRevvedUnitEffectId effect)
{
    return rule.civilization == civilization &&
           rule.base_unit_type == base_unit_type && rule.identity == identity &&
           rule.effect == effect;
}

bool RuleKeyMatches(const ReRevvedUnitEffectRule& left,
                    const ReRevvedUnitEffectRule& right)
{
    return std::strcmp(left.provider_id, right.provider_id) == 0 &&
           std::strcmp(left.rule_id, right.rule_id) == 0;
}

bool RuleKeyLess(const ReRevvedUnitEffectRule& left,
                 const ReRevvedUnitEffectRule& right)
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
    return REREVVED_UNIT_EFFECT_RULES_OK;
}

} // namespace

bool TryGetNativeSpecialUpgradeMask(ReRevvedUnitEffectId effect,
                                    uint32_t& mask)
{
    const auto mapping = std::find_if(
        kNativeSpecialUpgradeMappings.begin(),
        kNativeSpecialUpgradeMappings.end(),
        [effect](const NativeSpecialUpgradeMapping& candidate)
        {
            return candidate.effect == effect;
        });
    if (mapping == kNativeSpecialUpgradeMappings.end())
    {
        return false;
    }

    mask = mapping->mask;
    return true;
}

bool TryEvaluate(ReRevvedCivilizationId civilization,
                 ReRevvedUnitTypeId base_unit_type,
                 ReRevvedUnitIdentityId identity,
                 ReRevvedUnitEffectId effect,
                 int32_t native_level,
                 ReRevvedUnitEffectEvaluation& evaluation)
{
    if (!IsTargetValid(civilization, base_unit_type, identity) ||
        !IsEffectValid(effect))
    {
        return false;
    }

    evaluation = {
        sizeof(ReRevvedUnitEffectEvaluation),
        native_level,
        native_level,
        0,
        0,
        {},
    };

    std::shared_lock lock(registry_mutex);
    for (const auto& rule : registry)
    {
        if (TargetMatches(rule,
                          civilization,
                          base_unit_type,
                          identity,
                          effect))
        {
            ++evaluation.grant_count;
        }
    }

    if (evaluation.grant_count != 0)
    {
        evaluation.status_flags |= REREVVED_UNIT_EFFECT_EVALUATION_GRANTED;
        if (effect == REREVVED_UNIT_EFFECT_CREATION_VETERAN &&
            evaluation.final_level < kVeteranLevel)
        {
            evaluation.final_level = kVeteranLevel;
        }
    }
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registry_mutex);
    registry.clear();
}

} // namespace rerevved::unit_effect_rules

static_assert(sizeof(ReRevvedUnitEffectRule) == 168);
static_assert(sizeof(ReRevvedUnitEffectRuleInfo) == 192);
static_assert(sizeof(ReRevvedUnitEffectQuery) == 44);
static_assert(sizeof(ReRevvedUnitEffectEvaluation) == 40);

extern "C" uint32_t ReRevvedUnitEffectRulesAbiVersion(void)
{
    return REREVVED_UNIT_EFFECT_RULES_ABI_VERSION;
}

extern "C" int32_t ReRevvedRegisterUnitEffectRule(
    const ReRevvedUnitEffectRule* rule)
{
    using namespace rerevved::unit_effect_rules;
    if (!rule || rule->struct_size < sizeof(ReRevvedUnitEffectRule) ||
        !IsRuleIdValid(rule->provider_id) || !IsRuleIdValid(rule->rule_id) ||
        !IsTargetValid(rule->civilization,
                       rule->base_unit_type,
                       rule->identity) ||
        !IsEffectValid(rule->effect) || !IsZeroed(rule->reserved))
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitEffectRule normalized = *rule;
    normalized.struct_size            = sizeof(normalized);
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
                       ? REREVVED_UNIT_EFFECT_RULES_OK
                       : REREVVED_UNIT_EFFECT_RULES_ERR_DUPLICATE_RULE_ID;
        }

        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), RuleKeyLess);
    }
    catch (...)
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_INTERNAL;
    }
    return REREVVED_UNIT_EFFECT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUnitEffectRuleCount(uint32_t* out_count)
{
    if (!out_count)
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    std::shared_lock lock(rerevved::unit_effect_rules::registry_mutex);
    *out_count =
        static_cast<uint32_t>(rerevved::unit_effect_rules::registry.size());
    return REREVVED_UNIT_EFFECT_RULES_OK;
}

extern "C" int32_t ReRevvedGetUnitEffectRule(
    uint32_t index,
    ReRevvedUnitEffectRuleInfo* out,
    uint32_t out_size)
{
    using namespace rerevved::unit_effect_rules;
    if (!out)
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kRuleInfoPrefix)
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL;
    }

    std::shared_lock lock(registry_mutex);
    if (index >= registry.size())
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    const auto& rule = registry[index];
    ReRevvedUnitEffectRuleInfo result{};
    result.struct_size    = sizeof(result);
    result.civilization   = rule.civilization;
    result.base_unit_type = rule.base_unit_type;
    result.identity       = rule.identity;
    result.effect         = rule.effect;
    std::memcpy(result.provider_id, rule.provider_id, sizeof(result.provider_id));
    std::memcpy(result.rule_id, rule.rule_id, sizeof(result.rule_id));
    return CopyOutput(out, out_size, result);
}

extern "C" int32_t ReRevvedEvaluateUnitEffect(
    const ReRevvedUnitEffectQuery* query,
    ReRevvedUnitEffectEvaluation* out,
    uint32_t out_size)
{
    using namespace rerevved::unit_effect_rules;
    if (!out)
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kEvaluationPrefix)
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->struct_size < sizeof(ReRevvedUnitEffectQuery) ||
        !IsZeroed(query->reserved))
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    ReRevvedUnitEffectEvaluation result{};
    if (!TryEvaluate(query->civilization,
                     query->base_unit_type,
                     query->identity,
                     query->effect,
                     query->native_level,
                     result))
    {
        return REREVVED_UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }
    return CopyOutput(out, out_size, result);
}
