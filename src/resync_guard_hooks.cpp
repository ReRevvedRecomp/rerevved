#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REX_EXTERN(__imp__sub_82D8FCC8);
REX_EXTERN(__imp__sub_82D8AB00);

namespace
{

// Network manager; sub_82D8AB00(manager, index) is nonzero while the session has that player.
constexpr uint32_t kNetworkManagerGlobal = 0x8314EFE0;

uint32_t loadU32(const uint8_t* base, uint32_t address)
{
    const uint8_t* source = base + address;
    return uint32_t{ source[0] } << 24 | uint32_t{ source[1] } << 16 | uint32_t{ source[2] } << 8 |
           uint32_t{ source[3] };
}

bool inSession(PPCContext& caller, uint8_t* base, uint32_t manager, uint32_t player)
{
    PPCContext call = caller;
    call.r3.u64     = manager;
    call.r4.u64     = player;
    __imp__sub_82D8AB00(call, base);
    caller.fpscr = call.fpscr;
    return static_cast<uint8_t>(call.r3.u64) != 0;
}

} // namespace

// Resync start: r3 is the hot-join owner, r4 the target player.
REX_HOOK_RAW(sub_82D8FCC8)
{
    const uint32_t player  = ctx.r4.u32;
    const uint32_t manager = loadU32(base, kNetworkManagerGlobal);
    if (manager != 0 && !inSession(ctx, base, manager, player))
    {
        REXLOG_WARN("Resync guard: skipped a resync for player {}, who has left the game", static_cast<int32_t>(player));
        return;
    }
    __imp__sub_82D8FCC8(ctx, base);
}
