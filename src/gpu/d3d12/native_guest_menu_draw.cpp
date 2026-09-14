#include "native_guest_menu_draw.h"

#include <array>
#include <bit>
#include <cstring>
#include <limits>

#include <rex/graphics/xenos.h>

namespace rerevved::gpu
{

bool BuildNativeGuestMenuDrawRecipe(const NativeGuestMenuDraw& draw,
                                    const NativeMenuShaders&   shaders,
                                    NativeDrawReplayRecipe&    recipe,
                                    std::string&               error)
{
    recipe                   = {};
    const bool label         = !draw.indexed && draw.stride == 28;
    const bool scene         = draw.indexed && draw.stride == 32;
    const bool indexed       = draw.indexed && (draw.stride == 4 || draw.stride == 8 || draw.stride == 12 || scene);
    const bool sceneGeometry = !scene ||
                               ((draw.vertexCount == 8 && draw.indexCount == 12) ||
                                (draw.vertexCount == 289 && draw.indexCount == 1536));
    if (draw.state.size() != 0x3500 || draw.primitive != 4 || draw.minimumVertex != 0 ||
        (!label && !indexed) || !draw.vertexCount || !sceneGeometry ||
        draw.vertexLiterals.size() != (scene ? 64U : 0U) ||
        std::uint64_t(draw.vertexCount) * draw.stride != draw.vertices.size() ||
        (indexed && (draw.indexFormat != 1 || std::uint64_t(draw.indexCount) * 2 != draw.indices.size())) ||
        (label && (draw.indexFormat != 0 || draw.indexCount != 0 || !draw.indices.empty())))
    {
        error = "unsupported guest menu input tuple or state extent";
        return false;
    }
    const auto word = [](std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
               (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
    };
    std::vector<std::uint32_t> registers(0x5003);
    const auto                 copy = [&](std::size_t source, std::size_t target, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
            registers[target + i] = word(draw.state, source + i * 4);
    };
    // Device shadows consumed by the guest dirty-state emitter. The window
    // registers follow the first 16 words, in a separate hardware range.
    copy(0x2880, 0x2000, 16);
    copy(0x28C0, 0x2080, 3);
    copy(0x28CC, 0x2100, 21);
    copy(0x2920, 0x2180, 5);
    copy(0x2934, 0x2200, 12);
    copy(0x29B8, 0x2300, 38);
    copy(0x780, 0x4000, 1024);
    copy(0x1780, 0x4400, 896);
    copy(0x2780, 0x4900, 8);
    // The resource loader emits these after the device constant shadow.
    if (scene)
        for (std::size_t i = 0; i < 16; ++i)
            registers[0x4000 + 1008 + i] = word(draw.vertexLiterals, i * 4);
    // 0x82690138 retains x, y, clipped width/height, and min/max depth here.
    const std::array<float, 6> viewport{ 0, 0, 1280, 720, 0, scene ? 1.0F : 0.0F };
    for (std::size_t i = 0; i < viewport.size(); ++i)
        if (word(draw.state, 0x3168 + i * 4) != std::bit_cast<std::uint32_t>(viewport[i]))
        {
            error = "unsupported guest menu viewport";
            return false;
        }
    const auto microcode = [&](std::span<const std::uint8_t> bytes)
    {
        std::vector<std::uint32_t> words;
        if (!bytes.empty() && bytes.size() <= 64 * 1024 && bytes.size() % 4 == 0)
            for (std::size_t i = 0; i < bytes.size(); i += 4)
                words.push_back(word(bytes, i));
        return words;
    };
    const auto         vs = microcode(draw.vertexMicrocode), ps = microcode(draw.pixelMicrocode);
    NativeMenuDrawView view;
    view.registers       = registers;
    view.vertexMicrocode = vs;
    view.pixelMicrocode  = ps;
    // BuildNativeMenuDrawRecipe verifies both byte digests before selecting DXIL.
    view.vertexShaderHash = 0x11213E38D7154104ULL;
    view.pixelShaderHash  = 0x3A92D78FE55C7B83ULL;
    NativeDrawReplaySampler sampler;
    if (draw.stride != 8)
    {
        view.vertexShaderHash = scene ? 0xC3BAB94E67553E12ULL : draw.stride == 4 ? 0x5F6EB3BC96CE8FC0ULL
                                                                                 : 0x1EE55F3AB5213177ULL;
        view.pixelShaderHash  = scene ? 0x64D8E6A475FFCB2DULL : draw.stride == 4 ? 0x6831098A8316F932ULL
                                                                                 : 0x47F2D46F3B8F1668ULL;
        if (label)
        {
            view.vertexShaderHash = 0x2BA2325A7EA93DE3ULL;
            view.pixelShaderHash  = 0xC3BEC99768EF0D6BULL;
        }
        // These pinned shader pairs have one normalized 2D fetch from slot 0,
        // with all sampler filters inherited from its fetch constant.
        if (ps.size() < 6 || ps[3] != (scene ? 0x10081001U : 0x10080001U) ||
            ps[4] != (label ? 0x1F1FF7FFU : 0x1F1FF688U) || ps[5] != 0x00004000)
        {
            error = "unsupported guest menu texture instruction";
            return false;
        }
        std::array<std::uint32_t, 6> words;
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = word(draw.state, 0x480 + i * 4);
        namespace xenos = rex::graphics::xenos;
        xenos::xe_gpu_texture_fetch_t fetch{};
        std::memcpy(&fetch, words.data(), sizeof(fetch));
        if (fetch.type != xenos::FetchConstantType::kTexture || fetch.dimension != xenos::DataDimension::k2DOrStacked ||
            fetch.stacked || fetch.mip_min_level || fetch.mip_max_level || fetch.mip_address || fetch.packed_mips ||
            static_cast<unsigned>(fetch.mag_filter) > 1 || static_cast<unsigned>(fetch.min_filter) > 1 ||
            static_cast<unsigned>(fetch.mip_filter) > 1 || static_cast<unsigned>(fetch.aniso_filter) ||
            static_cast<unsigned>(fetch.border_color))
        {
            error = "unsupported guest menu base-level sampler";
            return false;
        }
        // Match texture_util's 2D axis normalization and D3D12 WriteSampler.
        // LOD bias stays in translated shader state; the texture has one level.
        constexpr std::array<std::uint32_t, 8> addressModes{ 1, 2, 3, 5, 3, 5, 4, 5 };
        sampler.address   = { addressModes[static_cast<unsigned>(fetch.clamp_x)],
                              addressModes[static_cast<unsigned>(fetch.clamp_y)],
                              3 };
        sampler.magLinear = fetch.mag_filter == xenos::TextureFilter::kLinear;
        sampler.minLinear = fetch.min_filter == xenos::TextureFilter::kLinear;
        sampler.mipLinear = fetch.mip_filter == xenos::TextureFilter::kLinear;
        sampler.maxLod    = std::numeric_limits<float>::max();
        view.texture      = draw.texture;
        view.sampler      = &sampler;
    }
    view.vertexBytes         = draw.vertices;
    view.indexBytes          = draw.indices;
    view.indexed             = draw.indexed;
    view.indexCount          = draw.indexed ? draw.indexCount : draw.vertexCount;
    view.fullViewportTarget  = true;
    view.guestSourceVertices = true;
    if (!BuildNativeMenuDrawRecipe(view, shaders, recipe, error))
        return false;
    if (draw.firstInFrame)
    {
        // The guest resolve helper retains both copy-clear words in the
        // device shadow. Preserve the observed black clear's alpha, including
        // loading frames whose first draw does not cover the target.
        const auto clear = registers[0x231E];
        if (clear != registers[0x231F] || (clear != 0 && clear != 0xFF000000U))
        {
            error = "unsupported guest frame clear state";
            return false;
        }
        recipe.clearColor[3] = static_cast<std::uint8_t>(clear >> 24);
    }
    return true;
}

} // namespace rerevved::gpu
