#include "unique_unit_rules_registry.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

ReRevvedUniqueUnitScalarRule MakeRule(
    const char*                       provider,
    const char*                       rule_id,
    ReRevvedCivilizationId            civilization,
    ReRevvedUnitTypeId                base_unit_type,
    ReRevvedUnitIdentityId            identity,
    ReRevvedUniqueUnitScalarProperty  property,
    ReRevvedUniqueUnitScalarOperation operation,
    int32_t                           value)
{
    ReRevvedUniqueUnitScalarRule rule{};
    rule.struct_size    = sizeof(rule);
    rule.civilization   = civilization;
    rule.base_unit_type = base_unit_type;
    rule.identity       = identity;
    rule.property       = property;
    rule.operation      = operation;
    rule.value          = value;
    std::snprintf(rule.provider_id, sizeof(rule.provider_id), "%s", provider);
    std::snprintf(rule.rule_id, sizeof(rule.rule_id), "%s", rule_id);
    return rule;
}

ReRevvedUniqueUnitScalarRule CataphractRule(
    const char*                       provider,
    const char*                       rule_id,
    ReRevvedUniqueUnitScalarOperation operation,
    int32_t                           value)
{
    return MakeRule(provider,
                    rule_id,
                    REREVVED_CIVILIZATION_ROMAN,
                    REREVVED_UNIT_TYPE_KNIGHTS,
                    REREVVED_UNIT_IDENTITY_CATAPHRACT,
                    REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                    operation,
                    value);
}

ReRevvedUniqueUnitScalarEvaluation Evaluate(
    ReRevvedCivilizationId           civilization,
    ReRevvedUnitTypeId               base_unit_type,
    ReRevvedUnitIdentityId           identity,
    ReRevvedUniqueUnitScalarProperty property,
    int32_t                          native_value)
{
    const ReRevvedUniqueUnitScalarQuery query = {
        sizeof(ReRevvedUniqueUnitScalarQuery),
        civilization,
        base_unit_type,
        identity,
        property,
        native_value,
        {},
    };
    ReRevvedUniqueUnitScalarEvaluation evaluation{};
    Require(ReRevvedEvaluateUniqueUnitScalar(
                &query, &evaluation, sizeof(evaluation)) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "evaluation succeeds");
    return evaluation;
}

void TestLayout()
{
    static_assert(sizeof(ReRevvedUniqueUnitScalarRule) == 176);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, provider_id) == 4);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, rule_id) == 68);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, civilization) == 132);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, value) == 152);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, reserved) == 156);
    static_assert(sizeof(ReRevvedUniqueUnitScalarRuleInfo) == 192);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRuleInfo, status_flags) ==
                  156);
    static_assert(sizeof(ReRevvedUniqueUnitScalarQuery) == 40);
    static_assert(sizeof(ReRevvedUniqueUnitScalarEvaluation) == 40);
    static_assert(offsetof(ReRevvedUniqueUnitScalarEvaluation, status_flags) ==
                  12);
    Require(ReRevvedUniqueUnitRulesAbiVersion() ==
                REREVVED_UNIQUE_UNIT_RULES_ABI_VERSION,
            "ABI version");
}

void TestValidation()
{
    rerevved::unique_unit_rules::ResetForTests();
    Require(ReRevvedRegisterUniqueUnitScalarRule(nullptr) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "null rule rejected");

    auto rule        = CataphractRule("aeshur.cataphracts-test",
                                      "cataphract-attack",
                                      REREVVED_UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    rule.struct_size = sizeof(rule) - 1;
    Require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "short rule rejected");

    rule = CataphractRule("Aeshur", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "invalid provider rejected");

    rule = CataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    std::memset(rule.rule_id, 'a', sizeof(rule.rule_id));
    Require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "unterminated rule ID rejected");

    rule          = CataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.identity = REREVVED_UNIT_IDENTITY_BASE;
    Require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "base identity rejected");

    rule              = CataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.civilization = REREVVED_CIVILIZATION_JAPANESE;
    Require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "mismatched identity rejected");

    rule          = CataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.property = 2;
    Require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "invalid property rejected");

    rule = CataphractRule("aeshur.cataphracts-test", "rule", 2, 50);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "invalid operation rejected");

    rule             = CataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.reserved[2] = 1;
    Require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "nonzero reserved field rejected");

    uint32_t count = 0;
    Require(ReRevvedGetUniqueUnitScalarRuleCount(nullptr) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "null count rejected");
    Require(ReRevvedGetUniqueUnitScalarRuleCount(&count) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                count == 0,
            "empty registry count");
}

void TestRegistrationAndReadback()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto z = CataphractRule("z.provider", "z-rule", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 2);
    auto a = CataphractRule("a.provider", "a-rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&z) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "first registration");
    Require(ReRevvedRegisterUniqueUnitScalarRule(&a) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "second registration");
    Require(ReRevvedRegisterUniqueUnitScalarRule(&a) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "identical registration is idempotent");

    a.value = 49;
    Require(ReRevvedRegisterUniqueUnitScalarRule(&a) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_DUPLICATE_RULE_ID,
            "changed duplicate rejected");

    uint32_t count = 0;
    Require(ReRevvedGetUniqueUnitScalarRuleCount(&count) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                count == 2,
            "registry count");

    ReRevvedUniqueUnitScalarRuleInfo info{};
    Require(ReRevvedGetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "read first rule");
    Require(std::string_view(info.provider_id) == "a.provider" &&
                std::string_view(info.rule_id) == "a-rule" && info.value == 50,
            "canonical readback order and copied data");
    Require(ReRevvedGetUniqueUnitScalarRule(2, &info, sizeof(info)) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "readback bounds");
    Require(ReRevvedGetUniqueUnitScalarRule(0, &info, 159) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL,
            "readback short buffer");

    std::memset(&info, 0x5a, sizeof(info));
    Require(ReRevvedGetUniqueUnitScalarRule(0, &info, 160) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "readback prefix accepted");
    const auto* info_bytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 160; index < sizeof(info); ++index)
    {
        Require(info_bytes[index] == 0x5a,
                "readback does not overwrite caller tail");
    }
}

void TestComposition()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto replacement = CataphractRule("aeshur.cataphracts-test",
                                      "cataphract-attack",
                                      REREVVED_UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&replacement) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "replacement registration");

    auto evaluation = Evaluate(REREVVED_CIVILIZATION_ROMAN,
                               REREVVED_UNIT_TYPE_KNIGHTS,
                               REREVVED_UNIT_IDENTITY_CATAPHRACT,
                               REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                               4);
    Require(evaluation.native_value == 4 && evaluation.final_value == 50 &&
                evaluation.replacement_count == 1 &&
                evaluation.additive_count == 0 && evaluation.status_flags == 0,
            "Cataphract replacement");

    evaluation = Evaluate(REREVVED_CIVILIZATION_ROMAN,
                          REREVVED_UNIT_TYPE_KNIGHTS,
                          REREVVED_UNIT_IDENTITY_CATAPHRACT,
                          REREVVED_UNIQUE_UNIT_SCALAR_BASE_DEFENSE,
                          2);
    Require(evaluation.final_value == 2 &&
                evaluation.replacement_count == 0,
            "defense unchanged");

    auto add_two = CataphractRule(
        "b.provider", "add-two", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 2);
    auto sub_one = CataphractRule(
        "c.provider", "sub-one", REREVVED_UNIQUE_UNIT_SCALAR_ADD, -1);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&add_two) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&sub_one) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK,
            "additive registrations");
    evaluation = Evaluate(REREVVED_CIVILIZATION_ROMAN,
                          REREVVED_UNIT_TYPE_KNIGHTS,
                          REREVVED_UNIT_IDENTITY_CATAPHRACT,
                          REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                          4);
    Require(evaluation.final_value == 51 && evaluation.additive_count == 2,
            "replacement plus additions");

    auto second_replacement = CataphractRule(
        "d.provider", "replacement", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 60);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&second_replacement) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "conflicting replacement registration");
    evaluation = Evaluate(REREVVED_CIVILIZATION_ROMAN,
                          REREVVED_UNIT_TYPE_KNIGHTS,
                          REREVVED_UNIT_IDENTITY_CATAPHRACT,
                          REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                          4);
    Require(evaluation.final_value == 5 &&
                evaluation.replacement_count == 2 &&
                (evaluation.status_flags &
                 REREVVED_UNIQUE_UNIT_EVALUATION_REPLACEMENT_CONFLICT) != 0,
            "replacement conflict preserves native baseline plus additions");

    ReRevvedUniqueUnitScalarRuleInfo info{};
    Require(ReRevvedGetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                (info.status_flags &
                 REREVVED_UNIQUE_UNIT_RULE_REPLACEMENT_CONFLICT) != 0,
            "replacement conflict readback");
}

void TestRegistrationOrderIndependence()
{
    auto replacement = CataphractRule("b.provider",
                                      "replacement",
                                      REREVVED_UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    auto add_two     = CataphractRule(
        "a.provider", "add-two", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 2);
    auto sub_one = CataphractRule(
        "c.provider", "sub-one", REREVVED_UNIQUE_UNIT_SCALAR_ADD, -1);

    rerevved::unique_unit_rules::ResetForTests();
    Require(ReRevvedRegisterUniqueUnitScalarRule(&replacement) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&add_two) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&sub_one) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK,
            "forward-order registrations");
    const auto forward = Evaluate(
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        4);

    rerevved::unique_unit_rules::ResetForTests();
    Require(ReRevvedRegisterUniqueUnitScalarRule(&sub_one) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&add_two) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&replacement) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK,
            "reverse-order registrations");
    const auto reverse = Evaluate(
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        4);
    Require(forward.final_value == 51 &&
                reverse.final_value == forward.final_value &&
                reverse.status_flags == forward.status_flags,
            "registration order does not change composition");
}

void TestCopiedInputAndConcurrentAccess()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto replacement = CataphractRule("a.provider",
                                      "replacement",
                                      REREVVED_UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&replacement) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "copied-input registration");

    replacement.provider_id[0] = 'z';
    replacement.value          = 4;
    ReRevvedUniqueUnitScalarRuleInfo info{};
    Require(ReRevvedGetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                std::string_view(info.provider_id) == "a.provider" &&
                info.value == 50,
            "registry owns a normalized rule copy");

    const ReRevvedUniqueUnitScalarQuery query = {
        sizeof(ReRevvedUniqueUnitScalarQuery),
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        4,
        {},
    };
    std::atomic<bool> start{ false };
    std::atomic<bool> writer_done{ false };
    std::atomic<bool> failed{ false };

    auto read_registry = [&]
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        do
        {
            ReRevvedUniqueUnitScalarEvaluation evaluation{};
            const int32_t                      result = ReRevvedEvaluateUniqueUnitScalar(
                &query, &evaluation, sizeof(evaluation));
            const int32_t additions = evaluation.final_value - 50;
            if (result != REREVVED_UNIQUE_UNIT_RULES_OK || additions < 0 ||
                additions > 16 ||
                evaluation.additive_count !=
                    static_cast<uint32_t>(additions) ||
                evaluation.replacement_count != 1 ||
                evaluation.status_flags != 0)
            {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield();
        } while (!writer_done.load(std::memory_order_acquire));
    };

    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index)
    {
        readers.emplace_back(read_registry);
    }
    start.store(true, std::memory_order_release);
    for (int index = 0; index < 16; ++index)
    {
        char provider[32]{};
        std::snprintf(provider, sizeof(provider), "writer.%02d", index);
        auto addition = CataphractRule(
            provider, "add-one", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 1);
        if (ReRevvedRegisterUniqueUnitScalarRule(&addition) !=
            REREVVED_UNIQUE_UNIT_RULES_OK)
        {
            failed.store(true, std::memory_order_relaxed);
            break;
        }
    }
    writer_done.store(true, std::memory_order_release);
    for (auto& reader : readers)
    {
        reader.join();
    }

    const auto final = Evaluate(REREVVED_CIVILIZATION_ROMAN,
                                REREVVED_UNIT_TYPE_KNIGHTS,
                                REREVVED_UNIT_IDENTITY_CATAPHRACT,
                                REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                                4);
    Require(!failed.load(std::memory_order_relaxed) &&
                final.final_value == 66 && final.additive_count == 16,
            "registration and evaluation remain atomic across threads");
}

void TestOverflowAndQueryValidation()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto add = CataphractRule(
        "a.provider", "overflow", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 1);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&add) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "overflow rule registration");
    const auto evaluation = Evaluate(
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        std::numeric_limits<int32_t>::max());
    Require(evaluation.final_value == std::numeric_limits<int32_t>::max() &&
                (evaluation.status_flags &
                 REREVVED_UNIQUE_UNIT_EVALUATION_OVERFLOW) != 0,
            "positive overflow falls back");

    rerevved::unique_unit_rules::ResetForTests();
    add = CataphractRule(
        "a.provider", "underflow", REREVVED_UNIQUE_UNIT_SCALAR_ADD, -1);
    Require(ReRevvedRegisterUniqueUnitScalarRule(&add) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "underflow rule registration");
    const auto underflow = Evaluate(
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        std::numeric_limits<int32_t>::min());
    Require(underflow.final_value == std::numeric_limits<int32_t>::min() &&
                (underflow.status_flags &
                 REREVVED_UNIQUE_UNIT_EVALUATION_OVERFLOW) != 0,
            "negative overflow falls back");

    ReRevvedUniqueUnitScalarQuery query{};
    query.struct_size    = sizeof(query);
    query.civilization   = REREVVED_CIVILIZATION_ROMAN;
    query.base_unit_type = REREVVED_UNIT_TYPE_KNIGHTS;
    query.identity       = REREVVED_UNIT_IDENTITY_CATAPHRACT;
    query.property       = REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK;
    ReRevvedUniqueUnitScalarEvaluation out{};
    Require(ReRevvedEvaluateUniqueUnitScalar(&query, nullptr, sizeof(out)) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "null evaluation output rejected");
    Require(ReRevvedEvaluateUniqueUnitScalar(&query, &out, 23) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL,
            "short evaluation output rejected");
    query.reserved[0] = 1;
    Require(ReRevvedEvaluateUniqueUnitScalar(&query, &out, sizeof(out)) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "query reserved field rejected");
}

} // namespace

int main()
{
    TestLayout();
    TestValidation();
    TestRegistrationAndReadback();
    TestComposition();
    TestRegistrationOrderIndependence();
    TestCopiedInputAndConcurrentAccess();
    TestOverflowAndQueryValidation();
    return 0;
}
