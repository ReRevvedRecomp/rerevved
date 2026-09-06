#include "presentation_text_registry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "unit_catalog_api.h"

namespace rerevved::presentation_text
{

namespace
{

constexpr uint32_t kRuleInfoPrefix   = 420;
constexpr uint32_t kEvaluationPrefix = 268;

std::shared_mutex                         registry_mutex;
std::vector<ReRevvedPresentationTextRule> registry;

bool IsIdentifierValid(const char* value)
{
    const void* terminator = std::memchr(
        value, '\0', REREVVED_PRESENTATION_TEXT_RULE_ID_CAPACITY);
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

bool IsTextValid(const char* value)
{
    const void* terminator =
        std::memchr(value, '\0', REREVVED_PRESENTATION_TEXT_CAPACITY);
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
bool IsZeroed(const int32_t (&values)[Size])
{
    return std::all_of(std::begin(values), std::end(values), [](int32_t value)
                       {
                           return value == 0;
                       });
}

void NormalizeString(char* value, size_t capacity)
{
    const size_t length = std::strlen(value);
    std::memset(value + length + 1, 0, capacity - length - 1);
}

bool IsCivilizationValid(ReRevvedCivilizationId civilization)
{
    return civilization >= 0 && civilization < REREVVED_CIVILIZATION_COUNT;
}

bool HasUnusedSelectors(const ReRevvedPresentationTextQuery& query)
{
    return query.unlock_era == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
           query.ability == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
           query.base_unit_type == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
           query.identity == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
           query.display_form == REREVVED_PRESENTATION_SELECTOR_UNUSED;
}

bool IsQueryValid(const ReRevvedPresentationTextQuery& query)
{
    if (!IsZeroed(query.reserved))
    {
        return false;
    }

    if (query.surface == REREVVED_PRESENTATION_SURFACE_LEADER_NAME ||
        query.surface == REREVVED_PRESENTATION_SURFACE_CIVILIZATION_NAME ||
        query.surface == REREVVED_PRESENTATION_SURFACE_CIVILIZATION_TRAIT)
    {
        return IsCivilizationValid(query.civilization) &&
               HasUnusedSelectors(query);
    }
    if (query.surface ==
        REREVVED_PRESENTATION_SURFACE_UNIQUE_UNIT_SECTION_HEADING)
    {
        return query.civilization == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
               HasUnusedSelectors(query);
    }
    if (query.surface == REREVVED_PRESENTATION_SURFACE_ERA_HEADING)
    {
        return query.civilization == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
               query.unlock_era >= REREVVED_UNIQUE_ERA_ANCIENT &&
               query.unlock_era <= REREVVED_UNIQUE_ERA_MODERN &&
               query.ability == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
               query.base_unit_type == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
               query.identity == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
               query.display_form == REREVVED_PRESENTATION_SELECTOR_UNUSED;
    }
    if (query.surface == REREVVED_PRESENTATION_SURFACE_ERA_ABILITY)
    {
        return IsCivilizationValid(query.civilization) &&
               query.unlock_era >= REREVVED_UNIQUE_ERA_ANCIENT &&
               query.unlock_era <= REREVVED_UNIQUE_ERA_MODERN &&
               query.ability > 0 &&
               query.base_unit_type == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
               query.identity == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
               query.display_form == REREVVED_PRESENTATION_SELECTOR_UNUSED;
    }
    if (query.surface == REREVVED_PRESENTATION_SURFACE_UNIQUE_UNIT)
    {
        ReRevvedUnitIdentityId resolved = REREVVED_UNIT_IDENTITY_BASE;
        return query.unlock_era == REREVVED_PRESENTATION_SELECTOR_UNUSED &&
               query.ability == 0 && query.base_unit_type >= 0 &&
               query.base_unit_type < REREVVED_UNIT_TYPE_COUNT &&
               query.identity > REREVVED_UNIT_IDENTITY_BASE &&
               query.identity < REREVVED_UNIT_IDENTITY_COUNT &&
               query.display_form == REREVVED_UNIT_DISPLAY_FORM_UNIT &&
               unit_catalog::TryResolveUnitIdentity(query.civilization,
                                                    query.base_unit_type,
                                                    resolved) &&
               resolved == query.identity;
    }
    return false;
}

ReRevvedPresentationTextQuery QueryFromRule(
    const ReRevvedPresentationTextRule& rule)
{
    return {
        sizeof(ReRevvedPresentationTextQuery),
        rule.surface,
        rule.civilization,
        rule.unlock_era,
        rule.ability,
        rule.base_unit_type,
        rule.identity,
        rule.display_form,
        {},
    };
}

bool TargetMatches(const ReRevvedPresentationTextRule&  rule,
                   const ReRevvedPresentationTextQuery& query)
{
    return rule.surface == query.surface &&
           rule.civilization == query.civilization &&
           rule.unlock_era == query.unlock_era &&
           rule.ability == query.ability &&
           rule.base_unit_type == query.base_unit_type &&
           rule.identity == query.identity &&
           rule.display_form == query.display_form;
}

bool RuleKeyMatches(const ReRevvedPresentationTextRule& left,
                    const ReRevvedPresentationTextRule& right)
{
    return std::strcmp(left.provider_id, right.provider_id) == 0 &&
           std::strcmp(left.rule_id, right.rule_id) == 0;
}

bool RuleKeyLess(const ReRevvedPresentationTextRule& left,
                 const ReRevvedPresentationTextRule& right)
{
    const int provider_order = std::strcmp(left.provider_id, right.provider_id);
    return provider_order < 0 ||
           (provider_order == 0 &&
            std::strcmp(left.rule_id, right.rule_id) < 0);
}

uint32_t ReplacementCount(const ReRevvedPresentationTextRule& target)
{
    const auto query = QueryFromRule(target);
    return static_cast<uint32_t>(std::count_if(
        registry.begin(), registry.end(), [&](const auto& candidate)
        {
            return TargetMatches(candidate, query);
        }));
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
                   const Record& producer)
{
    uint32_t copy_size = std::min<uint32_t>(out_size, sizeof(Record));
    copy_size -= copy_size % sizeof(uint32_t);
    std::memcpy(out, &producer, copy_size);
    return REREVVED_PRESENTATION_TEXT_OK;
}

} // namespace

bool TryEvaluate(const ReRevvedPresentationTextQuery& query,
                 ReRevvedPresentationTextEvaluation&  evaluation)
{
    if (!IsQueryValid(query))
    {
        return false;
    }
    evaluation = { sizeof(evaluation), 0, 0, {}, {} };

    std::shared_lock lock(registry_mutex);
    for (const auto& rule : registry)
    {
        if (!TargetMatches(rule, query))
        {
            continue;
        }
        ++evaluation.replacement_count;
        if (evaluation.replacement_count == 1)
        {
            std::memcpy(evaluation.text, rule.text, sizeof(evaluation.text));
        }
    }
    if (evaluation.replacement_count == 1)
    {
        evaluation.status_flags |=
            REREVVED_PRESENTATION_TEXT_EVALUATION_REPLACED;
    }
    else if (evaluation.replacement_count > 1)
    {
        evaluation.status_flags |=
            REREVVED_PRESENTATION_TEXT_EVALUATION_REPLACEMENT_CONFLICT;
        std::memset(evaluation.text, 0, sizeof(evaluation.text));
    }
    return true;
}

void ResetForTests()
{
    std::unique_lock lock(registry_mutex);
    registry.clear();
}

} // namespace rerevved::presentation_text

static_assert(sizeof(ReRevvedPresentationTextRule) == 448);
static_assert(sizeof(ReRevvedPresentationTextRuleInfo) == 452);
static_assert(sizeof(ReRevvedPresentationTextQuery) == 64);
static_assert(sizeof(ReRevvedPresentationTextEvaluation) == 300);

extern "C" uint32_t ReRevvedPresentationTextAbiVersion(void)
{
    return REREVVED_PRESENTATION_TEXT_ABI_VERSION;
}

extern "C" int32_t ReRevvedRegisterPresentationTextRule(
    const ReRevvedPresentationTextRule* rule)
{
    using namespace rerevved::presentation_text;
    if (!rule || rule->struct_size < sizeof(*rule) ||
        !IsIdentifierValid(rule->provider_id) ||
        !IsIdentifierValid(rule->rule_id) || !IsTextValid(rule->text) ||
        !IsZeroed(rule->reserved) || !IsQueryValid(QueryFromRule(*rule)))
    {
        return REREVVED_PRESENTATION_TEXT_ERR_INVALID_ARGUMENT;
    }

    ReRevvedPresentationTextRule normalized = *rule;
    normalized.struct_size                  = sizeof(normalized);
    NormalizeString(normalized.provider_id, sizeof(normalized.provider_id));
    NormalizeString(normalized.rule_id, sizeof(normalized.rule_id));
    NormalizeString(normalized.text, sizeof(normalized.text));

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
                       ? REREVVED_PRESENTATION_TEXT_OK
                       : REREVVED_PRESENTATION_TEXT_ERR_DUPLICATE_RULE_ID;
        }
        registry.push_back(normalized);
        std::sort(registry.begin(), registry.end(), RuleKeyLess);
    }
    catch (...)
    {
        return REREVVED_PRESENTATION_TEXT_ERR_INTERNAL;
    }
    return REREVVED_PRESENTATION_TEXT_OK;
}

extern "C" int32_t ReRevvedGetPresentationTextRuleCount(uint32_t* out_count)
{
    if (!out_count)
    {
        return REREVVED_PRESENTATION_TEXT_ERR_INVALID_ARGUMENT;
    }
    std::shared_lock lock(rerevved::presentation_text::registry_mutex);
    *out_count = static_cast<uint32_t>(
        rerevved::presentation_text::registry.size());
    return REREVVED_PRESENTATION_TEXT_OK;
}

extern "C" int32_t ReRevvedGetPresentationTextRule(
    uint32_t                          index,
    ReRevvedPresentationTextRuleInfo* out,
    uint32_t                          out_size)
{
    using namespace rerevved::presentation_text;
    if (!out)
    {
        return REREVVED_PRESENTATION_TEXT_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kRuleInfoPrefix)
    {
        return REREVVED_PRESENTATION_TEXT_ERR_BUFFER_TOO_SMALL;
    }
    std::shared_lock lock(registry_mutex);
    if (index >= registry.size())
    {
        return REREVVED_PRESENTATION_TEXT_ERR_INVALID_ARGUMENT;
    }

    const auto&                      rule = registry[index];
    ReRevvedPresentationTextRuleInfo result{};
    result.struct_size    = sizeof(result);
    result.surface        = rule.surface;
    result.civilization   = rule.civilization;
    result.unlock_era     = rule.unlock_era;
    result.ability        = rule.ability;
    result.base_unit_type = rule.base_unit_type;
    result.identity       = rule.identity;
    result.display_form   = rule.display_form;
    std::memcpy(result.provider_id, rule.provider_id, sizeof(result.provider_id));
    std::memcpy(result.rule_id, rule.rule_id, sizeof(result.rule_id));
    std::memcpy(result.text, rule.text, sizeof(result.text));
    if (ReplacementCount(rule) > 1)
    {
        result.status_flags |=
            REREVVED_PRESENTATION_TEXT_RULE_REPLACEMENT_CONFLICT;
    }
    return CopyOutput(out, out_size, result);
}

extern "C" int32_t ReRevvedEvaluatePresentationText(
    const ReRevvedPresentationTextQuery* query,
    ReRevvedPresentationTextEvaluation*  out,
    uint32_t                             out_size)
{
    using namespace rerevved::presentation_text;
    if (!out)
    {
        return REREVVED_PRESENTATION_TEXT_ERR_INVALID_ARGUMENT;
    }
    ClearOutput(out, out_size);
    if (out_size < kEvaluationPrefix)
    {
        return REREVVED_PRESENTATION_TEXT_ERR_BUFFER_TOO_SMALL;
    }
    if (!query || query->struct_size < sizeof(*query))
    {
        return REREVVED_PRESENTATION_TEXT_ERR_INVALID_ARGUMENT;
    }
    ReRevvedPresentationTextEvaluation result{};
    if (!TryEvaluate(*query, result))
    {
        return REREVVED_PRESENTATION_TEXT_ERR_INVALID_ARGUMENT;
    }
    return CopyOutput(out, out_size, result);
}
