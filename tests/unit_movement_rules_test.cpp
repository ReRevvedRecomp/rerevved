#include "registry_scope.hpp"
#include "unit_movement_rules_registry.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

extern "C" int rerevved_unit_movement_rules_c_header_check(void);

namespace
{

UnitMovementRule makeRule(const char* provider,
                          const char* ruleId,
                          int32_t     value)
{
    UnitMovementRule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization = CIVILIZATION_AZTEC;
    rule.baseUnitType = UNIT_TYPE_WARRIOR;
    rule.identity     = UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.value        = value;
    return rule;
}

UnitMovementEvaluation evaluate(int32_t nativeValue)
{
    const UnitMovementQuery query = {
        sizeof(UnitMovementQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        nativeValue,
        {},
    };
    UnitMovementEvaluation evaluation{};
    UNSCOPED_INFO("movement evaluation failed");
    REQUIRE(EvaluateUnitMovement(
                &query, &evaluation, sizeof(evaluation)) ==
            UNIT_MOVEMENT_RULES_OK);
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(offsetof(UnitMovementRule, civilization) == 132);
    static_assert(offsetof(UnitMovementRule, value) == 144);
    static_assert(offsetof(UnitMovementRuleInfo, statusFlags) == 148);
    static_assert(offsetof(UnitMovementEvaluation, finalValue) == 8);
    UNSCOPED_INFO("movement ABI version mismatch");
    REQUIRE(UnitMovementRulesAbiVersion() ==
            UNIT_MOVEMENT_RULES_ABI_VERSION);

    rerevved::unit_movement_rules::ResetForTests();
    UNSCOPED_INFO("null movement rule accepted");
    REQUIRE(RegisterUnitMovementRule(nullptr) ==
            UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT);
    auto invalid     = makeRule("test.provider", "invalid", 1);
    invalid.identity = UNIT_IDENTITY_BASE;
    UNSCOPED_INFO("base identity movement rule accepted");
    REQUIRE(RegisterUnitMovementRule(&invalid) ==
            UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT);
    invalid             = makeRule("test.provider", "invalid", 1);
    invalid.reserved[0] = 1;
    UNSCOPED_INFO("nonzero movement reserved field accepted");
    REQUIRE(RegisterUnitMovementRule(&invalid) ==
            UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT);
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_movement_rules::ResetForTests();
    auto later   = makeRule("z.provider", "late", -2);
    auto earlier = makeRule("a.provider", "early", 3);
    UNSCOPED_INFO("movement rules did not register");
    REQUIRE((RegisterUnitMovementRule(&later) ==
                 UNIT_MOVEMENT_RULES_OK &&
             RegisterUnitMovementRule(&earlier) ==
                 UNIT_MOVEMENT_RULES_OK));
    UNSCOPED_INFO("idempotent movement registration failed");
    REQUIRE(RegisterUnitMovementRule(&earlier) ==
            UNIT_MOVEMENT_RULES_OK);
    earlier.value = 4;
    UNSCOPED_INFO("conflicting movement rule id accepted");
    REQUIRE(RegisterUnitMovementRule(&earlier) ==
            UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID);

    uint32_t count = 0;
    UNSCOPED_INFO("movement rule count mismatch");
    REQUIRE((GetUnitMovementRuleCount(&count) ==
                 UNIT_MOVEMENT_RULES_OK &&
             count == 2));
    UnitMovementRuleInfo info{};
    UNSCOPED_INFO("movement readback ordering mismatch");
    REQUIRE((GetUnitMovementRule(0, &info, sizeof(info)) ==
                 UNIT_MOVEMENT_RULES_OK &&
             std::string_view(info.providerId) == "a.provider" &&
             info.value == 3));

    const auto evaluation = evaluate(10);
    UNSCOPED_INFO("movement additive composition mismatch");
    REQUIRE((evaluation.nativeValue == 10 && evaluation.finalValue == 11 &&
             evaluation.additiveCount == 2 && evaluation.statusFlags == 0));

    const UnitMovementQuery impiQuery = {
        sizeof(UnitMovementQuery),
        CIVILIZATION_ZULU,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_IMPI_WARRIOR,
        10,
        {},
    };
    UnitMovementEvaluation impi{};
    UNSCOPED_INFO("movement identity target leaked to control unit");
    REQUIRE((EvaluateUnitMovement(
                 &impiQuery, &impi, sizeof(impi)) ==
                 UNIT_MOVEMENT_RULES_OK &&
             impi.finalValue == 10 && impi.additiveCount == 0));
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_movement_rules::ResetForTests();
    auto overflow = makeRule("a.provider", "overflow", 1);
    UNSCOPED_INFO("movement overflow rule did not register");
    REQUIRE(RegisterUnitMovementRule(&overflow) ==
            UNIT_MOVEMENT_RULES_OK);
    const auto evaluation = evaluate(std::numeric_limits<int32_t>::max());
    UNSCOPED_INFO("movement overflow did not preserve native value");
    REQUIRE((evaluation.finalValue == evaluation.nativeValue &&
             (evaluation.statusFlags &
              UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW) != 0));

    const UnitMovementQuery query = {
        sizeof(UnitMovementQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        4,
        {},
    };
    UnitMovementEvaluation output{};
    UNSCOPED_INFO("short movement evaluation output accepted");
    REQUIRE(EvaluateUnitMovement(&query, &output, 19) ==
            UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL);
    std::memset(&output, 0x5a, sizeof(output));
    UNSCOPED_INFO("movement minimum evaluation prefix rejected");
    REQUIRE((EvaluateUnitMovement(&query, &output, 20) ==
                 UNIT_MOVEMENT_RULES_OK &&
             output.structSize ==
                 sizeof(output)));
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        UNSCOPED_INFO("movement minimum prefix overwrote caller tail");
        REQUIRE(bytes[index] == 0x5a);
    }
}

} // namespace

TEST_CASE("unit_movement_rules_contract", "[ctest][unit-movement-rules][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestLayoutAndValidation();
    TestRegistrationReadbackAndEvaluation();
    TestOverflowAndSizedOutput();
}

TEST_CASE("unit_movement_rules_c_header", "[ctest][unit-movement-rules][local-fast]")
{
    const int status = rerevved_unit_movement_rules_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}
