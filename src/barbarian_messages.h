#pragma once

// Gating for the leader message hooks in barbarian_message_hooks.cpp, used by
// hide_village_barbarian_messages and ai_civilizations, kept free of guest
// memory so it can be tested on the host.

#include <cstdint>

namespace rerevved::barbarian_messages
{

// Stack frame of the advisor show sub_82CE8338 (stwu r1,-1280(r1)); its r1
// plus this size is the r1 of the leader message sub_82CC85D0 that called it.
constexpr uint32_t kShowFrameSize = 1280;

// Capture sites call sub_82CC85D0 for a leader index; callers ignore its
// result.
inline constexpr uint32_t kVillageSites[] = {
    0x82D1B8B4,
    0x82D1BA0C,
    0x82D1BA38,
    0x82D1BBA8,
    0x82D1BD98,
    0x82D1BF54,
    0x82D1C060,
    0x82D1C174,
    0x82D09138,
};

// Whether returnAddress (the leader message's entry LR, call site + 4) belongs
// to a barbarian village call.
inline bool IsVillageReturn(uint32_t returnAddress)
{
    for (uint32_t site : kVillageSites)
    {
        if (site + 4 == returnAddress)
        {
            return true;
        }
    }
    return false;
}

// Recursive call 0x82CC87A0 qualifies only when p is below player count,
// 0x830E4550 bit 0x400 is set, 0x830ED2D8[local * 8 + p] == -1, and 0x83156348
// bit p is clear.
constexpr uint32_t kNestedMessageReturn = 0x82CC87A4;

// A hidden nested leader message remains hidden; a new message replaces stale
// per-thread state.
class LeaderMessageGate
{
public:
    void Begin(bool hidden, uint32_t messageFrame, bool nested = false)
    {
        // The stack grows down: only a frame above this one can enclose it.
        while (depth_ > 0 && frames_[depth_ - 1] <= messageFrame)
        {
            --depth_;
        }
        if (nested && depth_ > 0)
        {
            hidden = true;
        }
        else if (!nested)
        {
            depth_ = 0;
        }
        if (hidden && depth_ < kMaxDepth)
        {
            frames_[depth_++] = messageFrame;
        }
    }

    void End(uint32_t messageFrame)
    {
        if (depth_ > 0 && frames_[depth_ - 1] == messageFrame)
        {
            --depth_;
        }
    }

    // Inside the show: true once, for the show called by the innermost hidden
    // message.
    bool SuppressShow(uint32_t showFrame)
    {
        if (depth_ == 0 || showFrame + kShowFrameSize != frames_[depth_ - 1])
        {
            return false;
        }
        --depth_;
        return true;
    }

private:
    static constexpr uint32_t kMaxDepth = 4;

    uint32_t frames_[kMaxDepth] = {};
    uint32_t depth_             = 0;
};

} // namespace rerevved::barbarian_messages
