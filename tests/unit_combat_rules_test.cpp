#include "registry_scope.hpp"
#include "unit_combat_rules_registry.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

extern "C" int rerevved_unit_combat_rules_c_header_check(void);

namespace
{

UnitCombatRule makeRule(const char*        provider,
                        const char*        ruleId,
                        UnitCombatProperty property,
                        int32_t            percentageDelta)
{
    UnitCombatRule rule{};
    rule.structSize      = sizeof(rule);
    rule.civilization    = CIVILIZATION_AZTEC;
    rule.baseUnitType    = UNIT_TYPE_WARRIOR;
    rule.identity        = UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.terrain         = TERRAIN_FOREST;
    rule.property        = property;
    rule.percentageDelta = percentageDelta;
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    return rule;
}

UnitCombatEvaluation evaluate(
    UnitCombatProperty property = UNIT_COMBAT_ATTACK)
{
    const UnitCombatQuery query = {
        sizeof(UnitCombatQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        TERRAIN_FOREST,
        property,
        {},
    };
    UnitCombatEvaluation evaluation{};
    UNSCOPED_INFO("combat evaluation failed");
    REQUIRE(EvaluateUnitCombat(&query,
                               &evaluation,
                               sizeof(evaluation)) ==
            UNIT_COMBAT_RULES_OK);
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(offsetof(UnitCombatRule, civilization) == 132);
    static_assert(offsetof(UnitCombatRule, terrain) == 144);
    static_assert(offsetof(UnitCombatRule, percentageDelta) == 152);
    static_assert(offsetof(UnitCombatRuleInfo, statusFlags) == 156);
    static_assert(offsetof(UnitCombatEvaluation, finalPercent) == 8);
    UNSCOPED_INFO("combat ABI version mismatch");
    REQUIRE(UnitCombatRulesAbiVersion() ==
            UNIT_COMBAT_RULES_ABI_VERSION);

    rerevved::unit_combat_rules::ResetForTests();
    UNSCOPED_INFO("null combat rule accepted");
    REQUIRE(RegisterUnitCombatRule(nullptr) ==
            UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT);

    auto invalid    = makeRule("test.provider",
                               "invalid",
                               UNIT_COMBAT_ATTACK,
                               1);
    invalid.terrain = TERRAIN_PLAINS;
    UNSCOPED_INFO("non-Forest combat rule accepted");
    REQUIRE(RegisterUnitCombatRule(&invalid) ==
            UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT);

    invalid             = makeRule("test.provider",
                                   "invalid",
                                   UNIT_COMBAT_ATTACK,
                                   1);
    invalid.reserved[0] = 1;
    UNSCOPED_INFO("nonzero combat reserved field accepted");
    REQUIRE(RegisterUnitCombatRule(&invalid) ==
            UNIT_COMBAT_RULES_ERR_INVALID_ARGUMENT);
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_combat_rules::ResetForTests();
    const auto later   = makeRule("z.provider",
                                  "late",
                                  UNIT_COMBAT_ATTACK,
                                  -25);
    auto       earlier = makeRule("a.provider",
                                  "early",
                                  UNIT_COMBAT_ATTACK,
                                  50);
    UNSCOPED_INFO("combat rules did not register");
    REQUIRE((RegisterUnitCombatRule(&later) ==
                 UNIT_COMBAT_RULES_OK &&
             RegisterUnitCombatRule(&earlier) ==
                 UNIT_COMBAT_RULES_OK));
    UNSCOPED_INFO("idempotent combat registration failed");
    REQUIRE(RegisterUnitCombatRule(&earlier) ==
            UNIT_COMBAT_RULES_OK);

    earlier.percentageDelta = 40;
    UNSCOPED_INFO("conflicting combat rule id accepted");
    REQUIRE(RegisterUnitCombatRule(&earlier) ==
            UNIT_COMBAT_RULES_ERR_DUPLICATE_RULE_ID);

    uint32_t count = 0;
    UNSCOPED_INFO("combat rule count mismatch");
    REQUIRE((GetUnitCombatRuleCount(&count) ==
                 UNIT_COMBAT_RULES_OK &&
             count == 2));
    UnitCombatRuleInfo info{};
    UNSCOPED_INFO("combat readback ordering mismatch");
    REQUIRE((GetUnitCombatRule(0, &info, sizeof(info)) ==
                 UNIT_COMBAT_RULES_OK &&
             std::string_view(info.providerId) == "a.provider" &&
             info.terrain == TERRAIN_FOREST &&
             info.percentageDelta == 50));

    const auto evaluation = evaluate();
    UNSCOPED_INFO("combat additive composition mismatch");
    REQUIRE((evaluation.nativePercent == 100 &&
             evaluation.finalPercent == 125 &&
             evaluation.additiveCount == 2 && evaluation.statusFlags == 0));

    const auto defense = evaluate(UNIT_COMBAT_DEFENSE);
    UNSCOPED_INFO("combat property target leaked to defense");
    REQUIRE((defense.finalPercent == defense.nativePercent &&
             defense.additiveCount == 0));
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_combat_rules::ResetForTests();
    const auto overflow = makeRule("a.provider",
                                   "overflow",
                                   UNIT_COMBAT_ATTACK,
                                   std::numeric_limits<int32_t>::max());
    UNSCOPED_INFO("combat overflow rule did not register");
    REQUIRE(RegisterUnitCombatRule(&overflow) ==
            UNIT_COMBAT_RULES_OK);
    const auto evaluation = evaluate();
    UNSCOPED_INFO("combat overflow did not preserve native percent");
    REQUIRE((evaluation.finalPercent == evaluation.nativePercent &&
             (evaluation.statusFlags &
              UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0));

    rerevved::unit_combat_rules::ResetForTests();
    const auto nonpositive = makeRule("a.provider",
                                      "nonpositive",
                                      UNIT_COMBAT_ATTACK,
                                      -100);
    UNSCOPED_INFO("nonpositive combat rule did not register");
    REQUIRE(RegisterUnitCombatRule(&nonpositive) ==
            UNIT_COMBAT_RULES_OK);
    const auto invalid = evaluate();
    UNSCOPED_INFO("nonpositive combat percentage did not preserve native percent");
    REQUIRE((invalid.finalPercent == invalid.nativePercent &&
             (invalid.statusFlags &
              UNIT_COMBAT_EVALUATION_OUT_OF_RANGE) != 0));

    const UnitCombatQuery query = {
        sizeof(UnitCombatQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        TERRAIN_FOREST,
        UNIT_COMBAT_ATTACK,
        {},
    };
    UnitCombatEvaluation output{};
    UNSCOPED_INFO("short combat evaluation output accepted");
    REQUIRE(EvaluateUnitCombat(&query, &output, 19) ==
            UNIT_COMBAT_RULES_ERR_BUFFER_TOO_SMALL);
    std::memset(&output, 0x5a, sizeof(output));
    UNSCOPED_INFO("combat minimum evaluation prefix rejected");
    REQUIRE((EvaluateUnitCombat(&query, &output, 20) ==
                 UNIT_COMBAT_RULES_OK &&
             output.structSize == sizeof(output)));
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        UNSCOPED_INFO("combat minimum prefix overwrote caller tail");
        REQUIRE(bytes[index] == 0x5a);
    }
}

} // namespace

TEST_CASE("unit_combat_rules_contract", "[ctest][unit-combat-rules][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestLayoutAndValidation();
    TestRegistrationReadbackAndEvaluation();
    TestOverflowAndSizedOutput();
}

TEST_CASE("unit_combat_rules_c_header", "[ctest][unit-combat-rules][local-fast]")
{
    const int status = rerevved_unit_combat_rules_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}
