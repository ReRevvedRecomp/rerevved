#include "nation_select_text_registry.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ReRevvedNationSelectTextRule makeFieldRule(
    const char*                     provider,
    const char*                     ruleId,
    ReRevvedNationSelectTextSurface surface,
    ReRevvedCivilizationId          civilization,
    ReRevvedUniqueEraUnlockEra      unlockEra,
    ReRevvedUniqueEraAbilityId      ability,
    ReRevvedUnitTypeId              baseUnitType,
    ReRevvedUnitIdentityId          identity,
    ReRevvedUnitDisplayForm         displayForm,
    const char*                     text)
{
    ReRevvedNationSelectTextRule rule{};
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

ReRevvedNationSelectTextRule makeEraRule(const char* provider,
                                         const char* ruleId,
                                         const char* text)
{
    ReRevvedNationSelectTextRule rule{};
    rule.structSize   = sizeof(rule);
    rule.surface      = REREVVED_NATION_SELECT_TEXT_SURFACE_ERA_ABILITY;
    rule.civilization = REREVVED_CIVILIZATION_MONGOLIAN;
    rule.unlockEra    = REREVVED_UNIQUE_ERA_ANCIENT;
    rule.ability =
        REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
    rule.baseUnitType = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.identity     = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.displayForm  = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    std::memcpy(rule.text, text, std::strlen(text) + 1);
    return rule;
}

ReRevvedNationSelectTextRule makeUnitRule(const char* provider,
                                          const char* ruleId,
                                          const char* text)
{
    ReRevvedNationSelectTextRule rule{};
    rule.structSize   = sizeof(rule);
    rule.surface      = REREVVED_NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT;
    rule.civilization = REREVVED_CIVILIZATION_MONGOLIAN;
    rule.unlockEra    = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.ability      = 0;
    rule.baseUnitType = REREVVED_UNIT_TYPE_HORSEMEN;
    rule.identity     = REREVVED_UNIT_IDENTITY_KESHIK;
    rule.displayForm  = REREVVED_UNIT_DISPLAY_FORM_UNIT;
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    std::memcpy(rule.text, text, std::strlen(text) + 1);
    return rule;
}

ReRevvedNationSelectTextEvaluation evaluate(
    const ReRevvedNationSelectTextRule& rule)
{
    const ReRevvedNationSelectTextQuery query = {
        sizeof(ReRevvedNationSelectTextQuery),
        rule.surface,
        rule.civilization,
        rule.unlockEra,
        rule.ability,
        rule.baseUnitType,
        rule.identity,
        rule.displayForm,
        {},
    };
    ReRevvedNationSelectTextEvaluation evaluation{};
    require(ReRevvedEvaluateNationSelectText(
                &query, &evaluation, sizeof(evaluation)) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "presentation evaluation failed");
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(sizeof(ReRevvedNationSelectTextRule) == 448);
    static_assert(offsetof(ReRevvedNationSelectTextRule, surface) == 132);
    static_assert(offsetof(ReRevvedNationSelectTextRule, text) == 160);
    static_assert(sizeof(ReRevvedNationSelectTextRuleInfo) == 452);
    static_assert(offsetof(ReRevvedNationSelectTextRuleInfo, statusFlags) ==
                  416);
    static_assert(sizeof(ReRevvedNationSelectTextQuery) == 64);
    static_assert(sizeof(ReRevvedNationSelectTextEvaluation) == 300);
    require(ReRevvedNationSelectTextAbiVersion() ==
                REREVVED_NATION_SELECT_TEXT_ABI_VERSION,
            "presentation ABI version mismatch");

    rerevved::nation_select_text::ResetForTests();
    require(ReRevvedRegisterNationSelectTextRule(nullptr) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "null presentation rule accepted");
    auto invalid         = makeUnitRule("test.provider", "invalid", "Keshik");
    invalid.baseUnitType = REREVVED_UNIT_TYPE_KNIGHTS;
    require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "mismatched unique-unit identity accepted");
    invalid             = makeUnitRule("test.provider", "invalid", "Keshik");
    invalid.displayForm = REREVVED_UNIT_DISPLAY_FORM_ARMY;
    require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "unsupported army presentation form accepted");
    invalid = makeEraRule("test.provider", "invalid", "bad\ntext");
    require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "control character accepted in presentation text");
}

void TestRegistrationEvaluationAndConflict()
{
    rerevved::nation_select_text::ResetForTests();
    auto era = makeEraRule("test.provider", "horseback", "Knowledge of Horseback Riding");
    require(ReRevvedRegisterNationSelectTextRule(&era) ==
                    REREVVED_NATION_SELECT_TEXT_OK &&
                ReRevvedRegisterNationSelectTextRule(&era) ==
                    REREVVED_NATION_SELECT_TEXT_OK,
            "idempotent era text registration failed");
    auto evaluation = evaluate(era);
    require(evaluation.replacementCount == 1 &&
                std::string_view(evaluation.text) ==
                    "Knowledge of Horseback Riding" &&
                (evaluation.statusFlags &
                 REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0,
            "era text was not selected");

    auto unit = makeUnitRule("test.provider", "keshik", "Keshik - Horseman with +1 movement");
    require(ReRevvedRegisterNationSelectTextRule(&unit) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "unit text registration failed");
    evaluation = evaluate(unit);
    require(std::string_view(evaluation.text) ==
                "Keshik - Horseman with +1 movement",
            "unique-unit text was not selected");

    auto conflict = makeUnitRule("other.provider", "keshik", "Other Keshik");
    require(ReRevvedRegisterNationSelectTextRule(&conflict) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "conflicting target registration failed");
    evaluation = evaluate(unit);
    require(evaluation.replacementCount == 2 && evaluation.text[0] == '\0' &&
                (evaluation.statusFlags &
                 REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACEMENT_CONFLICT) !=
                    0,
            "presentation conflict did not preserve native fallback");
}

void TestReadbackAndSizedOutput()
{
    uint32_t count = 0;
    require(ReRevvedGetNationSelectTextRuleCount(&count) ==
                    REREVVED_NATION_SELECT_TEXT_OK &&
                count == 3,
            "presentation rule count mismatch");
    ReRevvedNationSelectTextRuleInfo info{};
    require(ReRevvedGetNationSelectTextRule(0, &info, sizeof(info)) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "presentation readback failed");
    require(ReRevvedGetNationSelectTextRule(0, &info, 419) ==
                REREVVED_NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL,
            "short presentation readback accepted");

    const auto                          rule  = makeEraRule("test.provider", "horseback", "unused");
    const ReRevvedNationSelectTextQuery query = {
        sizeof(ReRevvedNationSelectTextQuery),
        rule.surface,
        rule.civilization,
        rule.unlockEra,
        rule.ability,
        rule.baseUnitType,
        rule.identity,
        rule.displayForm,
        {},
    };
    ReRevvedNationSelectTextEvaluation evaluation{};
    require(ReRevvedEvaluateNationSelectText(&query, &evaluation, 267) ==
                REREVVED_NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL,
            "short presentation evaluation accepted");
}

void TestAdditionalSurfaces()
{
    rerevved::nation_select_text::ResetForTests();
    constexpr auto unused = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;

    auto leader = makeFieldRule(
        "test.provider",
        "leader",
        REREVVED_NATION_SELECT_TEXT_SURFACE_LEADER_NAME,
        REREVVED_CIVILIZATION_MONGOLIAN,
        unused,
        unused,
        unused,
        unused,
        unused,
        "Genghis Khan");
    auto civilization = makeFieldRule(
        "test.provider",
        "civilization",
        REREVVED_NATION_SELECT_TEXT_SURFACE_CIVILIZATION_NAME,
        REREVVED_CIVILIZATION_MONGOLIAN,
        unused,
        unused,
        unused,
        unused,
        unused,
        "Mongolia");
    auto trait = makeFieldRule(
        "test.provider",
        "trait",
        REREVVED_NATION_SELECT_TEXT_SURFACE_CIVILIZATION_TRAIT,
        REREVVED_CIVILIZATION_MONGOLIAN,
        unused,
        unused,
        unused,
        unused,
        unused,
        "Horse Lords");
    auto eraHeading = makeFieldRule(
        "test.provider",
        "era-heading",
        REREVVED_NATION_SELECT_TEXT_SURFACE_ERA_HEADING,
        unused,
        REREVVED_UNIQUE_ERA_MEDIEVAL,
        unused,
        unused,
        unused,
        unused,
        "Medieval");
    auto unitSection = makeFieldRule(
        "test.provider",
        "unit-section",
        REREVVED_NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT_SECTION_HEADING,
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
        require(ReRevvedRegisterNationSelectTextRule(rule) ==
                    REREVVED_NATION_SELECT_TEXT_OK,
                "additional presentation surface registration failed");
        const auto evaluation = evaluate(*rule);
        require(evaluation.replacementCount == 1 &&
                    std::string_view(evaluation.text) == rule->text &&
                    (evaluation.statusFlags &
                     REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0,
                "additional presentation surface was not selected");
    }

    auto invalid      = leader;
    invalid.unlockEra = REREVVED_UNIQUE_ERA_ANCIENT;
    require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "leader selector accepted an unrelated era");
    invalid              = eraHeading;
    invalid.civilization = REREVVED_CIVILIZATION_MONGOLIAN;
    require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "global era heading accepted a civilization selector");
    invalid         = unitSection;
    invalid.surface = REREVVED_NATION_SELECT_TEXT_SURFACE_RESERVED_5;
    require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "reserved presentation surface was accepted");

    auto conflict = leader;
    std::memcpy(conflict.providerId, "other.provider", 15);
    std::memcpy(conflict.ruleId, "leader-conflict", 16);
    std::memcpy(conflict.text, "Temujin", 8);
    require(ReRevvedRegisterNationSelectTextRule(&conflict) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "leader conflict registration failed");
    const auto evaluation = evaluate(leader);
    require(evaluation.replacementCount == 2 && evaluation.text[0] == '\0' &&
                (evaluation.statusFlags &
                 REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACEMENT_CONFLICT) !=
                    0,
            "global presentation conflict did not preserve native fallback");
}

} // namespace

int main()
{
    TestLayoutAndValidation();
    TestRegistrationEvaluationAndConflict();
    TestReadbackAndSizedOutput();
    TestAdditionalSurfaces();
    return 0;
}
