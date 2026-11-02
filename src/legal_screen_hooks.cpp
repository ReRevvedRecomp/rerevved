#include "legal_screen.h"

#include <cstdint>

#include <rex/cvar.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(skip_legal_screens, false, "ReRevved", "Finish the legal screens as soon as the main menu has started loading")
    .audience(rex::cvar::Audience::kPlayer)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace ls = rerevved::legal_screen;

// Before cmplwi cr6,r10,5000 at 0x82DEE89C (r10 elapsed dwell, r31 legal-screen owner):
// report full dwell only after preload (+0x434 set), so exactly one preload runs.
void SkipLegalScreen(PPCRegister& elapsed, PPCRegister& owner)
{
    if (!REXCVAR_GET(skip_legal_screens) || elapsed.u32 >= ls::kLegalScreenDwell)
    {
        return;
    }
    auto* kernel = REX_KERNEL_STATE();
    auto* memory = kernel ? kernel->memory() : nullptr;
    if (!memory)
    {
        return;
    }
    const uint32_t flag = owner.u32 + ls::kPreloadStartedOffset;
    auto*          heap = memory->LookupHeap(owner.u32);
    if (!heap || flag < owner.u32 || heap->QueryRangeAccess(owner.u32, flag) == rex::memory::PageAccess::kNoAccess)
    {
        return;
    }
    const uint32_t vtable = rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(owner.u32));
    if (vtable == ls::kLegalScreenVtable && *memory->TranslateVirtual<const uint8_t*>(flag) != 0)
    {
        elapsed.u64 = ls::kLegalScreenDwell;
    }
}
