#pragma once

#include <algorithm>
#include <deque>
#include <mutex>

#include "native_presentation.h"

namespace rerevved::gpu
{

// CPU queue policy shared by the validated GPU ingress and its tests.
// Locked methods require mutex; public entry points take it themselves.
struct OfferGate
{
    std::mutex                                     mutex;
    std::deque<std::shared_ptr<NativeOutputLease>> queue;
    PresentationLedger                             ledger;
    std::size_t                                    capacity          = 2;
    std::uint64_t                                  nextToken         = 1;
    std::uint64_t                                  surfaceGeneration = 0;
    std::uint64_t                                  gammaGeneration   = 0;
    bool                                           accepting         = true;
    bool                                           suspended         = false;
    std::vector<std::weak_ptr<NativeOutputLease>>  outstanding;

    std::shared_ptr<NativeOutputLease> offerLocked(std::shared_ptr<const void> payload, std::optional<std::uint64_t> publishedGamma = std::nullopt)
    {
        if (!accepting)
        {
            ledger.OnRejected();
            return nullptr;
        }
        std::erase_if(outstanding, [](const auto& weak)
                      {
                          auto lease = weak.lock();
                          return !lease || IsTerminalOutputLeaseState(lease->State());
                      });
        const auto generation = publishedGamma.value_or(gammaGeneration);
        auto       lease      = std::make_shared<NativeOutputLease>(nextToken++, std::move(payload), generation);
        outstanding.push_back(lease);
        ledger.OnOffered(lease->Token(), surfaceGeneration, generation);
        if (suspended)
        {
            lease->Cancel();
            ledger.OnDropped(true);
            return lease;
        }
        queue.push_back(lease);
        while (queue.size() > capacity)
        {
            auto oldest = std::move(queue.front());
            queue.pop_front();
            if (oldest->Cancel() || oldest->State() == OutputLeaseState::Cancelled)
                ledger.OnDropped(false);
        }
        return lease;
    }

    void close(OperationDeadline& deadline)
    {
        std::lock_guard lock(mutex);
        accepting = false;
        for (const auto& weak : outstanding)
            if (auto lease = weak.lock())
                lease->SetDeadline(deadline.Expiry());
        cancelQueuedLocked(false);
    }

    void terminateLocked(OutputLeaseFailure reason)
    {
        for (const auto& weak : outstanding)
        {
            if (auto lease = weak.lock())
                lease->Terminate(reason);
        }
        queue.clear();
    }

    void cancelQueuedLocked(bool whileSuspended)
    {
        for (const auto& lease : queue)
        {
            if (lease->Cancel() || lease->State() == OutputLeaseState::Cancelled)
            {
                ledger.OnDropped(whileSuspended);
            }
        }
        queue.clear();
    }

    void setSuspended(bool value)
    {
        std::lock_guard lock(mutex);
        suspended = value;
        if (value)
        {
            cancelQueuedLocked(true);
        }
    }

    // Claims the newest live offer; older unclaimed offers cancel.
    std::shared_ptr<NativeOutputLease> claimNewest()
    {
        std::lock_guard                    lock(mutex);
        std::shared_ptr<NativeOutputLease> claimed;
        while (!queue.empty())
        {
            auto lease = std::move(queue.back());
            queue.pop_back();
            if (!claimed)
            {
                if (lease->Claim())
                {
                    ledger.OnClaimed();
                    claimed = std::move(lease);
                }
                else if (lease->State() == OutputLeaseState::Cancelled)
                {
                    ledger.OnDropped(false);
                }
                else
                {
                    ledger.OnStale();
                }
            }
            else if (lease->Cancel() || lease->State() == OutputLeaseState::Cancelled)
            {
                ledger.OnDropped(false);
            }
            else
            {
                ledger.OnStale();
            }
        }
        return claimed;
    }
};

} // namespace rerevved::gpu
