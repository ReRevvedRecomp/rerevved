#include "version_label.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <string>

#include <rex/kernel/init.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

void ReplaceMainMenuVersion(PPCRegister& version);

TEST_CASE("version_label_hook_replaces_only_the_retail_literal", "[ctest][version-label]")
{
    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto* memory = runtime.memory();

    PPCRegister other{};
    other.u64 = 0x82139CB0;
    ReplaceMainMenuVersion(other);
    CHECK(other.u64 == 0x82139CB0);

    PPCRegister first{};
    first.u64 = rerevved::version_label::kRetailVersionLiteral;
    ReplaceMainMenuVersion(first);
    REQUIRE(first.u64 != rerevved::version_label::kRetailVersionLiteral);
    CHECK(std::string(memory->TranslateVirtual<const char*>(first.u32)) == std::string("v") + REREVVED_TEST_GAME_VERSION);

    PPCRegister again{};
    again.u64 = rerevved::version_label::kRetailVersionLiteral;
    ReplaceMainMenuVersion(again);
    CHECK(again.u64 == first.u64);
}
