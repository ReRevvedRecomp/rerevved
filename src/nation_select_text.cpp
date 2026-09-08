#include "nation_select_text_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "unit_catalog_api.h"

namespace rerevved::nation_select_text
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 420;
constexpr uint32_t kEvaluationPrefix = 268;

std::shared_mutex                 registryMutex;
std::vector<NationSelectTextRule> registry;

bool isIdentifierValid(const char* value)
{
    const void* terminator = std::memchr(
        value, '\0', NATION_SELECT_TEXT_RULE_ID_CAPACITY);
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

bool isTextValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', NATION_SELECT_TEXT_CAPACITY);
    if (!terminator || value[0] == '\0')
    {
        return false;
    }
    for (const unsigned char* current =
             reinterpret_cast<const unsigned char*>(value);
         *current != '\0';
         ++current)
    {
        if (*current < 0x20 || *current > 0x7e)
        {
            return false;
        }
    }
    return true;
}

template <size_t Size>
bool isZeroed(const int32_t (&values)[Size])
{
    return std::all_of(std::begin(values), std::end(values), [](int32_t value)
                       {
                           return value == 0;
                       });
}

void normalizeString(char* value, size_t capacity)
{
    const size_t length = std::strlen(value);
    std::memset(value + length + 1, 0, capacity - length - 1);
}

bool isCivilizationValid(CivilizationId civilization)
{
    return civilization >= 0 && civilization < CIVILIZATION_COUNT;
}

bool hasUnusedSelectors(const NationSelectTextQuery& query)
{
    return query.unlockEra == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
           query.ability == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
           query.baseUnitType == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
           query.identity == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
           query.displayForm == NATION_SELECT_TEXT_SELECTOR_UNUSED;
}

bool isQueryValid(const NationSelectTextQuery& query)
{
    if (!isZeroed(query.reserved))
    {
        return false;
    }

    if (query.surface == NATION_SELECT_TEXT_SURFACE_LEADER_NAME ||
        query.surface == NATION_SELECT_TEXT_SURFACE_CIVILIZATION_NAME ||
        query.surface == NATION_SELECT_TEXT_SURFACE_CIVILIZATION_TRAIT)
    {
        return isCivilizationValid(query.civilization) &&
               hasUnusedSelectors(query);
    }
    if (query.surface ==
        NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT_SECTION_HEADING)
    {
        return query.civilization == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
               hasUnusedSelectors(query);
    }
    if (query.surface == NATION_SELECT_TEXT_SURFACE_ERA_HEADING)
    {
        return query.civilization == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
               query.unlockEra >= UNLOCK_ERA_ANCIENT &&
               query.unlockEra <= UNLOCK_ERA_MODERN &&
               query.ability == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
               query.baseUnitType == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
               query.identity == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
               query.displayForm == NATION_SELECT_TEXT_SELECTOR_UNUSED;
    }
    if (query.surface == NATION_SELECT_TEXT_SURFACE_ERA_ABILITY)
    {
        return isCivilizationValid(query.civilization) &&
               query.unlockEra >= UNLOCK_ERA_ANCIENT &&
               query.unlockEra <= UNLOCK_ERA_MODERN &&
               query.ability > 0 &&
               query.baseUnitType == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
               query.identity == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
               query.displayForm == NATION_SELECT_TEXT_SELECTOR_UNUSED;
    }
    if (query.surface == NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT)
    {
        UnitIdentityId resolved = UNIT_IDENTITY_BASE;
        return query.unlockEra == NATION_SELECT_TEXT_SELECTOR_UNUSED &&
               query.ability == 0 && query.baseUnitType >= 0 &&
               query.baseUnitType < UNIT_TYPE_COUNT &&
               query.identity > UNIT_IDENTITY_BASE &&
               query.identity < UNIT_IDENTITY_COUNT &&
               query.displayForm == UNIT_DISPLAY_FORM_UNIT &&
               unit_catalog::TryResolveUnitIdentity(query.civilization,
                                                    query.baseUnitType,
                                                    resolved) &&
               resolved == query.identity;
    }
    return false;
}

NationSelectTextQuery queryFromRule(
    const NationSelectTextRule& rule)
{
    return {
        sizeof(NationSelectTextQuery),
        rule.surface,
        rule.civilization,
        rule.unlockEra,
        rule.ability,
        rule.baseUnitType,
        rule.identity,
        rule.displayForm,
        {},
    };
}

bool targetMatches(const NationSelectTextRule&  rule,
                   const NationSelectTextQuery& query)
{
    return rule.surface == query.surface &&
           rule.civilization == query.civilization &&
           rule.unlockEra == query.unlockEra &&
           rule.ability == query.ability &&
           rule.baseUnitType == query.baseUnitType &&
           rule.identity == query.identity &&
           rule.displayForm == query.displayForm;
}

bool ruleKeyMatches(const NationSelectTextRule& left,
                    const NationSelectTextRule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

bool ruleKeyLess(const NationSelectTextRule& left,
                 const NationSelectTextRule& right)
{
    const int providerOrder = std::strcmp(left.providerId, right.providerId);
    return providerOrder < 0 ||
           (providerOrder == 0 &&
            std::strcmp(left.ruleId, right.ruleId) < 0);
}

uint32_t replacementCount(const NationSelectTextRule& target)
{
    const auto query = queryFromRule(target);
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return targetMatches(candidate, query);
        }));
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
                   const Record& producer)
{
    uint32_t copySize = std::min<uint32_t>(outSize, sizeof(Record));
    copySize -= copySize % sizeof(uint32_t);
    std::memcpy(out, &producer, copySize);
    return NATION_SELECT_TEXT_OK;
}

} // namespace

bool TryEvaluate(const NationSelectTextQuery& query,
                 NationSelectTextEvaluation&  evaluation)
{
    if (!isQueryValid(query))
    {
        return false;
    }
    evaluation = { sizeof(evaluation), 0, 0, {}, {} };

    std::shared_lock lock(registryMutex);
    for (const auto& rule : registry)
    {
        if (!targetMatches(rule, query))
        {
            continue;
        }
        ++evaluation.replacementCount;
        if (evaluation.replacementCount == 1)
        {
            std::memcpy(evaluation.text, rule.text, sizeof(evaluation.text));
        }
    }
    if (evaluation.replacementCount == 1)
    {
        evaluation.statusFlags |=
            NATION_SELECT_TEXT_EVALUATION_REPLACED;
    }
    else if (evaluation.replacementCount > 1)
    {
        evaluation.statusFlags |=
            NATION_SELECT_TEXT_EVALUATION_REPLACEMENT_CONFLICT;
        std::memset(evaluation.text, 0, sizeof(evaluation.text));
    }
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registryMutex);
    registry.clear();
}

} // namespace rerevved::nation_select_text

static_assert(sizeof(NationSelectTextRule) == 448);
static_assert(sizeof(NationSelectTextRuleInfo) == 452);
static_assert(sizeof(NationSelectTextQuery) == 64);
static_assert(sizeof(NationSelectTextEvaluation) == 300);

extern "C" uint32_t NationSelectTextAbiVersion(void)
{
    return NATION_SELECT_TEXT_ABI_VERSION;
}

extern "C" int32_t RegisterNationSelectTextRule(
    const NationSelectTextRule* rule)
{
    using namespace rerevved::nation_select_text;
    if (!rule || rule->structSize < sizeof(*rule) ||
        !isIdentifierValid(rule->providerId) ||
        !isIdentifierValid(rule->ruleId) || !isTextValid(rule->text) ||
        !isZeroed(rule->reserved) || !isQueryValid(queryFromRule(*rule)))
    {
        return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
    }

    NationSelectTextRule normalized = *rule;
    normalized.structSize           = sizeof(normalized);
    normalizeString(normalized.providerId, sizeof(normalized.providerId));
    normalizeString(normalized.ruleId, sizeof(normalized.ruleId));
    normalizeString(normalized.text, sizeof(normalized.text));

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
                       ? NATION_SELECT_TEXT_OK
                       : NATION_SELECT_TEXT_ERR_DUPLICATE_RULE_ID;
        }
        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), ruleKeyLess);
    }
    catch (...)
    {
        return NATION_SELECT_TEXT_ERR_INTERNAL;
    }
    return NATION_SELECT_TEXT_OK;
}

extern "C" int32_t GetNationSelectTextRuleCount(uint32_t* outCount)
{
    if (!outCount)
    {
        return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
    }
    std::shared_lock lock(rerevved::nation_select_text::registryMutex);
    *outCount = static_cast<uint32_t>(
        rerevved::nation_select_text::registry.size());
    return NATION_SELECT_TEXT_OK;
}

extern "C" int32_t GetNationSelectTextRule(
    uint32_t                  index,
    NationSelectTextRuleInfo* out,
    uint32_t                  outSize)
{
    using namespace rerevved::nation_select_text;
    if (!out)
    {
        return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kRuleInfoPrefix)
    {
        return NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL;
    }
    std::shared_lock lock(registryMutex);
    if (index >= registry.size())
    {
        return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
    }

    const auto&              rule = registry[index];
    NationSelectTextRuleInfo result{};
    result.structSize   = sizeof(result);
    result.surface      = rule.surface;
    result.civilization = rule.civilization;
    result.unlockEra    = rule.unlockEra;
    result.ability      = rule.ability;
    result.baseUnitType = rule.baseUnitType;
    result.identity     = rule.identity;
    result.displayForm  = rule.displayForm;
    std::memcpy(result.providerId, rule.providerId, sizeof(result.providerId));
    std::memcpy(result.ruleId, rule.ruleId, sizeof(result.ruleId));
    std::memcpy(result.text, rule.text, sizeof(result.text));
    if (replacementCount(rule) > 1)
    {
        result.statusFlags |=
            NATION_SELECT_TEXT_RULE_REPLACEMENT_CONFLICT;
    }
    return copyOutput(out, outSize, result);
}

extern "C" int32_t EvaluateNationSelectText(
    const NationSelectTextQuery* query,
    NationSelectTextEvaluation*  out,
    uint32_t                     outSize)
{
    using namespace rerevved::nation_select_text;
    if (!out)
    {
        return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
    }
    clearOutput(out, outSize);
    if (outSize < kEvaluationPrefix)
    {
        return NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->structSize < sizeof(*query))
    {
        return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
    }
    NationSelectTextEvaluation result{};
    if (!TryEvaluate(*query, result))
    {
        return NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT;
    }
    return copyOutput(out, outSize, result);
}
