#include "scene_resource_capture.h"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include <vector>

#include <rex/graphics/format/ucode.h>

namespace rerevved::gpu::diagnostics
{

SceneTexture DecodeSceneFetch(const SceneFetch& w) noexcept
{
    SceneTexture t{};
    t.type       = w[0] & 3;
    t.signs      = (w[0] >> 2) & 255;
    t.pitch      = ((w[0] >> 22) & 511) * 32;
    t.tiled      = w[0] >> 31;
    t.format     = w[1] & 63;
    t.endian     = (w[1] >> 6) & 3;
    t.stacked    = (w[1] >> 10) & 1;
    t.base       = w[1] & 0xFFFFF000;
    t.mipBase    = w[5] & 0xFFFFF000;
    t.dimension  = (w[5] >> 9) & 3;
    t.packedMips = (w[5] >> 11) & 1;
    t.minMip     = (w[4] >> 2) & 15;
    t.maxMip     = (w[4] >> 6) & 15;
    t.swizzle    = (w[3] >> 1) & 4095;
    t.numFormat  = w[3] & 1;
    t.expAdjust  = static_cast<std::int32_t>((w[3] >> 13) & 63);
    if (t.expAdjust & 32)
        t.expAdjust -= 64;
    t.width  = (w[2] & (t.dimension == 0 ? 0xFFFFFF : t.dimension == 2 ? 0x7FF
                                                                       : 0x1FFF)) +
               1;
    t.height = t.dimension == 0 ? 1 : ((w[2] >> (t.dimension == 2 ? 11 : 13)) & (t.dimension == 2 ? 0x7FF : 0x1FFF)) + 1;
    // SDK texture/util.cpp:39 uses depth-minus-one 0 for non-stacked 2D.
    t.depth = t.dimension == 0 ? 1 : t.dimension == 2             ? (w[2] >> 22) + 1
                                 : t.dimension == 1 && !t.stacked ? 1
                                                                  : (w[2] >> 26) + 1;
    return t;
}

SceneResolve DecodeSceneResolve(std::uint32_t flags, const SceneFetch& w) noexcept
{
    const auto increment = (std::rotl(w[3], 2) & 2) + 1;
    const bool three     = (w[5] & 0x600) == 0x400;
    // sub_8269E520 after 8269E7F8: r29 is descriptor word1;
    // rlwinm r9,r29,26,30,31 extracts endian bits 6..7, not sign_x.
    return { flags & 7, w[1] & 0xFFFFF000, w[1] & 63, (w[2] & (three ? 0x7FF : 0x1FFF)) + increment, ((w[2] >> (three ? 11 : 13)) & (three ? 0x7FF : 0x1FFF)) + increment, increment, std::rotl(w[0], 15) & 0x3FE0, (w[1] >> 6) & 3 };
}

std::size_t ScenePacket::Bytes() const noexcept
{
    return sizeof(*this) + metadata.size() + state.size() + vertexCode.size() + pixelCode.size();
}

SceneLog::SceneLog(std::size_t capacity, std::size_t byteLimit)
: capacity(capacity)
, byteLimit(byteLimit)
{
}

bool SceneLog::Push(std::unique_ptr<ScenePacket> packet)
{
    std::lock_guard lock(mutex);
    const auto      size = packet->Bytes();
    if (queue.size() >= capacity || size > byteLimit - bytes)
    {
        overflow.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bytes += size;
    queue.push_back(std::move(packet));
    return true;
}

std::unique_ptr<ScenePacket> SceneLog::Pop()
{
    std::lock_guard lock(mutex);
    if (queue.empty())
        return {};
    auto packet = std::move(queue.front());
    queue.pop_front();
    bytes -= packet->Bytes();
    return packet;
}

// Texture-binding gather follows Shader::AnalyzeUcode in
// src/graphics/pipeline/shader/translator.cpp, excluding
// vertex fetch and binding-free set-texture ops.
std::uint32_t SceneTextureFetchMask(std::span<const std::uint8_t> code)
{
    if (code.size() % 4)
        throw std::runtime_error("scene_shader_word_extent");
    namespace ucode = rex::graphics::ucode;
    std::vector<std::uint32_t> words(code.size() / 4);
    for (std::size_t i = 0; i < words.size(); ++i)
        words[i] = std::uint32_t(code[i * 4]) << 24 | std::uint32_t(code[i * 4 + 1]) << 16 |
                   std::uint32_t(code[i * 4 + 2]) << 8 | code[i * 4 + 3];
    auto bound = words.size() / 3;
    for (std::size_t pair = 0; pair < bound; ++pair)
    {
        ucode::ControlFlowInstruction cf[2];
        ucode::UnpackControlFlowInstructions(words.data() + pair * 3, cf);
        for (const auto& c : cf)
            if (ucode::IsControlFlowOpcodeExec(c.opcode()))
                bound = std::min(bound, std::size_t(c.exec.address()));
    }
    std::uint32_t mask = 0;
    for (std::size_t pair = 0; pair < bound; ++pair)
    {
        ucode::ControlFlowInstruction cf[2];
        ucode::UnpackControlFlowInstructions(words.data() + pair * 3, cf);
        for (const auto& c : cf)
        {
            if (!ucode::IsControlFlowOpcodeExec(c.opcode()))
                continue;
            // ucode.h:218..328: all three exec views share word0 bits
            // 0..11 address, 12..14 count and 16..27 sequence.
            auto sequence = c.exec.sequence();
            for (std::uint32_t i = 0; i < c.exec.count(); ++i, sequence >>= 2)
            {
                const std::size_t at = (std::size_t(c.exec.address()) + i) * 3;
                if (at + 3 > words.size())
                    throw std::runtime_error("scene_shader_exec_extent");
                if (!(sequence & 1))
                    continue;
                const auto& op = *reinterpret_cast<const ucode::FetchInstruction*>(words.data() + at);
                switch (op.opcode())
                {
                    case ucode::FetchOpcode::kVertexFetch:
                    case ucode::FetchOpcode::kSetTextureLod:
                    case ucode::FetchOpcode::kSetTextureGradientsHorz:
                    case ucode::FetchOpcode::kSetTextureGradientsVert:
                        break;
                    default:
                        mask |= 1U << op.texture_fetch().fetch_constant_index();
                }
            }
        }
    }
    return mask;
}

} // namespace rerevved::gpu::diagnostics
