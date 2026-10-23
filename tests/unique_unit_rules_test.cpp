#include "registry_scope.hpp"
#include "unique_unit_rules_registry.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

extern "C" int rerevved_unique_unit_rules_c_header_check(void);

namespace
{

UniqueUnitScalarRule makeRule(
    const char*               provider,
    const char*               ruleId,
    CivilizationId            civilization,
    UnitTypeId                baseUnitType,
    UnitIdentityId            identity,
    UniqueUnitScalarProperty  property,
    UniqueUnitScalarOperation operation,
    int32_t                   value)
{
    UniqueUnitScalarRule rule{};
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

UniqueUnitScalarRule cataphractRule(
    const char*               provider,
    const char*               ruleId,
    UniqueUnitScalarOperation operation,
    int32_t                   value)
{
    return makeRule(provider,
                    ruleId,
                    CIVILIZATION_ROMAN,
                    UNIT_TYPE_KNIGHTS,
                    UNIT_IDENTITY_CATAPHRACT,
                    UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                    operation,
                    value);
}

UniqueUnitScalarEvaluation evaluate(
    CivilizationId           civilization,
    UnitTypeId               baseUnitType,
    UnitIdentityId           identity,
    UniqueUnitScalarProperty property,
    int32_t                  nativeValue)
{
    const UniqueUnitScalarQuery query = {
        sizeof(UniqueUnitScalarQuery),
        civilization,
        baseUnitType,
        identity,
        property,
        nativeValue,
        {},
    };
    UniqueUnitScalarEvaluation evaluation{};
    UNSCOPED_INFO("evaluation succeeds");
    REQUIRE(EvaluateUniqueUnitScalar(
                &query, &evaluation, sizeof(evaluation)) ==
            UNIQUE_UNIT_RULES_OK);
    return evaluation;
}

void TestLayout()
{
    static_assert(offsetof(UniqueUnitScalarRule, providerId) == 4);
    static_assert(offsetof(UniqueUnitScalarRule, ruleId) == 68);
    static_assert(offsetof(UniqueUnitScalarRule, civilization) == 132);
    static_assert(offsetof(UniqueUnitScalarRule, value) == 152);
    static_assert(offsetof(UniqueUnitScalarRule, reserved) == 156);
    static_assert(offsetof(UniqueUnitScalarRuleInfo, statusFlags) ==
                  156);
    static_assert(offsetof(UniqueUnitScalarEvaluation, statusFlags) ==
                  12);
    UNSCOPED_INFO("ABI version");
    REQUIRE(UniqueUnitRulesAbiVersion() ==
            UNIQUE_UNIT_RULES_ABI_VERSION);
}

void TestValidation()
{
    rerevved::unique_unit_rules::ResetForTests();
    UNSCOPED_INFO("null rule rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(nullptr) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    auto rule       = cataphractRule("aeshur.cataphracts-test",
                                     "cataphract-attack",
                                     UNIQUE_UNIT_SCALAR_REPLACE,
                                     50);
    rule.structSize = sizeof(rule) - 1;
    UNSCOPED_INFO("short rule rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&rule) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    rule = cataphractRule("Aeshur", "rule", UNIQUE_UNIT_SCALAR_REPLACE, 50);
    UNSCOPED_INFO("invalid provider rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&rule) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    rule = cataphractRule("aeshur.cataphracts-test", "rule", UNIQUE_UNIT_SCALAR_REPLACE, 50);
    std::memset(rule.ruleId, 'a', sizeof(rule.ruleId));
    UNSCOPED_INFO("unterminated rule ID rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&rule) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    rule          = cataphractRule("aeshur.cataphracts-test", "rule", UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.identity = UNIT_IDENTITY_BASE;
    UNSCOPED_INFO("base identity rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&rule) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    rule              = cataphractRule("aeshur.cataphracts-test", "rule", UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.civilization = CIVILIZATION_JAPANESE;
    UNSCOPED_INFO("mismatched identity rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&rule) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    rule          = cataphractRule("aeshur.cataphracts-test", "rule", UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.property = 2;
    UNSCOPED_INFO("invalid property rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&rule) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    rule = cataphractRule("aeshur.cataphracts-test", "rule", 2, 50);
    UNSCOPED_INFO("invalid operation rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&rule) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    rule             = cataphractRule("aeshur.cataphracts-test", "rule", UNIQUE_UNIT_SCALAR_REPLACE, 50);
    rule.reserved[2] = 1;
    UNSCOPED_INFO("nonzero reserved field rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&rule) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);

    uint32_t count = 0;
    UNSCOPED_INFO("null count rejected");
    REQUIRE(GetUniqueUnitScalarRuleCount(nullptr) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("empty registry count");
    REQUIRE((GetUniqueUnitScalarRuleCount(&count) ==
                 UNIQUE_UNIT_RULES_OK &&
             count == 0));
}

void TestRegistrationAndReadback()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto z = cataphractRule("z.provider", "z-rule", UNIQUE_UNIT_SCALAR_ADD, 2);
    auto a = cataphractRule("a.provider", "a-rule", UNIQUE_UNIT_SCALAR_REPLACE, 50);
    UNSCOPED_INFO("first registration");
    REQUIRE(RegisterUniqueUnitScalarRule(&z) ==
            UNIQUE_UNIT_RULES_OK);
    UNSCOPED_INFO("second registration");
    REQUIRE(RegisterUniqueUnitScalarRule(&a) ==
            UNIQUE_UNIT_RULES_OK);
    UNSCOPED_INFO("identical registration is idempotent");
    REQUIRE(RegisterUniqueUnitScalarRule(&a) ==
            UNIQUE_UNIT_RULES_OK);

    a.value = 49;
    UNSCOPED_INFO("changed duplicate rejected");
    REQUIRE(RegisterUniqueUnitScalarRule(&a) ==
            UNIQUE_UNIT_RULES_ERR_DUPLICATE_RULE_ID);

    uint32_t count = 0;
    UNSCOPED_INFO("registry count");
    REQUIRE((GetUniqueUnitScalarRuleCount(&count) ==
                 UNIQUE_UNIT_RULES_OK &&
             count == 2));

    UniqueUnitScalarRuleInfo info{};
    UNSCOPED_INFO("read first rule");
    REQUIRE(GetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
            UNIQUE_UNIT_RULES_OK);
    UNSCOPED_INFO("canonical readback order and copied data");
    REQUIRE((std::string_view(info.providerId) == "a.provider" &&
             std::string_view(info.ruleId) == "a-rule" && info.value == 50));
    UNSCOPED_INFO("readback bounds");
    REQUIRE(GetUniqueUnitScalarRule(2, &info, sizeof(info)) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("readback short buffer");
    REQUIRE(GetUniqueUnitScalarRule(0, &info, 159) ==
            UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL);

    std::memset(&info, 0x5a, sizeof(info));
    UNSCOPED_INFO("readback prefix accepted");
    REQUIRE(GetUniqueUnitScalarRule(0, &info, 160) ==
            UNIQUE_UNIT_RULES_OK);
    const auto* infoBytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 160; index < sizeof(info); ++index)
    {
        UNSCOPED_INFO("readback does not overwrite caller tail");
        REQUIRE(infoBytes[index] == 0x5a);
    }
}

void TestComposition()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto replacement = cataphractRule("aeshur.cataphracts-test",
                                      "cataphract-attack",
                                      UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    UNSCOPED_INFO("replacement registration");
    REQUIRE(RegisterUniqueUnitScalarRule(&replacement) ==
            UNIQUE_UNIT_RULES_OK);

    auto evaluation = evaluate(CIVILIZATION_ROMAN,
                               UNIT_TYPE_KNIGHTS,
                               UNIT_IDENTITY_CATAPHRACT,
                               UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                               4);
    UNSCOPED_INFO("Cataphract replacement");
    REQUIRE((evaluation.nativeValue == 4 && evaluation.finalValue == 50 &&
             evaluation.replacementCount == 1 &&
             evaluation.additiveCount == 0 && evaluation.statusFlags == 0));

    evaluation = evaluate(CIVILIZATION_ROMAN,
                          UNIT_TYPE_KNIGHTS,
                          UNIT_IDENTITY_CATAPHRACT,
                          UNIQUE_UNIT_SCALAR_BASE_DEFENSE,
                          2);
    UNSCOPED_INFO("defense unchanged");
    REQUIRE((evaluation.finalValue == 2 &&
             evaluation.replacementCount == 0));

    auto addTwo = cataphractRule(
        "b.provider", "add-two", UNIQUE_UNIT_SCALAR_ADD, 2);
    auto subOne = cataphractRule(
        "c.provider", "sub-one", UNIQUE_UNIT_SCALAR_ADD, -1);
    UNSCOPED_INFO("additive registrations");
    REQUIRE((RegisterUniqueUnitScalarRule(&addTwo) ==
                 UNIQUE_UNIT_RULES_OK &&
             RegisterUniqueUnitScalarRule(&subOne) ==
                 UNIQUE_UNIT_RULES_OK));
    evaluation = evaluate(CIVILIZATION_ROMAN,
                          UNIT_TYPE_KNIGHTS,
                          UNIT_IDENTITY_CATAPHRACT,
                          UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                          4);
    UNSCOPED_INFO("replacement plus additions");
    REQUIRE((evaluation.finalValue == 51 && evaluation.additiveCount == 2));

    auto secondReplacement = cataphractRule(
        "d.provider", "replacement", UNIQUE_UNIT_SCALAR_REPLACE, 60);
    UNSCOPED_INFO("conflicting replacement registration");
    REQUIRE(RegisterUniqueUnitScalarRule(&secondReplacement) ==
            UNIQUE_UNIT_RULES_OK);
    evaluation = evaluate(CIVILIZATION_ROMAN,
                          UNIT_TYPE_KNIGHTS,
                          UNIT_IDENTITY_CATAPHRACT,
                          UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                          4);
    UNSCOPED_INFO("replacement conflict preserves native baseline plus additions");
    REQUIRE((evaluation.finalValue == 5 &&
             evaluation.replacementCount == 2 &&
             (evaluation.statusFlags &
              UNIQUE_UNIT_EVALUATION_REPLACEMENT_CONFLICT) != 0));

    UniqueUnitScalarRuleInfo info{};
    UNSCOPED_INFO("replacement conflict readback");
    REQUIRE((GetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
                 UNIQUE_UNIT_RULES_OK &&
             (info.statusFlags &
              UNIQUE_UNIT_RULE_REPLACEMENT_CONFLICT) != 0));
}

void TestRegistrationOrderIndependence()
{
    auto replacement = cataphractRule("b.provider",
                                      "replacement",
                                      UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    auto addTwo      = cataphractRule(
        "a.provider", "add-two", UNIQUE_UNIT_SCALAR_ADD, 2);
    auto subOne = cataphractRule(
        "c.provider", "sub-one", UNIQUE_UNIT_SCALAR_ADD, -1);

    rerevved::unique_unit_rules::ResetForTests();
    UNSCOPED_INFO("forward-order registrations");
    REQUIRE((RegisterUniqueUnitScalarRule(&replacement) ==
                 UNIQUE_UNIT_RULES_OK &&
             RegisterUniqueUnitScalarRule(&addTwo) ==
                 UNIQUE_UNIT_RULES_OK &&
             RegisterUniqueUnitScalarRule(&subOne) ==
                 UNIQUE_UNIT_RULES_OK));
    const auto forward = evaluate(
        CIVILIZATION_ROMAN,
        UNIT_TYPE_KNIGHTS,
        UNIT_IDENTITY_CATAPHRACT,
        UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        4);

    rerevved::unique_unit_rules::ResetForTests();
    UNSCOPED_INFO("reverse-order registrations");
    REQUIRE((RegisterUniqueUnitScalarRule(&subOne) ==
                 UNIQUE_UNIT_RULES_OK &&
             RegisterUniqueUnitScalarRule(&addTwo) ==
                 UNIQUE_UNIT_RULES_OK &&
             RegisterUniqueUnitScalarRule(&replacement) ==
                 UNIQUE_UNIT_RULES_OK));
    const auto reverse = evaluate(
        CIVILIZATION_ROMAN,
        UNIT_TYPE_KNIGHTS,
        UNIT_IDENTITY_CATAPHRACT,
        UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        4);
    UNSCOPED_INFO("registration order does not change composition");
    REQUIRE((forward.finalValue == 51 &&
             reverse.finalValue == forward.finalValue &&
             reverse.statusFlags == forward.statusFlags));
}

void TestCopiedInputAndConcurrentAccess()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto replacement = cataphractRule("a.provider",
                                      "replacement",
                                      UNIQUE_UNIT_SCALAR_REPLACE,
                                      50);
    UNSCOPED_INFO("copied-input registration");
    REQUIRE(RegisterUniqueUnitScalarRule(&replacement) ==
            UNIQUE_UNIT_RULES_OK);

    replacement.providerId[0] = 'z';
    replacement.value         = 4;
    UniqueUnitScalarRuleInfo info{};
    UNSCOPED_INFO("registry owns a normalized rule copy");
    REQUIRE((GetUniqueUnitScalarRule(0, &info, sizeof(info)) ==
                 UNIQUE_UNIT_RULES_OK &&
             std::string_view(info.providerId) == "a.provider" &&
             info.value == 50));

    const UniqueUnitScalarQuery query = {
        sizeof(UniqueUnitScalarQuery),
        CIVILIZATION_ROMAN,
        UNIT_TYPE_KNIGHTS,
        UNIT_IDENTITY_CATAPHRACT,
        UNIQUE_UNIT_SCALAR_BASE_ATTACK,
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
            UniqueUnitScalarEvaluation evaluation{};
            const int32_t              result = EvaluateUniqueUnitScalar(
                &query, &evaluation, sizeof(evaluation));
            const int32_t additions = evaluation.finalValue - 50;
            if (result != UNIQUE_UNIT_RULES_OK || additions < 0 ||
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
            provider, "add-one", UNIQUE_UNIT_SCALAR_ADD, 1);
        if (RegisterUniqueUnitScalarRule(&addition) !=
            UNIQUE_UNIT_RULES_OK)
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

    const auto final = evaluate(CIVILIZATION_ROMAN,
                                UNIT_TYPE_KNIGHTS,
                                UNIT_IDENTITY_CATAPHRACT,
                                UNIQUE_UNIT_SCALAR_BASE_ATTACK,
                                4);
    UNSCOPED_INFO("registration and evaluation remain atomic across threads");
    REQUIRE((!failed.load(std::memory_order_relaxed) &&
             final.finalValue == 66 && final.additiveCount == 16));
}

void TestOverflowAndQueryValidation()
{
    rerevved::unique_unit_rules::ResetForTests();
    auto add = cataphractRule(
        "a.provider", "overflow", UNIQUE_UNIT_SCALAR_ADD, 1);
    UNSCOPED_INFO("overflow rule registration");
    REQUIRE(RegisterUniqueUnitScalarRule(&add) ==
            UNIQUE_UNIT_RULES_OK);
    const auto evaluation = evaluate(
        CIVILIZATION_ROMAN,
        UNIT_TYPE_KNIGHTS,
        UNIT_IDENTITY_CATAPHRACT,
        UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        std::numeric_limits<int32_t>::max());
    UNSCOPED_INFO("positive overflow falls back");
    REQUIRE((evaluation.finalValue == std::numeric_limits<int32_t>::max() &&
             (evaluation.statusFlags &
              UNIQUE_UNIT_EVALUATION_OVERFLOW) != 0));

    rerevved::unique_unit_rules::ResetForTests();
    add = cataphractRule(
        "a.provider", "underflow", UNIQUE_UNIT_SCALAR_ADD, -1);
    UNSCOPED_INFO("underflow rule registration");
    REQUIRE(RegisterUniqueUnitScalarRule(&add) ==
            UNIQUE_UNIT_RULES_OK);
    const auto underflow = evaluate(
        CIVILIZATION_ROMAN,
        UNIT_TYPE_KNIGHTS,
        UNIT_IDENTITY_CATAPHRACT,
        UNIQUE_UNIT_SCALAR_BASE_ATTACK,
        std::numeric_limits<int32_t>::min());
    UNSCOPED_INFO("negative overflow falls back");
    REQUIRE((underflow.finalValue == std::numeric_limits<int32_t>::min() &&
             (underflow.statusFlags &
              UNIQUE_UNIT_EVALUATION_OVERFLOW) != 0));

    UniqueUnitScalarQuery query{};
    query.structSize   = sizeof(query);
    query.civilization = CIVILIZATION_ROMAN;
    query.baseUnitType = UNIT_TYPE_KNIGHTS;
    query.identity     = UNIT_IDENTITY_CATAPHRACT;
    query.property     = UNIQUE_UNIT_SCALAR_BASE_ATTACK;
    UniqueUnitScalarEvaluation out{};
    UNSCOPED_INFO("null evaluation output rejected");
    REQUIRE(EvaluateUniqueUnitScalar(&query, nullptr, sizeof(out)) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("short evaluation output rejected");
    REQUIRE(EvaluateUniqueUnitScalar(&query, &out, 23) ==
            UNIQUE_UNIT_RULES_ERR_BUFFER_TOO_SMALL);
    query.reserved[0] = 1;
    UNSCOPED_INFO("query reserved field rejected");
    REQUIRE(EvaluateUniqueUnitScalar(&query, &out, sizeof(out)) ==
            UNIQUE_UNIT_RULES_ERR_INVALID_ARGUMENT);
}

} // namespace

TEST_CASE("unique_unit_rules_contract", "[ctest][unique-unit-rules][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestLayout();
    TestValidation();
    TestRegistrationAndReadback();
    TestComposition();
    TestRegistrationOrderIndependence();
    TestCopiedInputAndConcurrentAccess();
    TestOverflowAndQueryValidation();
}

TEST_CASE("unique_unit_rules_c_header", "[ctest][unique-unit-rules][local-fast]")
{
    const int status = rerevved_unique_unit_rules_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}
