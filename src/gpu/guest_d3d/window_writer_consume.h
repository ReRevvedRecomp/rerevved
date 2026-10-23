#pragma once

#include "guest_render_capture.h"
#include "ring_stubs.h"
#include "window_writer_derive.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

// Window-writer consume fact table:
// Skip key: native_consume_window_writer may skip 0x8268D3A0 only when
//   DeriveWindowWriterExitState knows the exit exactly and sets r3, the one
//   body result a reader can take.
// Omitted Original stores: every consume skips ring words because no host
//   reader takes them, the ring cursor G+0x30 (which stays at entry), and stack
//   saves below the caller's r1 (dead at every caller).
// Caller stores: G+0x3184..0x3190 and G+0x28C4/G+0x28C8 of 0x8268F9C0 occur
//   before the call and stay; details and callers are in
//   window_writer_derive.h.
// Stack branch: skipping requires thread stack reach [limit, r1) clear of
//   device words [G, G + 0x3340); if the stack holds them, saves land there and
//   the epilogue reads them back.
// Ring branch: skipping requires no flush, with both the entry cursor and the
//   cursor after the last tile bounded by the soft limit G+0x38.
// Tile branch: skipping requires at most kWindowWriterMaxTiles tiles.
// Fallback: otherwise Original runs. Shader harvest, scene capture, route
//   trace, the exit probe, and other consume exit checks force
//   Original because they measure the ring cursor the skipped words would have
//   moved.

// First failure wins:Stubs, StackOverlap, Unreadable, RingSpace(entry cursor),
// TileCount, RingSpace(after last tile), RingSpace(words past 2^32),
// Diagnostics. Duplicate ring checks cover distinct bounds.
enum class WindowWriterFallback : std::uint8_t
{
    Stubs,        // ring stubs are off or have not checked this device
    StackOverlap, // the thread's stack reach holds a device word the model reads, or is unknown
    Unreadable,   // a guest word could not be read
    TileCount,    // a tiled call with more tiles than the model bounds
    RingSpace,    // Original could flush: the cursor or the words pass the soft limit
    Diagnostics,  // a harvest, capture, route trace or exit check needs the body
};
inline constexpr std::size_t                                              kWindowWriterFallbackCount = static_cast<std::size_t>(WindowWriterFallback::Diagnostics) + 1;
inline constexpr std::array<std::string_view, kWindowWriterFallbackCount> kWindowWriterFallbackNames{
    "stubs",
    "stack_overlap",
    "unreadable",
    "tile_count",
    "ring_space",
    "diagnostics",
};

#if REREVVED_DEVELOPER
// The guest function that called the writer, from the return address.
enum class WindowWriterCaller : std::uint8_t
{
    Setter8268F9C0, // g188:13025
    Setter82690040, // g167:12996
    Setter826922D0, // g160:13243, a tail call: the return address is its caller's, in 0x82693030
    Setter82698720, // g208:13450
    ClearBody,      // 0x826A22F8, g158:13912
    Resolve,        // 0x8269E520, g167:13639 and g167:14913
    Other,
};
inline constexpr std::size_t kWindowWriterCallerCount = static_cast<std::size_t>(WindowWriterCaller::Other) + 1;

inline constexpr std::array<std::string_view, kWindowWriterCallerCount> kWindowWriterCallerNames{
    "8268f9c0",
    "82690040",
    "826922d0",
    "82698720",
    "clear",
    "resolve",
    "other",
};

WindowWriterCaller WindowWriterCallerOf(std::uint32_t returnAddress) noexcept;
#endif

// The cvar native_consume_window_writer while ring stub services are published;
// a consume does nothing without them.
bool WindowWriterConsumeRequested() noexcept;

// What the caller knows of the host before the guest words are read.
struct WindowWriterConsumeState
{
    bool stubsReady  = false; // RingStubsReady(G)
    bool diagnostics = false; // something needs the body
};

// Everything a consumed call replays.
struct WindowWriterPlan
{
    WindowWriterEntry     entry;
    WindowWriterWatch     watch;
    WindowWriterExitState exit;
};

// An empty refusal fills plan and permits skip. Check stubs, then stack against
// device before any guest word read, then readable words, model refusals and
// diagnostics; first reason wins.
std::optional<WindowWriterFallback> WindowWriterConsumeVerdict(const WindowWriterConsumeState& state, const GuestWordReader& read, const WindowWriterEntry& entry, WindowWriterPlan* plan = nullptr) noexcept;

// True skips Original with r3 holding result. Cvar off/no services returns
// false and counts nothing; otherwise count verdict. Diagnostics needing body
// keep Original; own probe is added here.
bool ConsumeWindowWriter(PPCContext& ctx, IRingStubMemory* memory, bool diagnostics) noexcept;

#if REREVVED_DEVELOPER
struct WindowWriterConsumeCounts
{
    std::uint64_t                                         consumed = 0;
    std::array<std::uint64_t, kWindowWriterCallerCount>   callers{};                           // the consumed calls by caller
    std::uint64_t                                         ringWordsConsumed = 0;               // the words the consumed calls would have stored
    std::uint64_t                                         ringWordsOriginal = 0, original = 0; // the words and calls of Original runs whose cursor moved by a bounded number of words
    std::uint64_t                                         originalUnbounded = 0;               // Original runs whose cursor moved by more than a call stores, or back: a flush
    std::array<std::uint64_t, kWindowWriterFallbackCount> fallback{};
};

// Counts the ring words of an Original run under the consume (no exit check),
// from the two cursors read around the call.
void NoteWindowWriterOriginal(std::uint32_t before, std::uint32_t after) noexcept;

// Returns the counts since the previous take and resets them.
WindowWriterConsumeCounts TakeWindowWriterConsumeCounts() noexcept;

std::string FormatWindowWriterConsumeCounts(const WindowWriterConsumeCounts& counts);

// window_writer_derive.h compares entry-derived state and exit state after
// Original returns.
class WindowWriterExitProbe
{
public:
    WindowWriterExitProbe(const PPCContext& ctx, IRingStubMemory* memory) noexcept;
    ~WindowWriterExitProbe();

    WindowWriterExitProbe(const WindowWriterExitProbe&)            = delete;
    WindowWriterExitProbe& operator=(const WindowWriterExitProbe&) = delete;

    // True while the probe holds an entry state to compare or a cursor to count.
    bool Armed() const noexcept
    {
        return static_cast<bool>(state) || observing;
    }

private:
    struct State;
    std::unique_ptr<State> state;
    IRingStubMemory*       memory     = nullptr;
    const PPCContext*      context    = nullptr;
    std::uint32_t          g          = 0;
    std::uint32_t          cursor     = 0;
    bool                   observing  = false;
    int                    exceptions = 0;
};
#else
// The shipping build has no exit probe; the hook keeps its source shape.
class WindowWriterExitProbe
{
public:
    WindowWriterExitProbe(const PPCContext&, IRingStubMemory*) noexcept
    {
    }
};
#endif

} // namespace rerevved::gpu::guest_d3d
