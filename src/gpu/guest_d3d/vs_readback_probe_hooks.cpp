#include "vs_readback_probe.h"

#include <rex/hook.h>

namespace guest = rerevved::gpu::guest_d3d;

// Each VS wrapper calls Original once; observer reads guarded scalars only, and
// unwind abandons rather than ending the call.
REX_EXTERN(__imp__sub_82662D28);
REX_EXTERN(__imp__sub_82694480);

REX_HOOK_RAW(sub_82662D28)
{
    if (!guest::VsReadbackProbeRequested()) [[likely]]
    {
        __imp__sub_82662D28(ctx, base);
        return;
    }
    guest::VsReadbackScope refresh(guest::VsReadbackFunction::Refresh, ctx);
    __imp__sub_82662D28(ctx, base);
    refresh.End(ctx);
}

REX_HOOK_RAW(sub_82694480)
{
    if (!guest::VsReadbackProbeRequested()) [[likely]]
    {
        __imp__sub_82694480(ctx, base);
        return;
    }
    guest::VsReadbackScope readback(guest::VsReadbackFunction::Readback, ctx);
    __imp__sub_82694480(ctx, base);
    readback.End(ctx);
}
