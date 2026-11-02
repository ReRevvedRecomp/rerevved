#include "cvar_scope.hpp"
#include "legal_screen.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, skip_legal_screens);

void SkipLegalScreen(PPCRegister& elapsed, PPCRegister& owner);

namespace ls = rerevved::legal_screen;

TEST_CASE("legal_screen_hook_completes_only_after_preload", "[ctest][legal-screen]")
{
    REREVVED_RESTORE_CVAR(skip_legal_screens);
    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto*              memory = runtime.memory();
    constexpr uint32_t kPage  = 0x830E0000;
    auto*              heap   = memory->LookupHeap(kPage);
    REQUIRE((heap && heap->AllocFixed(kPage, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)));
    constexpr uint32_t kLegal = kPage;
    constexpr uint32_t kOther = kPage + 0x1000;
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kLegal), ls::kLegalScreenVtable);
    // The IntroMovie state table.
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kOther), 0x82131A18);
    auto preload = [memory](uint32_t object, uint8_t value)
    {
        *memory->TranslateVirtual<uint8_t*>(object + ls::kPreloadStartedOffset) = value;
    };

    auto run = [](uint32_t object, uint32_t elapsed)
    {
        PPCRegister time{}, owner{};
        time.u64  = elapsed;
        owner.u64 = object;
        SkipLegalScreen(time, owner);
        CHECK(owner.u64 == object);
        return time.u64;
    };

    preload(kLegal, 1);
    preload(kOther, 1);
    REQUIRE(rex::cvar::SetFlagByName("skip_legal_screens", "false"));
    CHECK(run(kLegal, 10) == 10);
    REQUIRE(rex::cvar::SetFlagByName("skip_legal_screens", "true"));
    CHECK(run(kLegal, 10) == ls::kLegalScreenDwell);
    CHECK(run(kLegal, 7000) == 7000);
    CHECK(run(kOther, 10) == 10);
    preload(kLegal, 0);
    CHECK(run(kLegal, 10) == 10);
    CHECK(run(0x1000, 10) == 10);
}
