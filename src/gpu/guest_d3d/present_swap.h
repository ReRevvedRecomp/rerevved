#pragma once

#include "ring_stubs.h"

#include <rex/system/interfaces/video_delegate.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace rerevved::gpu::guest_d3d
{

// Owner 0x826A4638 calls VdSwap at 0x826A4884: r3=64-dword
// reservation, r4=six fetch dwords, r9=format, r10=color space;
// width/height pointers r1+0x54/r1+0x5C.
struct PresentSwapArguments
{
    std::uint32_t r1 = 0, r3 = 0, r4 = 0, r9 = 0, r10 = 0;
};

// The swap as the SDK's VdSwap decodes it, with the fetch naming the
// frontbuffer by physical page; empty when any read or the mapping fails.
std::optional<rex::system::DetachedVideoSwap> DecodePresentSwap(IRingStubMemory& memory, const PresentSwapArguments& arguments);

// What the preflight reads from the runtime.
struct PresentSwapServices
{
    IRingStubMemory*                                     memory  = nullptr;
    bool                                                 runtime = false; // the kernel and its owner exist
    std::shared_ptr<rex::system::IDetachedVideoDelegate> delegate;
};

// The runtime behind the replacement. Services is the only preflight read;
// ObserveSource and the delegate's OnSwap are the commit.
class IPresentSwapHost
{
public:
    virtual ~IPresentSwapHost()                 = default;
    virtual PresentSwapServices Services()      = 0;
    virtual void                ObserveSource() = 0;
    // The stub_present (consumed) or stub_present_original (refused) report.
    virtual void Note(bool consumed, std::string_view detail) noexcept = 0;
    // The first commit failure and each later power of two; `total` counts them.
    virtual void LogCommitFailure(std::uint64_t total, std::string_view what) noexcept = 0;
};

// After commit starts, every path returns true; exceptions count as
// commit failure and are not rethrown.
bool ReplacePresentSwap(IPresentSwapHost& host, const PresentSwapArguments& arguments) noexcept;

// Test seam: clears the commit failure total that paces the log.
void ResetPresentSwapLogForTesting() noexcept;

} // namespace rerevved::gpu::guest_d3d
