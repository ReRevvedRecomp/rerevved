#include "native_output_lease.h"

#include <utility>

namespace rerevved::gpu
{

const char* OutputLeaseStateName(OutputLeaseState state) noexcept
{
    switch (state)
    {
        case OutputLeaseState::Published:
            return "published";
        case OutputLeaseState::Claimed:
            return "claimed";
        case OutputLeaseState::CopySubmitted:
            return "copy-submitted";
        case OutputLeaseState::Retired:
            return "retired";
        case OutputLeaseState::Cancelled:
            return "cancelled";
        case OutputLeaseState::Failed:
            return "failed";
    }
    return "unknown";
}

NativeOutputLease::NativeOutputLease(std::uint64_t token, std::shared_ptr<const void> payload, std::uint64_t gammaGeneration)
: token_(token)
, gammaGeneration_(gammaGeneration)
, payload_(std::move(payload))
{
}

NativeOutputLease::~NativeOutputLease() = default;

std::uint64_t NativeOutputLease::Token() const noexcept
{
    return token_;
}

const std::shared_ptr<const void>& NativeOutputLease::Payload() const noexcept
{
    return payload_;
}

OutputLeaseState NativeOutputLease::State() const
{
    std::lock_guard lock(mutex_);
    return state_;
}

OutputLeaseFailure NativeOutputLease::Failure() const
{
    std::lock_guard lock(mutex_);
    return failure_;
}

std::uint64_t NativeOutputLease::CopySubmissionId() const
{
    std::lock_guard lock(mutex_);
    return copySubmissionId_;
}

bool NativeOutputLease::Claim()
{
    std::lock_guard lock(mutex_);
    if (state_ != OutputLeaseState::Published)
    {
        return false;
    }
    state_ = OutputLeaseState::Claimed;
    return true;
}

bool NativeOutputLease::Cancel()
{
    std::lock_guard lock(mutex_);
    if (state_ != OutputLeaseState::Published)
    {
        return false;
    }
    state_ = OutputLeaseState::Cancelled;
    changed_.notify_all();
    return true;
}

bool NativeOutputLease::MarkCopySubmitted(std::uint64_t copySubmissionId)
{
    std::lock_guard lock(mutex_);
    if (state_ != OutputLeaseState::Claimed || copySubmissionId == 0 || copySubmissionId == UINT64_MAX)
    {
        return false;
    }
    copySubmissionId_ = copySubmissionId;
    state_            = OutputLeaseState::CopySubmitted;
    return true;
}

bool NativeOutputLease::Retire()
{
    std::lock_guard lock(mutex_);
    if (state_ != OutputLeaseState::CopySubmitted)
    {
        return false;
    }
    state_ = OutputLeaseState::Retired;
    changed_.notify_all();
    return true;
}

bool NativeOutputLease::Fail(OutputLeaseFailure reason)
{
    std::lock_guard lock(mutex_);
    if (reason == OutputLeaseFailure::None || (state_ != OutputLeaseState::Claimed && state_ != OutputLeaseState::CopySubmitted))
    {
        return false;
    }
    state_   = OutputLeaseState::Failed;
    failure_ = reason;
    changed_.notify_all();
    return true;
}

bool NativeOutputLease::Terminate(OutputLeaseFailure reason)
{
    std::lock_guard lock(mutex_);
    switch (state_)
    {
        case OutputLeaseState::Published:
            state_ = OutputLeaseState::Cancelled;
            break;
        case OutputLeaseState::Claimed:
        case OutputLeaseState::CopySubmitted:
            state_   = OutputLeaseState::Failed;
            failure_ = reason == OutputLeaseFailure::None ? OutputLeaseFailure::HostClosed : reason;
            break;
        default:
            return false;
    }
    changed_.notify_all();
    return true;
}

void NativeOutputLease::SetDeadline(Clock::time_point expiry)
{
    std::lock_guard lock(mutex_);
    if (!hasDeadline_ || expiry < deadline_)
    {
        hasDeadline_ = true;
        deadline_    = expiry;
    }
    changed_.notify_all();
}

bool NativeOutputLease::AwaitReuse(std::chrono::milliseconds timeout)
{
    Cancel();
    return AwaitRetirement(timeout);
}

bool NativeOutputLease::DeadlineExpired() const
{
    std::lock_guard lock(mutex_);
    return hasDeadline_ && Clock::now() >= deadline_;
}

bool NativeOutputLease::AwaitRetirement(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    const auto       until = Clock::now() + (timeout < std::chrono::milliseconds::zero() ? std::chrono::milliseconds::zero() : timeout);
    // A deadline installed during the wait must also end it.
    while (!IsTerminalOutputLeaseState(state_))
    {
        const auto limit = hasDeadline_ && deadline_ < until ? deadline_ : until;
        if (Clock::now() >= limit)
        {
            break;
        }
        changed_.wait_until(lock, limit);
    }
    return state_ == OutputLeaseState::Retired || state_ == OutputLeaseState::Cancelled;
}

} // namespace rerevved::gpu
