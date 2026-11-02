#pragma once

// Modal Options runner 0x82E40738 (r4=r5=0): caller routing, PageGate closure signaling and loop break rules.

#include <atomic>
#include <cstdint>
#include <optional>

namespace rerevved::options_modal
{

// Pause entry 0x82D84580 is reached from 0x82D06398 via 0x8314F76C virtual +0xCC; it returns to 0x82D068E4.
constexpr uint32_t kPauseReturn   = 0x82D068E4;
constexpr uint32_t kPauseReceiver = 0x8314F76C;

// Receiver slot +0x1F4 takes a token that 0x82D84580 hands back to +0x1F0 when its low byte is nonzero.
constexpr uint32_t kTokenSlot   = 0x1F4;
constexpr uint32_t kRestoreSlot = 0x1F0;

// Application byte +5 is the interface-update gate; the runner stops the input owner's rumble (0x8211B944).
constexpr uint32_t kModalDepthGlobal  = 0x83156368;
constexpr uint32_t kModalFlagGlobal   = 0x8315636C;
constexpr uint32_t kApplicationGlobal = 0x8314F28C;
constexpr uint32_t kInputOwnerGlobal  = 0x8314F2A4;
constexpr uint32_t kGateByteOffset    = 5;

constexpr uint32_t kSlotBreakA = 0x20;
constexpr uint32_t kSlotBreakB = 0x2C;
constexpr uint32_t kSlotBreakC = 0x24;
constexpr uint32_t kSlotFrame  = 0x44;

enum class Route
{
    kPage,
    kRetail,
};

Route ChooseRoute(uint32_t returnAddress, uint32_t receiver, bool hostBound);

// UI thread publishes; guest reads lock-free. Shutdown ends every wait.
class PageGate
{
public:
    // UI thread: a page was created. Returns its id, never zero.
    uint64_t Opened();
    // UI thread: the page with this id is gone. Ids at or below it are closed.
    void Closed(uint64_t id);
    // Any thread: no page will be served again.
    void Shutdown();

    bool IsOpen(uint64_t id) const;

private:
    std::atomic<uint64_t> opened_{ 0 };
    std::atomic<uint64_t> closed_{ 0 };
    std::atomic<bool>     shutdown_{ false };
};

// Nonzero low byte ends the loop, except frame service, which ends on zero.
struct Call
{
    uint32_t slot;
    uint32_t r4;
    uint32_t r5;
};

inline constexpr Call kLoopCalls[] = {
    { kSlotBreakA, 0, 0 },
    { kSlotBreakB, 0, 0 },
    { kSlotBreakC, 0, 0 },
    { kSlotFrame, 0, 1 },
    { kSlotBreakB, 0, 0 },
};

constexpr bool BreaksLoop(const Call& call, bool lowByteNonzero)
{
    return call.slot == kSlotFrame ? !lowByteNonzero : lowByteNonzero;
}

enum class Exit
{
    kPageClosed,
    kApplicationBreak,
    kNoApplication,
    kCancelled,
    kShutdown,
};

// Retail dialog update 0x82DE6B38 cancels at 0x82DE6B64..0x82DE6B9C; the page has nothing to roll back, so it just closes.
constexpr uint32_t kFrontendGlobal     = 0x82FFD624;
constexpr uint32_t kFrontendStateField = 0x70;
constexpr uint32_t kFrontendTypeField  = 4;
constexpr uint32_t kFrontendTypeCancel = 2;
constexpr uint32_t kStatusWord         = 0x82FFE7C4;
constexpr uint32_t kStatusCancelBit    = 0x4000;

constexpr bool CancelRequested(uint32_t frontendState, uint32_t frontendType, uint32_t status)
{
    return frontendState != 0 && frontendType == kFrontendTypeCancel && (status & kStatusCancelBit) != 0;
}

template <typename Env>
void CloseUntilGone(Env& env)
{
    while (env.Alive() && env.PageOpen())
        env.RequestClose();
}

// Env: Alive() false on shutdown; PageOpen(); CancelRequested(); CallApplication(Call) is the low byte nonzero,
// nullopt if no call; RequestClose() may block; EndIteration() services and paces.
template <typename Env>
Exit RunLoop(Env& env)
{
    if (!env.Alive())
        return Exit::kShutdown;
    if (env.CancelRequested())
    {
        CloseUntilGone(env);
        return env.Alive() ? Exit::kCancelled : Exit::kShutdown;
    }
    if (!env.PageOpen())
        return Exit::kPageClosed;
    for (;;)
    {
        if (!env.Alive())
            return Exit::kShutdown;
        for (const Call& call : kLoopCalls)
        {
            const std::optional<bool> result = env.CallApplication(call);
            if (!result || BreaksLoop(call, *result))
            {
                CloseUntilGone(env);
                if (!env.Alive())
                    return Exit::kShutdown;
                return result ? Exit::kApplicationBreak : Exit::kNoApplication;
            }
        }
        env.EndIteration();
        if (env.CancelRequested())
        {
            CloseUntilGone(env);
            return env.Alive() ? Exit::kCancelled : Exit::kShutdown;
        }
        if (!env.PageOpen())
            return env.Alive() ? Exit::kPageClosed : Exit::kShutdown;
    }
}

} // namespace rerevved::options_modal
