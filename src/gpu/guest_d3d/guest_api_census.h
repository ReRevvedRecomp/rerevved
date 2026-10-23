#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rerevved::gpu::guest_d3d
{

// Roles of the title's statically linked D3D runtime entries. The census
// counts calls into each table entry by role; every hook still runs Original.
enum class GuestApiRole : std::uint8_t
{
    Device,
    ResourceCreate,
    Lock,
    Unlock,
    Release,
    SetRenderTarget,
    SetDepthStencil,
    SetTexture,
    SetSamplerState,
    SetStreamSource,
    SetIndices,
    SetVertexShader,
    SetPixelShader,
    SetVertexDeclaration,
    SetShaderConstants,
    SetRenderState,
    Viewport,
    Scissor,
    Clear,
    Draw,
    Resolve,
    Present,
    RingInternal, // ring reservation and PM4 internals
    Other,
};
inline constexpr std::size_t kGuestApiRoleCount = static_cast<std::size_t>(GuestApiRole::Other) + 1;

// Timing-line field names, `api_<name>`, indexed by GuestApiRole.
inline constexpr std::array<std::string_view, kGuestApiRoleCount> kGuestApiRoleNames{
    "device",
    "resource_create",
    "lock",
    "unlock",
    "release",
    "set_render_target",
    "set_depth_stencil",
    "set_texture",
    "set_sampler_state",
    "set_stream_source",
    "set_indices",
    "set_vertex_shader",
    "set_pixel_shader",
    "set_vertex_declaration",
    "set_shader_constants",
    "set_render_state",
    "viewport",
    "scissor",
    "clear",
    "draw",
    "resolve",
    "present",
    "ring_internal",
    "other",
};

struct GuestApiEntry
{
    std::uint32_t    address = 0;
    GuestApiRole     role    = GuestApiRole::Other;
    std::string_view name;
};

// One line per entry in guest_api_entries.inc.
inline constexpr auto kGuestApiEntries = std::to_array<GuestApiEntry>({
#define GUEST_API_ENTRY(address, role, name, hook) { 0x##address, GuestApiRole::role, name },
#include "guest_api_entries.inc"
#undef GUEST_API_ENTRY
});

constexpr std::optional<GuestApiEntry> FindGuestApiEntry(std::uint32_t address) noexcept
{
    for (const auto& entry : kGuestApiEntries)
        if (entry.address == address)
            return entry;
    return std::nullopt;
}

using GuestApiCounts = std::array<std::uint64_t, kGuestApiRoleCount>;

// Off unless native_frame_timing is on; a disabled hook pays one relaxed load.
extern std::atomic<bool>                                          guestApiCensusEnabled;
extern std::array<std::atomic<std::uint64_t>, kGuestApiRoleCount> guestApiCensusCalls;

void EnableGuestApiCensus(bool enabled) noexcept;
// Returns the calls counted since the previous take and resets them; the
// timing owner takes once per VdSwap.
GuestApiCounts TakeGuestApiCounts() noexcept;
// " api_<role>=N" for each role with a nonzero count, in GuestApiRole order.
std::string FormatGuestApiCounts(const GuestApiCounts& counts);

// Calls of each role this thread has in progress. A call made inside another
// call of its role is part of that call and is not counted again.
inline thread_local std::array<std::uint32_t, kGuestApiRoleCount> guestApiCallDepth{};

// Held by a hook for the whole guest call, so nested calls see its role.
class [[nodiscard]] GuestApiCall
{
public:
    explicit GuestApiCall(GuestApiRole role) noexcept
    {
        if (guestApiCensusEnabled.load(std::memory_order_relaxed)) [[unlikely]]
        {
            index = static_cast<std::size_t>(role);
            if (guestApiCallDepth[index]++ == 0)
                guestApiCensusCalls[index].fetch_add(1, std::memory_order_relaxed);
        }
    }

    ~GuestApiCall()
    {
        if (index != kGuestApiRoleCount)
            --guestApiCallDepth[index];
    }

    GuestApiCall(const GuestApiCall&)            = delete;
    GuestApiCall& operator=(const GuestApiCall&) = delete;

private:
    std::size_t index = kGuestApiRoleCount; // not counting
};

struct GuestApiUncounted
{
};

// Expands to an empty object for an address outside the table, so shared
// hook macros cost nothing for every other entry.
template <std::uint32_t Address>
[[nodiscard]] auto CountGuestApiCall() noexcept
{
    if constexpr (constexpr auto entry = FindGuestApiEntry(Address); entry.has_value())
        return GuestApiCall(entry->role);
    else
        return GuestApiUncounted{};
}

} // namespace rerevved::gpu::guest_d3d
