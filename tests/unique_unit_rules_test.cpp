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

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

ReRevvedUniqueUnitScalarRule makeRule(
    const char*                       provider,
    const char*                       ruleId,
    ReRevvedCivilizationId            civilization,
    ReRevvedUnitTypeId                baseUnitType,
    ReRevvedUnitIdentityId            identity,
    ReRevvedUniqueUnitScalarProperty  property,
    ReRevvedUniqueUnitScalarOperation operation,
    int32_t                           value)
{
    ReRevvedUniqueUnitScalarRule rule{};
    rule.structSize   = sizeof(rule);
    rule.civilization = civilization;
    rule.baseUnitType = baseUnitType;
    rule.identity     = identity;
    rule.property     = property;
    rule.operation    = operation;
    rule.value        = value;
    std::snprintf(rule.providerId, sizeof(rule.providerId), "%s", provider);
    std::snprintf(rule.ruleId, sizeof(rule.ruleId), "%s", ruleId);
    return rule;
}

ReRevvedUniqueUnitScalarRule cataphractRule(
    const char*                       provider,
    const char*                       ruleId,
    ReRevvedUniqueUnitScalarOperation operation,
    int32_t                           value)
{
    return makeRule(provider,
                    ruleId,
                    REREVVED_CIVILIZATION_ROMAN,
                    REREVVED_UNIT_TYPE_KNIGHTS,
                    REREVVED_UNIT_IDENTITY_CATAPHRACT,
                    REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                    operation,
                    value);
}

ReRevvedUniqueUnitScalarEvaluation evaluate(
    ReRevvedCivilizationId           civilization,
    ReRevvedUnitTypeId               baseUnitType,
    ReRevvedUnitIdentityId           identity,
    ReRevvedUniqueUnitScalarProperty property,
    int32_t                          nativeValue)
{
    const ReRevvedUniqueUnitScalarQuery query = {
        sizeof(ReRevvedUniqueUnitScalarQuery),
        civilization,
        baseUnitType,
        identity,
        property,
        nativeValue,
        {},
    };
    ReRevvedUniqueUnitScalarEvaluation evaluation{};
    require(ReRevvedEvaluateUniqueUnitScalar(
                &query, &evaluation, sizeof(evaluation)) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "evaluation succeeds");
    return evaluation;
}

void TestLayout()
{
    static_assert(sizeof(ReRevvedUniqueUnitScalarRule) == 176);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, providerId) == 4);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, ruleId) == 68);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, civilization) == 132);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, value) == 152);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRule, reserved) == 156);
    static_assert(sizeof(ReRevvedUniqueUnitScalarRuleInfo) == 192);
    static_assert(offsetof(ReRevvedUniqueUnitScalarRuleInfo, statusFlags) ==
                  156);
    static_assert(sizeof(ReRevvedUniqueUnitScalarQuery) == 40);
    static_assert(sizeof(ReRevvedUniqueUnitScalarEvaluation) == 40);
    static_assert(offsetof(ReRevvedUniqueUnitScalarEvaluation, statusFlags) ==
                  12);
    require(ReRevvedUniqueUnitRulesAbiVersion() ==
                REREVVED_UNIQUE_UNIT_RULES_ABI_VERSION,
            "ABI version");
}

void TestValidation()
{
    rerevved::unique_unit_rules::ResetForTests();
    require(ReRevvedRegisterUniqueUnitScalarRule(nullptr) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "null rule rejected");

    auto rule       = cataphractRule("aeshur.cataphracts-test",
                                     "cataphract-attack",
                                     REREVVED_UNIQUE_UNIT_SCALAR_REPLACE,
                                     50);
    rule.structSize = sizeof(rule) - 1;
    require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "short rule rejected");

    rule = cataphractRule("Aeshur", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "invalid provider rejected");

    rule = cataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    std::memset(rule.ruleId, 'a', sizeof(rule.ruleId));
    require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "unterminated rule ID rejected");

    rule          = cataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.identity = REREVVED_UNIT_IDENTITY_BASE;
    require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "base identity rejected");

    rule              = cataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.civilization = REREVVED_CIVILIZATION_JAPANESE;
    require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "mismatched identity rejected");

    rule          = cataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.property = 2;
    require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "invalid property rejected");

    rule = cataphractRule("aeshur.cataphracts-test", "rule", 2, 50);
    require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "invalid operation rejected");

    rule             = cataphractRule("aeshur.cataphracts-test", "rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.reserved[2] = 1;
    require(ReRevvedRegisterUniqueUnitScalarRule(&rule) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "nonzero reserved field rejected");

    uint32_t count = 0;
    require(ReRevvedGetUniqueUnitScalarRuleCount(nullptr) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "null count rejected");
    require(ReRevvedGetUniqueUnitScalarRuleCount(&count) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                count == 0,
            "empty registry count");
}

void TestRegistrationAndReadback()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto z = cataphractRule("z.provider", "z-rule", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 2);
    auto a = cataphractRule("a.provider", "a-rule", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 50);
    require(ReRevvedRegisterUniqueUnitScalarRule(&z) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "first registration");
    require(ReRevvedRegisterUniqueUnitScalarRule(&a) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "second registration");
    require(ReRevvedRegisterUniqueUnitScalarRule(&a) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "identical registration is idempotent");

    a.value = 49;
    require(ReRevvedRegisterUniqueUnitScalarRule(&a) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_DUPLICATE_RULE_ID,
            "changed duplicate rejected");

    uint32_t count = 0;
    require(ReRevvedGetUniqueUnitScalarRuleCount(&count) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                count == 2,
            "registry count");

    ReRevvedUniqueUnitScalarRuleInfo info{};
    require(ReRevvedGetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "read first rule");
    require(std::string_view(info.providerId) == "a.provider" &&
                std::string_view(info.ruleId) == "a-rule" && info.value == 50,
            "canonical readback order and copied data");
    require(ReRevvedGetUniqueUnitScalarRule(2, &info, sizeof(info)) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "readback bounds");
    require(ReRevvedGetUniqueUnitScalarRule(0, &info, 159) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL,
            "readback short buffer");

    std::memset(&info, 0x5a, sizeof(info));
    require(ReRevvedGetUniqueUnitScalarRule(0, &info, 160) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "readback prefix accepted");
    const auto* infoBytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 160; index < sizeof(info); ++index)
    {
        require(infoBytes[index] == 0x5a,
                "readback does not overwrite caller tail");
    }
}

void TestComposition()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto replacement = cataphractRule("aeshur.cataphracts-test",
                                      "cataphract-attack",
                                      REREVVED_UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    require(ReRevvedRegisterUniqueUnitScalarRule(&replacement) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "replacement registration");

    auto evaluation = evaluate(REREVVED_CIVILIZATION_ROMAN,
                               REREVVED_UNIT_TYPE_KNIGHTS,
                               REREVVED_UNIT_IDENTITY_CATAPHRACT,
                               REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                               4);
    require(evaluation.nativeValue == 4 && evaluation.finalValue == 50 &&
                evaluation.replacementCount == 1 &&
                evaluation.additiveCount == 0 && evaluation.statusFlags == 0,
            "Cataphract replacement");

    evaluation = evaluate(REREVVED_CIVILIZATION_ROMAN,
                          REREVVED_UNIT_TYPE_KNIGHTS,
                          REREVVED_UNIT_IDENTITY_CATAPHRACT,
                          REREVVED_UNIQUE_UNIT_SCALAR_BASE_DEFENSE,
                          2);
    require(evaluation.finalValue == 2 &&
                evaluation.replacementCount == 0,
            "defense unchanged");

    auto addTwo = cataphractRule(
        "b.provider", "add-two", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 2);
    auto subOne = cataphractRule(
        "c.provider", "sub-one", REREVVED_UNIQUE_UNIT_SCALAR_ADD, -1);
    require(ReRevvedRegisterUniqueUnitScalarRule(&addTwo) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&subOne) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK,
            "additive registrations");
    evaluation = evaluate(REREVVED_CIVILIZATION_ROMAN,
                          REREVVED_UNIT_TYPE_KNIGHTS,
                          REREVVED_UNIT_IDENTITY_CATAPHRACT,
                          REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                          4);
    require(evaluation.finalValue == 51 && evaluation.additiveCount == 2,
            "replacement plus additions");

    auto secondReplacement = cataphractRule(
        "d.provider", "replacement", REREVVED_UNIQUE_UNIT_SCALAR_REPLACE, 60);
    require(ReRevvedRegisterUniqueUnitScalarRule(&secondReplacement) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "conflicting replacement registration");
    evaluation = evaluate(REREVVED_CIVILIZATION_ROMAN,
                          REREVVED_UNIT_TYPE_KNIGHTS,
                          REREVVED_UNIT_IDENTITY_CATAPHRACT,
                          REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                          4);
    require(evaluation.finalValue == 5 &&
                evaluation.replacementCount == 2 &&
                (evaluation.statusFlags &
                 REREVVED_UNIQUE_UNIT_EVALUATION_REPLACEMENT_CONFLICT) != 0,
            "replacement conflict preserves native baseline plus additions");

    ReRevvedUniqueUnitScalarRuleInfo info{};
    require(ReRevvedGetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                (info.statusFlags &
                 REREVVED_UNIQUE_UNIT_RULE_REPLACEMENT_CONFLICT) != 0,
            "replacement conflict readback");
}

void TestRegistrationOrderIndependence()
{
    auto replacement = cataphractRule("b.provider",
                                      "replacement",
                                      REREVVED_UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    auto addTwo      = cataphractRule(
        "a.provider", "add-two", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 2);
    auto subOne = cataphractRule(
        "c.provider", "sub-one", REREVVED_UNIQUE_UNIT_SCALAR_ADD, -1);

    rerevved::unique_unit_rules::ResetForTests();
    require(ReRevvedRegisterUniqueUnitScalarRule(&replacement) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&addTwo) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&subOne) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK,
            "forward-order registrations");
    const auto forward = evaluate(
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        4);

    rerevved::unique_unit_rules::ResetForTests();
    require(ReRevvedRegisterUniqueUnitScalarRule(&subOne) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&addTwo) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                ReRevvedRegisterUniqueUnitScalarRule(&replacement) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK,
            "reverse-order registrations");
    const auto reverse = evaluate(
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        4);
    require(forward.finalValue == 51 &&
                reverse.finalValue == forward.finalValue &&
                reverse.statusFlags == forward.statusFlags,
            "registration order does not change composition");
}

void TestCopiedInputAndConcurrentAccess()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto replacement = cataphractRule("a.provider",
                                      "replacement",
                                      REREVVED_UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    require(ReRevvedRegisterUniqueUnitScalarRule(&replacement) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "copied-input registration");

    replacement.providerId[0] = 'z';
    replacement.value         = 4;
    ReRevvedUniqueUnitScalarRuleInfo info{};
    require(ReRevvedGetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
                    REREVVED_UNIQUE_UNIT_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
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
    std::atomic<bool> writerDone{ false };
    std::atomic<bool> failed{ false };

    auto readRegistry = [&]
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
            const int32_t additions = evaluation.finalValue - 50;
            if (result != REREVVED_UNIQUE_UNIT_RULES_OK || additions < 0 ||
                additions > 16 ||
                evaluation.additiveCount !=
                    static_cast<uint32_t>(additions) ||
                evaluation.replacementCount != 1 ||
                evaluation.statusFlags != 0)
            {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield();
        } while (!writerDone.load(std::memory_order_acquire));
    };

    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index)
    {
        readers.emplace_back(readRegistry);
    }
    start.store(true, std::memory_order_release);
    for (int index = 0; index < 16; ++index)
    {
        char provider[32]{};
        std::snprintf(provider, sizeof(provider), "writer.%02d", index);
        auto addition = cataphractRule(
            provider, "add-one", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 1);
        if (ReRevvedRegisterUniqueUnitScalarRule(&addition) !=
            REREVVED_UNIQUE_UNIT_RULES_OK)
        {
            failed.store(true, std::memory_order_relaxed);
            break;
        }
    }
    writerDone.store(true, std::memory_order_release);
    for (auto& reader : readers)
    {
        reader.join();
    }

    const auto final = evaluate(REREVVED_CIVILIZATION_ROMAN,
                                REREVVED_UNIT_TYPE_KNIGHTS,
                                REREVVED_UNIT_IDENTITY_CATAPHRACT,
                                REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                                4);
    require(!failed.load(std::memory_order_relaxed) &&
                final.finalValue == 66 && final.additiveCount == 16,
            "registration and evaluation remain atomic across threads");
}

void TestOverflowAndQueryValidation()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto add = cataphractRule(
        "a.provider", "overflow", REREVVED_UNIQUE_UNIT_SCALAR_ADD, 1);
    require(ReRevvedRegisterUniqueUnitScalarRule(&add) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "overflow rule registration");
    const auto evaluation = evaluate(
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        std::numeric_limits<int32_t>::max());
    require(evaluation.finalValue == std::numeric_limits<int32_t>::max() &&
                (evaluation.statusFlags &
                 REREVVED_UNIQUE_UNIT_EVALUATION_OVERFLOW) != 0,
            "positive overflow falls back");

    rerevved::unique_unit_rules::ResetForTests();
    add = cataphractRule(
        "a.provider", "underflow", REREVVED_UNIQUE_UNIT_SCALAR_ADD, -1);
    require(ReRevvedRegisterUniqueUnitScalarRule(&add) ==
                REREVVED_UNIQUE_UNIT_RULES_OK,
            "underflow rule registration");
    const auto underflow = evaluate(
        REREVVED_CIVILIZATION_ROMAN,
        REREVVED_UNIT_TYPE_KNIGHTS,
        REREVVED_UNIT_IDENTITY_CATAPHRACT,
        REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        std::numeric_limits<int32_t>::min());
    require(underflow.finalValue == std::numeric_limits<int32_t>::min() &&
                (underflow.statusFlags &
                 REREVVED_UNIQUE_UNIT_EVALUATION_OVERFLOW) != 0,
            "negative overflow falls back");

    ReRevvedUniqueUnitScalarQuery query{};
    query.structSize   = sizeof(query);
    query.civilization = REREVVED_CIVILIZATION_ROMAN;
    query.baseUnitType = REREVVED_UNIT_TYPE_KNIGHTS;
    query.identity     = REREVVED_UNIT_IDENTITY_CATAPHRACT;
    query.property     = REREVVED_UNIQUE_UNIT_SCALAR_BASE_ATTACK;
    ReRevvedUniqueUnitScalarEvaluation out{};
    require(ReRevvedEvaluateUniqueUnitScalar(&query, nullptr, sizeof(out)) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT,
            "null evaluation output rejected");
    require(ReRevvedEvaluateUniqueUnitScalar(&query, &out, 23) ==
                REREVVED_UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL,
            "short evaluation output rejected");
    query.reserved[0] = 1;
    require(ReRevvedEvaluateUniqueUnitScalar(&query, &out, sizeof(out)) ==
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
