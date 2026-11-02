#include "cvar_scope.hpp"
#include "intro_movie.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/kernel/init.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(bool, skip_intro_movie);

void SkipIntroMovie(PPCRegister& flag, PPCRegister& state);

namespace im = rerevved::intro_movie;

TEST_CASE("intro_movie_hook_clears_only_the_intro_flag", "[ctest][intro-movie]")
{
    REREVVED_RESTORE_CVAR(skip_intro_movie);
    rex::Runtime runtime(std::filesystem::path{});
    REQUIRE(runtime.Setup(rex::RuntimeConfig{
                .kernel_init = rex::kernel::InitializeKernel,
                .tool_mode   = true,
            }) == 0);
    auto*              memory = runtime.memory();
    constexpr uint32_t kPage  = 0x830E0000;
    auto*              heap   = memory->LookupHeap(kPage);
    REQUIRE((heap && heap->AllocFixed(kPage, 0x10000, 0x10000, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)));
    constexpr uint32_t kIntro   = kPage;
    constexpr uint32_t kAttract = kPage + 0x10;
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kIntro), im::kIntroStateVtable);
    rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual<uint8_t*>(kAttract), 0x82121320);

    auto run = [](uint32_t object)
    {
        PPCRegister flag{}, state{};
        flag.u64  = 1;
        state.u64 = object;
        SkipIntroMovie(flag, state);
        CHECK(state.u64 == object);
        return flag.u64;
    };

    REQUIRE(rex::cvar::SetFlagByName("skip_intro_movie", "false"));
    CHECK(run(kIntro) == 1);
    REQUIRE(rex::cvar::SetFlagByName("skip_intro_movie", "true"));
    CHECK(run(kIntro) == 0);
    CHECK(run(kAttract) == 1);
    CHECK(run(0x1000) == 1);
}
