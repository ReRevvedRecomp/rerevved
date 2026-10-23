#include "nation_select_text_registry.h"
#include "registry_scope.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <string_view>

extern "C" int rerevved_nation_select_text_c_header_check(void);

namespace
{

NationSelectTextRule makeFieldRule(
    const char*             provider,
    const char*             ruleId,
    NationSelectTextSurface surface,
    CivilizationId          civilization,
    UnlockEra               unlockEra,
    EraAbilityId            ability,
    UnitTypeId              baseUnitType,
    UnitIdentityId          identity,
    UnitDisplayForm         displayForm,
    const char*             text)
{
    NationSelectTextRule rule{};
    rule.structSize   = sizeof(rule);
    rule.surface      = surface;
    rule.civilization = civilization;
    rule.unlockEra    = unlockEra;
    rule.ability      = ability;
    rule.baseUnitType = baseUnitType;
    rule.identity     = identity;
    rule.displayForm  = displayForm;
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    std::memcpy(rule.text, text, std::strlen(text) + 1);
    return rule;
}

NationSelectTextRule makeEraRule(const char* provider,
                                 const char* ruleId,
                                 const char* text)
{
    NationSelectTextRule rule{};
    rule.structSize   = sizeof(rule);
    rule.surface      = NATION_SELECT_TEXT_SURFACE_ERA_ABILITY;
    rule.civilization = CIVILIZATION_MONGOLIAN;
    rule.unlockEra    = UNLOCK_ERA_ANCIENT;
    rule.ability =
        ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
    rule.baseUnitType = NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.identity     = NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.displayForm  = NATION_SELECT_TEXT_SELECTOR_UNUSED;
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    std::memcpy(rule.text, text, std::strlen(text) + 1);
    return rule;
}

NationSelectTextRule makeUnitRule(const char* provider,
                                  const char* ruleId,
                                  const char* text)
{
    NationSelectTextRule rule{};
    rule.structSize   = sizeof(rule);
    rule.surface      = NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT;
    rule.civilization = CIVILIZATION_MONGOLIAN;
    rule.unlockEra    = NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.ability      = 0;
    rule.baseUnitType = UNIT_TYPE_HORSEMEN;
    rule.identity     = UNIT_IDENTITY_KESHIK;
    rule.displayForm  = UNIT_DISPLAY_FORM_UNIT;
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    std::memcpy(rule.text, text, std::strlen(text) + 1);
    return rule;
}

NationSelectTextEvaluation evaluate(
    const NationSelectTextRule& rule)
{
    const NationSelectTextQuery query = {
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
    NationSelectTextEvaluation evaluation{};
    UNSCOPED_INFO("presentation evaluation failed");
    REQUIRE(EvaluateNationSelectText(
                &query, &evaluation, sizeof(evaluation)) ==
            NATION_SELECT_TEXT_OK);
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(offsetof(NationSelectTextRule, surface) == 132);
    static_assert(offsetof(NationSelectTextRule, text) == 160);
    static_assert(offsetof(NationSelectTextRuleInfo, statusFlags) ==
                  416);
    UNSCOPED_INFO("presentation ABI version mismatch");
    REQUIRE(NationSelectTextAbiVersion() ==
            NATION_SELECT_TEXT_ABI_VERSION);

    rerevved::nation_select_text::ResetForTests();
    UNSCOPED_INFO("null presentation rule accepted");
    REQUIRE(RegisterNationSelectTextRule(nullptr) ==
            NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT);
    auto invalid         = makeUnitRule("test.provider", "invalid", "Keshik");
    invalid.baseUnitType = UNIT_TYPE_KNIGHTS;
    UNSCOPED_INFO("mismatched unique-unit identity accepted");
    REQUIRE(RegisterNationSelectTextRule(&invalid) ==
            NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT);
    invalid             = makeUnitRule("test.provider", "invalid", "Keshik");
    invalid.displayForm = UNIT_DISPLAY_FORM_ARMY;
    UNSCOPED_INFO("unsupported army presentation form accepted");
    REQUIRE(RegisterNationSelectTextRule(&invalid) ==
            NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT);
    invalid = makeEraRule("test.provider", "invalid", "bad\ntext");
    UNSCOPED_INFO("control character accepted in presentation text");
    REQUIRE(RegisterNationSelectTextRule(&invalid) ==
            NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT);
}

void TestRegistrationEvaluationAndConflict()
{
    rerevved::nation_select_text::ResetForTests();
    auto era = makeEraRule("test.provider", "horseback", "Knowledge of Horseback Riding");
    UNSCOPED_INFO("idempotent era text registration failed");
    REQUIRE((RegisterNationSelectTextRule(&era) ==
                 NATION_SELECT_TEXT_OK &&
             RegisterNationSelectTextRule(&era) ==
                 NATION_SELECT_TEXT_OK));
    auto evaluation = evaluate(era);
    UNSCOPED_INFO("era text was not selected");
    REQUIRE((evaluation.replacementCount == 1 &&
             std::string_view(evaluation.text) ==
                 "Knowledge of Horseback Riding" &&
             (evaluation.statusFlags &
              NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0));

    auto unit = makeUnitRule("test.provider", "keshik", "Keshik - Horseman with +1 movement");
    UNSCOPED_INFO("unit text registration failed");
    REQUIRE(RegisterNationSelectTextRule(&unit) ==
            NATION_SELECT_TEXT_OK);
    evaluation = evaluate(unit);
    UNSCOPED_INFO("unique-unit text was not selected");
    REQUIRE(std::string_view(evaluation.text) ==
            "Keshik - Horseman with +1 movement");

    auto conflict = makeUnitRule("other.provider", "keshik", "Other Keshik");
    UNSCOPED_INFO("conflicting target registration failed");
    REQUIRE(RegisterNationSelectTextRule(&conflict) ==
            NATION_SELECT_TEXT_OK);
    evaluation = evaluate(unit);
    UNSCOPED_INFO("presentation conflict did not preserve native fallback");
    REQUIRE((evaluation.replacementCount == 2 && evaluation.text[0] == '\0' &&
             (evaluation.statusFlags &
              NATION_SELECT_TEXT_EVALUATION_REPLACEMENT_CONFLICT) !=
                 0));
}

void TestReadbackAndSizedOutput()
{
    uint32_t count = 0;
    UNSCOPED_INFO("presentation rule count mismatch");
    REQUIRE((GetNationSelectTextRuleCount(&count) ==
                 NATION_SELECT_TEXT_OK &&
             count == 3));
    NationSelectTextRuleInfo info{};
    UNSCOPED_INFO("presentation readback failed");
    REQUIRE(GetNationSelectTextRule(0, &info, sizeof(info)) ==
            NATION_SELECT_TEXT_OK);
    UNSCOPED_INFO("short presentation readback accepted");
    REQUIRE(GetNationSelectTextRule(0, &info, 419) ==
            NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL);

    const auto                  rule  = makeEraRule("test.provider", "horseback", "unused");
    const NationSelectTextQuery query = {
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
    NationSelectTextEvaluation evaluation{};
    UNSCOPED_INFO("short presentation evaluation accepted");
    REQUIRE(EvaluateNationSelectText(&query, &evaluation, 267) ==
            NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL);
}

void TestAdditionalSurfaces()
{
    rerevved::nation_select_text::ResetForTests();
    constexpr auto unused = NATION_SELECT_TEXT_SELECTOR_UNUSED;

    auto leader = makeFieldRule(
        "test.provider",
        "leader",
        NATION_SELECT_TEXT_SURFACE_LEADER_NAME,
        CIVILIZATION_MONGOLIAN,
        unused,
        unused,
        unused,
        unused,
        unused,
        "Genghis Khan");
    auto civilization = makeFieldRule(
        "test.provider",
        "civilization",
        NATION_SELECT_TEXT_SURFACE_CIVILIZATION_NAME,
        CIVILIZATION_MONGOLIAN,
        unused,
        unused,
        unused,
        unused,
        unused,
        "Mongolia");
    auto trait = makeFieldRule(
        "test.provider",
        "trait",
        NATION_SELECT_TEXT_SURFACE_CIVILIZATION_TRAIT,
        CIVILIZATION_MONGOLIAN,
        unused,
        unused,
        unused,
        unused,
        unused,
        "Horse Lords");
    auto eraHeading = makeFieldRule(
        "test.provider",
        "era-heading",
        NATION_SELECT_TEXT_SURFACE_ERA_HEADING,
        unused,
        UNLOCK_ERA_MEDIEVAL,
        unused,
        unused,
        unused,
        unused,
        "Medieval");
    auto unitSection = makeFieldRule(
        "test.provider",
        "unit-section",
        NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT_SECTION_HEADING,
        unused,
        unused,
        unused,
        unused,
        unused,
        unused,
        "Special Units");

    for (const auto* rule : { &leader,
                              &civilization,
                              &trait,
                              &eraHeading,
                              &unitSection })
    {
        UNSCOPED_INFO("additional presentation surface registration failed");
        REQUIRE(RegisterNationSelectTextRule(rule) ==
                NATION_SELECT_TEXT_OK);
        const auto evaluation = evaluate(*rule);
        UNSCOPED_INFO("additional presentation surface was not selected");
        REQUIRE((evaluation.replacementCount == 1 &&
                 std::string_view(evaluation.text) == rule->text &&
                 (evaluation.statusFlags &
                  NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0));
    }

    auto invalid      = leader;
    invalid.unlockEra = UNLOCK_ERA_ANCIENT;
    UNSCOPED_INFO("leader selector accepted an unrelated era");
    REQUIRE(RegisterNationSelectTextRule(&invalid) ==
            NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT);
    invalid              = eraHeading;
    invalid.civilization = CIVILIZATION_MONGOLIAN;
    UNSCOPED_INFO("global era heading accepted a civilization selector");
    REQUIRE(RegisterNationSelectTextRule(&invalid) ==
            NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT);
    invalid         = unitSection;
    invalid.surface = NATION_SELECT_TEXT_SURFACE_RESERVED_5;
    UNSCOPED_INFO("reserved presentation surface was accepted");
    REQUIRE(RegisterNationSelectTextRule(&invalid) ==
            NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT);

    auto conflict = leader;
    std::memcpy(conflict.providerId, "other.provider", 15);
    std::memcpy(conflict.ruleId, "leader-conflict", 16);
    std::memcpy(conflict.text, "Temujin", 8);
    UNSCOPED_INFO("leader conflict registration failed");
    REQUIRE(RegisterNationSelectTextRule(&conflict) ==
            NATION_SELECT_TEXT_OK);
    const auto evaluation = evaluate(leader);
    UNSCOPED_INFO("global presentation conflict did not preserve native fallback");
    REQUIRE((evaluation.replacementCount == 2 && evaluation.text[0] == '\0' &&
             (evaluation.statusFlags &
              NATION_SELECT_TEXT_EVALUATION_REPLACEMENT_CONFLICT) !=
                 0));
}

} // namespace

TEST_CASE("nation_select_text_contract", "[ctest][nation-select-text][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestLayoutAndValidation();
    TestRegistrationEvaluationAndConflict();
    TestReadbackAndSizedOutput();
    TestAdditionalSurfaces();
}

TEST_CASE("nation_select_text_c_header", "[ctest][nation-select-text][local-fast]")
{
    const int status = rerevved_nation_select_text_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}
