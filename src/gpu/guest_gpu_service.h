#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rex/system/interfaces/video_delegate.h>
#include <rex/system/xtypes.h>

#include "gpu/completion/completion_ledger.h"

namespace rex
{

class Runtime;

}

namespace rerevved::gpu
{

inline constexpr uint32_t kMaxVblankCatchUp = 4;

inline constexpr std::chrono::milliseconds kVblankJoinTimeout{ 1000 };

// The vblank join gets at most kVblankJoinTimeout and never extends past the
// window-close drain deadline.
inline std::chrono::milliseconds VblankJoinTimeout(std::chrono::steady_clock::time_point now,
                                                   std::chrono::steady_clock::time_point closeDeadline)
{
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(closeDeadline - now);
    return std::clamp(remaining, std::chrono::milliseconds::zero(), kVblankJoinTimeout);
}

struct VblankStep
{
    uint32_t due           = 0;
    uint64_t lastFrameTime = 0;
};

// Title vblank policy: emit at most kMaxVblankCatchUp intervals per pass, then
// resynchronize to `now` when still behind. The SDK vsync loop has no cap.
VblankStep AdvanceVblank(uint64_t now, uint64_t lastFrameTime, uint64_t intervalTicks);

struct CompletionDebt
{
    uint32_t ringPointer              = 0;
    uint32_t ringSizeLog2             = 0;
    uint32_t writebackPointer         = 0;
    uint32_t writePointer             = 0; // Last CP_RB_WPTR value the guest published.
    uint32_t readPointer              = 0; // Guest word at the writeback pointer, when readable.
    bool     readPointerReadable      = false;
    uint64_t writePointerPublications = 0; // Publications in this writeback generation.
    uint64_t swapCount                = 0;
    uint64_t vblankCount              = 0;
    uint32_t ringGeneration           = 0;
    uint32_t writebackGeneration      = 0;
    bool     configurationFrozen      = false;
    // B+0 at ledger.fenceBlock, the newest fence record's or fence wait's block;
    // empty when that block is unknown (zero) or unreadable.
    std::optional<uint32_t>    completedFence;
    completion::LedgerSnapshot ledger;
};

// Reports once when the published write pointer has differed from an unchanged
// writeback word for the threshold. An unreadable word counts as no progress.
// Reset starts a new writeback generation. It never writes guest memory.
class CompletionDebtMonitor
{
public:
    using Clock = std::chrono::steady_clock;

    explicit CompletionDebtMonitor(Clock::duration threshold);

    bool Observe(Clock::time_point       now,
                 uint64_t                writePointerPublications,
                 uint32_t                writePointer,
                 std::optional<uint32_t> writebackWord);
    bool Reported() const;
    void Reset();

private:
    Clock::duration         threshold;
    Clock::time_point       pendingSince{};
    std::optional<uint32_t> pendingWriteback;
    bool                    pending  = false;
    bool                    reported = false;
};

// Defined inline so the route tests can feed the monitor the ledger's RPTR observations.
inline CompletionDebtMonitor::CompletionDebtMonitor(Clock::duration threshold)
: threshold(threshold)
{
}

inline bool CompletionDebtMonitor::Observe(Clock::time_point       now,
                                           uint64_t                writePointerPublications,
                                           uint32_t                writePointer,
                                           std::optional<uint32_t> writebackWord)
{
    if (reported)
    {
        return false;
    }
    if (writePointerPublications == 0 || writebackWord == writePointer)
    {
        pending = false;
        return false;
    }
    if (!pending)
    {
        pending          = true;
        pendingSince     = now;
        pendingWriteback = writebackWord;
        return false;
    }
    if (writebackWord)
    {
        if (pendingWriteback && *pendingWriteback != *writebackWord)
        {
            pendingSince     = now;
            pendingWriteback = writebackWord;
            return false;
        }
        pendingWriteback = writebackWord;
    }
    if (now - pendingSince < threshold)
    {
        return false;
    }
    reported = true;
    return true;
}

inline bool CompletionDebtMonitor::Reported() const
{
    return reported;
}

inline void CompletionDebtMonitor::Reset()
{
    pending  = false;
    reported = false;
    pendingWriteback.reset();
}

struct SwapEpoch
{
    uint64_t                sequence            = 0;
    uint64_t                vblankCount         = 0;
    uint32_t                reservation         = 0;
    uint32_t                frontbufferPhysical = 0;
    std::array<uint32_t, 6> fetch{};
    uint32_t                textureFormat    = 0;
    uint32_t                width            = 0;
    uint32_t                height           = 0;
    uint32_t                writePointer     = 0;
    bool                    sizeMatchesFetch = true;
    bool                    partial          = false; // skip mode released part of this epoch
};

// Native video executes no PM4. Hook-ledger spans release on GPU Completion for
// fences/scratch/source 1/RPTR; vblank raises both sources. Ring registration
// starts epoch; unsafe range freezes metadata/fails ledger.
class GuestVideoServices final : public rex::system::IDetachedVideoDelegate
{
public:
    static constexpr size_t kMaxSwapEpochs = 64;

    // A host draw failure the completion worker polled. A fatal one leaves no
    // later span able to complete, so it fails the ledger in census mode too.
    struct DrawFailure
    {
        std::string detail;
        bool        fatal = false;
    };

    struct Options
    {
        CompletionDebtMonitor::Clock::duration debtThreshold = std::chrono::seconds(2);
        // Called at most once per writeback generation on the vblank thread,
        // outside service locks.
        std::function<void(const CompletionDebt&)> reportDebt;
        guest_d3d::NativeDrawMode                  drawMode = guest_d3d::NativeDrawMode::Strict;
        // Not owned; null leaves spans with admitted draws unowned.
        completion::IDrawCompletion* draws = nullptr;
        // GPU Completion polls outside the ledger lock; census counts each
        // failure until a fatal one fails the ledger.
        std::function<std::optional<DrawFailure>()> pollDrawFailure;
        // Called once for the ledger's session failure, outside service locks.
        std::function<void(const completion::LedgerFailure&)> reportFailure;
        // Runs on VdSwap before ledger->Swap; a returned detail fails both draw modes.
        // An in-flight call may outlive StopProducers. The app keeps the dispatcher
        // and assembler alive until OnShutdown after the guest exits.
        std::function<std::optional<std::string>(const rex::system::DetachedVideoSwap&)> assembleSwap;
    };

    GuestVideoServices();
    ~GuestVideoServices() override;

    GuestVideoServices(const GuestVideoServices&)            = delete;
    GuestVideoServices& operator=(const GuestVideoServices&) = delete;

    // Creates the ledger, the GPU Completion and vblank threads, binds the GPU
    // MMIO range and publishes the progress hooks. Call after Runtime::Setup
    // and before the guest module launches.
    rex::X_STATUS Start(rex::Runtime& runtime, Options options);
    // Closes ledger admission, then joins the GPU Completion and vblank
    // threads by one deadline. No ledger write starts after the close. Returns
    // false when a thread did not exit in time; MMIO stays bound.
    bool StopProducers(std::chrono::milliseconds timeout);
    // Stops producers, then unbinds MMIO after in-flight callbacks finish.
    // Returns the producer join result.
    bool Shutdown(std::chrono::milliseconds timeout = kVblankJoinTimeout);

    void SetInterruptCallback(uint32_t callback, uint32_t userData) override;
    void InitializeRingBuffer(uint32_t ptr, uint32_t sizeLog2) override;
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t blockSizeLog2) override;
    void OnSwap(const rex::system::DetachedVideoSwap& swap) override;

    uint64_t               VblankCount() const;
    uint32_t               InterruptCallback() const;
    uint32_t               LastWritePointer() const;
    uint64_t               WritePointerPublications() const;
    bool                   ConfigurationFrozen() const;
    uint32_t               RingGeneration() const;
    uint32_t               WritebackGeneration() const;
    uint32_t               WritebackPointer() const;
    uint64_t               SwapCount() const;
    uint64_t               SwapSizeMismatches() const;
    std::vector<SwapEpoch> SwapEpochs() const;
    bool                   ProducersRunning() const;
    bool                   CompletionDebtReported() const;
    uint64_t               CallbackInterrupts() const;
    // A source-1 request is queued and the vblank thread has not taken it.
    bool CallbackInterruptPending() const;
    // Null before Start. Draw observers hold their own reference.
    std::shared_ptr<completion::CompletionLedger> Ledger() const;

    struct State;

private:
    std::shared_ptr<State> state;
};

} // namespace rerevved::gpu
