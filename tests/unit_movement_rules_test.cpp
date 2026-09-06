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

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

ReRevvedUnitMovementRule MakeRule(const char* provider,
                                  const char* rule_id,
                                  int32_t     value)
{
    ReRevvedUnitMovementRule rule{};
    rule.struct_size = sizeof(rule);
    std::memcpy(rule.provider_id, provider, std::strlen(provider) + 1);
    std::memcpy(rule.rule_id, rule_id, std::strlen(rule_id) + 1);
    rule.civilization   = REREVVED_CIVILIZATION_AZTEC;
    rule.base_unit_type = REREVVED_UNIT_TYPE_WARRIOR;
    rule.identity       = REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.value          = value;
    return rule;
}

ReRevvedUnitMovementEvaluation Evaluate(int32_t native_value)
{
    const ReRevvedUnitMovementQuery query = {
        sizeof(ReRevvedUnitMovementQuery),
        REREVVED_CIVILIZATION_AZTEC,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_JAGUAR_WARRIOR,
        native_value,
        {},
    };
    ReRevvedUnitMovementEvaluation evaluation{};
    Require(ReRevvedEvaluateUnitMovement(
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
    static_assert(offsetof(ReRevvedUnitMovementRuleInfo, status_flags) == 148);
    static_assert(sizeof(ReRevvedUnitMovementQuery) == 40);
    static_assert(sizeof(ReRevvedUnitMovementEvaluation) == 40);
    static_assert(offsetof(ReRevvedUnitMovementEvaluation, final_value) == 8);
    Require(ReRevvedUnitMovementRulesAbiVersion() ==
                REREVVED_UNIT_MOVEMENT_RULES_ABI_VERSION,
            "movement ABI version mismatch");

    rerevved::unit_movement_rules::ResetForTests();
    Require(ReRevvedRegisterUnitMovementRule(nullptr) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "null movement rule accepted");
    auto invalid = MakeRule("test.provider", "invalid", 1);
    invalid.identity = REREVVED_UNIT_IDENTITY_BASE;
    Require(ReRevvedRegisterUnitMovementRule(&invalid) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "base identity movement rule accepted");
    invalid = MakeRule("test.provider", "invalid", 1);
    invalid.reserved[0] = 1;
    Require(ReRevvedRegisterUnitMovementRule(&invalid) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_INVALID_ARGUMENT,
            "nonzero movement reserved field accepted");
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_movement_rules::ResetForTests();
    auto later  = MakeRule("z.provider", "late", -2);
    auto earlier = MakeRule("a.provider", "early", 3);
    Require(ReRevvedRegisterUnitMovementRule(&later) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK &&
                ReRevvedRegisterUnitMovementRule(&earlier) ==
                    REREVVED_UNIT_MOVEMENT_RULES_OK,
            "movement rules did not register");
    Require(ReRevvedRegisterUnitMovementRule(&earlier) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK,
            "idempotent movement registration failed");
    earlier.value = 4;
    Require(ReRevvedRegisterUnitMovementRule(&earlier) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_DUPLICATE_RULE_ID,
            "conflicting movement rule id accepted");

    uint32_t count = 0;
    Require(ReRevvedGetUnitMovementRuleCount(&count) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK && count == 2,
            "movement rule count mismatch");
    ReRevvedUnitMovementRuleInfo info{};
    Require(ReRevvedGetUnitMovementRule(0, &info, sizeof(info)) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK &&
                std::string_view(info.provider_id) == "a.provider" &&
                info.value == 3,
            "movement readback ordering mismatch");

    const auto evaluation = Evaluate(10);
    Require(evaluation.native_value == 10 && evaluation.final_value == 11 &&
                evaluation.additive_count == 2 && evaluation.status_flags == 0,
            "movement additive composition mismatch");

    const ReRevvedUnitMovementQuery impi_query = {
        sizeof(ReRevvedUnitMovementQuery),
        REREVVED_CIVILIZATION_ZULU,
        REREVVED_UNIT_TYPE_WARRIOR,
        REREVVED_UNIT_IDENTITY_IMPI_WARRIOR,
        10,
        {},
    };
    ReRevvedUnitMovementEvaluation impi{};
    Require(ReRevvedEvaluateUnitMovement(
                &impi_query, &impi, sizeof(impi)) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK &&
                impi.final_value == 10 && impi.additive_count == 0,
            "movement identity target leaked to control unit");
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_movement_rules::ResetForTests();
    auto overflow = MakeRule("a.provider", "overflow", 1);
    Require(ReRevvedRegisterUnitMovementRule(&overflow) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK,
            "movement overflow rule did not register");
    const auto evaluation = Evaluate(std::numeric_limits<int32_t>::max());
    Require(evaluation.final_value == evaluation.native_value &&
                (evaluation.status_flags &
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
    Require(ReRevvedEvaluateUnitMovement(&query, &output, 19) ==
                REREVVED_UNIT_MOVEMENT_RULES_ERR_BUFFER_TOO_SMALL,
            "short movement evaluation output accepted");
    std::memset(&output, 0x5a, sizeof(output));
    Require(ReRevvedEvaluateUnitMovement(&query, &output, 20) ==
                REREVVED_UNIT_MOVEMENT_RULES_OK && output.struct_size ==
                    sizeof(output),
            "movement minimum evaluation prefix rejected");
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        Require(bytes[index] == 0x5a,
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
