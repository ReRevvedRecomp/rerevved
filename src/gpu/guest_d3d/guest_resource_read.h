#pragma once

#include "gpu/guest_d3d/guest_bytes.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace rerevved::gpu::guest_d3d
{

// Guest D3D resource kinds the mirror registry tracks. Draws read texture,
// vertex buffer and index buffer bytes; a surface is counted only when a lock
// names it, before the lock core resolves it to its texture.
enum class GuestResourceKind : std::uint8_t
{
    Texture,
    VertexBuffer,
    IndexBuffer,
    Surface,
};
inline constexpr std::size_t kGuestResourceKindCount = 4;

// Resource words: index object G+0x3094 uses words+0x18/words+0x1C; textures
// have six fetch words, streams two.
struct GuestResourceBindHint;

struct GuestResourceRead
{
    GuestResourceKind            kind    = GuestResourceKind::Texture;
    std::uint32_t                address = 0, size = 0;
    std::uint32_t                header = 0;
    std::array<std::uint32_t, 6> words{};
    // Where the reader keeps the registry's lookup for this bind site between
    // draws, on one thread; null looks the read up afresh.
    GuestResourceBindHint* hint = nullptr;
};

// Reuse a mirror lookup only for an identical read in the same registry
// generation while its object is still live; otherwise look it up again and
// replace the hint.
struct GuestResourceBindHint
{
    std::uint64_t                registry = 0, generation = 0, serial = 0;
    std::size_t                  range   = 0;
    bool                         found   = false;
    GuestResourceKind            kind    = GuestResourceKind::Texture;
    std::uint32_t                address = 0, size = 0, header = 0;
    std::array<std::uint32_t, 6> words{};
};

// Returns the bytes a mirror holds for the read, or nothing to send the read
// to guest memory.
using MirroredResourceQuery = std::function<std::optional<GuestBytes>(const GuestResourceRead&)>;

// Which draw input a guest read serves, for native_frame_timing's split of
// resolved reads. A reader names it for the read's duration on its thread;
// a read nothing names is Other. Mirror* are a mirror's refreshing reads.
enum class GuestReadSite : std::uint8_t
{
    Other,
    ShaderCode,
    Texture,
    VertexBuffer,
    IndexBuffer,
    MirrorTexture,
    MirrorVertexBuffer,
    MirrorIndexBuffer,
};
inline constexpr std::size_t                                  kGuestReadSiteCount = 8;
inline constexpr std::array<const char*, kGuestReadSiteCount> kGuestReadSiteNames{ "other", "code", "texture", "vertex", "index", "mirror_texture", "mirror_vertex", "mirror_index" };

inline GuestReadSite& CurrentGuestReadSite() noexcept
{
    thread_local GuestReadSite site = GuestReadSite::Other;
    return site;
}

// Names the reads on this thread until it is destroyed.
class GuestReadSiteScope
{
public:
    explicit GuestReadSiteScope(GuestReadSite site) noexcept
    : saved(CurrentGuestReadSite())
    {
        CurrentGuestReadSite() = site;
    }

    ~GuestReadSiteScope()
    {
        CurrentGuestReadSite() = saved;
    }

    GuestReadSiteScope(const GuestReadSiteScope&)            = delete;
    GuestReadSiteScope& operator=(const GuestReadSiteScope&) = delete;

private:
    GuestReadSite saved;
};

// The site of a resource read: the bound kind's, or its mirror's refresh.
constexpr GuestReadSite GuestResourceReadSite(GuestResourceKind kind, bool mirror) noexcept
{
    switch (kind)
    {
        case GuestResourceKind::Texture:
            return mirror ? GuestReadSite::MirrorTexture : GuestReadSite::Texture;
        case GuestResourceKind::VertexBuffer:
            return mirror ? GuestReadSite::MirrorVertexBuffer : GuestReadSite::VertexBuffer;
        case GuestResourceKind::IndexBuffer:
            return mirror ? GuestReadSite::MirrorIndexBuffer : GuestReadSite::IndexBuffer;
        default:
            return GuestReadSite::Other;
    }
}

} // namespace rerevved::gpu::guest_d3d
