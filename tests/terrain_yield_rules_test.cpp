#include "registry_scope.hpp"
#include "terrain_yield_rules_registry.h"

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

#include <catch2/catch_test_macros.hpp>
#include <rex/ppc.h>

extern "C" int rerevved_terrain_yield_rules_c_header_check(void);

void ApplyTerrainTradeBase(PPCRegister& terrain, PPCRegister& value);
void ApplyTerrainProductionBase(PPCRegister& terrain,
                                PPCRegister& value);
void ApplyTerrainFoodBase(PPCRegister& terrain, PPCRegister& value);

namespace
{

TerrainYieldRule makeRule(
    const char*           provider,
    const char*           ruleId,
    TerrainId             terrain,
    TerrainYieldComponent component,
    TerrainYieldOperation operation,
    int32_t               value)
{
    TerrainYieldRule rule{};
    rule.structSize = sizeof(rule);
    std::snprintf(rule.providerId, sizeof(rule.providerId), "%s", provider);
    std::snprintf(rule.ruleId, sizeof(rule.ruleId), "%s", ruleId);
    rule.terrain   = terrain;
    rule.component = component;
    rule.operation = operation;
    rule.value     = value;
    return rule;
}

TerrainYieldEvaluation evaluate(TerrainId             terrain,
                                TerrainYieldComponent component,
                                int32_t               nativeValue)
{
    const TerrainYieldQuery query = {
        sizeof(TerrainYieldQuery), terrain, component, nativeValue, {}
    };
    TerrainYieldEvaluation evaluation{};
    UNSCOPED_INFO("evaluation succeeds");
    REQUIRE(EvaluateTerrainYield(
                &query, &evaluation, sizeof(evaluation)) ==
            TERRAIN_YIELD_RULES_OK);
    return evaluation;
}

void TestLayout()
{
    static_assert(TERRAIN_YIELD_RULES_OK == 0);
    static_assert(TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT == -10);
    static_assert(TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL == -11);
    static_assert(TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID == -12);
    static_assert(TERRAIN_YIELD_RULES_ERR_INTERNAL == -13);
    static_assert(TERRAIN_YIELD_FOOD == 0);
    static_assert(TERRAIN_YIELD_PRODUCTION == 1);
    static_assert(TERRAIN_YIELD_TRADE == 2);
    static_assert(TERRAIN_YIELD_REPLACE == 0);
    static_assert(TERRAIN_YIELD_ADD == 1);
    static_assert(TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT == 1u);
    static_assert(TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT == 1u);
    static_assert(TERRAIN_YIELD_EVALUATION_OVERFLOW == 2u);
    static_assert(TERRAIN_UNKNOWN == -1);
    static_assert(TERRAIN_SEA == 0);
    static_assert(TERRAIN_PLAINS == 1);
    static_assert(TERRAIN_FOREST == 2);
    static_assert(TERRAIN_HILL == 3);
    static_assert(TERRAIN_DESERT == 4);
    static_assert(TERRAIN_MOUNTAIN == 5);
    static_assert(TERRAIN_COUNT == 6);
    static_assert(offsetof(TerrainYieldRule, providerId) == 4);
    static_assert(offsetof(TerrainYieldRule, ruleId) == 68);
    static_assert(offsetof(TerrainYieldRule, terrain) == 132);
    static_assert(offsetof(TerrainYieldRule, value) == 144);
    static_assert(offsetof(TerrainYieldRule, reserved) == 148);
    static_assert(offsetof(TerrainYieldRuleInfo, statusFlags) == 148);
    static_assert(offsetof(TerrainYieldQuery, nativeValue) == 12);
    static_assert(offsetof(TerrainYieldEvaluation, finalValue) == 8);
    static_assert(offsetof(TerrainYieldEvaluation, statusFlags) == 12);
    UNSCOPED_INFO("ABI version");
    REQUIRE(TerrainYieldRulesAbiVersion() ==
            TERRAIN_YIELD_RULES_ABI_VERSION);
}

void TestValidation()
{
    rerevved::terrain_yield_rules::ResetForTests();
    UNSCOPED_INFO("null rule rejected");
    REQUIRE(RegisterTerrainYieldRule(nullptr) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);

    auto rule       = makeRule("aeshur.fertile-plains",
                               "plains-food",
                               TERRAIN_PLAINS,
                               TERRAIN_YIELD_FOOD,
                               TERRAIN_YIELD_ADD,
                               1);
    rule.structSize = sizeof(rule) - 1;
    UNSCOPED_INFO("short rule rejected");
    REQUIRE(RegisterTerrainYieldRule(&rule) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);

    rule = makeRule("Aeshur", "rule", TERRAIN_PLAINS, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_ADD, 1);
    UNSCOPED_INFO("invalid provider rejected");
    REQUIRE(RegisterTerrainYieldRule(&rule) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);

    rule = makeRule("a.provider", "rule", TERRAIN_PLAINS, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_ADD, 1);
    std::memset(rule.ruleId, 'a', sizeof(rule.ruleId));
    UNSCOPED_INFO("unterminated rule ID rejected");
    REQUIRE(RegisterTerrainYieldRule(&rule) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);

    rule = makeRule("a.provider", "rule", -1, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_ADD, 1);
    UNSCOPED_INFO("unknown terrain rejected");
    REQUIRE(RegisterTerrainYieldRule(&rule) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    rule.terrain = TERRAIN_COUNT;
    UNSCOPED_INFO("upper terrain rejected");
    REQUIRE(RegisterTerrainYieldRule(&rule) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);

    rule.terrain   = TERRAIN_PLAINS;
    rule.component = 3;
    UNSCOPED_INFO("invalid component rejected");
    REQUIRE(RegisterTerrainYieldRule(&rule) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    rule.component = TERRAIN_YIELD_FOOD;
    rule.operation = 2;
    UNSCOPED_INFO("invalid operation rejected");
    REQUIRE(RegisterTerrainYieldRule(&rule) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    rule.operation   = TERRAIN_YIELD_REPLACE;
    rule.reserved[4] = 1;
    UNSCOPED_INFO("nonzero reserved field rejected");
    REQUIRE(RegisterTerrainYieldRule(&rule) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);

    uint32_t count = 99;
    UNSCOPED_INFO("null count rejected");
    REQUIRE(GetTerrainYieldRuleCount(nullptr) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("invalid registration mutated registry");
    REQUIRE((GetTerrainYieldRuleCount(&count) ==
                 TERRAIN_YIELD_RULES_OK &&
             count == 0));
}

void TestGuestTerrainMapping()
{
    const int32_t   guestValues[]    = { 0, 2, 3, 4, 5, 6 };
    const TerrainId semanticValues[] = {
        TERRAIN_SEA,
        TERRAIN_PLAINS,
        TERRAIN_FOREST,
        TERRAIN_HILL,
        TERRAIN_DESERT,
        TERRAIN_MOUNTAIN,
    };
    for (size_t index = 0; index < 6; ++index)
    {
        TerrainId terrain = TERRAIN_UNKNOWN;
        UNSCOPED_INFO("accepted guest terrain mapped incorrectly");
        REQUIRE((rerevved::terrain_yield_rules::TryMapGuestTerrain(
                     guestValues[index], terrain) &&
                 terrain == semanticValues[index]));
    }
    for (int32_t guest = -16; guest <= 16; ++guest)
    {
        if (guest == 0 || (guest >= 2 && guest <= 6))
        {
            continue;
        }
        TerrainId terrain = TERRAIN_UNKNOWN;
        UNSCOPED_INFO("unrecognized guest terrain accepted");
        REQUIRE((!rerevved::terrain_yield_rules::TryMapGuestTerrain(guest, terrain) &&
                 terrain == TERRAIN_UNKNOWN));
    }
    TerrainId terrain = TERRAIN_UNKNOWN;
    UNSCOPED_INFO("extreme guest terrain accepted");
    REQUIRE((!rerevved::terrain_yield_rules::TryMapGuestTerrain(
                 std::numeric_limits<int32_t>::min(), terrain) &&
             !rerevved::terrain_yield_rules::TryMapGuestTerrain(
                 std::numeric_limits<int32_t>::max(), terrain)));
}

void TestRegistrationAndReadback()
{
    rerevved::terrain_yield_rules::ResetForTests();
    auto z = makeRule("z.provider", "z-rule", TERRAIN_DESERT, TERRAIN_YIELD_TRADE, TERRAIN_YIELD_ADD, 2);
    auto a = makeRule("a.provider", "a-rule", TERRAIN_PLAINS, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_REPLACE, 50);
    UNSCOPED_INFO("valid and identical registrations");
    REQUIRE((RegisterTerrainYieldRule(&z) ==
                 TERRAIN_YIELD_RULES_OK &&
             RegisterTerrainYieldRule(&a) ==
                 TERRAIN_YIELD_RULES_OK &&
             RegisterTerrainYieldRule(&a) ==
                 TERRAIN_YIELD_RULES_OK));
    a.value = 49;
    UNSCOPED_INFO("changed duplicate rejected");
    REQUIRE(RegisterTerrainYieldRule(&a) ==
            TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID);

    uint32_t count = 0;
    UNSCOPED_INFO("registry count");
    REQUIRE((GetTerrainYieldRuleCount(&count) ==
                 TERRAIN_YIELD_RULES_OK &&
             count == 2));
    TerrainYieldRuleInfo info{};
    UNSCOPED_INFO("canonical readback order");
    REQUIRE((GetTerrainYieldRule(0, &info, sizeof(info)) ==
                 TERRAIN_YIELD_RULES_OK &&
             std::string_view(info.providerId) == "a.provider" &&
             info.terrain == TERRAIN_PLAINS && info.value == 50));
    UNSCOPED_INFO("readback bounds rejected");
    REQUIRE(GetTerrainYieldRule(2, &info, sizeof(info)) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("null readback output rejected");
    REQUIRE(GetTerrainYieldRule(
                0, nullptr, sizeof(TerrainYieldRuleInfo)) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("short readback rejected");
    REQUIRE(GetTerrainYieldRule(0, &info, 151) ==
            TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL);
    std::memset(&info, 0x5a, sizeof(info));
    UNSCOPED_INFO("minimum readback prefix rejected");
    REQUIRE(GetTerrainYieldRule(0, &info, 152) ==
            TERRAIN_YIELD_RULES_OK);
    const auto* infoBytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 152; index < sizeof(info); ++index)
    {
        UNSCOPED_INFO("readback overwrote caller tail");
        REQUIRE(infoBytes[index] == 0x5a);
    }
    std::memset(&info, 0x5a, sizeof(info));
    UNSCOPED_INFO("odd readback output size rejected");
    REQUIRE((GetTerrainYieldRule(0, &info, 153) ==
                 TERRAIN_YIELD_RULES_OK &&
             info.structSize == sizeof(info)));
    const auto* oddInfoBytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 153; index < sizeof(info); ++index)
    {
        UNSCOPED_INFO("odd readback output overwrote caller tail");
        REQUIRE(oddInfoBytes[index] == 0x5a);
    }

    a.providerId[0] = 'z';
    a.value         = 50;
    UNSCOPED_INFO("registry owns copied input");
    REQUIRE((GetTerrainYieldRule(0, &info, sizeof(info)) ==
                 TERRAIN_YIELD_RULES_OK &&
             std::string_view(info.providerId) == "a.provider" &&
             info.value == 50));
}

void TestComposition()
{
    struct CompositionCase
    {
        TerrainId             terrain;
        TerrainYieldComponent component;
        int32_t               nativeValue;
        int32_t               replacement;
        int32_t               positiveAdd;
        int32_t               negativeAdd;
    };

    constexpr CompositionCase cases[] = {
        { TERRAIN_PLAINS,
          TERRAIN_YIELD_FOOD,
          4,
          -7,
          3,
          -2 },
        { TERRAIN_FOREST,
          TERRAIN_YIELD_PRODUCTION,
          10,
          12,
          4,
          -2 },
        { TERRAIN_DESERT,
          TERRAIN_YIELD_TRADE,
          6,
          -3,
          5,
          -1 },
    };

    for (const auto& testCase : cases)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        auto replacement = makeRule(
            "b.replace", "replace", testCase.terrain, testCase.component, TERRAIN_YIELD_REPLACE, testCase.replacement);
        auto positive = makeRule(
            "a.add", "positive", testCase.terrain, testCase.component, TERRAIN_YIELD_ADD, testCase.positiveAdd);
        auto negative = makeRule(
            "c.add", "negative", testCase.terrain, testCase.component, TERRAIN_YIELD_ADD, testCase.negativeAdd);
        UNSCOPED_INFO("component Add and Replace rules register");
        REQUIRE((RegisterTerrainYieldRule(&replacement) == 0 &&
                 RegisterTerrainYieldRule(&positive) == 0 &&
                 RegisterTerrainYieldRule(&negative) == 0));
        auto evaluation = evaluate(testCase.terrain,
                                   testCase.component,
                                   testCase.nativeValue);
        UNSCOPED_INFO("component Add and Replace composition");
        REQUIRE((evaluation.nativeValue == testCase.nativeValue &&
                 evaluation.finalValue ==
                     testCase.replacement + testCase.positiveAdd +
                         testCase.negativeAdd &&
                 evaluation.replacementCount == 1 &&
                 evaluation.additiveCount == 2 &&
                 evaluation.statusFlags == 0));

        auto conflict = makeRule(
            "d.replace", "conflict", testCase.terrain, testCase.component, TERRAIN_YIELD_REPLACE, 60);
        UNSCOPED_INFO("component replacement conflict register");
        REQUIRE(RegisterTerrainYieldRule(&conflict) == 0);
        evaluation = evaluate(testCase.terrain,
                              testCase.component,
                              testCase.nativeValue);
        UNSCOPED_INFO("component replacement conflict preserves native plus additions");
        REQUIRE((evaluation.finalValue ==
                     testCase.nativeValue + testCase.positiveAdd +
                         testCase.negativeAdd &&
                 evaluation.replacementCount == 2 &&
                 (evaluation.statusFlags &
                  TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT) != 0));

        uint32_t conflictReadbackCount = 0;
        for (uint32_t index = 0; index < 4; ++index)
        {
            TerrainYieldRuleInfo info{};
            UNSCOPED_INFO("component conflict readback");
            REQUIRE(GetTerrainYieldRule(index, &info, sizeof(info)) == 0);
            if (info.operation == TERRAIN_YIELD_REPLACE)
            {
                ++conflictReadbackCount;
                UNSCOPED_INFO("component conflict omitted from readback");
                REQUIRE((info.statusFlags &
                         TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT) != 0);
            }
        }
        UNSCOPED_INFO("component conflict readback count");
        REQUIRE(conflictReadbackCount == 2);
    }
}

void TestRegistrationOrderIndependence()
{
    auto replacement = makeRule("b.provider", "replacement", TERRAIN_HILL, TERRAIN_YIELD_PRODUCTION, TERRAIN_YIELD_REPLACE, 50);
    auto addTwo      = makeRule("a.provider", "add-two", TERRAIN_HILL, TERRAIN_YIELD_PRODUCTION, TERRAIN_YIELD_ADD, 2);
    auto subOne      = makeRule("c.provider", "sub-one", TERRAIN_HILL, TERRAIN_YIELD_PRODUCTION, TERRAIN_YIELD_ADD, -1);
    rerevved::terrain_yield_rules::ResetForTests();
    UNSCOPED_INFO("forward registration");
    REQUIRE((RegisterTerrainYieldRule(&replacement) == 0 &&
             RegisterTerrainYieldRule(&addTwo) == 0 &&
             RegisterTerrainYieldRule(&subOne) == 0));
    const auto forward = evaluate(TERRAIN_HILL,
                                  TERRAIN_YIELD_PRODUCTION,
                                  4);
    rerevved::terrain_yield_rules::ResetForTests();
    UNSCOPED_INFO("reverse registration");
    REQUIRE((RegisterTerrainYieldRule(&subOne) == 0 &&
             RegisterTerrainYieldRule(&addTwo) == 0 &&
             RegisterTerrainYieldRule(&replacement) == 0));
    const auto reverse = evaluate(TERRAIN_HILL,
                                  TERRAIN_YIELD_PRODUCTION,
                                  4);
    UNSCOPED_INFO("registration order changed composition");
    REQUIRE((forward.finalValue == 51 && reverse.finalValue == 51 &&
             forward.statusFlags == reverse.statusFlags));
}

void TestBridges()
{
    struct BridgeCase
    {
        TerrainYieldComponent component;
        int32_t               guestTerrain;
        void (*bridge)(PPCRegister&, PPCRegister&);
    };

    const BridgeCase bridges[] = {
        { TERRAIN_YIELD_FOOD, 2, ApplyTerrainFoodBase },
        { TERRAIN_YIELD_PRODUCTION,
          3,
          ApplyTerrainProductionBase },
        { TERRAIN_YIELD_TRADE, 5, ApplyTerrainTradeBase },
    };

    // Empty registries must preserve both live registers for every bridge.
    rerevved::terrain_yield_rules::ResetForTests();
    for (const auto& testCase : bridges)
    {
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000000ull |
                                         static_cast<uint32_t>(testCase.guestTerrain);
        value.u64                      = 0x112233440000000Aull;
        const uint64_t originalTerrain = terrain.u64;
        const uint64_t originalValue   = value.u64;
        testCase.bridge(terrain, value);
        UNSCOPED_INFO("empty bridge changed a 64-bit live register");
        REQUIRE((terrain.u64 == originalTerrain && value.u64 == originalValue));
    }

    // Each bridge applies only its matching terrain/component Add rule.
    rerevved::terrain_yield_rules::ResetForTests();
    const auto food       = makeRule("a.food", "add", TERRAIN_PLAINS, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_ADD, 1);
    const auto production = makeRule(
        "a.production", "add", TERRAIN_FOREST, TERRAIN_YIELD_PRODUCTION, TERRAIN_YIELD_ADD, 1);
    const auto trade = makeRule("a.trade", "add", TERRAIN_DESERT, TERRAIN_YIELD_TRADE, TERRAIN_YIELD_ADD, 1);
    UNSCOPED_INFO("bridge Add rules register");
    REQUIRE((RegisterTerrainYieldRule(&food) == 0 &&
             RegisterTerrainYieldRule(&production) == 0 &&
             RegisterTerrainYieldRule(&trade) == 0));
    for (const auto& testCase : bridges)
    {
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000000ull |
                                         static_cast<uint32_t>(testCase.guestTerrain);
        value.s64                      = 10;
        const uint64_t originalTerrain = terrain.u64;
        testCase.bridge(terrain, value);
        UNSCOPED_INFO("matching bridge Add rule did not apply");
        REQUIRE((terrain.u64 == originalTerrain && value.s64 == 11));
    }

    // A target terrain with the wrong component remains byte-for-byte intact.
    for (const auto& testCase : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto wrongComponent = makeRule(
            "a.wrong-component", "add", TERRAIN_PLAINS, static_cast<TerrainYieldComponent>((testCase.component + 1) % 3), TERRAIN_YIELD_ADD, 1);
        UNSCOPED_INFO("wrong component bridge rule register");
        REQUIRE(RegisterTerrainYieldRule(&wrongComponent) == 0);
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000002ull;
        value.u64                      = 0x112233440000000Aull;
        const uint64_t originalTerrain = terrain.u64;
        const uint64_t originalValue   = value.u64;
        testCase.bridge(terrain, value);
        UNSCOPED_INFO("wrong component bridge changed a live register");
        REQUIRE((terrain.u64 == originalTerrain && value.u64 == originalValue));
    }

    // A matching component with another terrain remains unchanged.
    for (const auto& testCase : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const bool    plainsTarget = testCase.guestTerrain == 2;
        const int32_t wrongGuest   = plainsTarget ? 3 : 2;
        const auto    wrongTerrain = makeRule(
            "a.wrong-terrain", "add", TERRAIN_HILL, testCase.component, TERRAIN_YIELD_ADD, 1);
        UNSCOPED_INFO("wrong terrain bridge rule register");
        REQUIRE(RegisterTerrainYieldRule(&wrongTerrain) == 0);
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000000ull |
                                         static_cast<uint32_t>(wrongGuest);
        value.u64                      = 0x112233440000000Aull;
        const uint64_t originalTerrain = terrain.u64;
        const uint64_t originalValue   = value.u64;
        testCase.bridge(terrain, value);
        UNSCOPED_INFO("wrong terrain bridge changed a live register");
        REQUIRE((terrain.u64 == originalTerrain && value.u64 == originalValue));
    }

    // A replacement conflict still applies compatible additions on every bridge.
    for (const auto& testCase : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto replacement = makeRule(
            "a.replace", "one", TERRAIN_PLAINS, testCase.component, TERRAIN_YIELD_REPLACE, 20);
        const auto conflict = makeRule(
            "b.replace", "two", TERRAIN_PLAINS, testCase.component, TERRAIN_YIELD_REPLACE, 30);
        const auto add = makeRule("c.add", "one", TERRAIN_PLAINS, testCase.component, TERRAIN_YIELD_ADD, 1);
        UNSCOPED_INFO("bridge conflict rules register");
        REQUIRE((RegisterTerrainYieldRule(&replacement) == 0 &&
                 RegisterTerrainYieldRule(&conflict) == 0 &&
                 RegisterTerrainYieldRule(&add) == 0));
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000002ull;
        value.s64                      = 10;
        const uint64_t originalTerrain = terrain.u64;
        testCase.bridge(terrain, value);
        UNSCOPED_INFO("bridge conflict did not preserve native plus addition");
        REQUIRE((terrain.u64 == originalTerrain && value.s64 == 11));
    }

    // Final signed-32 overflow leaves the full base register untouched.
    for (const auto& testCase : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto overflow = makeRule(
            "a.overflow", "one", TERRAIN_PLAINS, testCase.component, TERRAIN_YIELD_ADD, 1);
        UNSCOPED_INFO("bridge overflow rule register");
        REQUIRE(RegisterTerrainYieldRule(&overflow) == 0);
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000002ull;
        value.u64                      = 0x112233440000000Aull;
        const uint64_t originalTerrain = terrain.u64;
        const uint64_t originalBase    = value.u64;
        value.s32                      = std::numeric_limits<int32_t>::max();
        const uint64_t overflowValue   = value.u64;
        testCase.bridge(terrain, value);
        UNSCOPED_INFO("bridge overflow changed the full base register");
        REQUIRE((terrain.u64 == originalTerrain && value.u64 == overflowValue &&
                 value.u64 != originalBase));
    }

    // Unrecognized guest terrain values pass through every bridge unchanged.
    for (const auto& testCase : bridges)
    {
        for (const int32_t rejectedGuest : { 1, 7, 8 })
        {
            rerevved::terrain_yield_rules::ResetForTests();
            const auto rule = makeRule("a.rejected", "one", TERRAIN_PLAINS, testCase.component, TERRAIN_YIELD_ADD, 1);
            UNSCOPED_INFO("rejected guest bridge rule register");
            REQUIRE(RegisterTerrainYieldRule(&rule) == 0);
            PPCRegister terrain{};
            PPCRegister value{};
            terrain.u64                    = 0xAABBCCDD00000000ull |
                                             static_cast<uint32_t>(rejectedGuest);
            value.u64                      = 0x112233440000000Aull;
            const uint64_t originalTerrain = terrain.u64;
            const uint64_t originalValue   = value.u64;
            testCase.bridge(terrain, value);
            UNSCOPED_INFO("rejected guest terrain changed a live register");
            REQUIRE((terrain.u64 == originalTerrain &&
                     value.u64 == originalValue));
        }
    }
}

void TestCopiedInputAndConcurrentAccess()
{
    rerevved::terrain_yield_rules::ResetForTests();
    auto replacement = makeRule("a.provider", "replacement", TERRAIN_PLAINS, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_REPLACE, 50);
    UNSCOPED_INFO("copied-input registration");
    REQUIRE(RegisterTerrainYieldRule(&replacement) == 0);
    replacement.providerId[0] = 'z';
    replacement.value         = 4;
    TerrainYieldRuleInfo info{};
    UNSCOPED_INFO("registry did not copy input");
    REQUIRE((GetTerrainYieldRule(0, &info, sizeof(info)) == 0 &&
             std::string_view(info.providerId) == "a.provider" &&
             info.value == 50));

    const TerrainYieldQuery query = {
        sizeof(TerrainYieldQuery),
        TERRAIN_PLAINS,
        TERRAIN_YIELD_FOOD,
        4,
        {},
    };
    std::atomic<bool> start{ false };
    std::atomic<bool> writerDone{ false };
    std::atomic<bool> failed{ false };
    auto              readRegistry = [&]
    {
        while (!start.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        do
        {
            TerrainYieldEvaluation evaluation{};
            const int32_t          result = EvaluateTerrainYield(
                &query, &evaluation, sizeof(evaluation));
            uint32_t             count = 0;
            TerrainYieldRuleInfo info{};
            const int32_t        countResult =
                GetTerrainYieldRuleCount(&count);
            const int32_t readbackResult =
                count == 0
                    ? TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT
                    : GetTerrainYieldRule(
                          count - 1, &info, sizeof(info));
            const int32_t additions = evaluation.finalValue - 50;
            if (result != 0 || countResult != 0 || readbackResult != 0 ||
                count < 1 || count > 17 ||
                info.structSize != sizeof(info) ||
                info.terrain != TERRAIN_PLAINS ||
                info.component != TERRAIN_YIELD_FOOD ||
                additions < 0 || additions > 16 ||
                evaluation.additiveCount != static_cast<uint32_t>(additions) ||
                evaluation.replacementCount != 1 || evaluation.statusFlags != 0)
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
        auto addition = makeRule(provider, "add-one", TERRAIN_PLAINS, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_ADD, 1);
        if (RegisterTerrainYieldRule(&addition) != 0)
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
    const auto final = evaluate(TERRAIN_PLAINS,
                                TERRAIN_YIELD_FOOD,
                                4);
    UNSCOPED_INFO("concurrent registration was not atomic");
    REQUIRE((!failed.load(std::memory_order_relaxed) &&
             final.finalValue == 66 && final.additiveCount == 16));
}

void TestOverflowAndQueryValidation()
{
    int64_t result = 0;
    UNSCOPED_INFO("positive int64 accumulator overflow accepted");
    REQUIRE(!rerevved::terrain_yield_rules::TryAddChecked(
        std::numeric_limits<int64_t>::max(), 1, result));
    UNSCOPED_INFO("negative int64 accumulator overflow accepted");
    REQUIRE(!rerevved::terrain_yield_rules::TryAddChecked(
        std::numeric_limits<int64_t>::min(), -1, result));

    rerevved::terrain_yield_rules::ResetForTests();
    auto add = makeRule("a.provider", "overflow", TERRAIN_PLAINS, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_ADD, 1);
    UNSCOPED_INFO("positive overflow rule registration");
    REQUIRE(RegisterTerrainYieldRule(&add) == 0);
    auto evaluation = evaluate(TERRAIN_PLAINS,
                               TERRAIN_YIELD_FOOD,
                               std::numeric_limits<int32_t>::max());
    UNSCOPED_INFO("positive int32 overflow did not fall back");
    REQUIRE((evaluation.finalValue == std::numeric_limits<int32_t>::max() &&
             (evaluation.statusFlags &
              TERRAIN_YIELD_EVALUATION_OVERFLOW) != 0));

    rerevved::terrain_yield_rules::ResetForTests();
    add = makeRule("a.provider", "underflow", TERRAIN_PLAINS, TERRAIN_YIELD_FOOD, TERRAIN_YIELD_ADD, -1);
    UNSCOPED_INFO("negative overflow rule registration");
    REQUIRE(RegisterTerrainYieldRule(&add) == 0);
    evaluation = evaluate(TERRAIN_PLAINS,
                          TERRAIN_YIELD_FOOD,
                          std::numeric_limits<int32_t>::min());
    UNSCOPED_INFO("negative int32 overflow did not fall back");
    REQUIRE((evaluation.finalValue == std::numeric_limits<int32_t>::min() &&
             (evaluation.statusFlags &
              TERRAIN_YIELD_EVALUATION_OVERFLOW) != 0));

    TerrainYieldQuery query{};
    query.structSize = sizeof(query);
    query.terrain    = TERRAIN_PLAINS;
    query.component  = TERRAIN_YIELD_FOOD;
    TerrainYieldEvaluation out{};
    UNSCOPED_INFO("null evaluation output accepted");
    REQUIRE(EvaluateTerrainYield(&query, nullptr, sizeof(out)) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    UNSCOPED_INFO("short evaluation output accepted");
    REQUIRE(EvaluateTerrainYield(&query, &out, 23) ==
            TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL);
    UNSCOPED_INFO("null query accepted");
    REQUIRE(EvaluateTerrainYield(nullptr, &out, sizeof(out)) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    std::memset(&out, 0x5a, sizeof(out));
    UNSCOPED_INFO("minimum evaluation prefix rejected");
    REQUIRE((EvaluateTerrainYield(&query, &out, 24) ==
                 TERRAIN_YIELD_RULES_OK &&
             out.structSize == sizeof(out)));
    const auto* minimumBytes = reinterpret_cast<const unsigned char*>(&out);
    for (size_t index = 24; index < sizeof(out); ++index)
    {
        UNSCOPED_INFO("minimum evaluation prefix overwrote caller tail");
        REQUIRE(minimumBytes[index] == 0x5a);
    }
    std::memset(&out, 0x5a, sizeof(out));
    UNSCOPED_INFO("odd evaluation output size rejected");
    REQUIRE((EvaluateTerrainYield(&query, &out, 25) ==
                 TERRAIN_YIELD_RULES_OK &&
             out.structSize == sizeof(out)));
    const auto* oddBytes = reinterpret_cast<const unsigned char*>(&out);
    for (size_t index = 25; index < sizeof(out); ++index)
    {
        UNSCOPED_INFO("odd evaluation output overwrote caller tail");
        REQUIRE(oddBytes[index] == 0x5a);
    }
    query.structSize = sizeof(query) - 1;
    UNSCOPED_INFO("short query accepted");
    REQUIRE(EvaluateTerrainYield(&query, &out, sizeof(out)) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    query.structSize  = sizeof(query);
    query.reserved[0] = 1;
    UNSCOPED_INFO("query reserved field accepted");
    REQUIRE(EvaluateTerrainYield(&query, &out, sizeof(out)) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    query.reserved[0] = 0;
    query.component   = 3;
    UNSCOPED_INFO("invalid query component accepted");
    REQUIRE(EvaluateTerrainYield(&query, &out, sizeof(out)) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
    query.component = TERRAIN_YIELD_FOOD;
    query.terrain   = TERRAIN_UNKNOWN;
    UNSCOPED_INFO("invalid query terrain accepted");
    REQUIRE(EvaluateTerrainYield(&query, &out, sizeof(out)) ==
            TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT);
}

} // namespace

TEST_CASE("terrain_yield_rules_contract", "[ctest][terrain-yield-rules][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestLayout();
    TestValidation();
    TestGuestTerrainMapping();
    TestRegistrationAndReadback();
    TestComposition();
    TestRegistrationOrderIndependence();
    TestBridges();
    TestCopiedInputAndConcurrentAccess();
    TestOverflowAndQueryValidation();
}

TEST_CASE("terrain_yield_rules_c_header", "[ctest][terrain-yield-rules][local-fast]")
{
    const int status = rerevved_terrain_yield_rules_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}
