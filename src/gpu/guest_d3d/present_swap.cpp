#include "present_swap.h"

#include <atomic>
#include <exception>
#include <utility>

namespace rerevved::gpu::guest_d3d
{

namespace
{

std::atomic<std::uint64_t> commitFailures{ 0 };

// The commit threw after its first effect. The call stays consumed, so the
// failure is counted and logged on the first occurrence and each power of two.
void failCommit(IPresentSwapHost& host, std::string_view what) noexcept
{
    CountPresentOutcome(PresentOutcome::CommitFailure);
    const auto total = commitFailures.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((total & (total - 1)) == 0)
        host.LogCommitFailure(total, what);
}

} // namespace

std::optional<rex::system::DetachedVideoSwap> DecodePresentSwap(IRingStubMemory& memory, const PresentSwapArguments& arguments)
{
    rex::system::DetachedVideoSwap swap{};
    swap.reservation = arguments.r3;
    for (std::uint32_t i = 0; i < 6; ++i)
    {
        const auto word = memory.Read32(arguments.r4 + 4 * i);
        if (!word)
            return std::nullopt;
        swap.fetch[i] = *word;
    }
    const auto format        = memory.Read32(arguments.r9);
    const auto colorSpace    = memory.Read32(arguments.r10);
    const auto widthSlot     = memory.Read32(arguments.r1 + 0x54);
    const auto heightSlot    = memory.Read32(arguments.r1 + 0x5C);
    const auto width         = widthSlot ? memory.Read32(*widthSlot) : std::nullopt;
    const auto height        = heightSlot ? memory.Read32(*heightSlot) : std::nullopt;
    swap.frontbuffer_virtual = swap.fetch[1] & 0xFFFFF000U;
    const auto physical      = memory.Physical(swap.frontbuffer_virtual);
    if (!format || !colorSpace || !width || !height || !physical)
        return std::nullopt;
    // As the SDK's VdSwap: the fetch names the frontbuffer by physical page.
    swap.frontbuffer_physical = *physical;
    swap.fetch[1]             = (swap.fetch[1] & 0xFFFU) | (*physical & 0xFFFFF000U);
    swap.texture_format       = *format;
    swap.color_space          = *colorSpace;
    swap.width                = *width;
    swap.height               = *height;
    return swap;
}

bool ReplacePresentSwap(IPresentSwapHost& host, const PresentSwapArguments& arguments) noexcept
{
    CountPresentOutcome(PresentOutcome::Enter);

    std::shared_ptr<rex::system::IDetachedVideoDelegate> delegate;
    std::optional<rex::system::DetachedVideoSwap>        swap;
    auto                                                 refusal = PresentOutcome::RefusedException;
    std::string_view                                     detail  = "preflight exception";
    try
    {
        const auto services = host.Services();
        if (!services.memory || !services.runtime)
        {
            refusal = PresentOutcome::RefusedRuntime;
            detail  = "no guest memory or runtime";
        }
        else if (!services.delegate)
        {
            refusal = PresentOutcome::RefusedDelegate;
            detail  = "no video delegate";
        }
        else if (auto decoded = DecodePresentSwap(*services.memory, arguments); !decoded)
        {
            refusal = PresentOutcome::RefusedArgs;
            detail  = "swap arguments unreadable";
        }
        else
        {
            swap     = std::move(decoded);
            delegate = services.delegate;
        }
    }
    catch (...)
    {
        swap.reset();
        refusal = PresentOutcome::RefusedException;
        detail  = "preflight exception";
    }
    if (!swap)
    {
        CountPresentOutcome(refusal);
        host.Note(false, detail);
        return false;
    }

    // The commit: from here Original never runs.
    try
    {
        host.ObserveSource();
        delegate->OnSwap(*swap);
    }
    catch (const std::exception& error)
    {
        failCommit(host, error.what());
        return true;
    }
    catch (...)
    {
        failCommit(host, "unknown exception");
        return true;
    }
    CountPresentOutcome(PresentOutcome::Consumed);
    host.Note(true, {});
    return true;
}

void ResetPresentSwapLogForTesting() noexcept
{
    commitFailures.store(0, std::memory_order_relaxed);
}

} // namespace rerevved::gpu::guest_d3d
