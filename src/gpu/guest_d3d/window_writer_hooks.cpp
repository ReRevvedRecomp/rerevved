#include "clear_derive.h"
#include "developer_hooks.h"
#include "lowerer_derive.h"
#include "resolve_derive.h"
#include "ring_stubs.h"
#include "window_writer_consume.h"

#include <rex/hook.h>

// native_consume_window_writer may replace 0x8268D3A0 only for a known exit.
// native_window_writer_exit_check probes Original; consume runs first, never
// probing consumed calls. Other diagnostics retain body.
REX_EXTERN(__imp__sub_8268D3A0);

REX_HOOK_RAW(sub_8268D3A0)
{
    auto* const memory = rerevved::gpu::guest_d3d::GuestRingMemory();
    if (rerevved::gpu::guest_d3d::WindowWriterConsumeRequested() &&
        rerevved::gpu::guest_d3d::ConsumeWindowWriter(
            ctx,
            memory,
            rerevved::gpu::guest_d3d::DeveloperRecorderActive() ||
                rerevved::gpu::guest_d3d::ClearDeriveCheckRequested() ||
                rerevved::gpu::guest_d3d::ClearEffectCheckRequested() ||
                rerevved::gpu::guest_d3d::ResolveDeriveCheckRequested() ||
                rerevved::gpu::guest_d3d::ResolveEffectCheckRequested() ||
                rerevved::gpu::guest_d3d::LowererExitCheckRequested())) [[unlikely]]
        return;
    [[maybe_unused]] const rerevved::gpu::guest_d3d::WindowWriterExitProbe windowWriterExit(ctx, memory);
    __imp__sub_8268D3A0(ctx, base);
}
