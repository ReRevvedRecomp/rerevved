#include "unit_movement_rules_registry.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
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
    require(EvaluateUnitMovement(
                &query, &evaluation, sizeof(evaluation)) ==
                UNIT_MOVEMENT_RULES_OK,
            "movement evaluation failed");
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(sizeof(UnitMovementRule) == 168);
    static_assert(offsetof(UnitMovementRule, civilization) == 132);
    static_assert(offsetof(UnitMovementRule, value) == 144);
    static_assert(sizeof(UnitMovementRuleInfo) == 192);
    static_assert(offsetof(UnitMovementRuleInfo, statusFlags) == 148);
    static_assert(sizeof(UnitMovementQuery) == 40);
    static_assert(sizeof(UnitMovementEvaluation) == 40);
    static_assert(offsetof(UnitMovementEvaluation, finalValue) == 8);
    require(UnitMovementRulesAbiVersion() ==
                UNIT_MOVEMENT_RULES_ABI_VERSION,
            "movement ABI version mismatch");

    rerevved::unit_movement_rules::ResetForTests();
    require(RegisterUnitMovementRule(nullptr) ==
                UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "null movement rule accepted");
    auto invalid     = makeRule("test.provider", "invalid", 1);
    invalid.identity = UNIT_IDENTITY_BASE;
    require(RegisterUnitMovementRule(&invalid) ==
                UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "base identity movement rule accepted");
    invalid             = makeRule("test.provider", "invalid", 1);
    invalid.reserved[0] = 1;
    require(RegisterUnitMovementRule(&invalid) ==
                UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "nonzero movement reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_movement_rules::ResetForTests();
    auto later   = makeRule("z.provider", "late", -2);
    auto earlier = makeRule("a.provider", "early", 3);
    require(RegisterUnitMovementRule(&later) ==
                    UNIT_MOVEMENT_RULES_OK &&
                RegisterUnitMovementRule(&earlier) ==
                    UNIT_MOVEMENT_RULES_OK,
            "movement rules did not register");
    require(RegisterUnitMovementRule(&earlier) ==
                UNIT_MOVEMENT_RULES_OK,
            "idempotent movement registration failed");
    earlier.value = 4;
    require(RegisterUnitMovementRule(&earlier) ==
                UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting movement rule id accepted");

    uint32_t count = 0;
    require(GetUnitMovementRuleCount(&count) ==
                    UNIT_MOVEMENT_RULES_OK &&
                count == 2,
            "movement rule count mismatch");
    UnitMovementRuleInfo info{};
    require(GetUnitMovementRule(0, &info, sizeof(info)) ==
                    UNIT_MOVEMENT_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.value == 3,
            "movement readback ordering mismatch");

    const auto evaluation = evaluate(10);
    require(evaluation.nativeValue == 10 && evaluation.finalValue == 11 &&
                evaluation.additiveCount == 2 && evaluation.statusFlags == 0,
            "movement additive composition mismatch");

    const UnitMovementQuery impiQuery = {
        sizeof(UnitMovementQuery),
        CIVILIZATION_ZULU,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_IMPI_WARRIOR,
        10,
        {},
    };
    UnitMovementEvaluation impi{};
    require(EvaluateUnitMovement(
                &impiQuery, &impi, sizeof(impi)) ==
                    UNIT_MOVEMENT_RULES_OK &&
                impi.finalValue == 10 && impi.additiveCount == 0,
            "movement identity target leaked to control unit");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_movement_rules::ResetForTests();
    auto overflow = makeRule("a.provider", "overflow", 1);
    require(RegisterUnitMovementRule(&overflow) ==
                UNIT_MOVEMENT_RULES_OK,
            "movement overflow rule did not register");
    const auto evaluation = evaluate(std::numeric_limits<int32_t>::max());
    require(evaluation.finalValue == evaluation.nativeValue &&
                (evaluation.statusFlags &
                 UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW) != 0,
            "movement overflow did not preserve native value");

    const UnitMovementQuery query = {
        sizeof(UnitMovementQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        4,
        {},
    };
    UnitMovementEvaluation output{};
    require(EvaluateUnitMovement(&query, &output, 19) ==
                UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL,
            "short movement evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    require(EvaluateUnitMovement(&query, &output, 20) ==
                    UNIT_MOVEMENT_RULES_OK &&
                output.structSize ==
                    sizeof(output),
            "movement minimum evaluation prefix rejected");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        require(bytes[index] == 0x5a,
                "movement minimum prefix overwrote caller tail");
    }
}

} // namespace

int main()
{
    TestLayoutAndValidation();
    TestRegistrationReadbackAndEvaluation();
    TestOverflowAndSizedOutput();
    return 0;
}
