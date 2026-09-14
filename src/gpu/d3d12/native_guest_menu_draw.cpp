#include "native_guest_menu_draw.h"

#include <bit>

namespace rerevved::gpu
{

bool BuildNativeGuestMenuDrawRecipe(const NativeGuestMenuDraw& draw,
                                    const NativeMenuShaders&   shaders,
                                    NativeDrawReplayRecipe&    recipe,
                                    std::string&               error)
{
    recipe = {};
    if (draw.state.size() != 0x3500 || draw.primitive != 4 || draw.minimumVertex != 0 ||
        draw.stride != 8 || draw.indexFormat != 1 ||
        std::uint64_t(draw.vertexCount) * draw.stride != draw.vertices.size() ||
        std::uint64_t(draw.indexCount) * 2 != draw.indices.size())
    {
        error = "unsupported guest panel input tuple or state extent";
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
    // 0x82690138 retains x, y, clipped width/height, and min/max depth here.
    constexpr std::array<float, 6> viewport{ 0, 0, 1280, 720, 0, 0 };
    for (std::size_t i = 0; i < viewport.size(); ++i)
        if (word(draw.state, 0x3168 + i * 4) != std::bit_cast<std::uint32_t>(viewport[i]))
        {
            error = "unsupported guest panel viewport";
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
    view.vertexShaderHash    = 0x11213E38D7154104ULL;
    view.pixelShaderHash     = 0x3A92D78FE55C7B83ULL;
    view.vertexBytes         = draw.vertices;
    view.indexBytes          = draw.indices;
    view.indexCount          = draw.indexCount;
    view.fullViewportTarget  = true;
    view.guestSourceVertices = true;
    return BuildNativeMenuDrawRecipe(view, shaders, recipe, error);
}

} // namespace rerevved::gpu
