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

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

ReRevvedTerrainYieldRule MakeRule(
    const char* provider,
    const char* rule_id,
    ReRevvedTerrainId terrain,
    ReRevvedTerrainYieldComponent component,
    ReRevvedTerrainYieldOperation operation,
    int32_t value)
{
    ReRevvedTerrainYieldRule rule{};
    rule.struct_size = sizeof(rule);
    std::snprintf(rule.provider_id, sizeof(rule.provider_id), "%s", provider);
    std::snprintf(rule.rule_id, sizeof(rule.rule_id), "%s", rule_id);
    rule.terrain   = terrain;
    rule.component = component;
    rule.operation = operation;
    rule.value     = value;
    return rule;
}

ReRevvedTerrainYieldEvaluation Evaluate(ReRevvedTerrainId terrain,
                                        ReRevvedTerrainYieldComponent component,
                                        int32_t native_value)
{
    const ReRevvedTerrainYieldQuery query = {
        sizeof(ReRevvedTerrainYieldQuery), terrain, component, native_value, {}
    };
    ReRevvedTerrainYieldEvaluation evaluation{};
    Require(ReRevvedEvaluateTerrainYield(
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
    static_assert(offsetof(ReRevvedTerrainYieldRule, provider_id) == 4);
    static_assert(offsetof(ReRevvedTerrainYieldRule, rule_id) == 68);
    static_assert(offsetof(ReRevvedTerrainYieldRule, terrain) == 132);
    static_assert(offsetof(ReRevvedTerrainYieldRule, value) == 144);
    static_assert(offsetof(ReRevvedTerrainYieldRule, reserved) == 148);
    static_assert(sizeof(ReRevvedTerrainYieldRuleInfo) == 192);
    static_assert(offsetof(ReRevvedTerrainYieldRuleInfo, status_flags) == 148);
    static_assert(sizeof(ReRevvedTerrainYieldQuery) == 40);
    static_assert(offsetof(ReRevvedTerrainYieldQuery, native_value) == 12);
    static_assert(sizeof(ReRevvedTerrainYieldEvaluation) == 40);
    static_assert(offsetof(ReRevvedTerrainYieldEvaluation, final_value) == 8);
    static_assert(offsetof(ReRevvedTerrainYieldEvaluation, status_flags) == 12);
    Require(ReRevvedTerrainYieldRulesAbiVersion() ==
                REREVVED_TERRAIN_YIELD_RULES_ABI_VERSION,
            "ABI version");
}

void TestValidation()
{
    rerevved::terrain_yield_rules::ResetForTests();
    Require(ReRevvedRegisterTerrainYieldRule(nullptr) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null rule rejected");

    auto rule        = MakeRule("aeshur.fertile-plains",
                                "plains-food",
                                REREVVED_TERRAIN_PLAINS,
                                REREVVED_TERRAIN_YIELD_FOOD,
                                REREVVED_TERRAIN_YIELD_ADD,
                                1);
    rule.struct_size = sizeof(rule) - 1;
    Require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "short rule rejected");

    rule = MakeRule("Aeshur", "rule", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    Require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid provider rejected");

    rule = MakeRule("a.provider", "rule", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    std::memset(rule.rule_id, 'a', sizeof(rule.rule_id));
    Require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "unterminated rule ID rejected");

    rule = MakeRule("a.provider", "rule", -1, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    Require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "unknown terrain rejected");
    rule.terrain = REREVVED_TERRAIN_COUNT;
    Require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "upper terrain rejected");

    rule.terrain   = REREVVED_TERRAIN_PLAINS;
    rule.component = 3;
    Require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid component rejected");
    rule.component = REREVVED_TERRAIN_YIELD_FOOD;
    rule.operation = 2;
    Require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid operation rejected");
    rule.operation   = REREVVED_TERRAIN_YIELD_REPLACE;
    rule.reserved[4] = 1;
    Require(ReRevvedRegisterTerrainYieldRule(&rule) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "nonzero reserved field rejected");

    uint32_t count = 99;
    Require(ReRevvedGetTerrainYieldRuleCount(nullptr) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null count rejected");
    Require(ReRevvedGetTerrainYieldRuleCount(&count) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                count == 0,
            "invalid registration mutated registry");
}

void TestGuestTerrainMapping()
{
    const int32_t guest_values[]              = { 0, 2, 3, 4, 5, 6 };
    const ReRevvedTerrainId semantic_values[] = {
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
        Require(rerevved::terrain_yield_rules::TryMapGuestTerrain(
                    guest_values[index], terrain) &&
                    terrain == semantic_values[index],
                "accepted guest terrain mapped incorrectly");
    }
    for (int32_t guest = -16; guest <= 16; ++guest)
    {
        if (guest == 0 || (guest >= 2 && guest <= 6))
        {
            continue;
        }
        ReRevvedTerrainId terrain = REREVVED_TERRAIN_UNKNOWN;
        Require(!rerevved::terrain_yield_rules::TryMapGuestTerrain(guest, terrain) &&
                    terrain == REREVVED_TERRAIN_UNKNOWN,
                "unrecognized guest terrain accepted");
    }
    ReRevvedTerrainId terrain = REREVVED_TERRAIN_UNKNOWN;
    Require(!rerevved::terrain_yield_rules::TryMapGuestTerrain(
                std::numeric_limits<int32_t>::min(), terrain) &&
                !rerevved::terrain_yield_rules::TryMapGuestTerrain(
                    std::numeric_limits<int32_t>::max(), terrain),
            "extreme guest terrain accepted");
}

void TestRegistrationAndReadback()
{
    rerevved::terrain_yield_rules::ResetForTests();
    auto z = MakeRule("z.provider", "z-rule", REREVVED_TERRAIN_DESERT, REREVVED_TERRAIN_YIELD_TRADE, REREVVED_TERRAIN_YIELD_ADD, 2);
    auto a = MakeRule("a.provider", "a-rule", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_REPLACE, 50);
    Require(ReRevvedRegisterTerrainYieldRule(&z) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                ReRevvedRegisterTerrainYieldRule(&a) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                ReRevvedRegisterTerrainYieldRule(&a) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK,
            "valid and identical registrations");
    a.value = 49;
    Require(ReRevvedRegisterTerrainYieldRule(&a) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_DUPLICATE_RULE_ID,
            "changed duplicate rejected");

    uint32_t count = 0;
    Require(ReRevvedGetTerrainYieldRuleCount(&count) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                count == 2,
            "registry count");
    ReRevvedTerrainYieldRuleInfo info{};
    Require(ReRevvedGetTerrainYieldRule(0, &info, sizeof(info)) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                std::string_view(info.provider_id) == "a.provider" &&
                info.terrain == REREVVED_TERRAIN_PLAINS && info.value == 50,
            "canonical readback order");
    Require(ReRevvedGetTerrainYieldRule(2, &info, sizeof(info)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "readback bounds rejected");
    Require(ReRevvedGetTerrainYieldRule(
                0, nullptr, sizeof(ReRevvedTerrainYieldRuleInfo)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null readback output rejected");
    Require(ReRevvedGetTerrainYieldRule(0, &info, 151) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL,
            "short readback rejected");
    std::memset(&info, 0x5a, sizeof(info));
    Require(ReRevvedGetTerrainYieldRule(0, &info, 152) ==
                REREVVED_TERRAIN_YIELD_RULES_OK,
            "minimum readback prefix rejected");
    const auto* info_bytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 152; index < sizeof(info); ++index)
    {
        Require(info_bytes[index] == 0x5a, "readback overwrote caller tail");
    }
    std::memset(&info, 0x5a, sizeof(info));
    Require(ReRevvedGetTerrainYieldRule(0, &info, 153) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                info.struct_size == sizeof(info),
            "odd readback output size rejected");
    const auto* odd_info_bytes = reinterpret_cast<const unsigned char*>(&info);
    for (size_t index = 153; index < sizeof(info); ++index)
    {
        Require(odd_info_bytes[index] == 0x5a,
                "odd readback output overwrote caller tail");
    }

    a.provider_id[0] = 'z';
    a.value          = 50;
    Require(ReRevvedGetTerrainYieldRule(0, &info, sizeof(info)) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                std::string_view(info.provider_id) == "a.provider" &&
                info.value == 50,
            "registry owns copied input");
}

void TestComposition()
{
    struct CompositionCase
    {
        ReRevvedTerrainId terrain;
        ReRevvedTerrainYieldComponent component;
        int32_t native_value;
        int32_t replacement;
        int32_t positive_add;
        int32_t negative_add;
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

    for (const auto& test_case : cases)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        auto replacement = MakeRule(
            "b.replace", "replace", test_case.terrain, test_case.component, REREVVED_TERRAIN_YIELD_REPLACE, test_case.replacement);
        auto positive = MakeRule(
            "a.add", "positive", test_case.terrain, test_case.component, REREVVED_TERRAIN_YIELD_ADD, test_case.positive_add);
        auto negative = MakeRule(
            "c.add", "negative", test_case.terrain, test_case.component, REREVVED_TERRAIN_YIELD_ADD, test_case.negative_add);
        Require(ReRevvedRegisterTerrainYieldRule(&replacement) == 0 &&
                    ReRevvedRegisterTerrainYieldRule(&positive) == 0 &&
                    ReRevvedRegisterTerrainYieldRule(&negative) == 0,
                "component Add and Replace rules register");
        auto evaluation = Evaluate(test_case.terrain,
                                   test_case.component,
                                   test_case.native_value);
        Require(evaluation.native_value == test_case.native_value &&
                    evaluation.final_value ==
                        test_case.replacement + test_case.positive_add +
                            test_case.negative_add &&
                    evaluation.replacement_count == 1 &&
                    evaluation.additive_count == 2 &&
                    evaluation.status_flags == 0,
                "component Add and Replace composition");

        auto conflict = MakeRule(
            "d.replace", "conflict", test_case.terrain, test_case.component, REREVVED_TERRAIN_YIELD_REPLACE, 60);
        Require(ReRevvedRegisterTerrainYieldRule(&conflict) == 0,
                "component replacement conflict register");
        evaluation = Evaluate(test_case.terrain,
                              test_case.component,
                              test_case.native_value);
        Require(evaluation.final_value ==
                        test_case.native_value + test_case.positive_add +
                            test_case.negative_add &&
                    evaluation.replacement_count == 2 &&
                    (evaluation.status_flags &
                     REREVVED_TERRAIN_YIELD_EVALUATION_REPLACEMENT_CONFLICT) != 0,
                "component replacement conflict preserves native plus additions");

        uint32_t conflict_readback_count = 0;
        for (uint32_t index = 0; index < 4; ++index)
        {
            ReRevvedTerrainYieldRuleInfo info{};
            Require(ReRevvedGetTerrainYieldRule(index, &info, sizeof(info)) == 0,
                    "component conflict readback");
            if (info.operation == REREVVED_TERRAIN_YIELD_REPLACE)
            {
                ++conflict_readback_count;
                Require((info.status_flags &
                         REREVVED_TERRAIN_YIELD_RULE_REPLACEMENT_CONFLICT) != 0,
                        "component conflict omitted from readback");
            }
        }
        Require(conflict_readback_count == 2,
                "component conflict readback count");
    }
}

void TestRegistrationOrderIndependence()
{
    auto replacement = MakeRule("b.provider", "replacement", REREVVED_TERRAIN_HILL, REREVVED_TERRAIN_YIELD_PRODUCTION, REREVVED_TERRAIN_YIELD_REPLACE, 50);
    auto add_two     = MakeRule("a.provider", "add-two", REREVVED_TERRAIN_HILL, REREVVED_TERRAIN_YIELD_PRODUCTION, REREVVED_TERRAIN_YIELD_ADD, 2);
    auto sub_one     = MakeRule("c.provider", "sub-one", REREVVED_TERRAIN_HILL, REREVVED_TERRAIN_YIELD_PRODUCTION, REREVVED_TERRAIN_YIELD_ADD, -1);
    rerevved::terrain_yield_rules::ResetForTests();
    Require(ReRevvedRegisterTerrainYieldRule(&replacement) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&add_two) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&sub_one) == 0,
            "forward registration");
    const auto forward = Evaluate(REREVVED_TERRAIN_HILL,
                                  REREVVED_TERRAIN_YIELD_PRODUCTION,
                                  4);
    rerevved::terrain_yield_rules::ResetForTests();
    Require(ReRevvedRegisterTerrainYieldRule(&sub_one) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&add_two) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&replacement) == 0,
            "reverse registration");
    const auto reverse = Evaluate(REREVVED_TERRAIN_HILL,
                                  REREVVED_TERRAIN_YIELD_PRODUCTION,
                                  4);
    Require(forward.final_value == 51 && reverse.final_value == 51 &&
                forward.status_flags == reverse.status_flags,
            "registration order changed composition");
}

void TestBridges()
{
    struct BridgeCase
    {
        ReRevvedTerrainYieldComponent component;
        int32_t guest_terrain;
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
    for (const auto& test_case : bridges)
    {
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                     = 0xAABBCCDD00000000ull |
                                          static_cast<uint32_t>(test_case.guest_terrain);
        value.u64                       = 0x112233440000000Aull;
        const uint64_t original_terrain = terrain.u64;
        const uint64_t original_value   = value.u64;
        test_case.bridge(terrain, value);
        Require(terrain.u64 == original_terrain && value.u64 == original_value,
                "empty bridge changed a 64-bit live register");
    }

    // Each bridge applies only its matching terrain/component Add rule.
    rerevved::terrain_yield_rules::ResetForTests();
    const auto food       = MakeRule("a.food", "add", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    const auto production = MakeRule(
        "a.production", "add", REREVVED_TERRAIN_FOREST, REREVVED_TERRAIN_YIELD_PRODUCTION, REREVVED_TERRAIN_YIELD_ADD, 1);
    const auto trade = MakeRule("a.trade", "add", REREVVED_TERRAIN_DESERT, REREVVED_TERRAIN_YIELD_TRADE, REREVVED_TERRAIN_YIELD_ADD, 1);
    Require(ReRevvedRegisterTerrainYieldRule(&food) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&production) == 0 &&
                ReRevvedRegisterTerrainYieldRule(&trade) == 0,
            "bridge Add rules register");
    for (const auto& test_case : bridges)
    {
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                     = 0xAABBCCDD00000000ull |
                                          static_cast<uint32_t>(test_case.guest_terrain);
        value.s64                       = 10;
        const uint64_t original_terrain = terrain.u64;
        test_case.bridge(terrain, value);
        Require(terrain.u64 == original_terrain && value.s64 == 11,
                "matching bridge Add rule did not apply");
    }

    // A target terrain with the wrong component remains byte-for-byte intact.
    for (const auto& test_case : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto wrong_component = MakeRule(
            "a.wrong-component", "add", REREVVED_TERRAIN_PLAINS, static_cast<ReRevvedTerrainYieldComponent>((test_case.component + 1) % 3), REREVVED_TERRAIN_YIELD_ADD, 1);
        Require(ReRevvedRegisterTerrainYieldRule(&wrong_component) == 0,
                "wrong component bridge rule register");
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                     = 0xAABBCCDD00000002ull;
        value.u64                       = 0x112233440000000Aull;
        const uint64_t original_terrain = terrain.u64;
        const uint64_t original_value   = value.u64;
        test_case.bridge(terrain, value);
        Require(terrain.u64 == original_terrain && value.u64 == original_value,
                "wrong component bridge changed a live register");
    }

    // A matching component with another terrain remains unchanged.
    for (const auto& test_case : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const bool plains_target  = test_case.guest_terrain == 2;
        const int32_t wrong_guest = plains_target ? 3 : 2;
        const auto wrong_terrain  = MakeRule(
            "a.wrong-terrain", "add", REREVVED_TERRAIN_HILL, test_case.component, REREVVED_TERRAIN_YIELD_ADD, 1);
        Require(ReRevvedRegisterTerrainYieldRule(&wrong_terrain) == 0,
                "wrong terrain bridge rule register");
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                     = 0xAABBCCDD00000000ull |
                                          static_cast<uint32_t>(wrong_guest);
        value.u64                       = 0x112233440000000Aull;
        const uint64_t original_terrain = terrain.u64;
        const uint64_t original_value   = value.u64;
        test_case.bridge(terrain, value);
        Require(terrain.u64 == original_terrain && value.u64 == original_value,
                "wrong terrain bridge changed a live register");
    }

    // A replacement conflict still applies compatible additions on every bridge.
    for (const auto& test_case : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto replacement = MakeRule(
            "a.replace", "one", REREVVED_TERRAIN_PLAINS, test_case.component, REREVVED_TERRAIN_YIELD_REPLACE, 20);
        const auto conflict = MakeRule(
            "b.replace", "two", REREVVED_TERRAIN_PLAINS, test_case.component, REREVVED_TERRAIN_YIELD_REPLACE, 30);
        const auto add = MakeRule("c.add", "one", REREVVED_TERRAIN_PLAINS, test_case.component, REREVVED_TERRAIN_YIELD_ADD, 1);
        Require(ReRevvedRegisterTerrainYieldRule(&replacement) == 0 &&
                    ReRevvedRegisterTerrainYieldRule(&conflict) == 0 &&
                    ReRevvedRegisterTerrainYieldRule(&add) == 0,
                "bridge conflict rules register");
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                     = 0xAABBCCDD00000002ull;
        value.s64                       = 10;
        const uint64_t original_terrain = terrain.u64;
        test_case.bridge(terrain, value);
        Require(terrain.u64 == original_terrain && value.s64 == 11,
                "bridge conflict did not preserve native plus addition");
    }

    // Final signed-32 overflow leaves the full base register untouched.
    for (const auto& test_case : bridges)
    {
        rerevved::terrain_yield_rules::ResetForTests();
        const auto overflow = MakeRule(
            "a.overflow", "one", REREVVED_TERRAIN_PLAINS, test_case.component, REREVVED_TERRAIN_YIELD_ADD, 1);
        Require(ReRevvedRegisterTerrainYieldRule(&overflow) == 0,
                "bridge overflow rule register");
        PPCRegister terrain{};
        PPCRegister value{};
        terrain.u64                     = 0xAABBCCDD00000002ull;
        value.u64                       = 0x112233440000000Aull;
        const uint64_t original_terrain = terrain.u64;
        const uint64_t original_base    = value.u64;
        value.s32                       = std::numeric_limits<int32_t>::max();
        const uint64_t overflow_value   = value.u64;
        test_case.bridge(terrain, value);
        Require(terrain.u64 == original_terrain && value.u64 == overflow_value &&
                    value.u64 != original_base,
                "bridge overflow changed the full base register");
    }

    // Unrecognized guest terrain values pass through every bridge unchanged.
    for (const auto& test_case : bridges)
    {
        for (const int32_t rejected_guest : { 1, 7, 8 })
        {
            rerevved::terrain_yield_rules::ResetForTests();
            const auto rule = MakeRule("a.rejected", "one", REREVVED_TERRAIN_PLAINS, test_case.component, REREVVED_TERRAIN_YIELD_ADD, 1);
            Require(ReRevvedRegisterTerrainYieldRule(&rule) == 0,
                    "rejected guest bridge rule register");
            PPCRegister terrain{};
            PPCRegister value{};
            terrain.u64                     = 0xAABBCCDD00000000ull |
                                              static_cast<uint32_t>(rejected_guest);
            value.u64                       = 0x112233440000000Aull;
            const uint64_t original_terrain = terrain.u64;
            const uint64_t original_value   = value.u64;
            test_case.bridge(terrain, value);
            Require(terrain.u64 == original_terrain &&
                        value.u64 == original_value,
                    "rejected guest terrain changed a live register");
        }
    }
}

void TestCopiedInputAndConcurrentAccess()
{
    rerevved::terrain_yield_rules::ResetForTests();
    auto replacement = MakeRule("a.provider", "replacement", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_REPLACE, 50);
    Require(ReRevvedRegisterTerrainYieldRule(&replacement) == 0,
            "copied-input registration");
    replacement.provider_id[0] = 'z';
    replacement.value          = 4;
    ReRevvedTerrainYieldRuleInfo info{};
    Require(ReRevvedGetTerrainYieldRule(0, &info, sizeof(info)) == 0 &&
                std::string_view(info.provider_id) == "a.provider" &&
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
            ReRevvedTerrainYieldEvaluation evaluation{};
            const int32_t result = ReRevvedEvaluateTerrainYield(
                &query, &evaluation, sizeof(evaluation));
            uint32_t count = 0;
            ReRevvedTerrainYieldRuleInfo info{};
            const int32_t count_result =
                ReRevvedGetTerrainYieldRuleCount(&count);
            const int32_t readback_result =
                count == 0
                    ? REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT
                    : ReRevvedGetTerrainYieldRule(
                          count - 1, &info, sizeof(info));
            const int32_t additions = evaluation.final_value - 50;
            if (result != 0 || count_result != 0 || readback_result != 0 ||
                count < 1 || count > 17 ||
                info.struct_size != sizeof(info) ||
                info.terrain != REREVVED_TERRAIN_PLAINS ||
                info.component != REREVVED_TERRAIN_YIELD_FOOD ||
                additions < 0 || additions > 16 ||
                evaluation.additive_count != static_cast<uint32_t>(additions) ||
                evaluation.replacement_count != 1 || evaluation.status_flags != 0)
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
        auto addition = MakeRule(provider, "add-one", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
        if (ReRevvedRegisterTerrainYieldRule(&addition) != 0)
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
    const auto final = Evaluate(REREVVED_TERRAIN_PLAINS,
                                REREVVED_TERRAIN_YIELD_FOOD,
                                4);
    Require(!failed.load(std::memory_order_relaxed) &&
                final.final_value == 66 && final.additive_count == 16,
            "concurrent registration was not atomic");
}

void TestOverflowAndQueryValidation()
{
    int64_t result = 0;
    Require(!rerevved::terrain_yield_rules::TryAddChecked(
                std::numeric_limits<int64_t>::max(), 1, result),
            "positive int64 accumulator overflow accepted");
    Require(!rerevved::terrain_yield_rules::TryAddChecked(
                std::numeric_limits<int64_t>::min(), -1, result),
            "negative int64 accumulator overflow accepted");

    rerevved::terrain_yield_rules::ResetForTests();
    auto add = MakeRule("a.provider", "overflow", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, 1);
    Require(ReRevvedRegisterTerrainYieldRule(&add) == 0,
            "positive overflow rule registration");
    auto evaluation = Evaluate(REREVVED_TERRAIN_PLAINS,
                               REREVVED_TERRAIN_YIELD_FOOD,
                               std::numeric_limits<int32_t>::max());
    Require(evaluation.final_value == std::numeric_limits<int32_t>::max() &&
                (evaluation.status_flags &
                 REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW) != 0,
            "positive int32 overflow did not fall back");

    rerevved::terrain_yield_rules::ResetForTests();
    add = MakeRule("a.provider", "underflow", REREVVED_TERRAIN_PLAINS, REREVVED_TERRAIN_YIELD_FOOD, REREVVED_TERRAIN_YIELD_ADD, -1);
    Require(ReRevvedRegisterTerrainYieldRule(&add) == 0,
            "negative overflow rule registration");
    evaluation = Evaluate(REREVVED_TERRAIN_PLAINS,
                          REREVVED_TERRAIN_YIELD_FOOD,
                          std::numeric_limits<int32_t>::min());
    Require(evaluation.final_value == std::numeric_limits<int32_t>::min() &&
                (evaluation.status_flags &
                 REREVVED_TERRAIN_YIELD_EVALUATION_OVERFLOW) != 0,
            "negative int32 overflow did not fall back");

    ReRevvedTerrainYieldQuery query{};
    query.struct_size = sizeof(query);
    query.terrain     = REREVVED_TERRAIN_PLAINS;
    query.component   = REREVVED_TERRAIN_YIELD_FOOD;
    ReRevvedTerrainYieldEvaluation out{};
    Require(ReRevvedEvaluateTerrainYield(&query, nullptr, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null evaluation output accepted");
    Require(ReRevvedEvaluateTerrainYield(&query, &out, 23) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_BUFFER_TOO_SMALL,
            "short evaluation output accepted");
    Require(ReRevvedEvaluateTerrainYield(nullptr, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "null query accepted");
    std::memset(&out, 0x5a, sizeof(out));
    Require(ReRevvedEvaluateTerrainYield(&query, &out, 24) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                out.struct_size == sizeof(out),
            "minimum evaluation prefix rejected");
    const auto* minimum_bytes = reinterpret_cast<const unsigned char*>(&out);
    for (size_t index = 24; index < sizeof(out); ++index)
    {
        Require(minimum_bytes[index] == 0x5a,
                "minimum evaluation prefix overwrote caller tail");
    }
    std::memset(&out, 0x5a, sizeof(out));
    Require(ReRevvedEvaluateTerrainYield(&query, &out, 25) ==
                    REREVVED_TERRAIN_YIELD_RULES_OK &&
                out.struct_size == sizeof(out),
            "odd evaluation output size rejected");
    const auto* odd_bytes = reinterpret_cast<const unsigned char*>(&out);
    for (size_t index = 25; index < sizeof(out); ++index)
    {
        Require(odd_bytes[index] == 0x5a,
                "odd evaluation output overwrote caller tail");
    }
    query.struct_size = sizeof(query) - 1;
    Require(ReRevvedEvaluateTerrainYield(&query, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "short query accepted");
    query.struct_size = sizeof(query);
    query.reserved[0] = 1;
    Require(ReRevvedEvaluateTerrainYield(&query, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "query reserved field accepted");
    query.reserved[0] = 0;
    query.component   = 3;
    Require(ReRevvedEvaluateTerrainYield(&query, &out, sizeof(out)) ==
                REREVVED_TERRAIN_YIELD_RULES_ERR_INVALID_ARGUMENT,
            "invalid query component accepted");
    query.component = REREVVED_TERRAIN_YIELD_FOOD;
    query.terrain   = REREVVED_TERRAIN_UNKNOWN;
    Require(ReRevvedEvaluateTerrainYield(&query, &out, sizeof(out)) ==
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
