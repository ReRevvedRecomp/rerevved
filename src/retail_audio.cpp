#include "retail_audio.h"

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(retail_music_volume, -1, "ReRevved", "Music volume position 0..9 applied every time a profile loads; -1 keeps the loaded value")
    .range(-1, 9);
REXCVAR_DEFINE_INT32(retail_effects_volume, -1, "ReRevved", "Effects volume position 0..9 applied every time a profile loads; -1 keeps the loaded value")
    .range(-1, 9);
REXCVAR_DEFINE_INT32(retail_advisor_volume, -1, "ReRevved", "Advisor speech volume position 0..9 applied every time a profile loads; -1 keeps the loaded value")
    .range(-1, 9);
REXCVAR_DEFINE_INT32(retail_leader_volume, -1, "ReRevved", "Leader speech volume position 0..9 applied every time a profile loads; -1 keeps the loaded value")
    .range(-1, 9);

namespace rerevved::retail_audio
{

bool IsPosition(int value)
{
    return value >= kMinPosition && value <= kMaxPosition;
}

float PositionToGain(int position)
{
    position      = std::clamp(position, kMinPosition, kMaxPosition);
    const float v = static_cast<float>(position) * kStep;
    if (v > kSnap)
        return 1.0f;
    return std::clamp(v, 0.0f, 1.0f);
}

Reading ReadGain(float gain)
{
    Reading reading;
    if (!std::isfinite(gain))
    {
        reading.exact = false;
        return reading;
    }
    const float clamped = std::clamp(gain, 0.0f, 1.0f);
    // Nearest stored gain, ties up: the final step is 1.0, not a multiple of 0.11.
    float best = 2.0f;
    for (int position = kMinPosition; position <= kMaxPosition; ++position)
    {
        const float distance = std::fabs(PositionToGain(position) - clamped);
        if (distance <= best)
        {
            best             = distance;
            reading.position = position;
        }
    }
    reading.exact = gain == clamped && best <= 1e-4f;
    reading.below = kMinPosition;
    reading.above = kMaxPosition;
    for (int position = kMinPosition; position <= kMaxPosition; ++position)
    {
        if (PositionToGain(position) <= clamped + 1e-4f)
            reading.below = position;
    }
    for (int position = kMaxPosition; position >= kMinPosition; --position)
    {
        if (PositionToGain(position) >= clamped - 1e-4f)
            reading.above = position;
    }
    if (reading.exact)
        reading.below = reading.above = reading.position;
    return reading;
}

namespace
{

std::array<std::atomic<int>, kChannelCount> gOverrides = { { kNoOverride, kNoOverride, kNoOverride, kNoOverride } };

int cvarOverride(size_t channel)
{
    int32_t value = kNoOverride;
    switch (channel)
    {
        case 0:
            value = REXCVAR_GET(retail_music_volume);
            break;
        case 1:
            value = REXCVAR_GET(retail_effects_volume);
            break;
        case 2:
            value = REXCVAR_GET(retail_advisor_volume);
            break;
        case 3:
            value = REXCVAR_GET(retail_leader_volume);
            break;
        default:
            break;
    }
    return IsPosition(value) ? value : kNoOverride;
}

} // namespace

void PublishOverrides()
{
    for (size_t channel = 0; channel < kChannelCount; ++channel)
        gOverrides[channel].store(cvarOverride(channel), std::memory_order_release);
}

int OverrideOf(size_t channel)
{
    return channel < kChannelCount ? gOverrides[channel].load(std::memory_order_acquire) : kNoOverride;
}

uint64_t ApplyQueue::Generation() const
{
    std::lock_guard lock(mutex_);
    return generation_;
}

uint64_t ApplyQueue::Advance()
{
    std::lock_guard lock(mutex_);
    ++generation_;
    for (auto& request : pending_)
        request.reset();
    hasPending_.store(false, std::memory_order_relaxed);
    return generation_;
}

bool ApplyQueue::Post(size_t channel, int position)
{
    std::lock_guard lock(mutex_);
    if (channel >= kChannelCount || !IsPosition(position))
        return false;
    pending_[channel] = Request{ channel, position, generation_ };
    hasPending_.store(true, std::memory_order_relaxed);
    return true;
}

bool ApplyQueue::Post(size_t channel, int position, uint64_t generation)
{
    std::lock_guard lock(mutex_);
    if (channel >= kChannelCount || !IsPosition(position) || generation != generation_)
        return false;
    pending_[channel] = Request{ channel, position, generation };
    hasPending_.store(true, std::memory_order_relaxed);
    return true;
}

bool ApplyQueue::HasPending() const
{
    return hasPending_.load(std::memory_order_relaxed);
}

std::vector<Request> ApplyQueue::Take()
{
    std::lock_guard      lock(mutex_);
    std::vector<Request> taken;
    for (auto& request : pending_)
    {
        if (request && request->generation == generation_)
            taken.push_back(*request);
        request.reset();
    }
    hasPending_.store(false, std::memory_order_relaxed);
    return taken;
}

void ApplyQueue::Requeue(const Request& request)
{
    std::lock_guard lock(mutex_);
    if (request.channel >= kChannelCount || request.generation != generation_ || pending_[request.channel])
        return;
    pending_[request.channel] = request;
    hasPending_.store(true, std::memory_order_relaxed);
}

void LiveStore::Publish(const LiveSnapshot& snapshot)
{
    std::lock_guard lock(mutex_);
    if (snapshot == snapshot_)
        return;
    snapshot_ = snapshot;
    revision_.fetch_add(1, std::memory_order_release);
}

LiveSnapshot LiveStore::Read() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}

LiveSnapshot LiveStore::Read(uint64_t& revision) const
{
    std::lock_guard lock(mutex_);
    revision = revision_.load(std::memory_order_acquire);
    return snapshot_;
}

uint64_t LiveStore::Revision() const
{
    return revision_.load(std::memory_order_acquire);
}

void LiveStore::RequestRefresh()
{
    refresh_.store(true, std::memory_order_relaxed);
}

bool LiveStore::RefreshRequested() const
{
    return refresh_.load(std::memory_order_relaxed);
}

bool LiveStore::ConsumeRefresh()
{
    return refresh_.exchange(false, std::memory_order_relaxed);
}

ApplyQueue& Queue()
{
    static ApplyQueue queue;
    return queue;
}

LiveStore& Live()
{
    static LiveStore store;
    return store;
}

namespace
{

void onOverrideChanged(size_t channel)
{
    PublishOverrides();
    const int position = OverrideOf(channel);
    if (position != kNoOverride)
    {
        Queue().Post(channel, position);
        Live().RequestRefresh();
    }
}

} // namespace

void RegisterChangeCallbacks()
{
    for (size_t i = 0; i < kChannels.size(); ++i)
    {
        rex::cvar::RegisterChangeCallback(kChannels[i].cvar, [i](std::string_view, std::string_view)
                                          {
                                              onOverrideChanged(i);
                                          });
    }
}

} // namespace rerevved::retail_audio
