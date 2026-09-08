#include "nation_select_text_registry.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

namespace
{

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ReRevvedNationSelectTextRule MakeFieldRule(
    const char*                     provider,
    const char*                     rule_id,
    ReRevvedNationSelectTextSurface surface,
    ReRevvedCivilizationId          civilization,
    ReRevvedUniqueEraUnlockEra      unlock_era,
    ReRevvedUniqueEraAbilityId      ability,
    ReRevvedUnitTypeId              base_unit_type,
    ReRevvedUnitIdentityId          identity,
    ReRevvedUnitDisplayForm         display_form,
    const char*                     text)
{
    ReRevvedNationSelectTextRule rule{};
    rule.struct_size    = sizeof(rule);
    rule.surface        = surface;
    rule.civilization   = civilization;
    rule.unlock_era     = unlock_era;
    rule.ability        = ability;
    rule.base_unit_type = base_unit_type;
    rule.identity       = identity;
    rule.display_form   = display_form;
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    std::memcpy(rule.text, text, std::strlen(text) + 1);
    return rule;
}

ReRevvedNationSelectTextRule MakeEraRule(const char* provider,
                                         const char* rule_id,
                                         const char* text)
{
    ReRevvedNationSelectTextRule rule{};
    rule.struct_size  = sizeof(rule);
    rule.surface      = REREVVED_NATION_SELECT_TEXT_SURFACE_ERA_ABILITY;
    rule.civilization = REREVVED_CIVILIZATION_MONGOLIAN;
    rule.unlock_era   = REREVVED_UNIQUE_ERA_ANCIENT;
    rule.ability =
        REREVVED_UNIQUE_ERA_ABILITY_KNOWLEDGE_OF_HORSEBACK_RIDING;
    rule.base_unit_type = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.identity       = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.display_form   = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    std::memcpy(rule.text, text, std::strlen(text) + 1);
    return rule;
}

ReRevvedNationSelectTextRule MakeUnitRule(const char* provider,
                                          const char* rule_id,
                                          const char* text)
{
    ReRevvedNationSelectTextRule rule{};
    rule.struct_size    = sizeof(rule);
    rule.surface        = REREVVED_NATION_SELECT_TEXT_SURFACE_UNIQUE_UNIT;
    rule.civilization   = REREVVED_CIVILIZATION_MONGOLIAN;
    rule.unlock_era     = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;
    rule.ability        = 0;
    rule.base_unit_type = REREVVED_UNIT_TYPE_HORSEMEN;
    rule.identity       = REREVVED_UNIT_IDENTITY_KESHIK;
    rule.display_form   = REREVVED_UNIT_DISPLAY_FORM_UNIT;
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    std::memcpy(rule.text, text, std::strlen(text) + 1);
    return rule;
}

ReRevvedNationSelectTextEvaluation Evaluate(
    const ReRevvedNationSelectTextRule& rule)
{
    const ReRevvedNationSelectTextQuery query = {
        sizeof(ReRevvedNationSelectTextQuery),
        rule.surface,
        rule.civilization,
        rule.unlock_era,
        rule.ability,
        rule.base_unit_type,
        rule.identity,
        rule.display_form,
        {},
    };
    ReRevvedNationSelectTextEvaluation evaluation{};
    Require(ReRevvedEvaluateNationSelectText(
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
    static_assert(offsetof(ReRevvedNationSelectTextRuleInfo, status_flags) ==
                  416);
    static_assert(sizeof(ReRevvedNationSelectTextQuery) == 64);
    static_assert(sizeof(ReRevvedNationSelectTextEvaluation) == 300);
    Require(ReRevvedNationSelectTextAbiVersion() ==
                REREVVED_NATION_SELECT_TEXT_ABI_VERSION,
            "presentation ABI version mismatch");

    rerevved::nation_select_text::ResetForTests();
    Require(ReRevvedRegisterNationSelectTextRule(nullptr) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "null presentation rule accepted");
    auto invalid           = MakeUnitRule("test.provider", "invalid", "Keshik");
    invalid.base_unit_type = REREVVED_UNIT_TYPE_KNIGHTS;
    Require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "mismatched unique-unit identity accepted");
    invalid              = MakeUnitRule("test.provider", "invalid", "Keshik");
    invalid.display_form = REREVVED_UNIT_DISPLAY_FORM_ARMY;
    Require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "unsupported army presentation form accepted");
    invalid = MakeEraRule("test.provider", "invalid", "bad\ntext");
    Require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "control character accepted in presentation text");
}

void TestRegistrationEvaluationAndConflict()
{
    rerevved::nation_select_text::ResetForTests();
    auto era = MakeEraRule("test.provider", "horseback", "Knowledge of Horseback Riding");
    Require(ReRevvedRegisterNationSelectTextRule(&era) ==
                    REREVVED_NATION_SELECT_TEXT_OK &&
                ReRevvedRegisterNationSelectTextRule(&era) ==
                    REREVVED_NATION_SELECT_TEXT_OK,
            "idempotent era text registration failed");
    auto evaluation = Evaluate(era);
    Require(evaluation.replacement_count == 1 &&
                std::string_view(evaluation.text) ==
                    "Knowledge of Horseback Riding" &&
                (evaluation.status_flags &
                 REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0,
            "era text was not selected");

    auto unit = MakeUnitRule("test.provider", "keshik", "Keshik - Horseman with +1 movement");
    Require(ReRevvedRegisterNationSelectTextRule(&unit) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "unit text registration failed");
    evaluation = Evaluate(unit);
    Require(std::string_view(evaluation.text) ==
                "Keshik - Horseman with +1 movement",
            "unique-unit text was not selected");

    auto conflict = MakeUnitRule("other.provider", "keshik", "Other Keshik");
    Require(ReRevvedRegisterNationSelectTextRule(&conflict) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "conflicting target registration failed");
    evaluation = Evaluate(unit);
    Require(evaluation.replacement_count == 2 && evaluation.text[0] == '\0' &&
                (evaluation.status_flags &
                 REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACEMENT_CONFLICT) !=
                    0,
            "presentation conflict did not preserve native fallback");
}

void TestReadbackAndSizedOutput()
{
    uint32_t count = 0;
    Require(ReRevvedGetNationSelectTextRuleCount(&count) ==
                    REREVVED_NATION_SELECT_TEXT_OK &&
                count == 3,
            "presentation rule count mismatch");
    ReRevvedNationSelectTextRuleInfo info{};
    Require(ReRevvedGetNationSelectTextRule(0, &info, sizeof(info)) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "presentation readback failed");
    Require(ReRevvedGetNationSelectTextRule(0, &info, 419) ==
                REREVVED_NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL,
            "short presentation readback accepted");

    const auto                          rule  = MakeEraRule("test.provider", "horseback", "unused");
    const ReRevvedNationSelectTextQuery query = {
        sizeof(ReRevvedNationSelectTextQuery),
        rule.surface,
        rule.civilization,
        rule.unlock_era,
        rule.ability,
        rule.base_unit_type,
        rule.identity,
        rule.display_form,
        {},
    };
    ReRevvedNationSelectTextEvaluation evaluation{};
    Require(ReRevvedEvaluateNationSelectText(&query, &evaluation, 267) ==
                REREVVED_NATION_SELECT_TEXT_ERR_BUFFER_TOO_SMALL,
            "short presentation evaluation accepted");
}

void TestAdditionalSurfaces()
{
    rerevved::nation_select_text::ResetForTests();
    constexpr auto unused = REREVVED_NATION_SELECT_TEXT_SELECTOR_UNUSED;

    auto leader = MakeFieldRule(
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
    auto civilization = MakeFieldRule(
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
    auto trait = MakeFieldRule(
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
    auto era_heading = MakeFieldRule(
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
    auto unit_section = MakeFieldRule(
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
                              &era_heading,
                              &unit_section })
    {
        Require(ReRevvedRegisterNationSelectTextRule(rule) ==
                    REREVVED_NATION_SELECT_TEXT_OK,
                "additional presentation surface registration failed");
        const auto evaluation = Evaluate(*rule);
        Require(evaluation.replacement_count == 1 &&
                    std::string_view(evaluation.text) == rule->text &&
                    (evaluation.status_flags &
                     REREVVED_NATION_SELECT_TEXT_EVALUATION_REPLACED) != 0,
                "additional presentation surface was not selected");
    }

    auto invalid       = leader;
    invalid.unlock_era = REREVVED_UNIQUE_ERA_ANCIENT;
    Require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "leader selector accepted an unrelated era");
    invalid              = era_heading;
    invalid.civilization = REREVVED_CIVILIZATION_MONGOLIAN;
    Require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "global era heading accepted a civilization selector");
    invalid         = unit_section;
    invalid.surface = REREVVED_NATION_SELECT_TEXT_SURFACE_RESERVED_5;
    Require(ReRevvedRegisterNationSelectTextRule(&invalid) ==
                REREVVED_NATION_SELECT_TEXT_ERR_INVALID_ARGUMENT,
            "reserved presentation surface was accepted");

    auto conflict = leader;
    std::memcpy(conflict.provider_id, "other.provider", 15);
    std::memcpy(conflict.rule_id, "leader-conflict", 16);
    std::memcpy(conflict.text, "Temujin", 8);
    Require(ReRevvedRegisterNationSelectTextRule(&conflict) ==
                REREVVED_NATION_SELECT_TEXT_OK,
            "leader conflict registration failed");
    const auto evaluation = Evaluate(leader);
    Require(evaluation.replacement_count == 2 && evaluation.text[0] == '\0' &&
                (evaluation.status_flags &
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
