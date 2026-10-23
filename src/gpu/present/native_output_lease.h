#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

struct ID3D12Resource;
struct ID3D12Fence;

namespace rerevved::gpu
{

// Retirement states of one published native output. Retired and Cancelled
// return the source to its producer; Failed never does.
enum class OutputLeaseState : std::uint8_t
{
    Published,     // offered after native completion, not yet claimed
    Claimed,       // a consumer owns the offer, no GPU work is submitted
    CopySubmitted, // the consumer copy is submitted and may read the source
    Retired,       // the consumer copy fence completed
    Cancelled,     // ended before any consumer GPU use
    Failed         // outcome unproved
};

enum class OutputLeaseFailure : std::uint8_t
{
    None,
    Timeout,
    DeviceLost,
    SubmitFailed,
    HostClosed
};

const char* OutputLeaseStateName(OutputLeaseState state) noexcept;

inline bool IsTerminalOutputLeaseState(OutputLeaseState state) noexcept
{
    return state == OutputLeaseState::Retired || state == OutputLeaseState::Cancelled || state == OutputLeaseState::Failed;
}

enum class DeadlinePhase : std::uint8_t
{
    Producer,
    Copy,
    Host
};

// One expiry shared by every wait of a drain. Spend is attributed to a phase
// for diagnostics; it never extends the expiry.
class OperationDeadline
{
public:
    using Clock = std::chrono::steady_clock;

    static OperationDeadline After(std::chrono::nanoseconds budget, Clock::time_point now = Clock::now()) noexcept
    {
        return At(now + (budget < std::chrono::nanoseconds::zero() ? std::chrono::nanoseconds::zero() : budget));
    }

    static OperationDeadline At(Clock::time_point expiry) noexcept
    {
        OperationDeadline deadline;
        deadline.expiry_ = expiry;
        return deadline;
    }

    Clock::time_point Expiry() const noexcept
    {
        return expiry_;
    }

    std::chrono::nanoseconds Remaining(Clock::time_point now = Clock::now()) const noexcept
    {
        return now >= expiry_ ? std::chrono::nanoseconds::zero() : std::chrono::duration_cast<std::chrono::nanoseconds>(expiry_ - now);
    }

    bool Expired(Clock::time_point now = Clock::now()) const noexcept
    {
        return now >= expiry_;
    }

    // Timeout for one bounded wait: the lesser of the cap and the shared remainder.
    std::chrono::nanoseconds Allot(std::chrono::nanoseconds cap, Clock::time_point now = Clock::now()) const noexcept
    {
        const auto remaining = Remaining(now);
        return cap < remaining ? cap : remaining;
    }

    void Charge(DeadlinePhase phase, std::chrono::nanoseconds elapsed) noexcept
    {
        spent_[static_cast<std::size_t>(phase)] += elapsed < std::chrono::nanoseconds::zero() ? std::chrono::nanoseconds::zero() : elapsed;
    }

    std::chrono::nanoseconds Spent(DeadlinePhase phase) const noexcept
    {
        return spent_[static_cast<std::size_t>(phase)];
    }

    std::chrono::nanoseconds SpentTotal() const noexcept
    {
        return spent_[0] + spent_[1] + spent_[2];
    }

private:
    Clock::time_point                       expiry_{};
    std::array<std::chrono::nanoseconds, 3> spent_{};
};

// Output lease transitions are checked; producer publishes after native
// completion and consumer retires after copy fence.
class NativeOutputLease final
{
public:
    using Clock = std::chrono::steady_clock;

    // The payload is opaque to the lease and immutable for its lifetime.
    explicit NativeOutputLease(std::uint64_t token, std::shared_ptr<const void> payload = {}, std::uint64_t gammaGeneration = 0);
    ~NativeOutputLease();

    NativeOutputLease(const NativeOutputLease&)            = delete;
    NativeOutputLease& operator=(const NativeOutputLease&) = delete;

    std::uint64_t Token() const noexcept;

    std::uint64_t GammaGeneration() const noexcept
    {
        return gammaGeneration_;
    }

    const std::shared_ptr<const void>& Payload() const noexcept;
    OutputLeaseState                   State() const;
    OutputLeaseFailure                 Failure() const;
    std::uint64_t                      CopySubmissionId() const;

    bool Claim();
    // Producer cancellation is legal only before the consumer claims the offer.
    bool Cancel();
    bool MarkCopySubmitted(std::uint64_t copySubmissionId);
    bool Retire();
    bool Fail(OutputLeaseFailure reason);
    // Truthful terminal state for an event that ends every pending lease:
    // unclaimed offers cancel, claimed or submitted ones fail with the reason.
    bool Terminate(OutputLeaseFailure reason);

    // Caps every producer wait on this lease at the shared drain expiry.
    void SetDeadline(Clock::time_point expiry);

    // True only for Retired or Cancelled, bounded by timeout and the deadline.
    bool AwaitRetirement(std::chrono::milliseconds timeout);
    // Producer slot reuse drops an unclaimed offer, but never a consumer-owned one.
    bool AwaitReuse(std::chrono::milliseconds timeout);
    bool DeadlineExpired() const;

private:
    const std::uint64_t               token_;
    const std::uint64_t               gammaGeneration_;
    const std::shared_ptr<const void> payload_;
    mutable std::mutex                mutex_;
    std::condition_variable           changed_;
    OutputLeaseState                  state_            = OutputLeaseState::Published;
    OutputLeaseFailure                failure_          = OutputLeaseFailure::None;
    std::uint64_t                     copySubmissionId_ = 0;
    bool                              hasDeadline_      = false;
    Clock::time_point                 deadline_{};
};

using NativeTitleOutputPublisher = std::function<std::shared_ptr<NativeOutputLease>(ID3D12Resource*, ID3D12Fence*, std::uint64_t)>;

} // namespace rerevved::gpu
