#pragma once

// Options volumes use positions 0..9 (float steps of 0.11); cvar -1 retains the loaded value.
// The UI never accesses guest memory.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace rerevved::retail_audio
{

constexpr size_t kChannelCount = 4;

constexpr int kMinPosition     = 0;
constexpr int kMaxPosition     = 9;
constexpr int kDefaultPosition = 9;
constexpr int kNoOverride      = -1;

// Native step and snap of 0x82DE6170; above 0.98 becomes 1.0 (0x8216A50C, 0x8216A508).
constexpr float kStep = 0.11f;
constexpr float kSnap = 0.98f;

struct Channel
{
    std::string_view cvar;
    // Guest address of the USER_VOLUME_* catalog key the audio lookup takes.
    uint32_t catalogKey;
};

// In the retail Options order. Index is the channel everywhere.
inline constexpr std::array<Channel, kChannelCount> kChannels{ {
    { "retail_music_volume", 0x821234A0 },
    { "retail_effects_volume", 0x821234B4 },
    { "retail_advisor_volume", 0x821234C4 },
    { "retail_leader_volume", 0x821234DC },
} };

float PositionToGain(int position);

struct Reading
{
    int position = 0;
    // False for non-retail gains (hand-edited config); position is nearest.
    bool exact = true;
    // Highest step at or below the gain, lowest at or above; both equal position when exact.
    int below = 0;
    int above = 0;
};

Reading ReadGain(float gain);

bool IsPosition(int value);

// kNoOverride or a position; atomic copies allow guest reads during UI edits.
int OverrideOf(size_t channel);
// Copy cvars after every edit, on the UI thread or before the guest starts.
void PublishOverrides();

struct Request
{
    size_t   channel    = 0;
    int      position   = 0;
    uint64_t generation = 0;
};

// Guest drains the latest request per channel; a new generation drops pending requests.
class ApplyQueue
{
public:
    uint64_t Generation() const;
    uint64_t Advance();

    // False for a bad channel or position.
    bool Post(size_t channel, int position);
    bool Post(size_t channel, int position, uint64_t generation);

    // One relaxed load; true when Take would return something.
    bool                 HasPending() const;
    std::vector<Request> Take();
    // Requeues a request the guest could not apply yet, unless stale or superseded.
    void Requeue(const Request& request);

private:
    mutable std::mutex                                mutex_;
    uint64_t                                          generation_ = 0;
    std::array<std::optional<Request>, kChannelCount> pending_;
    std::atomic<bool>                                 hasPending_{ false };
};

struct LiveValue
{
    bool  available = false;
    float gain      = 0.0f;

    bool operator==(const LiveValue&) const = default;
};

struct LiveSnapshot
{
    uint64_t                             generation = 0;
    std::array<LiveValue, kChannelCount> volume{};

    bool operator==(const LiveSnapshot&) const = default;
};

class LiveStore
{
public:
    void         Publish(const LiveSnapshot& snapshot);
    LiveSnapshot Read() const;
    LiveSnapshot Read(uint64_t& revision) const;
    uint64_t     Revision() const;

    void RequestRefresh();
    bool RefreshRequested() const;
    bool ConsumeRefresh();

private:
    mutable std::mutex    mutex_;
    LiveSnapshot          snapshot_;
    std::atomic<uint64_t> revision_{ 0 };
    std::atomic<bool>     refresh_{ false };
};

ApplyQueue& Queue();
LiveStore&  Live();

// Call once; every volume cvar edit then republishes overrides and queues live apply.
void RegisterChangeCallbacks();

} // namespace rerevved::retail_audio
