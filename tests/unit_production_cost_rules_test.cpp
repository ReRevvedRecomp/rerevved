#include "registry_scope.hpp"
#include "unit_production_cost_rules_registry.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string_view>

#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

extern "C" int rerevved_unit_production_cost_rules_c_header_check(void);

void ApplyUnitProductionCostPercent(PPCRegister& cityOffset,
                                    PPCRegister& unitType,
                                    PPCRegister& costScalar);

namespace
{

UnitProductionCostRule makeRule(const char* provider,
                                const char* ruleId,
                                int32_t     percentageDelta)
{
    UnitProductionCostRule rule{};
    rule.structSize = sizeof(rule);
    std::memcpy(rule.providerId, provider, std::strlen(provider) + 1);
    std::memcpy(rule.ruleId, ruleId, std::strlen(ruleId) + 1);
    rule.civilization    = CIVILIZATION_AZTEC;
    rule.baseUnitType    = UNIT_TYPE_WARRIOR;
    rule.identity        = UNIT_IDENTITY_JAGUAR_WARRIOR;
    rule.percentageDelta = percentageDelta;
    return rule;
}

UnitProductionCostEvaluation evaluate()
{
    const UnitProductionCostQuery query = {
        sizeof(UnitProductionCostQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        {},
    };
    UnitProductionCostEvaluation evaluation{};
    UNSCOPED_INFO("production cost evaluation failed");
    REQUIRE(EvaluateUnitProductionCost(
                &query, &evaluation, sizeof(evaluation)) ==
            UNIT_PRODUCTION_COST_RULES_OK);
    return evaluation;
}

void TestLayoutAndValidation()
{
    static_assert(offsetof(UnitProductionCostRule, civilization) == 132);
    static_assert(offsetof(UnitProductionCostRule,
                           percentageDelta) == 144);
    static_assert(offsetof(UnitProductionCostRuleInfo, statusFlags) == 148);
    static_assert(offsetof(UnitProductionCostEvaluation, finalPercent) == 8);
    UNSCOPED_INFO("production cost ABI version mismatch");
    REQUIRE(UnitProductionCostRulesAbiVersion() ==
            UNIT_PRODUCTION_COST_RULES_ABI_VERSION);

    rerevved::unit_production_cost_rules::ResetForTests();
    UNSCOPED_INFO("null production cost rule accepted");
    REQUIRE(RegisterUnitProductionCostRule(nullptr) ==
            UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT);
    auto invalid     = makeRule("test.provider", "invalid", 1);
    invalid.identity = UNIT_IDENTITY_BASE;
    UNSCOPED_INFO("base identity production cost rule accepted");
    REQUIRE(RegisterUnitProductionCostRule(&invalid) ==
            UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT);
    invalid             = makeRule("test.provider", "invalid", 1);
    invalid.reserved[0] = 1;
    UNSCOPED_INFO("nonzero production cost reserved field accepted");
    REQUIRE(RegisterUnitProductionCostRule(&invalid) ==
            UNIT_PRODUCTION_COST_RULES_ERR_INVALID_ARGUMENT);
}

void TestRegistrationReadbackAndEvaluation()
{
    rerevved::unit_production_cost_rules::ResetForTests();
    auto later   = makeRule("z.provider", "late", -25);
    auto earlier = makeRule("a.provider", "early", 50);
    UNSCOPED_INFO("production cost rules did not register");
    REQUIRE((RegisterUnitProductionCostRule(&later) ==
                 UNIT_PRODUCTION_COST_RULES_OK &&
             RegisterUnitProductionCostRule(&earlier) ==
                 UNIT_PRODUCTION_COST_RULES_OK));
    UNSCOPED_INFO("idempotent production cost registration failed");
    REQUIRE(RegisterUnitProductionCostRule(&earlier) ==
            UNIT_PRODUCTION_COST_RULES_OK);
    earlier.percentageDelta = 40;
    UNSCOPED_INFO("conflicting production cost rule id accepted");
    REQUIRE(RegisterUnitProductionCostRule(&earlier) ==
            UNIT_PRODUCTION_COST_RULES_ERR_DUPLICATE_RULE_ID);

    uint32_t count = 0;
    UNSCOPED_INFO("production cost rule count mismatch");
    REQUIRE((GetUnitProductionCostRuleCount(&count) ==
                 UNIT_PRODUCTION_COST_RULES_OK &&
             count == 2));
    UnitProductionCostRuleInfo info{};
    UNSCOPED_INFO("production cost readback ordering mismatch");
    REQUIRE((GetUnitProductionCostRule(0, &info, sizeof(info)) ==
                 UNIT_PRODUCTION_COST_RULES_OK &&
             std::string_view(info.providerId) == "a.provider" &&
             info.percentageDelta == 50));

    const auto evaluation = evaluate();
    UNSCOPED_INFO("production cost additive composition mismatch");
    REQUIRE((evaluation.nativePercent == 100 &&
             evaluation.finalPercent == 125 &&
             evaluation.additiveCount == 2 && evaluation.statusFlags == 0));

    const UnitProductionCostQuery impiQuery = {
        sizeof(UnitProductionCostQuery),
        CIVILIZATION_ZULU,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_IMPI_WARRIOR,
        {},
    };
    UnitProductionCostEvaluation impi{};
    UNSCOPED_INFO("production cost identity target leaked to control unit");
    REQUIRE((EvaluateUnitProductionCost(
                 &impiQuery, &impi, sizeof(impi)) ==
                 UNIT_PRODUCTION_COST_RULES_OK &&
             impi.finalPercent == 100 && impi.additiveCount == 0));
}

void TestOverflowAndSizedOutput()
{
    rerevved::unit_production_cost_rules::ResetForTests();
    auto overflow = makeRule(
        "a.provider", "overflow", std::numeric_limits<int32_t>::max());
    UNSCOPED_INFO("production cost overflow rule did not register");
    REQUIRE(RegisterUnitProductionCostRule(&overflow) ==
            UNIT_PRODUCTION_COST_RULES_OK);
    const auto evaluation = evaluate();
    UNSCOPED_INFO("production cost overflow did not preserve native percent");
    REQUIRE((evaluation.finalPercent == evaluation.nativePercent &&
             (evaluation.statusFlags &
              UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0));

    rerevved::unit_production_cost_rules::ResetForTests();
    auto nonpositive = makeRule("a.provider", "nonpositive", -100);
    UNSCOPED_INFO("production cost nonpositive rule did not register");
    REQUIRE(RegisterUnitProductionCostRule(&nonpositive) ==
            UNIT_PRODUCTION_COST_RULES_OK);
    const auto invalid = evaluate();
    UNSCOPED_INFO("nonpositive production cost did not preserve native percent");
    REQUIRE((invalid.finalPercent == invalid.nativePercent &&
             (invalid.statusFlags &
              UNIT_PRODUCTION_COST_EVALUATION_OUT_OF_RANGE) != 0));

    const UnitProductionCostQuery query = {
        sizeof(UnitProductionCostQuery),
        CIVILIZATION_AZTEC,
        UNIT_TYPE_WARRIOR,
        UNIT_IDENTITY_JAGUAR_WARRIOR,
        {},
    };
    UnitProductionCostEvaluation output{};
    UNSCOPED_INFO("short production cost evaluation output accepted");
    REQUIRE(EvaluateUnitProductionCost(&query, &output, 19) ==
            UNIT_PRODUCTION_COST_RULES_ERR_BUFFER_TOO_SMALL);
    std::memset(&output, 0x5a, sizeof(output));
    UNSCOPED_INFO("production cost minimum evaluation prefix rejected");
    REQUIRE((EvaluateUnitProductionCost(&query, &output, 20) ==
                 UNIT_PRODUCTION_COST_RULES_OK &&
             output.structSize ==
                 sizeof(output)));
    const auto* bytes = reinterpret_cast<const unsigned char*>(&output);
    for (size_t index = 20; index < sizeof(output); ++index)
    {
        UNSCOPED_INFO("production cost minimum prefix overwrote caller tail");
        REQUIRE(bytes[index] == 0x5a);
    }
}

} // namespace

TEST_CASE("unit_production_cost_rules_contract", "[ctest][unit-production-cost-rules][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestLayoutAndValidation();
    TestRegistrationReadbackAndEvaluation();
    TestOverflowAndSizedOutput();
}

TEST_CASE("unit_production_cost_rules_c_header", "[ctest][unit-production-cost-rules][local-fast]")
{
    const int status = rerevved_unit_production_cost_rules_c_header_check();
    UNSCOPED_INFO("C header check exit status " << status);
    REQUIRE(status == 0);
}

TEST_CASE("unit_production_cost_hook_city_owner", "[ctest][unit-production-cost-rules]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    const auto                                 rule = makeRule("test.provider", "city-owner", -50);
    REQUIRE(RegisterUnitProductionCostRule(&rule) == UNIT_PRODUCTION_COST_RULES_OK);

    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto*              memory      = runtime.memory();
    constexpr uint32_t kAllocation = rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit;
    constexpr uint32_t kWritable   = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;
    for (const uint32_t page : { 0x830E0000u, 0x83140000u, 0x83150000u })
    {
        auto* heap = memory->LookupHeap(page);
        REQUIRE((heap && heap->AllocFixed(page, 0x10000, 0x10000, kAllocation, kWritable)));
    }

    // 0x82CF1154..0x82CF1164 reads owner at 0x8314FE28 + cityOrdinal * 0xBC.
    constexpr uint32_t kCityOwner                        = 0x8314FE28 + 0xBC;
    constexpr uint32_t kOtherPageOwner                   = 0x8315FE28 + 0xBC;
    *memory->TranslateVirtual<uint8_t*>(kCityOwner)      = 2;
    *memory->TranslateVirtual<uint8_t*>(kOtherPageOwner) = 3;
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(0x830ECD28 + 2 * 4), CIVILIZATION_AZTEC);
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(0x830ECD28 + 3 * 4), CIVILIZATION_GREEK);

    PPCRegister cityOffset{};
    PPCRegister unitType{};
    PPCRegister costScalar{};
    cityOffset.u64 = 0x12345678000000BCull;
    unitType.u64   = 0x8765432100000000ull | UNIT_TYPE_WARRIOR;
    costScalar.u64 = 0xABCDEF000000000Aull;
    ApplyUnitProductionCostPercent(cityOffset, unitType, costScalar);
    UNSCOPED_INFO("the native city owner selects the Jaguar cost rule");
    REQUIRE(costScalar.s64 == 5);
    REQUIRE(cityOffset.u64 == 0x12345678000000BCull);
    REQUIRE(unitType.u64 == (0x8765432100000000ull | UNIT_TYPE_WARRIOR));

    *memory->TranslateVirtual<uint8_t*>(kCityOwner)      = 3;
    *memory->TranslateVirtual<uint8_t*>(kOtherPageOwner) = 2;
    costScalar.u64                                       = 0xABCDEF000000000Aull;
    ApplyUnitProductionCostPercent(cityOffset, unitType, costScalar);
    UNSCOPED_INFO("an unrelated page cannot select the Jaguar rule for a Greek city");
    REQUIRE(costScalar.u64 == 0xABCDEF000000000Aull);
    REQUIRE(cityOffset.u64 == 0x12345678000000BCull);
    REQUIRE(unitType.u64 == (0x8765432100000000ull | UNIT_TYPE_WARRIOR));
}
