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

ReRevvedUnitMovementRule makeRule(const char* provider,
                                  const char* ruleId,
                                  int32_t     value)
{
    ReRevvedUnitMovementRule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization = REREVVED_CIVILIZATION_AZTEC;
    rule.baseUnitType = REREVVED_UNIT_TYPE_WARRIOR;
    rule.identity     = REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.value        = value;
    return rule;
}

ReRevvedUnitMovementEvaluation evaluate(int32_t nativeValue)
{
    const ReRevvedUnitMovementQuery query = {
        sizeof(ReRevvedUnitMovementQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        nativeValue,
        {},
    };
    ReRevvedUnitMovementEvaluation evaluation{};
    require(ReRevvedEvaluateUnitMovement(
                &query, &evaluation, sizeof(evaluation)) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK,
            "movement evaluation failed");
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(sizeof(ReRevvedUnitMovementRule) == 168);
    static_assert(offsetof(ReRevvedUnitMovementRule, civilization) == 132);
    static_assert(offsetof(ReRevvedUnitMovementRule, value) == 144);
    static_assert(sizeof(ReRevvedUnitMovementRuleInfo) == 192);
    static_assert(offsetof(ReRevvedUnitMovementRuleInfo, statusFlags) == 148);
    static_assert(sizeof(ReRevvedUnitMovementQuery) == 40);
    static_assert(sizeof(ReRevvedUnitMovementEvaluation) == 40);
    static_assert(offsetof(ReRevvedUnitMovementEvaluation, finalValue) == 8);
    require(ReRevvedUnitMovementRulesAbiVersion() ==
                REREVVED_UNIT_MOVEMENT_RULES_ABI_VERSION,
            "movement ABI version mismatch");

    rerevved::unit_movement_rules::ResetForTests();
    require(ReRevvedRegisterUnitMovementRule(nullptr) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "null movement rule accepted");
    auto invalid     = makeRule("test.provider", "invalid", 1);
    invalid.identity = REREVVED_UNIT_IDENTITY_BASE;
    require(ReRevvedRegisterUnitMovementRule(&invalid) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "base identity movement rule accepted");
    invalid             = makeRule("test.provider", "invalid", 1);
    invalid.reserved[0] = 1;
    require(ReRevvedRegisterUnitMovementRule(&invalid) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "nonzero movement reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_movement_rules::ResetForTests();
    auto later   = makeRule("z.provider", "late", -2);
    auto earlier = makeRule("a.provider", "early", 3);
    require(ReRevvedRegisterUnitMovementRule(&later) ==
                    REREVVED_UNIT_MOVEMENT_RULES_OK &&
                ReRevvedRegisterUnitMovementRule(&earlier) ==
                    REREVVED_UNIT_MOVEMENT_RULES_OK,
            "movement rules did not register");
    require(ReRevvedRegisterUnitMovementRule(&earlier) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK,
            "idempotent movement registration failed");
    earlier.value = 4;
    require(ReRevvedRegisterUnitMovementRule(&earlier) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting movement rule id accepted");

    uint32_t count = 0;
    require(ReRevvedGetUnitMovementRuleCount(&count) ==
                    REREVVED_UNIT_MOVEMENT_RULES_OK &&
                count == 2,
            "movement rule count mismatch");
    ReRevvedUnitMovementRuleInfo info{};
    require(ReRevvedGetUnitMovementRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIT_MOVEMENT_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.value == 3,
            "movement readback ordering mismatch");

    const auto evaluation = evaluate(10);
    require(evaluation.nativeValue == 10 && evaluation.finalValue == 11 &&
                evaluation.additiveCount == 2 && evaluation.statusFlags == 0,
            "movement additive composition mismatch");

    const ReRevvedUnitMovementQuery impiQuery = {
        sizeof(ReRevvedUnitMovementQuery),
        REREVVED_CIVILIZATION_ZULU,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_IMPI_WARRIOR,
        10,
        {},
    };
    ReRevvedUnitMovementEvaluation impi{};
    require(ReRevvedEvaluateUnitMovement(
                &impiQuery, &impi, sizeof(impi)) ==
                    REREVVED_UNIT_MOVEMENT_RULES_OK &&
                impi.finalValue == 10 && impi.additiveCount == 0,
            "movement identity target leaked to control unit");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_movement_rules::ResetForTests();
    auto overflow = makeRule("a.provider", "overflow", 1);
    require(ReRevvedRegisterUnitMovementRule(&overflow) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK,
            "movement overflow rule did not register");
    const auto evaluation = evaluate(std::numeric_limits<int32_t>::max());
    require(evaluation.finalValue == evaluation.nativeValue &&
                (evaluation.statusFlags &
                 REREVVED_UNIT_MOVEMENT_RULE_EVALUATION_OVERFLOW) != 0,
            "movement overflow did not preserve native value");

    const ReRevvedUnitMovementQuery query = {
        sizeof(ReRevvedUnitMovementQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        4,
        {},
    };
    ReRevvedUnitMovementEvaluation output{};
    require(ReRevvedEvaluateUnitMovement(&query, &output, 19) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL,
            "short movement evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    require(ReRevvedEvaluateUnitMovement(&query, &output, 20) ==
                    REREVVED_UNIT_MOVEMENT_RULES_OK &&
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
