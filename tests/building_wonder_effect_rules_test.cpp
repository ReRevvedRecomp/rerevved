#include "building_effect_rules_registry.h"
#include "registry_scope.hpp"
#include "wonder_effect_rules_registry.h"

#include <building_effect_rules.h>
#include <wonder_effect_rules.h>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <thread>
#include <vector>

extern "C" int rerevved_building_wonder_effect_rules_c_header_check(void);

namespace
{

template <typename Rule>
Rule makeRule(const char* provider, const char* id, int32_t effect)
{
    Rule rule{};
    rule.structSize = sizeof(rule);
    std::snprintf(rule.providerId, sizeof(rule.providerId), "%s", provider);
    std::snprintf(rule.ruleId, sizeof(rule.ruleId), "%s", id);
    rule.effect = effect;
    return rule;
}

void testReadinessAndValidation()
{
    rerevved::building_effect_rules::ResetForTests();
    rerevved::wonder_effect_rules::ResetForTests();
    auto                  building = makeRule<BuildingEffectRule>("aeshur.chinese-library", "fixed-gold", BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD);
    auto                  wonder   = makeRule<WonderEffectRule>("aeshur.stonehenge", "religion-culture", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE);
    BuildingEffectSupport buildingSupport{};
    WonderEffectSupport   wonderSupport{};
    UNSCOPED_INFO("independent ABI versions");
    REQUIRE((BuildingEffectRulesAbiVersion() == BUILDING_EFFECT_RULES_ABI_VERSION && WonderEffectRulesAbiVersion() == WONDER_EFFECT_RULES_ABI_VERSION));
    UNSCOPED_INFO("Building runtime readiness is true");
    REQUIRE((GetBuildingEffectSupport(&buildingSupport, sizeof(buildingSupport)) == BUILDING_EFFECT_RULES_OK && buildingSupport.runtimeAvailable == 1));
    UNSCOPED_INFO("Wonder runtime readiness is true");
    REQUIRE((GetWonderEffectSupport(&wonderSupport, sizeof(wonderSupport)) == WONDER_EFFECT_RULES_OK && wonderSupport.runtimeAvailable == 1));
    UNSCOPED_INFO("valid Building registration is retained");
    REQUIRE(RegisterBuildingEffectRule(&building) == BUILDING_EFFECT_RULES_OK);
    UNSCOPED_INFO("valid Culture replacement registration stays unavailable");
    REQUIRE(RegisterWonderEffectRule(&wonder) == WONDER_EFFECT_RULES_ERR_UNAVAILABLE);
    UNSCOPED_INFO("only the valid Building registration was retained");
    REQUIRE(rerevved::building_effect_rules::RuleCount() == 1);
    UNSCOPED_INFO("the Culture replacement retained nothing");
    REQUIRE((rerevved::wonder_effect_rules::RuleCount() == 0 && !rerevved::wonder_effect_rules::IsStonehengeReligionActive()));
    UNSCOPED_INFO("null registration rejected");
    REQUIRE((RegisterBuildingEffectRule(nullptr) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && RegisterWonderEffectRule(nullptr) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));

    building.effect = 2;
    wonder.effect   = 3;
    UNSCOPED_INFO("unsupported effects rejected before readiness");
    REQUIRE((RegisterBuildingEffectRule(&building) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && RegisterWonderEffectRule(&wonder) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));
    wonder.effect = 0;
    UNSCOPED_INFO("Wonder effect 0 is invalid");
    REQUIRE(RegisterWonderEffectRule(&wonder) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    building.effect      = BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD;
    wonder.effect        = WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE;
    building.reserved[5] = 1;
    wonder.reserved[5]   = 1;
    UNSCOPED_INFO("reserved fields validated");
    REQUIRE((RegisterBuildingEffectRule(&building) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && RegisterWonderEffectRule(&wonder) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));
    building.reserved[5] = 0;
    wonder.reserved[5]   = 0;
    building.structSize  = sizeof(building) - 1;
    wonder.structSize    = sizeof(wonder) - 1;
    UNSCOPED_INFO("short rules rejected");
    REQUIRE((RegisterBuildingEffectRule(&building) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && RegisterWonderEffectRule(&wonder) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));
    building.structSize = sizeof(building);
    wonder.structSize   = sizeof(wonder);

    for (const char* invalid : { "", "Owner", ".owner", "a/b", "a b" })
    {
        std::snprintf(building.providerId, sizeof(building.providerId), "%s", invalid);
        std::snprintf(wonder.ruleId, sizeof(wonder.ruleId), "%s", invalid);
        UNSCOPED_INFO("invalid IDs rejected");
        REQUIRE((RegisterBuildingEffectRule(&building) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && RegisterWonderEffectRule(&wonder) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));
    }
    std::memset(building.providerId, 'a', sizeof(building.providerId));
    std::memset(wonder.ruleId, 'a', sizeof(wonder.ruleId));
    UNSCOPED_INFO("unterminated IDs rejected");
    REQUIRE((RegisterBuildingEffectRule(&building) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && RegisterWonderEffectRule(&wonder) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));
    UNSCOPED_INFO("rejected Building registrations retained nothing");
    REQUIRE(rerevved::building_effect_rules::RuleCount() == 1);
    UNSCOPED_INFO("rejected Wonder registrations retained nothing");
    REQUIRE(rerevved::wonder_effect_rules::RuleCount() == 0);
}

void testLibraryPreview()
{
    rerevved::building_effect_rules::ResetForTests();
    const auto                 rule = makeRule<BuildingEffectRule>("aeshur.chinese-library", "fixed-gold", BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD);
    BuildingEffectPreviewQuery query{ sizeof(query), CIVILIZATION_CHINESE, 1, 12, 18, {} };
    BuildingEffectPreview      result{};
    UNSCOPED_INFO("Library preview succeeds");
    REQUIRE(PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_OK);
    UNSCOPED_INFO("fixed addition after supplied modifiers preserves science");
    REQUIRE((result.nativeGold == 12 && result.finalGold == 13 && result.nativeScience == 18 && result.finalScience == 18 && result.statusFlags == BUILDING_EFFECT_PREVIEW_MATCHED));
    UNSCOPED_INFO("preview cannot activate a rule");
    REQUIRE(rerevved::building_effect_rules::RuleCount() == 0);

    for (int32_t civilization = CIVILIZATION_ROMAN; civilization < CIVILIZATION_COUNT; ++civilization)
    {
        query.civilization = civilization;
        UNSCOPED_INFO("civilization preview succeeds");
        REQUIRE(PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_OK);
        UNSCOPED_INFO("only current Chinese ownership selects the bonus");
        REQUIRE((result.finalGold == (civilization == CIVILIZATION_CHINESE ? 13 : 12) && result.finalScience == 18));
    }
    query.civilization = CIVILIZATION_CHINESE;
    query.hasLibrary   = 0;
    UNSCOPED_INFO("absence of a Library preserves supplied values");
    REQUIRE((PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_OK && result.finalGold == 12 && !result.statusFlags));
    query.hasLibrary = 1;
    query.nativeGold = std::numeric_limits<int16_t>::max();
    UNSCOPED_INFO("Gold does not wrap or clamp");
    REQUIRE((PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_OK && result.finalGold == query.nativeGold && (result.statusFlags & BUILDING_EFFECT_PREVIEW_OVERFLOW) && result.finalScience == 18));
    query.nativeGold = std::numeric_limits<int16_t>::max() - 1;
    UNSCOPED_INFO("largest representable addition succeeds");
    REQUIRE((PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_OK && result.finalGold == std::numeric_limits<int16_t>::max() && !(result.statusFlags & BUILDING_EFFECT_PREVIEW_OVERFLOW)));
    query.nativeGold = std::numeric_limits<int16_t>::min();
    UNSCOPED_INFO("negative native yield uses checked addition");
    REQUIRE((PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_OK && result.finalGold == query.nativeGold + 1));
    query.nativeGold = std::numeric_limits<int32_t>::max();
    UNSCOPED_INFO("unrepresentable inputs rejected and output cleared");
    REQUIRE((PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && !result.structSize));
    query.nativeGold = 12;
    query.hasLibrary = 2;
    UNSCOPED_INFO("presence is a strict boolean");
    REQUIRE(PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    query.hasLibrary   = 1;
    query.civilization = CIVILIZATION_UNKNOWN;
    UNSCOPED_INFO("unknown owner civilization rejected");
    REQUIRE(PreviewBuildingEffect(&rule, &query, &result, sizeof(result)) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT);
}

void testStonehengePreview()
{
    const auto               rule = makeRule<WonderEffectRule>("aeshur.stonehenge", "religion-culture", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE);
    WonderEffectPreviewQuery query{ sizeof(query), 12, 1, 1, 0, {} };
    WonderEffectPreview      result{};
    UNSCOPED_INFO("new completion previews the selected replacement");
    REQUIRE((PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_OK && result.finalCulture == 17 && result.grantReligion && result.replaceNativeEffects));
    query.religionOwned = 1;
    UNSCOPED_INFO("already-owned Religion has no grant intent");
    REQUIRE((PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_OK && result.finalCulture == 17 && !result.grantReligion));
    query.isNewCompletion = 0;
    query.religionOwned   = 0;
    UNSCOPED_INFO("existing Stonehenge has ongoing culture without a grant intent");
    REQUIRE((PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_OK && result.finalCulture == 17 && !result.grantReligion));
    query.hasStonehenge = 0;
    UNSCOPED_INFO("other cities are unchanged");
    REQUIRE((PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_OK && result.finalCulture == 12 && !result.grantReligion && !result.replaceNativeEffects && !result.statusFlags));
    query.isNewCompletion = 1;
    UNSCOPED_INFO("completion requires the completed Wonder");
    REQUIRE(PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    query.hasStonehenge              = 1;
    query.cultureBeforeFixedAddition = std::numeric_limits<int16_t>::max() - 4;
    UNSCOPED_INFO("overflow returns the supplied baseline with no action intent");
    REQUIRE((PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_OK && result.finalCulture == query.cultureBeforeFixedAddition && !result.grantReligion && !result.replaceNativeEffects && (result.statusFlags & WONDER_EFFECT_PREVIEW_OVERFLOW)));
    query.cultureBeforeFixedAddition = std::numeric_limits<int16_t>::max() - 5;
    UNSCOPED_INFO("largest representable culture addition succeeds");
    REQUIRE((PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_OK && result.finalCulture == std::numeric_limits<int16_t>::max() && result.grantReligion));
    query.cultureBeforeFixedAddition = 12;
    query.religionOwned              = 2;
    UNSCOPED_INFO("ownership is a strict boolean");
    REQUIRE(PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    query.religionOwned = 0;
    query.reserved[2]   = 1;
    UNSCOPED_INFO("query reserved fields validated");
    REQUIRE(PreviewWonderEffect(&rule, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("preview cannot activate Wonder replacement");
    REQUIRE(RegisterWonderEffectRule(&rule) == WONDER_EFFECT_RULES_ERR_UNAVAILABLE);
}

void testOutputBounds()
{
    struct BuildingGuard
    {
        BuildingEffectSupport output;
        uint32_t              tail;
    } building{ {}, 0xAABBCCDD };

    UNSCOPED_INFO("oversized Building output does not write unknown tail bytes");
    REQUIRE((GetBuildingEffectSupport(&building.output, sizeof(building)) == BUILDING_EFFECT_RULES_OK && building.tail == 0xAABBCCDD && building.output.structSize == sizeof(building.output)));
    std::memset(&building.output, 0xCC, sizeof(building.output));
    UNSCOPED_INFO("short support output rejected");
    REQUIRE(GetBuildingEffectSupport(&building.output, 7) == BUILDING_EFFECT_RULES_ERR_BUFFER_TOO_SMALL);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&building.output);
    for (size_t index = 0; index < sizeof(building.output); ++index)
    {
        UNSCOPED_INFO("short output clears only the advertised bytes");
        REQUIRE(bytes[index] == (index < 7 ? 0 : 0xCC));
    }
    UNSCOPED_INFO("null support outputs rejected");
    REQUIRE((GetBuildingEffectSupport(nullptr, 32) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && GetWonderEffectSupport(nullptr, 32) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));
    BuildingEffectPreview buildingPreview{};
    WonderEffectPreview   wonderPreview{};
    UNSCOPED_INFO("output size is checked before inputs");
    REQUIRE((PreviewBuildingEffect(nullptr, nullptr, &buildingPreview, 0) == BUILDING_EFFECT_RULES_ERR_BUFFER_TOO_SMALL && PreviewWonderEffect(nullptr, nullptr, &wonderPreview, 0) == WONDER_EFFECT_RULES_ERR_BUFFER_TOO_SMALL));
    UNSCOPED_INFO("null preview outputs rejected");
    REQUIRE((PreviewBuildingEffect(nullptr, nullptr, nullptr, 32) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && PreviewWonderEffect(nullptr, nullptr, nullptr, 32) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));
    UNSCOPED_INFO("null preview inputs rejected");
    REQUIRE((PreviewBuildingEffect(nullptr, nullptr, &buildingPreview, 32) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && PreviewWonderEffect(nullptr, nullptr, &wonderPreview, 32) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT));
}

void testV2Previews()
{
    rerevved::building_effect_rules::ResetForTests();
    auto       building       = makeRule<BuildingEffectRuleV2>("aeshur.chinese-library", "fixed-gold", BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD);
    auto       wonder         = makeRule<WonderEffectRuleV2>("aeshur.stonehenge", "religion-culture", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE);
    const auto legacyBuilding = makeRule<BuildingEffectRule>("aeshur.chinese-library", "fixed-gold", BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD);
    const auto legacyWonder   = makeRule<WonderEffectRule>("aeshur.stonehenge", "religion-culture", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE);
    building.amount           = 1;
    wonder.amount             = 5;
    for (int32_t baseline : { -32768, -12, 0, 12, 32762, 32766, 32767 })
    {
        for (int32_t civilization = CIVILIZATION_ROMAN; civilization < CIVILIZATION_COUNT; ++civilization)
        {
            for (uint32_t present : { 0u, 1u })
            {
                BuildingEffectPreviewQuery query{ sizeof(query), civilization, present, baseline, baseline, {} };
                BuildingEffectPreview      v1{}, v2{};
                UNSCOPED_INFO("Library V2 default complete preview equivalence");
                REQUIRE((PreviewBuildingEffect(&legacyBuilding, &query, &v1, sizeof(v1)) == 0 &&
                         PreviewBuildingEffectV2(&building, &query, &v2, sizeof(v2)) == 0 &&
                         std::memcmp(&v1, &v2, sizeof(v1)) == 0));
            }
        }
        for (uint32_t present : { 0u, 1u })
        {
            for (uint32_t completion : { 0u, 1u })
            {
                for (uint32_t owned : { 0u, 1u })
                {
                    WonderEffectPreviewQuery query{ sizeof(query), baseline, present, completion, owned, {} };
                    WonderEffectPreview      v1{}, v2{};
                    const auto               status = PreviewWonderEffect(&legacyWonder, &query, &v1, sizeof(v1));
                    UNSCOPED_INFO("Stonehenge V2 default complete preview equivalence");
                    REQUIRE((PreviewWonderEffectV2(&wonder, &query, &v2, sizeof(v2)) == status &&
                             std::memcmp(&v1, &v2, sizeof(v1)) == 0));
                }
            }
        }
    }
    for (int32_t amount : { 2, 0, -1, INT32_MIN, INT32_MAX })
    {
        building.amount = amount;
        rerevved::building_effect_rules::ResetForTests();
        int32_t effective = 0;
        UNSCOPED_INFO("full int32 Library amount retained");
        REQUIRE((RegisterBuildingEffectRuleV2(&building) == BUILDING_EFFECT_RULES_OK &&
                 rerevved::building_effect_rules::TryGetEffectiveAmount(effective) && effective == amount));
        for (int32_t baseline : { -32768, -12, 0, 12, 32767 })
        {
            for (int32_t civilization : { CIVILIZATION_CHINESE, CIVILIZATION_ROMAN })
            {
                for (uint32_t present : { 0u, 1u })
                {
                    BuildingEffectPreviewQuery query{ sizeof(query), civilization, present, baseline, 18, {} };
                    BuildingEffectPreview      result{};
                    const int64_t              sum      = static_cast<int64_t>(baseline) + amount;
                    const bool                 matched  = civilization == CIVILIZATION_CHINESE && present;
                    const bool                 overflow = matched && (sum < INT16_MIN || sum > INT16_MAX);
                    UNSCOPED_INFO("Library signed amounts preserve science, predicates and overflow passthrough");
                    REQUIRE((PreviewBuildingEffectV2(&building, &query, &result, sizeof(result)) == 0 &&
                             result.nativeGold == baseline && result.finalGold == (matched && !overflow ? sum : baseline) &&
                             result.nativeScience == 18 && result.finalScience == 18 &&
                             result.statusFlags == ((matched ? BUILDING_EFFECT_PREVIEW_MATCHED : 0u) |
                                                    (overflow ? BUILDING_EFFECT_PREVIEW_OVERFLOW : 0u))));
                }
            }
        }
    }
    for (int32_t amount : { 7, 0, -2, INT32_MIN, INT32_MAX })
    {
        wonder.amount = amount;
        UNSCOPED_INFO("full int32 Stonehenge amount accepted but not retained");
        REQUIRE(RegisterWonderEffectRuleV2(&wonder) == WONDER_EFFECT_RULES_ERR_UNAVAILABLE);
        for (int32_t baseline : { -32768, -12, 0, 12, 32767 })
        {
            for (uint32_t present : { 0u, 1u })
            {
                for (uint32_t completion : { 0u, 1u })
                {
                    if (completion && !present)
                        continue;
                    for (uint32_t owned : { 0u, 1u })
                    {
                        WonderEffectPreviewQuery query{ sizeof(query), baseline, present, completion, owned, {} };
                        WonderEffectPreview      result{};
                        const int64_t            sum      = static_cast<int64_t>(baseline) + amount;
                        const bool               overflow = present && (sum < INT16_MIN || sum > INT16_MAX);
                        UNSCOPED_INFO("Stonehenge signed amounts preserve predicates and overflow suppresses all action intent");
                        REQUIRE((PreviewWonderEffectV2(&wonder, &query, &result, sizeof(result)) == 0 &&
                                 result.cultureBeforeFixedAddition == baseline && result.finalCulture == (present && !overflow ? sum : baseline) &&
                                 result.replaceNativeEffects == (present && !overflow) &&
                                 result.grantReligion == (present && !overflow && completion && !owned) &&
                                 result.statusFlags == ((present ? WONDER_EFFECT_PREVIEW_MATCHED : 0u) |
                                                        (overflow ? WONDER_EFFECT_PREVIEW_OVERFLOW : 0u))));
                    }
                }
            }
        }
    }
}

// Building alone admits effect 1; both supports report live amount ranges.
template <typename Rule, typename Support, typename Query, typename Preview>
void testV2Validation(bool live, int32_t minAmount, int32_t maxAmount, int32_t (*support)(Support*, uint32_t), int32_t (*registration)(const Rule*), int32_t (*preview)(const Rule*, const Query*, Preview*, uint32_t), Query query)
{
    rerevved::building_effect_rules::ResetForTests();
    rerevved::wonder_effect_rules::ResetForTests();
    auto    rule = makeRule<Rule>("provider", "rule", 1);
    Support result;
    std::memset(&result, 0xCC, sizeof(result));
    UNSCOPED_INFO("V2 support range and availability");
    REQUIRE((support(&result, sizeof(result)) == 0 && result.structSize == 32 && result.runtimeAvailable == 1 &&
             result.minAmount == minAmount && result.maxAmount == maxAmount));
    for (const auto value : result.reserved)
    {
        UNSCOPED_INFO("support reserved zero");
        REQUIRE(value == 0);
    }
    UNSCOPED_INFO("V2 null support rejected");
    REQUIRE(support(nullptr, 32) == -10);

    struct ExtendedSupport
    {
        Support       record;
        unsigned char tail[8];
    } extended;

    std::memset(&extended, 0xCC, sizeof(extended));
    UNSCOPED_INFO("V2 extended support output");
    REQUIRE(support(&extended.record, sizeof(extended)) == 0);
    for (auto value : extended.tail)
    {
        UNSCOPED_INFO("V2 support leaves trailing bytes intact");
        REQUIRE(value == 0xCC);
    }
    std::memset(&result, 0xCC, sizeof(result));
    UNSCOPED_INFO("V2 short support rejected");
    REQUIRE(support(&result, 7) == -11);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&result);
    for (size_t i = 0; i < sizeof(result); ++i)
    {
        UNSCOPED_INFO("V2 support bounded clearing");
        REQUIRE(bytes[i] == (i < 7 ? 0 : 0xCC));
    }
    Preview output{};

    struct ExtendedPreview
    {
        Preview       record;
        unsigned char tail[8];
    } extendedPreview;

    std::memset(&extendedPreview, 0xCC, sizeof(extendedPreview));
    UNSCOPED_INFO("V2 extended preview output");
    REQUIRE(preview(&rule, &query, &extendedPreview.record, sizeof(extendedPreview)) == 0);
    for (auto value : extendedPreview.tail)
    {
        UNSCOPED_INFO("V2 preview leaves trailing bytes intact");
        REQUIRE(value == 0xCC);
    }
    std::memset(&output, 0xCC, sizeof(output));
    UNSCOPED_INFO("V2 short preview rejected");
    REQUIRE(preview(nullptr, nullptr, &output, 7) == -11);
    const auto* previewBytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t i = 0; i < sizeof(output); ++i)
    {
        UNSCOPED_INFO("V2 preview bounded clearing");
        REQUIRE(previewBytes[i] == (i < 7 ? 0 : 0xCC));
    }
    const auto invalidRule = [&](Rule bad)
    {
        UNSCOPED_INFO("V2 malformed rule rejected before unavailability");
        REQUIRE((registration(&bad) == -10 && preview(&bad, &query, &output, sizeof(output)) == -10 && !output.structSize));
    };
    UNSCOPED_INFO("V2 null rule rejected");
    REQUIRE((registration(nullptr) == -10 && preview(nullptr, &query, &output, sizeof(output)) == -10));
    auto bad = rule;
    bad.structSize--;
    invalidRule(bad);
    bad        = rule;
    bad.effect = 3;
    invalidRule(bad);
    for (size_t i = 0; i < 5; ++i)
    {
        bad             = rule;
        bad.reserved[i] = 1;
        invalidRule(bad);
    }
    for (const char* id : { "", "UPPER", ".start", "a/b", "a b" })
    {
        bad = rule;
        std::snprintf(bad.providerId, sizeof(bad.providerId), "%s", id);
        invalidRule(bad);
        bad = rule;
        std::snprintf(bad.ruleId, sizeof(bad.ruleId), "%s", id);
        invalidRule(bad);
    }
    bad = rule;
    std::memset(bad.providerId, 'a', sizeof(bad.providerId));
    invalidRule(bad);
    bad = rule;
    std::memset(bad.ruleId, 'a', sizeof(bad.ruleId));
    invalidRule(bad);
    rule.structSize += 4;
    UNSCOPED_INFO("V2 larger request accepted");
    REQUIRE(registration(&rule) == (live ? 0 : -14));
    for (int32_t amount : { 1, 5, 0, -1, INT32_MIN, INT32_MAX, 1 })
    {
        rule.amount = amount;
        if (live)
        {
            rerevved::building_effect_rules::ResetForTests();
            UNSCOPED_INFO("V2 registration and identical replay accepted");
            REQUIRE((registration(&rule) == 0 && registration(&rule) == 0));
        }
        else
        {
            UNSCOPED_INFO("V2 repeated and conflicting payloads retain no registration");
            REQUIRE(registration(&rule) == -14);
        }
    }
    UNSCOPED_INFO("V2 null query rejected");
    REQUIRE(preview(&rule, nullptr, &output, sizeof(output)) == -10);
    UNSCOPED_INFO("V2 null preview output rejected");
    REQUIRE(preview(nullptr, nullptr, nullptr, 32) == -10);
    UNSCOPED_INFO("V2 output size precedes input validation");
    REQUIRE(preview(nullptr, nullptr, &output, 0) == -11);
    auto badQuery = query;
    badQuery.structSize--;
    UNSCOPED_INFO("V2 short query rejected");
    REQUIRE(preview(&rule, &badQuery, &output, sizeof(output)) == -10);
    for (size_t i = 0; i < 3; ++i)
    {
        badQuery             = query;
        badQuery.reserved[i] = 1;
        UNSCOPED_INFO("V2 query reserved rejected");
        REQUIRE(preview(&rule, &badQuery, &output, sizeof(output)) == -10);
    }
}

void testV2MalformedQueries()
{
    const auto                       building = makeRule<BuildingEffectRuleV2>("provider", "rule", 1);
    const auto                       wonder   = makeRule<WonderEffectRuleV2>("provider", "rule", 1);
    const BuildingEffectPreviewQuery bq{ sizeof(bq), CIVILIZATION_CHINESE, 1, 12, 18, {} };
    const WonderEffectPreviewQuery   wq{ sizeof(wq), 12, 1, 1, 0, {} };
    testV2Validation(true, INT32_MIN, INT32_MAX, GetBuildingEffectSupportV2, RegisterBuildingEffectRuleV2, PreviewBuildingEffectV2, bq);
    testV2Validation(false, 0, 0, GetWonderEffectSupportV2, RegisterWonderEffectRuleV2, PreviewWonderEffectV2, wq);
    UNSCOPED_INFO("V2 Culture replacement retained nothing");
    REQUIRE(rerevved::wonder_effect_rules::RuleCount() == 0);
    BuildingEffectPreview bp{};
    WonderEffectPreview   wp{};
    for (int32_t value : { -32769, 32768, INT32_MIN, INT32_MAX })
    {
        auto b       = bq;
        b.nativeGold = value;
        UNSCOPED_INFO("V2 invalid Gold input");
        REQUIRE(PreviewBuildingEffectV2(&building, &b, &bp, sizeof(bp)) == -10);
        b               = bq;
        b.nativeScience = value;
        UNSCOPED_INFO("V2 invalid Science input");
        REQUIRE(PreviewBuildingEffectV2(&building, &b, &bp, sizeof(bp)) == -10);
        auto w                       = wq;
        w.cultureBeforeFixedAddition = value;
        UNSCOPED_INFO("V2 invalid Culture input");
        REQUIRE(PreviewWonderEffectV2(&wonder, &w, &wp, sizeof(wp)) == -10);
    }
    for (int32_t civilization : { CIVILIZATION_UNKNOWN, CIVILIZATION_COUNT })
    {
        auto b         = bq;
        b.civilization = civilization;
        UNSCOPED_INFO("V2 invalid civilization");
        REQUIRE(PreviewBuildingEffectV2(&building, &b, &bp, sizeof(bp)) == -10);
    }
    auto b       = bq;
    b.hasLibrary = 2;
    UNSCOPED_INFO("V2 Library boolean");
    REQUIRE(PreviewBuildingEffectV2(&building, &b, &bp, sizeof(bp)) == -10);
    for (auto member : { &WonderEffectPreviewQuery::hasStonehenge, &WonderEffectPreviewQuery::isNewCompletion, &WonderEffectPreviewQuery::religionOwned })
    {
        auto w    = wq;
        w.*member = 2;
        UNSCOPED_INFO("V2 Wonder boolean");
        REQUIRE(PreviewWonderEffectV2(&wonder, &w, &wp, sizeof(wp)) == -10);
    }
}

namespace registry = rerevved::building_effect_rules;

int32_t effectiveAmount()
{
    int32_t amount = 0;
    return registry::TryGetEffectiveAmount(amount) ? amount : INT32_MIN;
}

BuildingEffectRuleV2 libraryV2(const char* provider, const char* id, int32_t amount)
{
    auto rule   = makeRule<BuildingEffectRuleV2>(provider, id, BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD);
    rule.amount = amount;
    return rule;
}

int32_t registerAmount(const char* provider, const char* id, int32_t amount)
{
    const auto rule = libraryV2(provider, id, amount);
    return RegisterBuildingEffectRuleV2(&rule);
}

void setAmount(int32_t amount)
{
    registry::ResetForTests();
    UNSCOPED_INFO("hook amount registered");
    REQUIRE(registerAmount("aeshur.hook", "gold", amount) == BUILDING_EFFECT_RULES_OK);
}

void testSupport()
{
    BuildingEffectSupport   building{};
    BuildingEffectSupportV2 buildingV2{};
    WonderEffectSupport     wonder{};
    WonderEffectSupportV2   wonderV2{};
    UNSCOPED_INFO("Building V1 support is live");
    REQUIRE((GetBuildingEffectSupport(&building, sizeof(building)) == 0 && building.runtimeAvailable == 1));
    UNSCOPED_INFO("Building V2 support is live over the full range");
    REQUIRE((GetBuildingEffectSupportV2(&buildingV2, sizeof(buildingV2)) == 0 && buildingV2.runtimeAvailable == 1 &&
             buildingV2.minAmount == INT32_MIN && buildingV2.maxAmount == INT32_MAX));
    UNSCOPED_INFO("Wonder V1 support is live");
    REQUIRE((GetWonderEffectSupport(&wonder, sizeof(wonder)) == 0 && wonder.runtimeAvailable == 1));
    UNSCOPED_INFO("Wonder V2 support is live for amount 0 only");
    REQUIRE((GetWonderEffectSupportV2(&wonderV2, sizeof(wonderV2)) == 0 && wonderV2.runtimeAvailable == 1 &&
             wonderV2.minAmount == 0 && wonderV2.maxAmount == 0));
    const auto wonderRule   = makeRule<WonderEffectRule>("p", "r", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE);
    const auto wonderRuleV2 = makeRule<WonderEffectRuleV2>("p", "r", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE);
    UNSCOPED_INFO("Culture replacement registration stays unavailable");
    REQUIRE((RegisterWonderEffectRule(&wonderRule) == WONDER_EFFECT_RULES_ERR_UNAVAILABLE &&
             RegisterWonderEffectRuleV2(&wonderRuleV2) == WONDER_EFFECT_RULES_ERR_UNAVAILABLE));
}

void testRegistration()
{
    registry::ResetForTests();
    UNSCOPED_INFO("empty registry has no effective amount");
    REQUIRE((effectiveAmount() == INT32_MIN && registry::RuleCount() == 0));

    const auto v1 = makeRule<BuildingEffectRule>("aeshur.library", "gold", BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD);
    UNSCOPED_INFO("V1 registers amount 1");
    REQUIRE((RegisterBuildingEffectRule(&v1) == BUILDING_EFFECT_RULES_OK && registry::RuleCount() == 1 && effectiveAmount() == 1));
    UNSCOPED_INFO("V1 replay is idempotent");
    REQUIRE((RegisterBuildingEffectRule(&v1) == BUILDING_EFFECT_RULES_OK && registry::RuleCount() == 1 && effectiveAmount() == 1));
    UNSCOPED_INFO("V2 amount 1 is the V1 identity");
    REQUIRE((registerAmount("aeshur.library", "gold", 1) == BUILDING_EFFECT_RULES_OK && registry::RuleCount() == 1 && effectiveAmount() == 1));
    UNSCOPED_INFO("V2 amount 2 conflicts with the V1 identity and retains nothing");
    REQUIRE((registerAmount("aeshur.library", "gold", 2) == BUILDING_EFFECT_RULES_ERR_CONFLICT && registry::RuleCount() == 1 && effectiveAmount() == 1));
    static_assert(BUILDING_EFFECT_RULES_ERR_CONFLICT == -12);

    // A larger request with dirty ID tails normalizes to the retained record.
    auto dirty       = libraryV2("aeshur.library", "gold", 1);
    dirty.structSize = sizeof(dirty) + 8;
    std::memset(dirty.providerId + std::strlen(dirty.providerId) + 1, 'x', sizeof(dirty.providerId) - std::strlen(dirty.providerId) - 1);
    UNSCOPED_INFO("dirty tails normalize to the same identity");
    REQUIRE((RegisterBuildingEffectRuleV2(&dirty) == BUILDING_EFFECT_RULES_OK && registry::RuleCount() == 1));

    UNSCOPED_INFO("distinct provider or rule IDs compose additively");
    REQUIRE((registerAmount("aeshur.other", "gold", 4) == 0 && registerAmount("aeshur.library", "silver", -2) == 0 &&
             registry::RuleCount() == 3 && effectiveAmount() == 3));
    UNSCOPED_INFO("zero amounts are retained");
    REQUIRE((registerAmount("aeshur.zero", "gold", 0) == 0 && registry::RuleCount() == 4 && effectiveAmount() == 3));
    UNSCOPED_INFO("a later conflict retains nothing");
    REQUIRE((registerAmount("aeshur.other", "gold", 5) == BUILDING_EFFECT_RULES_ERR_CONFLICT && registry::RuleCount() == 4 && effectiveAmount() == 3));

    registry::ResetForTests();
    UNSCOPED_INFO("maximum amount registers");
    REQUIRE((registerAmount("a", "big", INT32_MAX) == 0 && effectiveAmount() == INT32_MAX));
    UNSCOPED_INFO("a sum above int32 rejects and retains nothing");
    REQUIRE((registerAmount("a", "one", 1) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && registry::RuleCount() == 1 && effectiveAmount() == INT32_MAX));
    const auto v1Extra = makeRule<BuildingEffectRule>("a", "v1", BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD);
    UNSCOPED_INFO("V1 amount 1 joins the sum check");
    REQUIRE((RegisterBuildingEffectRule(&v1Extra) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && registry::RuleCount() == 1));
    UNSCOPED_INFO("opposite extremes compose");
    REQUIRE((registerAmount("a", "min", INT32_MIN) == 0 && effectiveAmount() == -1));
    UNSCOPED_INFO("negative sums within range register");
    REQUIRE((registerAmount("a", "neg", -1) == 0 && effectiveAmount() == -2));
    UNSCOPED_INFO("a sum below int32 rejects");
    REQUIRE((registerAmount("a", "under", INT32_MIN) == BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT && effectiveAmount() == -2));
    registry::ResetForTests();
}

constexpr uint32_t kCity = 0x40001000u;

registry::CacheQuery chineseLibrary(int32_t mode, uint32_t stackPointer = 1000)
{
    return { stackPointer, kCity, mode, 3, 3, CIVILIZATION_CHINESE, true, false, 10, 100, 1000, 7 };
}

registry::CreditDecision credit(uint32_t stackPointer, int32_t reserveWithCredit, uint32_t city = kCity, int32_t owner = 3, bool excludedPath = false)
{
    return registry::EvaluateCreditPhase({ stackPointer, city, owner, owner, CIVILIZATION_CHINESE, true, excludedPath, reserveWithCredit });
}

bool isNoAction(const registry::CacheDecision& decision, registry::HookStatus status)
{
    return decision.status == status && !decision.applyCache && decision.newCache == 0 && !decision.applyAggregate && decision.newAggregate == 0;
}

bool hasNoToken(uint32_t stackPointer = 1000)
{
    const auto decision = credit(stackPointer, 1007);
    return decision.status == registry::HookStatus::kUnmatched && !decision.apply && decision.newReserveGold == 0;
}

void testHookArithmetic()
{
    registry::ResetForTests();
    UNSCOPED_INFO("no retained rule is native");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)), registry::HookStatus::kUnmatched));

    for (int32_t amount : { 3, -4 })
    {
        setAmount(amount);
        auto decision = registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit));
        UNSCOPED_INFO("mode 0 composes cache and aggregate");
        REQUIRE((decision.status == registry::HookStatus::kMatched && decision.applyCache && decision.newCache == 10 + amount &&
                 decision.applyAggregate && decision.newAggregate == 100 + amount));
        const auto credited = credit(1000, 1007);
        UNSCOPED_INFO("credit uses the recorded amount");
        REQUIRE((credited.status == registry::HookStatus::kMatched && credited.apply && credited.newReserveGold == 1007 + amount));
        UNSCOPED_INFO("the token is consumed once");
        REQUIRE(hasNoToken());

        decision = registry::EvaluateCachePhase(chineseLibrary(registry::kModeCacheOnly));
        UNSCOPED_INFO("mode -1 composes the cache only");
        REQUIRE((decision.status == registry::HookStatus::kMatched && decision.applyCache && decision.newCache == 10 + amount &&
                 !decision.applyAggregate && decision.newAggregate == 0));
        UNSCOPED_INFO("mode -1 records no token");
        REQUIRE(hasNoToken());
    }

    setAmount(0);
    UNSCOPED_INFO("zero amount is native");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)), registry::HookStatus::kUnmatched));
    UNSCOPED_INFO("zero amount records no token");
    REQUIRE(hasNoToken());

    // Each limit accepts its exact boundary and falls back one step beyond it, with no writes and no token.
    setAmount(1);
    auto query        = chineseLibrary(registry::kModeCredit);
    query.nativeCache = INT16_MAX - 1;
    UNSCOPED_INFO("largest cache composes");
    REQUIRE(registry::EvaluateCachePhase(query).newCache == INT16_MAX);
    UNSCOPED_INFO("the boundary cache recorded a token");
    REQUIRE(!hasNoToken());
    for (auto mode : { registry::kModeCacheOnly, registry::kModeCredit })
    {
        query             = chineseLibrary(mode);
        query.nativeCache = INT16_MAX;
        UNSCOPED_INFO("int16 cache overflow falls back");
        REQUIRE(isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kOverflow));
        UNSCOPED_INFO("int16 fallback records no token");
        REQUIRE(hasNoToken());
    }
    setAmount(-1);
    query             = chineseLibrary(registry::kModeCacheOnly);
    query.nativeCache = INT16_MIN + 1;
    UNSCOPED_INFO("smallest cache composes");
    REQUIRE(registry::EvaluateCachePhase(query).newCache == INT16_MIN);
    query.nativeCache = INT16_MIN;
    UNSCOPED_INFO("negative int16 cache overflow falls back");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kOverflow));
    setAmount(40000);
    UNSCOPED_INFO("an amount beyond int16 never writes the cache");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCacheOnly)), registry::HookStatus::kOverflow));

    setAmount(1);
    query                = chineseLibrary(registry::kModeCredit);
    query.ownerAggregate = INT32_MAX - 1;
    UNSCOPED_INFO("largest aggregate composes");
    REQUIRE(registry::EvaluateCachePhase(query).newAggregate == INT32_MAX);
    UNSCOPED_INFO("the boundary aggregate recorded a token");
    REQUIRE(!hasNoToken());
    query.ownerAggregate = INT32_MAX;
    UNSCOPED_INFO("int32 aggregate overflow rejects the cache write too");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kOverflow));
    UNSCOPED_INFO("aggregate fallback records no token");
    REQUIRE(hasNoToken());
    query.mode = registry::kModeCacheOnly;
    UNSCOPED_INFO("mode -1 ignores the aggregate");
    REQUIRE(registry::EvaluateCachePhase(query).status == registry::HookStatus::kMatched);
    setAmount(-1);
    query                = chineseLibrary(registry::kModeCredit);
    query.ownerAggregate = INT32_MIN;
    UNSCOPED_INFO("negative int32 aggregate overflow falls back");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kOverflow));

    setAmount(1);
    query              = chineseLibrary(registry::kModeCredit);
    query.ownerReserve = INT32_MAX - 10;
    query.nativeCredit = 9;
    UNSCOPED_INFO("largest reserve credit composes");
    REQUIRE(registry::EvaluateCachePhase(query).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("the boundary reserve recorded a token");
    REQUIRE(!hasNoToken());
    query.nativeCredit = 10;
    UNSCOPED_INFO("int32 reserve overflow falls back");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kOverflow));
    UNSCOPED_INFO("reserve fallback records no token");
    REQUIRE(hasNoToken());
    query.mode = registry::kModeCacheOnly;
    UNSCOPED_INFO("mode -1 ignores the reserve");
    REQUIRE(registry::EvaluateCachePhase(query).status == registry::HookStatus::kMatched);
    query              = chineseLibrary(registry::kModeCredit);
    query.ownerReserve = INT32_MAX;
    query.nativeCredit = -5;
    UNSCOPED_INFO("reserve check widens before summing");
    REQUIRE(registry::EvaluateCachePhase(query).status == registry::HookStatus::kMatched);
    setAmount(-1);
    query              = chineseLibrary(registry::kModeCredit);
    query.ownerReserve = INT32_MIN;
    query.nativeCredit = 0;
    UNSCOPED_INFO("negative int32 reserve overflow falls back");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kOverflow));
    // A wrapped native sum would reach the credit phase as a different 32-bit value.
    query              = chineseLibrary(registry::kModeCredit);
    query.ownerReserve = INT32_MAX;
    query.nativeCredit = 1;
    UNSCOPED_INFO("a wrapping native reserve sum falls back");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kOverflow));
    UNSCOPED_INFO("native reserve wrap records no token");
    REQUIRE(hasNoToken());

    // The credit phase repeats the reserve check against the value actually being stored.
    setAmount(3);
    UNSCOPED_INFO("credit boundary cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    auto credited = credit(1000, INT32_MAX - 3);
    UNSCOPED_INFO("largest credit composes");
    REQUIRE((credited.status == registry::HookStatus::kMatched && credited.apply && credited.newReserveGold == INT32_MAX));
    UNSCOPED_INFO("credit overflow cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    credited = credit(1000, INT32_MAX - 2);
    UNSCOPED_INFO("credit overflow falls back");
    REQUIRE((credited.status == registry::HookStatus::kOverflow && !credited.apply && credited.newReserveGold == 0));
    UNSCOPED_INFO("a rechecked credit consumes its token");
    REQUIRE(hasNoToken());
    registry::ResetForTests();
}

void testHookUnmatched()
{
    setAmount(3);
    const auto unmatched = [](registry::CacheQuery query, const char* message)
    {
        UNSCOPED_INFO(message);
        REQUIRE((isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kUnmatched) && hasNoToken(query.stackPointer)));
    };
    for (auto mode : { registry::kModeCacheOnly, registry::kModeCredit })
    {
        for (int32_t civilization = CIVILIZATION_UNKNOWN; civilization < CIVILIZATION_COUNT; ++civilization)
        {
            auto query         = chineseLibrary(mode);
            query.civilization = civilization;
            if (civilization == CIVILIZATION_CHINESE)
            {
                UNSCOPED_INFO("Chinese owner matches");
                REQUIRE(registry::EvaluateCachePhase(query).status == registry::HookStatus::kMatched);
                setAmount(3);
                continue;
            }
            unmatched(query, "non-Chinese civilization is native");
        }
        auto query       = chineseLibrary(mode);
        query.hasLibrary = false;
        unmatched(query, "a city without a Library is native");
        query           = chineseLibrary(mode);
        query.cityOwner = 4;
        unmatched(query, "a city owned by another player is native");
        for (int32_t owner : { -1, registry::kPlayerCount, registry::kPlayerCount + 1, INT32_MIN, INT32_MAX })
        {
            query            = chineseLibrary(mode);
            query.ownerIndex = owner;
            query.cityOwner  = owner;
            unmatched(query, "an owner outside the slot range is native");
        }
        query            = chineseLibrary(mode);
        query.ownerIndex = 0;
        query.cityOwner  = 0;
        UNSCOPED_INFO("owner 0 matches");
        REQUIRE(registry::EvaluateCachePhase(query).status == registry::HookStatus::kMatched);
        query            = chineseLibrary(mode);
        query.ownerIndex = registry::kPlayerCount - 1;
        query.cityOwner  = registry::kPlayerCount - 1;
        UNSCOPED_INFO("the last owner slot matches");
        REQUIRE(registry::EvaluateCachePhase(query).status == registry::HookStatus::kMatched);
        setAmount(3);
    }
    for (int32_t mode : { -2, 1, 2, INT32_MIN, INT32_MAX })
    {
        unmatched(chineseLibrary(mode), "other modes are native");
    }
    static_assert(registry::kPlayerCount == 6);
    for (auto mode : { registry::kModeCacheOnly, registry::kModeCredit })
    {
        auto query         = chineseLibrary(mode);
        query.excludedPath = true;
        unmatched(query, "the native excluded path adds nothing");
    }
}

void testExcludedPath()
{
    // Cache phase on the excluded path records no token and discards a stale same-frame one.
    setAmount(3);
    UNSCOPED_INFO("excluded setup cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    auto query         = chineseLibrary(registry::kModeCredit);
    query.excludedPath = true;
    UNSCOPED_INFO("excluded cache phase is native");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(query), registry::HookStatus::kUnmatched));
    UNSCOPED_INFO("excluded cache phase leaves no token");
    REQUIRE(hasNoToken());

    // Credit phase on the excluded path changes nothing and still consumes the token.
    UNSCOPED_INFO("excluded credit setup cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    const auto excluded = credit(1000, 50, kCity, 3, true);
    UNSCOPED_INFO("excluded credit phase is native");
    REQUIRE((excluded.status == registry::HookStatus::kUnmatched && !excluded.apply && excluded.newReserveGold == 0));
    UNSCOPED_INFO("excluded credit phase consumed the token");
    REQUIRE(hasNoToken());

    // The credit phase re-gates on live state; a failed gate still consumes the token.
    for (int variant = 0; variant < 4; ++variant)
    {
        UNSCOPED_INFO("re-gate setup cache");
        REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
        registry::CreditQuery live{ 1000, kCity, 3, 3, CIVILIZATION_CHINESE, true, false, 50 };
        live.cityOwner    = variant == 0 ? 4 : live.cityOwner;
        live.civilization = variant == 1 ? CIVILIZATION_ROMAN : live.civilization;
        live.hasLibrary   = variant != 2;
        live.ownerIndex   = variant == 3 ? 7 : live.ownerIndex;
        const auto result = registry::EvaluateCreditPhase(live);
        UNSCOPED_INFO("credit re-gate failure changes nothing");
        REQUIRE((result.status == registry::HookStatus::kUnmatched && !result.apply));
        UNSCOPED_INFO("credit re-gate failure consumed the token");
        REQUIRE(hasNoToken());
    }
    registry::ResetForTests();
}

void testPendingTokens()
{
    // A deeper dead frame is discarded when a shallower traversal starts.
    setAmount(3);
    UNSCOPED_INFO("deep frame cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit, 900)).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("shallow frame cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit, 1000)).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("the deeper token was discarded");
    REQUIRE(hasNoToken(900));
    UNSCOPED_INFO("the same-frame token survived");
    REQUIRE(credit(1000, 50).newReserveGold == 53);

    // A same-frame token left by a finished traversal is replaced, even by an unmatched call.
    UNSCOPED_INFO("stale frame cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    auto roman         = chineseLibrary(registry::kModeCredit);
    roman.civilization = CIVILIZATION_ROMAN;
    UNSCOPED_INFO("later traversal is unmatched");
    REQUIRE(registry::EvaluateCachePhase(roman).status == registry::HookStatus::kUnmatched);
    UNSCOPED_INFO("a stale same-frame token is not carried over");
    REQUIRE(hasNoToken());

    // The credit phase drops deeper tokens and keeps shallower ones, and each uses its own recorded amount.
    setAmount(3);
    UNSCOPED_INFO("outer cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit, 2000)).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("second provider");
    REQUIRE(registerAmount("aeshur.second", "gold", 4) == 0);
    UNSCOPED_INFO("inner cache uses the current sum");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit, 1500)).newCache == 17);
    UNSCOPED_INFO("innermost cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit, 1000)).newCache == 17);
    UNSCOPED_INFO("the inner credit drops the deeper token");
    REQUIRE(credit(1500, 100).newReserveGold == 107);
    UNSCOPED_INFO("the deeper token is gone");
    REQUIRE(hasNoToken(1000));
    registry::ResetForTests();
    UNSCOPED_INFO("reset clears tokens");
    REQUIRE(credit(2000, 100).status == registry::HookStatus::kUnmatched);

    setAmount(3);
    UNSCOPED_INFO("outer cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit, 2000)).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("second provider");
    REQUIRE(registerAmount("aeshur.second", "gold", 4) == 0);
    UNSCOPED_INFO("the outer token keeps the amount from its cache phase");
    REQUIRE(credit(2000, 100).newReserveGold == 103);

    // The city record and owner must match, and a credit with no cache phase does nothing.
    setAmount(3);
    UNSCOPED_INFO("credit without a cache phase does nothing");
    REQUIRE(hasNoToken());
    UNSCOPED_INFO("city mismatch cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("another city does not consume the token");
    REQUIRE(credit(1000, 50, kCity + 4).status == registry::HookStatus::kUnmatched);
    UNSCOPED_INFO("owner mismatch cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("another owner does not consume the token");
    REQUIRE(credit(1000, 50, kCity, 4).status == registry::HookStatus::kUnmatched);
    UNSCOPED_INFO("frame mismatch cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("a deeper frame cannot consume the token");
    REQUIRE(credit(996, 50).status == registry::HookStatus::kUnmatched);
    UNSCOPED_INFO("a shallower frame cannot consume the token");
    REQUIRE(credit(1004, 50).status == registry::HookStatus::kUnmatched);

    // Capacity exhaustion applies nothing, including the cache write.
    setAmount(3);
    uint32_t frame = 9000;
    for (size_t i = 0; i < registry::kPendingTokenCapacity; ++i, frame -= 1000)
    {
        UNSCOPED_INFO("tokens fill the capacity");
        REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit, frame)).status == registry::HookStatus::kMatched);
    }
    UNSCOPED_INFO("a full stack falls back without writes");
    REQUIRE(isNoAction(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit, frame)), registry::HookStatus::kOverflow));
    UNSCOPED_INFO("the failed cache phase recorded no token");
    REQUIRE(hasNoToken(frame));
    UNSCOPED_INFO("mode -1 needs no token");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCacheOnly, frame)).status == registry::HookStatus::kMatched);
    UNSCOPED_INFO("the oldest tokens remain usable");
    REQUIRE(credit(frame + 1000, 20).newReserveGold == 23);
    registry::ResetForTests();
}

void testPendingTokensAreThreadLocal()
{
    setAmount(3);
    UNSCOPED_INFO("main thread cache");
    REQUIRE(registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched);
    // Assertions stay on this thread: the worker only records what it saw.
    bool    otherSawToken = true;
    bool    otherCached   = false;
    int32_t otherGold     = 0;
    std::thread([&]
                {
                    otherSawToken = !hasNoToken();
                    otherCached   = registry::EvaluateCachePhase(chineseLibrary(registry::kModeCredit)).status == registry::HookStatus::kMatched;
                    otherGold     = credit(1000, 0).newReserveGold;
                })
        .join();
    UNSCOPED_INFO("other thread cache");
    REQUIRE(otherCached);
    UNSCOPED_INFO("other thread consumed its own token");
    REQUIRE(otherGold == 3);
    UNSCOPED_INFO("a token does not cross threads");
    REQUIRE(!otherSawToken);
    UNSCOPED_INFO("the main thread kept its token");
    REQUIRE(credit(1000, 0).newReserveGold == 3);
    registry::ResetForTests();
}

namespace wonder = rerevved::wonder_effect_rules;

WonderEffectRuleV2 religionV2(const char* provider, const char* id, int32_t amount = 0)
{
    auto rule   = makeRule<WonderEffectRuleV2>(provider, id, WONDER_EFFECT_STONEHENGE_RELIGION);
    rule.amount = amount;
    return rule;
}

void testWonderRegistration()
{
    wonder::ResetForTests();
    UNSCOPED_INFO("empty Wonder registry is inactive");
    REQUIRE((wonder::RuleCount() == 0 && !wonder::IsStonehengeReligionActive()));

    const auto v1 = makeRule<WonderEffectRule>("aeshur.stonehenge", "religion", WONDER_EFFECT_STONEHENGE_RELIGION);
    UNSCOPED_INFO("V1 Religion grant registers");
    REQUIRE((RegisterWonderEffectRule(&v1) == WONDER_EFFECT_RULES_OK && wonder::RuleCount() == 1 && wonder::IsStonehengeReligionActive()));
    UNSCOPED_INFO("V1 replay is idempotent");
    REQUIRE((RegisterWonderEffectRule(&v1) == WONDER_EFFECT_RULES_OK && wonder::RuleCount() == 1));
    auto v2 = religionV2("aeshur.stonehenge", "religion");
    UNSCOPED_INFO("V2 amount 0 is the V1 identity");
    REQUIRE((RegisterWonderEffectRuleV2(&v2) == WONDER_EFFECT_RULES_OK && wonder::RuleCount() == 1));

    auto dirty       = v2;
    dirty.structSize = sizeof(dirty) + 8;
    std::memset(dirty.ruleId + std::strlen(dirty.ruleId) + 1, 'x', sizeof(dirty.ruleId) - std::strlen(dirty.ruleId) - 1);
    UNSCOPED_INFO("dirty tails normalize to the same identity");
    REQUIRE((RegisterWonderEffectRuleV2(&dirty) == WONDER_EFFECT_RULES_OK && wonder::RuleCount() == 1));

    for (int32_t amount : { 1, -1, 5, INT32_MIN, INT32_MAX })
    {
        v2.amount = amount;
        UNSCOPED_INFO("V2 Religion grant requires amount 0");
        REQUIRE((RegisterWonderEffectRuleV2(&v2) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT && wonder::RuleCount() == 1));
    }

    for (const char* provider : { "aeshur.stonehenge", "other" })
    {
        const auto culture   = makeRule<WonderEffectRule>(provider, "religion", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE);
        auto       cultureV2 = makeRule<WonderEffectRuleV2>(provider, "religion", WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE);
        cultureV2.amount     = 5;
        UNSCOPED_INFO("the Culture replacement stays unavailable, even under a retained identity");
        REQUIRE((RegisterWonderEffectRule(&culture) == WONDER_EFFECT_RULES_ERR_UNAVAILABLE &&
                 RegisterWonderEffectRuleV2(&cultureV2) == WONDER_EFFECT_RULES_ERR_UNAVAILABLE && wonder::RuleCount() == 1));
    }

    const auto other = religionV2("aeshur.other", "religion");
    UNSCOPED_INFO("distinct identities are retained");
    REQUIRE((RegisterWonderEffectRuleV2(&other) == WONDER_EFFECT_RULES_OK && wonder::RuleCount() == 2));

    // Every valid public request for one identity normalizes identically, so a
    // conflict is checked directly against the candidate builder.
    {
        auto                            lock = wonder::AcquirePublicationLock();
        std::vector<WonderEffectRuleV2> candidate;
        size_t                          offset = 99;
        const WonderEffectRuleV2        requests[]{ religionV2("aeshur.fresh", "religion"), religionV2("aeshur.other", "religion", 7) };
        UNSCOPED_INFO("a changed request under a retained identity conflicts");
        REQUIRE((wonder::BuildCandidate(requests, candidate, offset) == WONDER_EFFECT_RULES_ERR_CONFLICT && offset == 1));
    }
    static_assert(WONDER_EFFECT_RULES_ERR_CONFLICT == -12 && WONDER_EFFECT_RULES_ERR_INTERNAL == -13);
    UNSCOPED_INFO("a conflict leaves the registry unchanged");
    REQUIRE((wonder::RuleCount() == 2 && wonder::IsStonehengeReligionActive()));

    wonder::ResetForTests();
    UNSCOPED_INFO("reset deactivates the grant");
    REQUIRE((wonder::RuleCount() == 0 && !wonder::IsStonehengeReligionActive()));

    std::atomic<int>         concurrentFailures{ 0 };
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread)
    {
        threads.emplace_back([thread, &concurrentFailures]
                             {
                                 for (int index = 0; index < 25; ++index)
                                 {
                                     char provider[32];
                                     std::snprintf(provider, sizeof(provider), "p%d-%d", thread, index);
                                     const auto rule = religionV2(provider, "religion");
                                     if (RegisterWonderEffectRuleV2(&rule) != WONDER_EFFECT_RULES_OK || RegisterWonderEffectRuleV2(&rule) != WONDER_EFFECT_RULES_OK)
                                     {
                                         ++concurrentFailures;
                                     }
                                 }
                             });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    UNSCOPED_INFO("concurrent registration");
    REQUIRE(concurrentFailures.load() == 0);
    UNSCOPED_INFO("concurrent registrations are all retained once");
    REQUIRE((wonder::RuleCount() == 100 && wonder::IsStonehengeReligionActive()));
    wonder::ResetForTests();
}

void testReligionPreview()
{
    wonder::ResetForTests();
    const auto v1 = makeRule<WonderEffectRule>("aeshur.stonehenge", "religion", WONDER_EFFECT_STONEHENGE_RELIGION);
    const auto v2 = religionV2("aeshur.stonehenge", "religion");
    for (int32_t baseline : { -32768, -12, 0, 12, 32767 })
    {
        for (uint32_t present : { 0u, 1u })
        {
            for (uint32_t completion : { 0u, 1u })
            {
                if (completion && !present)
                    continue;
                for (uint32_t owned : { 0u, 1u })
                {
                    WonderEffectPreviewQuery query{ sizeof(query), baseline, present, completion, owned, {} };
                    WonderEffectPreview      first{}, second{};
                    UNSCOPED_INFO("Religion V1 and V2 previews match");
                    REQUIRE((PreviewWonderEffect(&v1, &query, &first, sizeof(first)) == WONDER_EFFECT_RULES_OK &&
                             PreviewWonderEffectV2(&v2, &query, &second, sizeof(second)) == WONDER_EFFECT_RULES_OK &&
                             std::memcmp(&first, &second, sizeof(first)) == 0));
                    UNSCOPED_INFO("Religion preview keeps native Culture and grants only on new completion without Religion");
                    REQUIRE((first.structSize == sizeof(first) && first.cultureBeforeFixedAddition == baseline && first.finalCulture == baseline &&
                             !first.replaceNativeEffects && first.statusFlags == (present ? WONDER_EFFECT_PREVIEW_MATCHED : 0u) &&
                             first.grantReligion == (present && completion && !owned)));
                }
            }
        }
    }
    WonderEffectPreviewQuery query{ sizeof(query), 12, 1, 1, 0, {} };
    WonderEffectPreview      result{};
    for (int32_t amount : { 1, -1, INT32_MIN, INT32_MAX })
    {
        const auto bad = religionV2("aeshur.stonehenge", "religion", amount);
        std::memset(&result, 0xCC, sizeof(result));
        UNSCOPED_INFO("V2 Religion preview requires amount 0");
        REQUIRE((PreviewWonderEffectV2(&bad, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT && !result.structSize));
    }
    query.hasStonehenge = 0;
    UNSCOPED_INFO("completion requires the completed Wonder");
    REQUIRE(PreviewWonderEffect(&v1, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    query.hasStonehenge = 1;
    query.religionOwned = 2;
    UNSCOPED_INFO("ownership is a strict boolean");
    REQUIRE(PreviewWonderEffectV2(&v2, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    query.religionOwned              = 0;
    query.cultureBeforeFixedAddition = 32768;
    UNSCOPED_INFO("Culture input keeps the signed 16-bit range");
    REQUIRE(PreviewWonderEffect(&v1, &query, &result, sizeof(result)) == WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("preview cannot activate the grant");
    REQUIRE((wonder::RuleCount() == 0 && !wonder::IsStonehengeReligionActive()));
}

// Owner 2 claims Stonehenge in city 10 among six players; player 7 is excluded.
wonder::GrantQuery grantQuery()
{
    return { wonder::kStonehengeWonderIndex, 10, 2, 10, 2, 6, 7, true, true };
}

void expectGrant(bool expected, void (*edit)(wonder::GrantQuery&), const char* message)
{
    auto query = grantQuery();
    edit(query);
    UNSCOPED_INFO(message);
    REQUIRE(wonder::ShouldGrantReligion(query) == expected);
}

void testGrantDecision()
{
    UNSCOPED_INFO("a consistent new claim grants");
    REQUIRE(wonder::ShouldGrantReligion(grantQuery()));
    expectGrant(false, [](auto& q)
                {
                    q.ruleActive = false;
                },
                "no retained rule is native");
    expectGrant(false, [](auto& q)
                {
                    q.readsOk = false;
                },
                "a failed guest read is native");
    expectGrant(false, [](auto& q)
                {
                    q.wonderIndex = 2;
                },
                "another Wonder is native");
    expectGrant(false, [](auto& q)
                {
                    q.wonderIndex = 8;
                },
                "the Oxford index is native");
    expectGrant(false, [](auto& q)
                {
                    q.cityIndex = q.claimCity = -1;
                },
                "a negative city is native");
    expectGrant(false, [](auto& q)
                {
                    q.cityIndex = q.claimCity = wonder::kCitySlots;
                },
                "a city past the slots is native");
    expectGrant(true, [](auto& q)
                {
                    q.cityIndex = q.claimCity = 0;
                },
                "city 0 grants");
    expectGrant(true, [](auto& q)
                {
                    q.cityIndex = q.claimCity = wonder::kCitySlots - 1;
                },
                "the last city slot grants");
    expectGrant(false, [](auto& q)
                {
                    q.claimCity = 11;
                },
                "a claim for another city is native");
    expectGrant(false, [](auto& q)
                {
                    q.liveOwner = 3;
                },
                "a live owner that differs from the claim is native");
    expectGrant(false, [](auto& q)
                {
                    q.claimOwner = q.liveOwner = -1;
                },
                "an unclaimed slot is native");
    expectGrant(true, [](auto& q)
                {
                    q.claimOwner = q.liveOwner = 0;
                },
                "owner 0 grants");
    expectGrant(true, [](auto& q)
                {
                    q.claimOwner = q.liveOwner = 5;
                },
                "the last ordinary player grants");
    expectGrant(false, [](auto& q)
                {
                    q.claimOwner = q.liveOwner = 6;
                },
                "an owner at the player count is native");
    expectGrant(false, [](auto& q)
                {
                    q.playerCount = 0;
                },
                "an empty player count is native");
    expectGrant(false, [](auto& q)
                {
                    q.playerCount = -1;
                },
                "a negative player count is native");
    expectGrant(true, [](auto& q)
                {
                    q.playerCount = 40;
                    q.claimOwner = q.liveOwner = wonder::kOwnerLimit - 1;
                },
                "owner 31 grants");
    expectGrant(false, [](auto& q)
                {
                    q.playerCount = 40;
                    q.claimOwner = q.liveOwner = wonder::kOwnerLimit;
                },
                "an owner beyond the 32-bit ownership word is native");
    expectGrant(false, [](auto& q)
                {
                    q.excludedPlayer = 2;
                },
                "the excluded player is native");
    expectGrant(true, [](auto& q)
                {
                    q.excludedPlayer = -1;
                },
                "an unused exclusion grants");
}

} // namespace

TEST_CASE("building_wonder_effect_rules_contract", "[ctest][building-wonder-effect-rules][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    static_assert(offsetof(BuildingEffectRule, effect) == 132);
    static_assert(offsetof(WonderEffectRule, effect) == 132);
    static_assert(offsetof(BuildingEffectPreview, statusFlags) == 20);
    static_assert(offsetof(WonderEffectPreview, statusFlags) == 20);
    testReadinessAndValidation();
    testLibraryPreview();
    testStonehengePreview();
    testOutputBounds();
    UNSCOPED_INFO("independent V1 and V2 versions");
    REQUIRE((BuildingEffectRulesAbiVersion() == 1 && WonderEffectRulesAbiVersion() == 1 &&
             BuildingEffectRulesV2AbiVersion() == 2 && WonderEffectRulesV2AbiVersion() == 2));
    testV2Previews();
    testV2MalformedQueries();
    testSupport();
    testRegistration();
    testHookArithmetic();
    testHookUnmatched();
    testExcludedPath();
    testPendingTokens();
    testPendingTokensAreThreadLocal();
    testWonderRegistration();
    testReligionPreview();
    testGrantDecision();
}

TEST_CASE("building_wonder_effect_rules_c_header", "[ctest][building-wonder-effect-rules][local-fast]")
{
    const int status = rerevved_building_wonder_effect_rules_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}
