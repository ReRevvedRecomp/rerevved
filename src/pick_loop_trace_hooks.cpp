// Peers mark only their own human in 0x830EE7DC, so the redraws per pick can differ
// between peers.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(pick_loop_trace, false, "ReRevved", "Log each turn selection pick with its draw count and player masks");

namespace
{

constexpr uint32_t kTurn = 0x8312B8DC;

uint32_t draws = 0;

uint32_t currentTurn()
{
    auto* memory = REX_KERNEL_MEMORY();
    if (!memory)
    {
        return UINT32_MAX;
    }
    return rex::memory::load_and_swap<uint32_t>(memory->TranslateVirtual<const uint8_t*>(kTurn));
}

} // namespace

void TracePickLoopGate()
{
    draws = 0;
}

void TracePickLoopDraw()
{
    ++draws;
}

void TracePickLoopPick(PPCRegister& pick, PPCRegister& candidates, PPCRegister& marked)
{
    if (!REXCVAR_GET(pick_loop_trace))
    {
        return;
    }
    REXLOG_INFO("Pick loop: turn={} pick={} draws={} candidates=0x{:08X} marked=0x{:08X}",
                currentTurn(),
                pick.s32,
                draws,
                candidates.u32,
                marked.u32);
}
