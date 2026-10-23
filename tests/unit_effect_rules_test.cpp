#include "registry_scope.hpp"
#include "unit_effect_rules_registry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

extern "C" int rerevved_unit_effect_rules_c_header_check(void);

namespace
{

UnitEffectRule makeRule(const char*    provider,
                        const char*    ruleId,
                        CivilizationId civilization,
                        UnitTypeId     baseUnitType,
                        UnitIdentityId identity,
                        UnitEffectId   effect =
                            UNIT_EFFECT_CREATION_VETERAN)
{
    UnitEffectRule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization = civilization;
    rule.baseUnitType = baseUnitType;
    rule.identity     = identity;
    rule.effect       = effect;
    return rule;
}

UnitEffectEvaluation evaluate(CivilizationId civilization,
                              UnitTypeId     baseUnitType,
                              UnitIdentityId identity,
                              int32_t        nativeLevel,
                              UnitEffectId   effect =
                                  UNIT_EFFECT_CREATION_VETERAN)
{
    const UnitEffectQuery query = {
        sizeof(UnitEffectQuery),
        civilization,
        baseUnitType,
        identity,
        effect,
        nativeLevel,
        {},
    };
    UnitEffectEvaluation evaluation{};
    UNSCOPED_INFO("unit effect evaluation failed");
    REQUIRE(EvaluateUnitEffect(
                &query, &evaluation, sizeof(evaluation)) ==
            UNIT_EFFECT_RULES_OK);
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(offsetof(UnitEffectRule, civilization) == 132);
    static_assert(offsetof(UnitEffectRule, identity) == 140);
    static_assert(offsetof(UnitEffectRule, effect) == 144);
    static_assert(offsetof(UnitEffectRuleInfo, identity) == 140);
    static_assert(offsetof(UnitEffectRuleInfo, statusFlags) == 148);
    static_assert(offsetof(UnitEffectQuery, identity) == 12);
    static_assert(offsetof(UnitEffectQuery, nativeLevel) == 20);
    static_assert(offsetof(UnitEffectEvaluation, finalLevel) == 8);
    UNSCOPED_INFO("unit effect ABI version mismatch");
    REQUIRE(UnitEffectRulesAbiVersion() ==
            UNIT_EFFECT_RULES_ABI_VERSION);

    rerevved::unit_effect_rules::ResetForTests();
    auto invalid   = makeRule("test.provider",
                              "invalid",
                              CIVILIZATION_AZTEC,
                              UNIT_TYPE_WARRIOR,
                              UNIT_IDENTITY_JAGUAR_WARRIOR);
    invalid.effect = 11;
    UNSCOPED_INFO("unsupported unit effect accepted");
    REQUIRE(RegisterUnitEffectRule(&invalid) ==
            UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    invalid = makeRule("test.provider",
                       "invalid",
                       CIVILIZATION_UNKNOWN,
                       UNIT_TYPE_WARRIOR,
                       UNIT_IDENTITY_JAGUAR_WARRIOR);
    UNSCOPED_INFO("unknown unit effect civilization accepted");
    REQUIRE(RegisterUnitEffectRule(&invalid) ==
            UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    invalid = makeRule("test.provider",
                       "invalid",
                       CIVILIZATION_AZTEC,
                       UNIT_TYPE_WARRIOR,
                       UNIT_IDENTITY_BASE);
    UNSCOPED_INFO("base identity unit effect accepted");
    REQUIRE(RegisterUnitEffectRule(&invalid) ==
            UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT);
    invalid.identity = UNIT_IDENTITY_IMPI_WARRIOR;
    UNSCOPED_INFO("mismatched identity unit effect accepted");
    REQUIRE(RegisterUnitEffectRule(&invalid) ==
            UNIT_EFFECT_RULES_ERR_INVALID_ARGUMENT);
}

void TestNamedSpecialEffects()
{
    struct ExpectedEffect
    {
        UnitEffectId effect;
        uint32_t     mask;
        const char*  ruleId;
    };

    constexpr std::array<ExpectedEffect, 9> expected = { {
        { UNIT_EFFECT_CREATION_GUERILLA, 1u << 2, "guerilla" },
        { UNIT_EFFECT_CREATION_BLITZ, 1u << 0, "blitz" },
        { UNIT_EFFECT_CREATION_INFILTRATION, 1u << 1, "infiltration" },
        { UNIT_EFFECT_CREATION_LOYALTY, 1u << 3, "loyalty" },
        { UNIT_EFFECT_CREATION_ENGINEER, 1u << 4, "engineer" },
        { UNIT_EFFECT_CREATION_LEADERSHIP, 1u << 5, "leadership" },
        { UNIT_EFFECT_CREATION_MARCH, 1u << 6, "march" },
        { UNIT_EFFECT_CREATION_MEDIC, 1u << 7, "medic" },
        { UNIT_EFFECT_CREATION_SCOUT, 1u << 8, "scout" },
    } };

    rerevved::unit_effect_rules::ResetForTests();
    uint32_t veteranMask = 0;
    UNSCOPED_INFO("Veteran was treated as a native special upgrade");
    REQUIRE(!rerevved::unit_effect_rules::TryGetNativeSpecialUpgradeMask(
        UNIT_EFFECT_CREATION_VETERAN,
        veteranMask));

    for (size_t index = 0; index < expected.size(); ++index)
    {
        CAPTURE(expected[index].ruleId);
        uint32_t mask = 0;
        UNSCOPED_INFO("named special effect mapped to the wrong native mask");
        REQUIRE((rerevved::unit_effect_rules::TryGetNativeSpecialUpgradeMask(
                     expected[index].effect,
                     mask) &&
                 mask == expected[index].mask));
    }

    // Every non-Veteran effect uses the same unchanged-rank grant path.
    const auto rule = makeRule("test.special",
                               expected.front().ruleId,
                               CIVILIZATION_AZTEC,
                               UNIT_TYPE_WARRIOR,
                               UNIT_IDENTITY_JAGUAR_WARRIOR,
                               UNIT_EFFECT_CREATION_GUERILLA);
    UNSCOPED_INFO("named special effect registration failed");
    REQUIRE(RegisterUnitEffectRule(&rule) ==
            UNIT_EFFECT_RULES_OK);

    const auto evaluation = evaluate(CIVILIZATION_AZTEC,
                                     UNIT_TYPE_WARRIOR,
                                     UNIT_IDENTITY_JAGUAR_WARRIOR,
                                     1,
                                     UNIT_EFFECT_CREATION_GUERILLA);
    UNSCOPED_INFO("special effect changed the native rank");
    REQUIRE((evaluation.finalLevel == 1 &&
             evaluation.grantCount == 1 &&
             (evaluation.statusFlags &
              UNIT_EFFECT_EVALUATION_GRANTED) != 0));
}

void TestRegistrationAndGrant()
{
    rerevved::unit_effect_rules::ResetForTests();
    auto later   = makeRule("z.provider",
                            "late",
                            CIVILIZATION_AZTEC,
                            UNIT_TYPE_WARRIOR,
                            UNIT_IDENTITY_JAGUAR_WARRIOR);
    auto earlier = makeRule("a.provider",
                            "early",
                            CIVILIZATION_AZTEC,
                            UNIT_TYPE_WARRIOR,
                            UNIT_IDENTITY_JAGUAR_WARRIOR);
    UNSCOPED_INFO("unit effect rules did not register");
    REQUIRE((RegisterUnitEffectRule(&later) ==
                 UNIT_EFFECT_RULES_OK &&
             RegisterUnitEffectRule(&earlier) ==
                 UNIT_EFFECT_RULES_OK));

    uint32_t count = 0;
    UNSCOPED_INFO("unit effect rule count mismatch");
    REQUIRE((GetUnitEffectRuleCount(&count) ==
                 UNIT_EFFECT_RULES_OK &&
             count == 2));
    UnitEffectRuleInfo info{};
    UNSCOPED_INFO("unit effect readback ordering mismatch");
    REQUIRE((GetUnitEffectRule(0, &info, sizeof(info)) ==
                 UNIT_EFFECT_RULES_OK &&
             std::string_view(info.providerId) == "a.provider" &&
             info.civilization == CIVILIZATION_AZTEC &&
             info.baseUnitType == UNIT_TYPE_WARRIOR &&
             info.identity == UNIT_IDENTITY_JAGUAR_WARRIOR &&
             info.effect == UNIT_EFFECT_CREATION_VETERAN));

    const auto granted = evaluate(CIVILIZATION_AZTEC,
                                  UNIT_TYPE_WARRIOR,
                                  UNIT_IDENTITY_JAGUAR_WARRIOR,
                                  1);
    UNSCOPED_INFO("Veteran grant did not raise lower rank to level two");
    REQUIRE((granted.finalLevel == 2 && granted.grantCount == 2 &&
             (granted.statusFlags &
              UNIT_EFFECT_EVALUATION_GRANTED) != 0));
    const auto preserved = evaluate(CIVILIZATION_AZTEC,
                                    UNIT_TYPE_WARRIOR,
                                    UNIT_IDENTITY_JAGUAR_WARRIOR,
                                    3);
    UNSCOPED_INFO("Veteran grant lowered native rank");
    REQUIRE(preserved.finalLevel == 3);
    const auto wrongTarget = evaluate(CIVILIZATION_ZULU,
                                      UNIT_TYPE_WARRIOR,
                                      UNIT_IDENTITY_IMPI_WARRIOR,
                                      1);
    UNSCOPED_INFO("Veteran grant crossed civilization target");
    REQUIRE((wrongTarget.finalLevel == 1 && wrongTarget.grantCount == 0));
}

void TestSizedOutput()
{
    rerevved::unit_effect_rules::ResetForTests();
    const auto rule = makeRule("a.provider",
                               "grant",
                               CIVILIZATION_AZTEC,
                               UNIT_TYPE_WARRIOR,
                               UNIT_IDENTITY_JAGUAR_WARRIOR);
    UNSCOPED_INFO("unit effect rule did not register");
    REQUIRE(RegisterUnitEffectRule(&rule) ==
            UNIT_EFFECT_RULES_OK);
    const UnitEffectQuery query = {
        sizeof(UnitEffectQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        UNIT_EFFECT_CREATION_VETERAN,
        1,
        {},
    };
    UnitEffectEvaluation output{};
    UNSCOPED_INFO("short unit effect evaluation output accepted");
    REQUIRE(EvaluateUnitEffect(&query, &output, 19) ==
            UNIT_EFFECT_RULES_ERR_BUFFER_TOO_SMALL);
    std::memset(&output, 0x5a, sizeof(output));
    UNSCOPED_INFO("unit effect minimum evaluation prefix rejected");
    REQUIRE((EvaluateUnitEffect(&query, &output, 20) ==
                 UNIT_EFFECT_RULES_OK &&
             output.structSize ==
                 sizeof(output)));
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        UNSCOPED_INFO("unit effect minimum prefix overwrote caller tail");
        REQUIRE(bytes[index] == 0x5a);
    }
}

} // namespace

TEST_CASE("unit_effect_rules_contract", "[ctest][unit-effect-rules][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestLayoutAndValidation();
    TestNamedSpecialEffects();
    TestRegistrationAndGrant();
    TestSizedOutput();
}

TEST_CASE("unit_effect_rules_c_header", "[ctest][unit-effect-rules][local-fast]")
{
    const int status = rerevved_unit_effect_rules_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}
