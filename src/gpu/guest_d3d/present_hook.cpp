#include "present_swap.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <cstdint>
#include <string_view>

// The swap observer at 0x826A4884, which the replacement jumps past.
void ObserveRendererSwapSource(PPCRegister& r3, PPCRegister& r4, PPCRegister& r30, PPCRegister& r31);

namespace
{

namespace guest = rerevved::gpu::guest_d3d;

constexpr std::uint32_t kSwapCallSite = 0x826A4884;

class PresentHost final : public guest::IPresentSwapHost
{
public:
    PresentHost(PPCRegister& r3, PPCRegister& r4, PPCRegister& r30, PPCRegister& r31)
    : r3(r3)
    , r4(r4)
    , r30(r30)
    , r31(r31)
    {
    }

    guest::PresentSwapServices Services() override
    {
        guest::PresentSwapServices services;
        services.memory  = guest::GuestRingMemory();
        auto* kernel     = rex::system::kernel_state();
        auto* owner      = kernel ? kernel->emulator() : nullptr;
        services.runtime = owner != nullptr;
        if (owner)
            services.delegate = owner->detached_video_delegate();
        return services;
    }

    void ObserveSource() override
    {
        ObserveRendererSwapSource(r3, r4, r30, r31);
    }

    void Note(bool consumed, std::string_view detail) noexcept override
    {
        guest::NoteRingStub(guest::RingStub::Present, consumed, kSwapCallSite, detail);
    }

    void LogCommitFailure(std::uint64_t total, std::string_view what) noexcept override
    {
        try
        {
            REXLOG_ERROR("native_ring_stubs: the VdSwap commit threw after its effect began, so Original is skipped (failure {}): {}", total, what);
        }
        catch (...)
        {
        }
    }

private:
    PPCRegister &r3, &r4, &r30, &r31;
};

} // namespace

// After 0x826A4880, stubs jump 0x826A4888; keep G+0x30 = r30+256 and owner
// fence/flip state.
bool ReplaceVdSwapCall(PPCRegister& r1, PPCRegister& r3, PPCRegister& r4, PPCRegister& r9, PPCRegister& r10, PPCRegister& r30, PPCRegister& r31)
{
    if (!guest::ringStubsActive.load(std::memory_order_relaxed)) [[likely]]
        return false;
    PresentHost host(r3, r4, r30, r31);
    return guest::ReplacePresentSwap(host, { r1.u32, r3.u32, r4.u32, r9.u32, r10.u32 });
}
