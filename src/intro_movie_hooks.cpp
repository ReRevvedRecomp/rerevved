#include "intro_movie.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(skip_intro_movie, false, "ReRevved", "Skip the legal screens and the opening movie at startup")
    .audience(rex::cvar::Audience::kPlayer)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// r9 holds frontend flag bit 0x1000, r31 the state object; clearing r9 for the intro object takes the native no-movie path.
void SkipIntroMovie(PPCRegister& flag, PPCRegister& state)
{
    if (!REXCVAR_GET(skip_intro_movie) || flag.u32 == 0)
    {
        return;
    }
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory)
    {
        return;
    }
    auto* heap = memory->LookupHeap(state.u32);
    if (!heap || heap->QueryRangeAccess(state.u32, state.u32 + 3) == rex::memory::PageAccess::kNoAccess)
    {
        return;
    }
    const uint32_t vtable = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(state.u32));
    if (vtable == rerevved::intro_movie::kIntroStateVtable)
    {
        flag.u64 = 0;
    }
}
