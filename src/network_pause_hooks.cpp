// The native resume request in a network game sends a state message whose
// local processing is not guaranteed while a peer is connected. Any player's
// resume is also applied locally through the same consumer.

#include "network_pause.h"

#include "ai_civilizations.h"

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REX_EXTERN(__imp__sub_82D7CE90);
REX_EXTERN(__imp__sub_82E2A998);

namespace
{

namespace pause = rerevved::network_pause;

uint8_t loadU8(const uint8_t* base, uint32_t address)
{
    return base[address];
}

uint32_t loadU32(const uint8_t* base, uint32_t address)
{
    const uint8_t* source = base + address;
    return uint32_t{ source[0] } << 24 | uint32_t{ source[1] } << 16 | uint32_t{ source[2] } << 8 |
           uint32_t{ source[3] };
}

// Calls the consumer on a copy of the caller's context so the caller's
// registers stay unchanged.
void callConsumer(PPCContext& caller, uint8_t* base, uint32_t root)
{
    PPCContext call = caller;
    call.r3.u64     = root;
    call.r4.u64     = 0;
    call.r5.u64     = pause::kResumeLocalArgument;
    __imp__sub_82E2A998(call, base);
    caller.fpscr = call.fpscr;
}

} // namespace

// Root child dispatch. low8(r4) is 0 for resume, nonzero for a pause request.
REX_HOOK_RAW(sub_82D7CE90)
{
    // The native call may clobber r4.
    const uint8_t requestByte = static_cast<uint8_t>(ctx.r4.u64);
    __imp__sub_82D7CE90(ctx, base);

    const uint8_t  networkByte = loadU8(base, rerevved::ai_civilizations::kNetworkGameByte);
    const uint32_t root        = loadU32(base, pause::kFrontendRootGlobal);
    const uint32_t rootFlags   = root != 0 ? loadU32(base, root + pause::kRootFlagsOffset) : 0;
    if (!pause::ShouldApplyLocalResume(networkByte, requestByte, root, rootFlags))
    {
        return;
    }

    // The consumer clears the pause bit, so the native message that reaches it
    // later finds the bit clear and skips every effect.
    callConsumer(ctx, base, root);
    REXLOG_INFO("Multiplayer pause: resume applied locally");
}
