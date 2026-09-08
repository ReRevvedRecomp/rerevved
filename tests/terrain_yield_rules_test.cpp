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

#include <rex/ppc.h>

void ReRevvedApplyTerrainTradeBase(PPCRegister& terrain, PPCRegister& value);
void ReRevvedApplyTerrainProductionBase(PPCRegister& terrain,
                                        PPCRegister& value);
void ReRevvedApplyTerrainFoodBase(PPCRegister& terrain, PPCRegister& value);

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

ReRevvedTerrainYieldRule makeRule(
    const char*                   provider,
    const char*                   ruleId,
    ReRevvedTerrainId             terrain,
    ReRevvedTerrainYieldComponent component,
    ReRevvedTerrainYieldOperation operation,
    int32_t                       value)
{
    ReRevvedTerrainYieldRule rule{};
    rule.structSize = sizeof(rule);
    std::snprintf(rule.providerId, sizeof(rule.providerId), "%s", provider);
    std::snprintf(rule.ruleId, sizeof(rule.ruleId), "%s", ruleId);
    rule.terrain   = terrain;
    rule.component = component;
    rule.operation = operation;
    rule.value     = value;
    return rule;
}

ReRevvedTerrainYieldEvaluation evaluate(ReRevvedTerrainId             terrain,
                                        ReRevvedTerrainYieldComponent component,
                                        int32_t                       nativeValue)
{
    const ReRevvedTerrainYieldQuery query = {
        sizeof(ReRevvedTerrainYieldQuery), terrain, component, nativeValue, {}
    };
    ReRevvedTerrainYieldEvaluation evaluation{};
    require(ReRevvedEvaluateTerrainYield(
                &query, &evaluation, sizeof(evaluation)) ==
                REREVVED_TERRAIN_YIELD_RULES_OK,
            "evaluation succeeds");
    return evaluation;
}

void TestLayout()
{
    static_assert(REREVVED_TERRAIN_YIELD_RULES_OK == 0);
    static_assert(REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT == -10);
    static_assert(REREVVED_TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL == -11);
    static_assert(REREVVED_TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID == -12);
    static_assert(REREVVED_TERRAIN_YIELD_RULES_ERR_INTERNAL == -13);
    static_assert(REREVVED_TERRAIN_YIELD_FOOD == 0);
    static_assert(REREVVED_TERRAIN_YIELD_PRODUCTION == 1);
    static_assert(REREVVED_TERRAIN_YIELD_TRADE == 2);
    static_assert(REREVVED_TERRAIN_YIELD_REPLACE == 0);
    static_assert(REREVVED_TERRAIN_YIELD_ADD == 1);
    static_assert(REREVVED_TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT == 1u);
    static_assert(REREVVED_TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT == 1u);
    static_assert(REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW == 2u);
    static_assert(REREVVED_TERRAIN_UNKNOWN == -1);
    static_assert(REREVVED_TERRAIN_SEA == 0);
    static_assert(REREVVED_TERRAIN_PLAINS == 1);
    static_assert(REREVVED_TERRAIN_FOREST == 2);
    static_assert(REREVVED_TERRAIN_HILL == 3);
    static_assert(REREVVED_TERRAIN_DESERT == 4);
    static_assert(REREVVED_TERRAIN_MOUNTAIN == 5);
    static_assert(REREVVED_TERRAIN_COUNT == 6);
    static_assert(sizeof(ReRevvedTerrainYieldRule) == 168);
    static_assert(offsetof(ReRevvedTerrainYieldRule, providerId) == 4);
    static_assert(offsetof(ReRevvedTerrainYieldRule, ruleId) == 68);
    static_assert(offsetof(ReRevvedTerrainYieldRule, terrain) == 132);
    static_assert(offsetof(ReRevvedTerrainYieldRule, value) == 144);
    static_assert(offsetof(ReRevvedTerrainYieldRule, reserved) == 148);
    static_assert(sizeof(ReRevvedTerrainYieldRuleInfo) == 192);
    static_assert(offsetof(ReRevvedTerrainYieldRuleInfo, statusFlags) == 148);
    static_assert(sizeof(ReRevvedTerrainYieldQuery) == 40);
    static_assert(offsetof(ReRevvedTerrainYieldQuery, nativeValue) == 12);
    static_assert(sizeof(ReRevvedTerrainYieldEvaluation) == 40);
    static_assert(offsetof(ReRevvedTerrainYieldEvaluation, finalValue) == 8);
    static_assert(offsetof(ReRevvedTerrainYieldEvaluation, statusFlags) == 12);
    require(ReRevvedTerrainYieldRulesAbiVersion() ==
                REREVVED_TERRAIN_YIELD_RULES_ABI_VERSION,
            "ABI version");
}

void TestValidation()
{
    rerevved::terrain_yield_rules::ResetForTests();
    require(ReRevvedRegisterTerrainYieldRule(nullptr) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null rule rejected");

    auto rule       = makeRule("aeshur.fertile-plains",
                               "plains-food",
                               REREVVED_TERRAIN_PLAINS,
                               REREVVED_TERRAIN_YIELD_FOOD,
                               REREVVED_TERRAIN_YIELD_ADD,
                               1);
    rule.structSize = sizeof(rule) - 1;
    require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "short rule rejected");

    rule = makeRule("Aeshur", "rule", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid provider rejected");

    rule = makeRule("a.provider", "rule", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    std::memset(rule.ruleId, 'a', sizeof(rule.ruleId));
    require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "unterminated rule ID rejected");

    rule = makeRule("a.provider", "rule", -1, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "unknown terrain rejected");
    rule.terrain = REREVVED_TERRAIN_COUNT;
    require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "upper terrain rejected");

    rule.terrain   = REREVVED_TERRAIN_PLAINS;
    rule.component = 3;
    require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid component rejected");
    rule.component = REREVVED_TERRAIN_YIELD_FOOD;
    rule.operation = 2;
    require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid operation rejected");
    rule.operation   = REREVVED_TERRAIN_YIELD_REPLACE;
    rule.reserved[4] = 1;
    require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "nonzero reserved field rejected");

    uint32_t count = 99;
    require(ReRevvedGetTerrainYieldRuleCount(nullptr) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null count rejected");
    require(ReRevvedGetTerrainYieldRuleCount(&count) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                count == 0,
            "invalid registration mutated registry");
}

void TestGuestTerrainMapping()
{
    const int32_t           guestValues[]    = { 0, 2, 3, 4, 5, 6 };
    const ReRevvedTerrainId semanticValues[] = {
        REREVVED_TERRAIN_SEA,
        REREVVED_TERRAIN_PLAINS,
        REREVVED_TERRAIN_FOREST,
        REREVVED_TERRAIN_HILL,
        REREVVED_TERRAIN_DESERT,
        REREVVED_TERRAIN_MOUNTAIN,
    };
    for (size_t index = 0; index < 6; ++index)
    {
        ReRevvedTerrainId terrain = REREVVED_TERRAIN_UNKNOWN;
        require(rerevved::terrain_yield_rules::TryMapGuestTerrain(
                    guestValues[index], terrain) &&
                    terrain == semanticValues[index],
                "accepted guest terrain mapped incorrectly");
    }
    for (int32_t guest = -16; guest <= 16; ++guest)
    {
        if (guest == 0 || (guest >= 2 && guest <= 6))
        {
            continue;
        }
        ReRevvedTerrainId terrain = REREVVED_TERRAIN_UNKNOWN;
        require(!rerevved::terrain_yield_rules::TryMapGuestTerrain(guest, terrain) &&
                    terrain == REREVVED_TERRAIN_UNKNOWN,
                "unrecognized guest terrain accepted");
    }
    ReRevvedTerrainId terrain = REREVVED_TERRAIN_UNKNOWN;
    require(!rerevved::terrain_yield_rules::TryMapGuestTerrain(
                std::numeric_limits<int32_t>::min(), terrain) &&
                !rerevved::terrain_yield_rules::TryMapGuestTerrain(
                    std::numeric_limits<int32_t>::max(), terrain),
            "extreme guest terrain accepted");
}

void TestRegistrationAndReadback()
{
    rerevved::terrain_yield_rules::ResetForTests();
    auto z = makeRule("z.provider", "z-rule", REREVVED_TERRAIN_DESERT, REREVVED_TERRAIN_YIELD_TRADE, REREVVED_TERRAIN_YIELD_ADD, 2);
    auto a = makeRule("a.provider", "a-rule", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_REPLACE, 50);
    require(ReRevvedRegisterTerrainYieldRule(&z) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                ReRevvedRegisterTerrainYieldRule(&a) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                ReRevvedRegisterTerrainYieldRule(&a) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK,
            "valid and identical registrations");
    a.value = 49;
    require(ReRevvedRegisterTerrainYieldRule(&a) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID,
            "changed duplicate rejected");

    uint32_t count = 0;
    require(ReRevvedGetTerrainYieldRuleCount(&count) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                count == 2,
            "registry count");
    ReRevvedTerrainYieldRuleInfo info{};
    require(ReRevvedGetTerrainYieldRule(0, &info, sizeof(info)) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.terrain == REREVVED_TERRAIN_PLAINS && info.value == 50,
            "canonical readback order");
    require(ReRevvedGetTerrainYieldRule(2, &info, sizeof(info)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "readback bounds rejected");
    require(ReRevvedGetTerrainYieldRule(
                0, nullptr, sizeof(ReRevvedTerrainYieldRuleInfo)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null readback output rejected");
    require(ReRevvedGetTerrainYieldRule(0, &info, 151) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL,
            "short readback rejected");
    std::memset(&info, 0x5a, sizeof(info));
    require(ReRevvedGetTerrainYieldRule(0, &info, 152) ==
                REREVVED_TERRAIN_YIELD_RULES_OK,
            "minimum readback prefix rejected");
    const auto* infoBytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 152; index < sizeof(info); ++index)
    {
        require(infoBytes[index] == 0x5a, "readback overwrote caller tail");
    }
    std::memset(&info, 0x5a, sizeof(info));
    require(ReRevvedGetTerrainYieldRule(0, &info, 153) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                info.structSize == sizeof(info),
            "odd readback output size rejected");
    const auto* oddInfoBytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 153; index < sizeof(info); ++index)
    {
        require(oddInfoBytes[index] == 0x5a,
                "odd readback output overwrote caller tail");
    }

    a.providerId[0] = 'z';
    a.value         = 50;
    require(ReRevvedGetTerrainYieldRule(0, &info, sizeof(info)) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                std::string_view(info.providerId) == "a.provider" &&
                info.value == 50,
            "registry owns copied input");
}

void TestComposition()
{
    struct CompositionCase
    {
        ReRevvedTerrainId             terrain;
        ReRevvedTerrainYieldComponent component;
        int32_t                       nativeValue;
        int32_t                       replacement;
        int32_t                       positiveAdd;
        int32_t                       negativeAdd;
    };

    constexpr CompositionCase cases[] = {
        { REREVVED_TERRAIN_PLAINS,
          REREVVED_TERRAIN_YIELD_FOOD,
          4,
          -7,
          3,
          -2 },
        { REREVVED_TERRAIN_FOREST,
          REREVVED_TERRAIN_YIELD_PRODUCTION,
          10,
          12,
          4,
          -2 },
        { REREVVED_TERRAIN_DESERT,
          REREVVED_TERRAIN_YIELD_TRADE,
          6,
          -3,
          5,
          -1 },
    };

    for (const auto& testCase : cases)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        auto replacement = makeRule(
            "b.replace", "replace", testCase.terrain, testCase.component, REREVVED_TERRAIN_YIELD_REPLACE, testCase.replacement);
        auto positive = makeRule(
            "a.add", "positive", testCase.terrain, testCase.component, REREVVED_TERRAIN_YIELD_ADD, testCase.positiveAdd);
        auto negative = makeRule(
            "c.add", "negative", testCase.terrain, testCase.component, REREVVED_TERRAIN_YIELD_ADD, testCase.negativeAdd);
        require(ReRevvedRegisterTerrainYieldRule(&replacement) == 0 &&
                    ReRevvedRegisterTerrainYieldRule(&positive) == 0 &&
                    ReRevvedRegisterTerrainYieldRule(&negative) == 0,
                "component Add and Replace rules register");
        auto evaluation = evaluate(testCase.terrain,
                                   testCase.component,
                                   testCase.nativeValue);
        require(evaluation.nativeValue == testCase.nativeValue &&
                    evaluation.finalValue ==
                        testCase.replacement + testCase.positiveAdd +
                            testCase.negativeAdd &&
                    evaluation.replacementCount == 1 &&
                    evaluation.additiveCount == 2 &&
                    evaluation.statusFlags == 0,
                "component Add and Replace composition");

        auto conflict = makeRule(
            "d.replace", "conflict", testCase.terrain, testCase.component, REREVVED_TERRAIN_YIELD_REPLACE, 60);
        require(ReRevvedRegisterTerrainYieldRule(&conflict) == 0,
                "component replacement conflict register");
        evaluation = evaluate(testCase.terrain,
                              testCase.component,
                              testCase.nativeValue);
        require(evaluation.finalValue ==
                        testCase.nativeValue + testCase.positiveAdd +
                            testCase.negativeAdd &&
                    evaluation.replacementCount == 2 &&
                    (evaluation.statusFlags &
                     REREVVED_TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT) != 0,
                "component replacement conflict preserves native plus additions");

        uint32_t conflictReadbackCount = 0;
        for (uint32_t index = 0; index < 4; ++index)
        {
            ReRevvedTerrainYieldRuleInfo info{};
            require(ReRevvedGetTerrainYieldRule(index, &info, sizeof(info)) == 0,
                    "component conflict readback");
            if (info.operation == REREVVED_TERRAIN_YIELD_REPLACE)
            {
                ++conflictReadbackCount;
                require((info.statusFlags &
                         REREVVED_TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT) != 0,
                        "component conflict omitted from readback");
            }
        }
        require(conflictReadbackCount == 2,
                "component conflict readback count");
    }
}

void TestRegistrationOrderIndependence()
{
    auto replacement = makeRule("b.provider", "replacement", REREVVED_TERRAIN_HILL, REREVVED_TERRAIN_YIELD_PRODUCTION, REREVVED_TERRAIN_YIELD_REPLACE, 50);
    auto addTwo      = makeRule("a.provider", "add-two", REREVVED_TERRAIN_HILL, REREVVED_TERRAIN_YIELD_PRODUCTION, REREVVED_TERRAIN_YIELD_ADD, 2);
    auto subOne      = makeRule("c.provider", "sub-one", REREVVED_TERRAIN_HILL, REREVVED_TERRAIN_YIELD_PRODUCTION, REREVVED_TERRAIN_YIELD_ADD, -1);
    rerevved::terrain_yield_rules::ResetForTests();
    require(ReRevvedRegisterTerrainYieldRule(&replacement) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&addTwo) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&subOne) == 0,
            "forward registration");
    const auto forward = evaluate(REREVVED_TERRAIN_HILL,
                                  REREVVED_TERRAIN_YIELD_PRODUCTION,
                                  4);
    rerevved::terrain_yield_rules::ResetForTests();
    require(ReRevvedRegisterTerrainYieldRule(&subOne) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&addTwo) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&replacement) == 0,
            "reverse registration");
    const auto reverse = evaluate(REREVVED_TERRAIN_HILL,
                                  REREVVED_TERRAIN_YIELD_PRODUCTION,
                                  4);
    require(forward.finalValue == 51 && reverse.finalValue == 51 &&
                forward.statusFlags == reverse.statusFlags,
            "registration order changed composition");
}

void TestBridges()
{
    struct BridgeCase
    {
        ReRevvedTerrainYieldComponent component;
        int32_t                       guestTerrain;
        void (*bridge)(PPCRegister&, PPCRegister&);
    };

    const BridgeCase bridges[] = {
        { REREVVED_TERRAIN_YIELD_FOOD, 2, ReRevvedApplyTerrainFoodBase },
        { REREVVED_TERRAIN_YIELD_PRODUCTION,
          3,
          ReRevvedApplyTerrainProductionBase },
        { REREVVED_TERRAIN_YIELD_TRADE, 5, ReRevvedApplyTerrainTradeBase },
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
        require(terrain.u64 == originalTerrain && value.u64 == originalValue,
                "empty bridge changed a 64-bit live register");
    }

    // Each bridge applies only its matching terrain/component Add rule.
    rerevved::terrain_yield_rules::ResetForTests();
    const auto food       = makeRule("a.food", "add", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    const auto production = makeRule(
        "a.production", "add", REREVVED_TERRAIN_FOREST, REREVVED_TERRAIN_YIELD_PRODUCTION, REREVVED_TERRAIN_YIELD_ADD, 1);
    const auto trade = makeRule("a.trade", "add", REREVVED_TERRAIN_DESERT, REREVVED_TERRAIN_YIELD_TRADE, REREVVED_TERRAIN_YIELD_ADD, 1);
    require(ReRevvedRegisterTerrainYieldRule(&food) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&production) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&trade) == 0,
            "bridge Add rules register");
    for (const auto& testCase : bridges)
    {
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000000ull |
                                         static_cast<uint32_t>(testCase.guestTerrain);
        value.s64                      = 10;
        const uint64_t originalTerrain = terrain.u64;
        testCase.bridge(terrain, value);
        require(terrain.u64 == originalTerrain && value.s64 == 11,
                "matching bridge Add rule did not apply");
    }

    // A target terrain with the wrong component remains byte-for-byte intact.
    for (const auto& testCase : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto wrongComponent = makeRule(
            "a.wrong-component", "add", REREVVED_TERRAIN_PLAINS, static_cast<ReRevvedTerrainYieldComponent>((testCase.component + 1) % 3), REREVVED_TERRAIN_YIELD_ADD, 1);
        require(ReRevvedRegisterTerrainYieldRule(&wrongComponent) == 0,
                "wrong component bridge rule register");
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000002ull;
        value.u64                      = 0x112233440000000Aull;
        const uint64_t originalTerrain = terrain.u64;
        const uint64_t originalValue   = value.u64;
        testCase.bridge(terrain, value);
        require(terrain.u64 == originalTerrain && value.u64 == originalValue,
                "wrong component bridge changed a live register");
    }

    // A matching component with another terrain remains unchanged.
    for (const auto& testCase : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const bool    plainsTarget = testCase.guestTerrain == 2;
        const int32_t wrongGuest   = plainsTarget ? 3 : 2;
        const auto    wrongTerrain = makeRule(
            "a.wrong-terrain", "add", REREVVED_TERRAIN_HILL, testCase.component, REREVVED_TERRAIN_YIELD_ADD, 1);
        require(ReRevvedRegisterTerrainYieldRule(&wrongTerrain) == 0,
                "wrong terrain bridge rule register");
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000000ull |
                                         static_cast<uint32_t>(wrongGuest);
        value.u64                      = 0x112233440000000Aull;
        const uint64_t originalTerrain = terrain.u64;
        const uint64_t originalValue   = value.u64;
        testCase.bridge(terrain, value);
        require(terrain.u64 == originalTerrain && value.u64 == originalValue,
                "wrong terrain bridge changed a live register");
    }

    // A replacement conflict still applies compatible additions on every bridge.
    for (const auto& testCase : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto replacement = makeRule(
            "a.replace", "one", REREVVED_TERRAIN_PLAINS, testCase.component, REREVVED_TERRAIN_YIELD_REPLACE, 20);
        const auto conflict = makeRule(
            "b.replace", "two", REREVVED_TERRAIN_PLAINS, testCase.component, REREVVED_TERRAIN_YIELD_REPLACE, 30);
        const auto add = makeRule("c.add", "one", REREVVED_TERRAIN_PLAINS, testCase.component, REREVVED_TERRAIN_YIELD_ADD, 1);
        require(ReRevvedRegisterTerrainYieldRule(&replacement) == 0 &&
                    ReRevvedRegisterTerrainYieldRule(&conflict) == 0 &&
                    ReRevvedRegisterTerrainYieldRule(&add) == 0,
                "bridge conflict rules register");
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000002ull;
        value.s64                      = 10;
        const uint64_t originalTerrain = terrain.u64;
        testCase.bridge(terrain, value);
        require(terrain.u64 == originalTerrain && value.s64 == 11,
                "bridge conflict did not preserve native plus addition");
    }

    // Final signed-32 overflow leaves the full base register untouched.
    for (const auto& testCase : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto overflow = makeRule(
            "a.overflow", "one", REREVVED_TERRAIN_PLAINS, testCase.component, REREVVED_TERRAIN_YIELD_ADD, 1);
        require(ReRevvedRegisterTerrainYieldRule(&overflow) == 0,
                "bridge overflow rule register");
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                    = 0xAABBCCDD00000002ull;
        value.u64                      = 0x112233440000000Aull;
        const uint64_t originalTerrain = terrain.u64;
        const uint64_t originalBase    = value.u64;
        value.s32                      = std::numeric_limits<int32_t>::max();
        const uint64_t overflowValue   = value.u64;
        testCase.bridge(terrain, value);
        require(terrain.u64 == originalTerrain && value.u64 == overflowValue &&
                    value.u64 != originalBase,
                "bridge overflow changed the full base register");
    }

    // Unrecognized guest terrain values pass through every bridge unchanged.
    for (const auto& testCase : bridges)
    {
        for (const int32_t rejectedGuest : { 1, 7, 8 })
        {
            rerevved::terrain_yield_rules::ResetForTests();
            const auto rule = makeRule("a.rejected", "one", REREVVED_TERRAIN_PLAINS, testCase.component, REREVVED_TERRAIN_YIELD_ADD, 1);
            require(ReRevvedRegisterTerrainYieldRule(&rule) == 0,
                    "rejected guest bridge rule register");
            PPCRegister terrain{};
            PPCRegister value{};
            terrain.u64                    = 0xAABBCCDD00000000ull |
                                             static_cast<uint32_t>(rejectedGuest);
            value.u64                      = 0x112233440000000Aull;
            const uint64_t originalTerrain = terrain.u64;
            const uint64_t originalValue   = value.u64;
            testCase.bridge(terrain, value);
            require(terrain.u64 == originalTerrain &&
                        value.u64 == originalValue,
                    "rejected guest terrain changed a live register");
        }
    }
}

void TestCopiedInputAndConcurrentAccess()
{
    rerevved::terrain_yield_rules::ResetForTests();
    auto replacement = makeRule("a.provider", "replacement", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_REPLACE, 50);
    require(ReRevvedRegisterTerrainYieldRule(&replacement) == 0,
            "copied-input registration");
    replacement.providerId[0] = 'z';
    replacement.value         = 4;
    ReRevvedTerrainYieldRuleInfo info{};
    require(ReRevvedGetTerrainYieldRule(0, &info, sizeof(info)) == 0 &&
                std::string_view(info.providerId) == "a.provider" &&
                info.value == 50,
            "registry did not copy input");

    const ReRevvedTerrainYieldQuery query = {
        sizeof(ReRevvedTerrainYieldQuery),
        REREVVED_TERRAIN_PLAINS,
        REREVVED_TERRAIN_YIELD_FOOD,
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
            ReRevvedTerrainYieldEvaluation evaluation{};
            const int32_t                  result = ReRevvedEvaluateTerrainYield(
                &query, &evaluation, sizeof(evaluation));
            uint32_t                     count = 0;
            ReRevvedTerrainYieldRuleInfo info{};
            const int32_t                countResult =
                ReRevvedGetTerrainYieldRuleCount(&count);
            const int32_t readbackResult =
                count == 0
                    ? REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT
                    : ReRevvedGetTerrainYieldRule(
                          count - 1, &info, sizeof(info));
            const int32_t additions = evaluation.finalValue - 50;
            if (result != 0 || countResult != 0 || readbackResult != 0 ||
                count < 1 || count > 17 ||
                info.structSize != sizeof(info) ||
                info.terrain != REREVVED_TERRAIN_PLAINS ||
                info.component != REREVVED_TERRAIN_YIELD_FOOD ||
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
        auto addition = makeRule(provider, "add-one", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
        if (ReRevvedRegisterTerrainYieldRule(&addition) != 0)
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
    const auto final = evaluate(REREVVED_TERRAIN_PLAINS,
                                REREVVED_TERRAIN_YIELD_FOOD,
                                4);
    require(!failed.load(std::memory_order_relaxed) &&
                final.finalValue == 66 && final.additiveCount == 16,
            "concurrent registration was not atomic");
}

void TestOverflowAndQueryValidation()
{
    int64_t result = 0;
    require(!rerevved::terrain_yield_rules::TryAddChecked(
                std::numeric_limits<int64_t>::max(), 1, result),
            "positive int64 accumulator overflow accepted");
    require(!rerevved::terrain_yield_rules::TryAddChecked(
                std::numeric_limits<int64_t>::min(), -1, result),
            "negative int64 accumulator overflow accepted");

    rerevved::terrain_yield_rules::ResetForTests();
    auto add = makeRule("a.provider", "overflow", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    require(ReRevvedRegisterTerrainYieldRule(&add) == 0,
            "positive overflow rule registration");
    auto evaluation = evaluate(REREVVED_TERRAIN_PLAINS,
                               REREVVED_TERRAIN_YIELD_FOOD,
                               std::numeric_limits<int32_t>::max());
    require(evaluation.finalValue == std::numeric_limits<int32_t>::max() &&
                (evaluation.statusFlags &
                 REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW) != 0,
            "positive int32 overflow did not fall back");

    rerevved::terrain_yield_rules::ResetForTests();
    add = makeRule("a.provider", "underflow", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, -1);
    require(ReRevvedRegisterTerrainYieldRule(&add) == 0,
            "negative overflow rule registration");
    evaluation = evaluate(REREVVED_TERRAIN_PLAINS,
                          REREVVED_TERRAIN_YIELD_FOOD,
                          std::numeric_limits<int32_t>::min());
    require(evaluation.finalValue == std::numeric_limits<int32_t>::min() &&
                (evaluation.statusFlags &
                 REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW) != 0,
            "negative int32 overflow did not fall back");

    ReRevvedTerrainYieldQuery query{};
    query.structSize = sizeof(query);
    query.terrain    = REREVVED_TERRAIN_PLAINS;
    query.component  = REREVVED_TERRAIN_YIELD_FOOD;
    ReRevvedTerrainYieldEvaluation out{};
    require(ReRevvedEvaluateTerrainYield(&query, nullptr, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null evaluation output accepted");
    require(ReRevvedEvaluateTerrainYield(&query, &out, 23) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL,
            "short evaluation output accepted");
    require(ReRevvedEvaluateTerrainYield(nullptr, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null query accepted");
    std::memset(&out, 0x5a, sizeof(out));
    require(ReRevvedEvaluateTerrainYield(&query, &out, 24) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                out.structSize == sizeof(out),
            "minimum evaluation prefix rejected");
    const auto* minimumBytes = reinterpret_cast<const unsigned char*>(&out);
    for (size_t index = 24; index < sizeof(out); ++index)
    {
        require(minimumBytes[index] == 0x5a,
                "minimum evaluation prefix overwrote caller tail");
    }
    std::memset(&out, 0x5a, sizeof(out));
    require(ReRevvedEvaluateTerrainYield(&query, &out, 25) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                out.structSize == sizeof(out),
            "odd evaluation output size rejected");
    const auto* oddBytes = reinterpret_cast<const unsigned char*>(&out);
    for (size_t index = 25; index < sizeof(out); ++index)
    {
        require(oddBytes[index] == 0x5a,
                "odd evaluation output overwrote caller tail");
    }
    query.structSize = sizeof(query) - 1;
    require(ReRevvedEvaluateTerrainYield(&query, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "short query accepted");
    query.structSize  = sizeof(query);
    query.reserved[0] = 1;
    require(ReRevvedEvaluateTerrainYield(&query, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "query reserved field accepted");
    query.reserved[0] = 0;
    query.component   = 3;
    require(ReRevvedEvaluateTerrainYield(&query, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid query component accepted");
    query.component = REREVVED_TERRAIN_YIELD_FOOD;
    query.terrain   = REREVVED_TERRAIN_UNKNOWN;
    require(ReRevvedEvaluateTerrainYield(&query, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid query terrain accepted");
}

} // namespace

int main()
{
    TestLayout();
    TestValidation();
    TestGuestTerrainMapping();
    TestRegistrationAndReadback();
    TestComposition();
    TestRegistrationOrderIndependence();
    TestBridges();
    TestCopiedInputAndConcurrentAccess();
    TestOverflowAndQueryValidation();
    return 0;
}
