#include "native_gameplay_draw.h"
#include "gpu/cached_sha256.h"
#include "native_render_frame.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <rex/graphics/format/ucode.h>
#include <rex/hash.h>

namespace rerevved::gpu
{
namespace
{

constexpr std::size_t kRegisterCount       = 0x5003;
constexpr std::size_t kMaxGeometrySize     = 16U * 1024U * 1024U;
constexpr std::size_t kMaxShaderSize       = 4U * 1024U * 1024U;
constexpr std::size_t kMaxVertexAttributes = 16;

bool fail(std::string& error, std::string_view message)
{
    error = message;
    return false;
}

// `recipe = {}` without the temporary, whose texture slots make its move
// assignment and destruction a measurable part of a cached draw build.
void resetRecipe(NativeDrawReplayRecipe& recipe) noexcept
{
    static_assert(std::is_nothrow_default_constructible_v<NativeDrawReplayRecipe>);
    std::destroy_at(&recipe);
    std::construct_at(&recipe);
}

std::string digest(std::span<const std::uint8_t> bytes)
{
    return CachedSha256::Of(bytes);
}

std::string wordDigest(std::span<const std::uint32_t> words)
{
    if (words.empty() || words.size_bytes() > 4U * 1024U * 1024U)
        return {};
    if constexpr (std::endian::native == std::endian::little)
        return digest({ reinterpret_cast<const std::uint8_t*>(words.data()), words.size_bytes() });
    std::vector<std::uint8_t> bytes(words.size_bytes());
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        const auto value = words[i];
        for (std::size_t byte = 0; byte < 4; ++byte)
            bytes[i * 4 + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
    }
    return digest(bytes);
}

bool hasMemoryExport(std::span<const std::uint32_t> words)
{
    if (words.size() < 3)
        return false;
    auto pairCount = words.size() / 3;
    // EXEC addresses bound the control-flow region; following clauses contain
    // ALU and fetch data that may share bit patterns with ALLOC instructions.
    for (std::size_t pair = 0; pair < pairCount; ++pair)
    {
        rex::graphics::ucode::ControlFlowInstruction instructions[2];
        rex::graphics::ucode::UnpackControlFlowInstructions(words.data() + pair * 3, instructions);
        for (const auto& instruction : instructions)
            if (rex::graphics::ucode::IsControlFlowOpcodeExec(instruction.opcode()))
                pairCount = std::min(pairCount, std::size_t(instruction.exec.address()));
    }
    for (std::size_t pair = 0; pair < pairCount; ++pair)
    {
        rex::graphics::ucode::ControlFlowInstruction instructions[2];
        rex::graphics::ucode::UnpackControlFlowInstructions(words.data() + pair * 3, instructions);
        for (const auto& instruction : instructions)
            if (instruction.opcode() == rex::graphics::ucode::ControlFlowOpcode::kAlloc &&
                instruction.alloc.alloc_type() == rex::graphics::ucode::AllocType::kMemory)
                return true;
    }
    return false;
}

// True when the control flow before the first exec clause names a loop, as
// the raw adapter's prepass finds the loops it reads g_LoopConstants for.
bool hasLoops(std::span<const std::uint32_t> words)
{
    using namespace rex::graphics::ucode;
    std::size_t end = words.size() / 3;
    for (std::size_t pair = 0; pair < end; ++pair)
    {
        ControlFlowInstruction instructions[2];
        UnpackControlFlowInstructions(words.data() + pair * 3, instructions);
        for (const auto& instruction : instructions)
        {
            const auto opcode = instruction.opcode();
            if (opcode == ControlFlowOpcode::kLoopStart || opcode == ControlFlowOpcode::kLoopEnd)
                return true;
            if (IsControlFlowOpcodeExec(opcode) && instruction.exec.address())
                end = std::min(end, std::size_t(instruction.exec.address()));
        }
    }
    return false;
}

// True when any executed ALU slot holds a vector or scalar kill opcode. A
// clause outside the microcode counts as a kill.
bool hasPixelKill(std::span<const std::uint32_t> words)
{
    using namespace rex::graphics::ucode;
    std::size_t                             end = words.size() / 3;
    std::vector<ControlFlowExecInstruction> clauses;
    for (std::size_t pair = 0; pair < end; ++pair)
    {
        ControlFlowInstruction instructions[2];
        UnpackControlFlowInstructions(words.data() + pair * 3, instructions);
        for (const auto& instruction : instructions)
            if (IsControlFlowOpcodeExec(instruction.opcode()))
            {
                end = std::min(end, std::size_t(instruction.exec.address()));
                clauses.push_back(instruction.exec);
            }
    }
    for (const auto& clause : clauses)
        for (std::uint32_t slot = 0; slot < clause.count(); ++slot)
        {
            const std::size_t index = clause.address() + slot;
            if ((index + 1) * 3 > words.size())
                return true;
            if ((clause.sequence() >> (slot * 2)) & 1U)
                continue;
            AluInstruction alu;
            std::memcpy(&alu, words.data() + index * 3, sizeof(alu));
            const auto vector = static_cast<std::uint32_t>(alu.vector_opcode());
            const auto scalar = static_cast<std::uint32_t>(alu.scalar_opcode());
            if ((vector >= std::uint32_t(AluVectorOpcode::kKillEq) && vector <= std::uint32_t(AluVectorOpcode::kKillNe)) ||
                (scalar >= std::uint32_t(AluScalarOpcode::kKillsEq) && scalar <= std::uint32_t(AluScalarOpcode::kKillsOne)))
                return true;
        }
    return false;
}

bool validateShaderBytes(const NativeGameplayDrawView&        view,
                         const NativeGameplayShaderInterface& shader,
                         std::string&                         error)
{
    // A depth-only interface has no pixel identity, and its draw no pixel shader.
    const bool pixel = !shader.depthOnly;
    if (!shader.vertexShaderHash || bool(shader.pixelShaderHash) != pixel || shader.vertexShaderDxil.empty() ||
        shader.pixelShaderDxil.empty() == pixel || shader.vertexShaderDxil.size() > kMaxShaderSize ||
        shader.pixelShaderDxil.size() > kMaxShaderSize ||
        (shader.vertexShaderDxil.size() & 3U) != 0 || (shader.pixelShaderDxil.size() & 3U) != 0)
        return fail(error, "gameplay shader descriptor is missing bounded pinned DXIL");
    if (!pixel && (!view.pixelMicrocode.empty() || !shader.pixelGuestWordDigest.empty() || !shader.pixelDxilDigest.empty()))
        return fail(error, "depth_only_pixel_shader");
    if (shader.vertexGuestWordDigest.empty() || shader.vertexDxilDigest.empty() ||
        wordDigest(view.vertexMicrocode) != shader.vertexGuestWordDigest ||
        digest(shader.vertexShaderDxil) != shader.vertexDxilDigest ||
        (pixel && (shader.pixelGuestWordDigest.empty() || shader.pixelDxilDigest.empty() ||
                   wordDigest(view.pixelMicrocode) != shader.pixelGuestWordDigest ||
                   digest(shader.pixelShaderDxil) != shader.pixelDxilDigest)))
        return fail(error, "gameplay shader bytes do not match the supplied pinned interface");
    if (hasMemoryExport(view.vertexMicrocode) || hasMemoryExport(view.pixelMicrocode))
        return fail(error, "gameplay shader memory export is outside the draw replay contract");
    return true;
}

void store(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::size_t byte = 0; byte < 4; ++byte)
        bytes[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}

std::uint16_t big16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>((std::uint16_t(bytes[0]) << 8) | bytes[1]);
}

std::uint32_t big32(const std::uint8_t* bytes)
{
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
           (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

std::int32_t signExtend(std::uint32_t value, unsigned bits)
{
    const std::uint32_t sign = 1U << (bits - 1);
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

float halfToFloat(std::uint16_t value)
{
    const std::uint32_t sign = (value >> 15) & 1U;
    const std::uint32_t exp  = (value >> 10) & 0x1FU;
    const std::uint32_t frac = value & 0x3FFU;
    std::uint32_t       bits;
    if (exp == 0)
    {
        if (!frac)
            bits = sign << 31;
        else
        {
            // Normalize a subnormal half before converting its exponent.
            std::uint32_t normalized = frac;
            int           exponent   = -14;
            while ((normalized & 0x400U) == 0)
            {
                normalized <<= 1;
                --exponent;
            }
            bits = (sign << 31) | (std::uint32_t(exponent + 127) << 23) |
                   ((normalized & 0x3FFU) << 13);
        }
    }
    else if (exp == 0x1FU)
        bits = (sign << 31) | 0x7F800000U | (frac << 13);
    else
        bits = (sign << 31) | ((exp + 112U) << 23) | (frac << 13);
    return std::bit_cast<float>(bits);
}

std::size_t formatBytes(std::uint32_t format)
{
    switch (format)
    {
        case 6:
        case 7:
        case 16:
        case 17:
        case 25:
        case 31:
        case 33:
        case 36:
            return format == 25 || format == 31 ? 4 : 4;
        case 26:
        case 32:
        case 34:
        case 37:
            return format == 26 || format == 32 ? 8 : 8;
        case 35:
        case 38:
            return 16;
        case 57:
            return 12;
        default:
            return 0;
    }
}

float integerComponent(std::uint32_t value, unsigned bits, bool isSigned, bool normalized)
{
    if (isSigned)
    {
        const auto signedValue = signExtend(value, bits);
        if (!normalized)
            return static_cast<float>(signedValue);
        const float denominator = static_cast<float>((std::uint32_t(1) << (bits - 1)) - 1U);
        return std::max(-1.0F, static_cast<float>(signedValue) / denominator);
    }
    if (!normalized)
        return static_cast<float>(value);
    const auto denominator = bits == 32 ? std::numeric_limits<std::uint32_t>::max()
                                        : (std::uint32_t(1) << bits) - 1U;
    return static_cast<float>(value) / static_cast<float>(denominator);
}

bool decodeAttribute(const NativeGameplayVertexAttribute& attribute,
                     const std::uint8_t*                  source,
                     std::array<float, 4>&                values,
                     std::string&                         error)
{
    values            = {};
    const auto format = attribute.format;
    switch (format)
    {
        case 6:
        {
            const auto packed = big32(source);
            for (unsigned component = 0; component < 4; ++component)
                values[component] = integerComponent((packed >> (component * 8)) & 0xFFU,
                                                     8,
                                                     attribute.isSigned,
                                                     attribute.normalized);
            return true;
        }
        case 25:
        case 26:
        {
            const std::size_t count = format == 25 ? 2 : 4;
            for (std::size_t component = 0; component < count; ++component)
                values[component] = integerComponent(big16(source + (component / 2) * 4 +
                                                           (1 - (component & 1)) * 2),
                                                     16,
                                                     attribute.isSigned,
                                                     attribute.normalized);
            return true;
        }
        case 31:
        case 32:
        {
            const std::size_t count = format == 31 ? 2 : 4;
            for (std::size_t component = 0; component < count; ++component)
                values[component] = halfToFloat(big16(source + (component / 2) * 4 +
                                                      (1 - (component & 1)) * 2));
            return true;
        }
        case 33:
        case 36:
        {
            const auto raw = big32(source);
            values[0]      = format == 36 ? std::bit_cast<float>(raw)
                                          : integerComponent(raw, 32, attribute.isSigned, attribute.normalized);
            return true;
        }
        case 34:
        case 37:
        {
            for (unsigned component = 0; component < 2; ++component)
            {
                const auto raw    = big32(source + component * 4);
                values[component] = format == 37
                                        ? std::bit_cast<float>(raw)
                                        : integerComponent(raw, 32, attribute.isSigned, attribute.normalized);
            }
            return true;
        }
        case 35:
        case 38:
        {
            for (unsigned component = 0; component < 4; ++component)
            {
                const auto raw    = big32(source + component * 4);
                values[component] = format == 38
                                        ? std::bit_cast<float>(raw)
                                        : integerComponent(raw, 32, attribute.isSigned, attribute.normalized);
            }
            return true;
        }
        case 57:
            for (unsigned component = 0; component < 3; ++component)
                values[component] = std::bit_cast<float>(big32(source + component * 4));
            return true;
        case 7:
        {
            const auto packed = big32(source);
            values[0]         = integerComponent(packed & 0x3FFU, 10, attribute.isSigned, attribute.normalized);
            values[1]         = integerComponent((packed >> 10) & 0x3FFU, 10, attribute.isSigned, attribute.normalized);
            values[2]         = integerComponent((packed >> 20) & 0x3FFU, 10, attribute.isSigned, attribute.normalized);
            values[3]         = integerComponent((packed >> 30) & 0x3U, 2, attribute.isSigned, attribute.normalized);
            return true;
        }
        case 16:
        case 17:
            return fail(error, "packed 10/11-bit vertex formats require translator-specific float semantics");
        default:
            return fail(error, "unsupported gameplay vertex fetch format");
    }
}

// RB_BLENDCONTROL factor codes. D3D12 forbids color factors in the alpha
// slots, so `alpha` maps them to their alpha forms, as kBlendFactorAlphaMap
// in the SDK's src/graphics/d3d12/pipeline_cache.cpp does.
bool decodeBlendFactor(std::uint32_t value, bool alpha, NativeDrawReplayBlendFactor& factor)
{
    switch (value)
    {
        case 0:
            factor = NativeDrawReplayBlendFactor::Zero;
            return true;
        case 1:
            factor = NativeDrawReplayBlendFactor::One;
            return true;
        case 4:
            factor = alpha ? NativeDrawReplayBlendFactor::SourceAlpha : NativeDrawReplayBlendFactor::SourceColor;
            return true;
        case 5:
            factor = alpha ? NativeDrawReplayBlendFactor::InverseSourceAlpha : NativeDrawReplayBlendFactor::InverseSourceColor;
            return true;
        case 6:
            factor = NativeDrawReplayBlendFactor::SourceAlpha;
            return true;
        case 7:
            factor = NativeDrawReplayBlendFactor::InverseSourceAlpha;
            return true;
        case 8:
            factor = alpha ? NativeDrawReplayBlendFactor::DestinationAlpha : NativeDrawReplayBlendFactor::DestinationColor;
            return true;
        case 9:
            factor = alpha ? NativeDrawReplayBlendFactor::InverseDestinationAlpha : NativeDrawReplayBlendFactor::InverseDestinationColor;
            return true;
        case 10:
            factor = NativeDrawReplayBlendFactor::DestinationAlpha;
            return true;
        case 11:
            factor = NativeDrawReplayBlendFactor::InverseDestinationAlpha;
            return true;
        case 16:
            factor = NativeDrawReplayBlendFactor::SourceAlphaSaturated;
            return true;
        default:
            return false;
    }
}

bool decodeBlendOp(std::uint32_t value, NativeDrawReplayBlendOp& op)
{
    switch (value)
    {
        case 0:
            op = NativeDrawReplayBlendOp::Add;
            return true;
        case 1:
            op = NativeDrawReplayBlendOp::Subtract;
            return true;
        case 2:
            op = NativeDrawReplayBlendOp::Minimum;
            return true;
        case 3:
            op = NativeDrawReplayBlendOp::Maximum;
            return true;
        case 4:
            op = NativeDrawReplayBlendOp::ReverseSubtract;
            return true;
        default:
            return false;
    }
}

bool decodeTargetFormat(std::uint32_t value, NativeDrawReplayTargetFormat& format)
{
    switch (value)
    {
        case 0:
            format = NativeDrawReplayTargetFormat::Rgba8;
            return true;
        case 2:
            format = NativeDrawReplayTargetFormat::Rgb10a2;
            return true;
        case 4:
            format = NativeDrawReplayTargetFormat::Rg16Snorm;
            return true;
        default:
            return false;
    }
}

bool equalViewport(const NativeDrawReplayViewport& left, const NativeDrawReplayViewport& right)
{
    return left.x == right.x && left.y == right.y && left.width == right.width &&
           left.height == right.height && left.minDepth == right.minDepth &&
           left.maxDepth == right.maxDepth;
}

bool equalScissor(const NativeDrawReplayScissor& left, const NativeDrawReplayScissor& right)
{
    return left.left == right.left && left.top == right.top && left.right == right.right &&
           left.bottom == right.bottom;
}

std::int32_t packedSigned15(std::uint32_t value, unsigned shift)
{
    return signExtend((value >> shift) & 0x7FFFU, 15);
}

std::uint32_t packedUnsigned14(std::uint32_t value, unsigned shift)
{
    return (value >> shift) & 0x3FFFU;
}

bool decodeViewportAndScissor(std::span<const std::uint32_t> registers,
                              NativeDrawReplayViewport&      viewport,
                              NativeDrawReplayScissor&       scissor,
                              std::string&                   error)
{
    const float xScale  = std::bit_cast<float>(registers[0x210F]);
    const float xOffset = std::bit_cast<float>(registers[0x2110]);
    const float yScale  = std::bit_cast<float>(registers[0x2111]);
    const float yOffset = std::bit_cast<float>(registers[0x2112]);
    const float zScale  = std::bit_cast<float>(registers[0x2113]);
    const float zOffset = std::bit_cast<float>(registers[0x2114]);
    if (!std::isfinite(xScale) || !std::isfinite(xOffset) || !std::isfinite(yScale) ||
        !std::isfinite(yOffset) || !std::isfinite(zScale) || !std::isfinite(zOffset) ||
        !std::isfinite(zOffset + zScale) || xScale <= 0.0F || yScale >= 0.0F ||
        zScale < 0.0F || zOffset < 0.0F || zOffset + zScale > 1.0F)
        return fail(error, "unsupported gameplay viewport scale or depth range");

    const auto windowX = packedSigned15(registers[0x2080], 0);
    const auto windowY = packedSigned15(registers[0x2080], 16);
    viewport           = { xOffset - xScale + static_cast<float>(windowX),
                           yOffset + yScale + static_cast<float>(windowY),
                           xScale * 2.0F,
                           -yScale * 2.0F,
                           zOffset,
                           zOffset + zScale };

    const bool offsetDisabled = (registers[0x2081] >> 31) != 0;
    const auto offsetX        = offsetDisabled ? 0 : windowX;
    const auto offsetY        = offsetDisabled ? 0 : windowY;
    scissor.left              = static_cast<std::int32_t>(packedUnsigned14(registers[0x2081], 0)) + offsetX;
    scissor.top               = static_cast<std::int32_t>(packedUnsigned14(registers[0x2081], 16)) + offsetY;
    scissor.right             = static_cast<std::int32_t>(packedUnsigned14(registers[0x2082], 0)) + offsetX;
    scissor.bottom            = static_cast<std::int32_t>(packedUnsigned14(registers[0x2082], 16)) + offsetY;
    if (scissor.right < scissor.left || scissor.bottom < scissor.top)
        return fail(error, "gameplay window scissor is inverted");

    const auto screenLeft   = packedSigned15(registers[0x200E], 0);
    const auto screenTop    = packedSigned15(registers[0x200E], 16);
    const auto screenRight  = packedSigned15(registers[0x200F], 0);
    const auto screenBottom = packedSigned15(registers[0x200F], 16);
    const bool empty        = scissor.left == scissor.right || scissor.top == scissor.bottom;
    if (screenRight <= screenLeft || screenBottom <= screenTop ||
        (!empty && (scissor.left < screenLeft || scissor.top < screenTop || scissor.right > screenRight ||
                    scissor.bottom > screenBottom)))
        return fail(error, "gameplay window scissor exceeds the screen scissor");
    // Signed window offsets can move an empty HUD scissor off the screen.
    // Canonicalize it without bypassing the remaining draw validation.
    if (empty)
        scissor = {};
    return true;
}

bool buildState(const NativeGameplayDrawView&        view,
                const NativeGameplayShaderInterface& shader,
                const NativeGameplayDrawTarget&      target,
                NativeDrawReplayRecipe&              recipe,
                std::string&                         error)
{
    const auto r        = view.registers;
    const auto required = [&](std::size_t index, std::uint32_t value)
    {
        if (r[index] != value)
        {
            std::ostringstream text;
            text << "unsupported gameplay register 0x" << std::hex << index << ": 0x"
                 << r[index] << ", expected 0x" << value;
            fail(error, text.str());
            return false;
        }
        return true;
    };
    if ((r[0x2002] & 0xFFFE0000U) != 0 || (r[0x2002] & 0x10000U) != 0)
        return fail(error, "unsupported gameplay depth target format");
    const auto surfacePitch = r[0x2000] & 0x3FFFU;
    const auto msaaEncoding = (r[0x2000] >> 16) & 3U;
    // The window scissor may lie inside the logical surface, as the civ select
    // leader's 1280x351 band and the GFx HUD movies' display rectangles do;
    // the recipe applies it.
    const bool fullViewport = target.fullViewportTarget && surfacePitch == 640 &&
                              target.width == 1280 && target.height == 720 && r[0x2080] == 0 &&
                              ((r[0x2081] | r[0x2082]) & 0xC000C000U) == 0 &&
                              packedUnsigned14(r[0x2082], 0) <= 1280 && packedUnsigned14(r[0x2082], 16) <= 720;
    // A single-sample surface's pitch is its width rounded up to the 80-pixel
    // EDRAM tile, as the 512x512 (pitch 560) and 1024x1024 (1040) targets are.
    const bool pitchMatches = surfacePitch == target.width || (msaaEncoding == 0U && surfacePitch == (target.width + 79U) / 80U * 80U);
    if ((target.fullViewportTarget ? !fullViewport : !pitchMatches) || msaaEncoding > 1U)
        return fail(error, "gameplay surface extent or sample encoding does not match the target");
    const auto sampleCount = msaaEncoding == 0U ? 1U : 4U;
    if (sampleCount != target.sampleCount || shader.sampleCount != target.sampleCount ||
        shader.sampleMask != target.sampleMask)
        return fail(error, "gameplay sample count or coverage does not match the target");
    if (!((target.sampleCount == 1 && target.sampleMask == 1) ||
          (target.sampleCount == 4 && (target.sampleMask == 9 || target.sampleMask == 0xFU))))
        return fail(error, "unsupported gameplay sample count or coverage mask");

    const auto                   colorFormat = (r[0x2001] >> 16) & 0xFU;
    const auto                   colorBias   = signExtend((r[0x2001] >> 20) & 0x3FU, 6);
    NativeDrawReplayTargetFormat decodedFormat;
    if (!decodeTargetFormat(colorFormat, decodedFormat) || shader.colorTargetFormat != colorFormat ||
        shader.colorExpBias != colorBias || decodedFormat != target.format)
        return fail(error, "unsupported or mismatched gameplay color target format");

    // Screen-space positions skip the viewport transform; the specialized VS
    // must then map pixels to NDC, and only it may.
    const bool screenSpace = r[0x2204] == 0x90000 && r[0x2206] == 0x400;
    if (screenSpace && !shader.screenSpacePosition)
        return fail(error, "screen_space_position_unpinned");
    if (shader.screenSpacePosition && !screenSpace)
        return fail(error, "viewport_transform_unexpected");
    // PA_CL_CLIP_CNTL 0x80001 adds UCP_ENA_0. The SDK clips by user plane 0
    // in the VS (command_processor.cpp:7480, dxbc_translator.cpp:949), so
    // only a VS specialized for it may draw with it.
    const bool userClip = r[0x2204] == 0x80001U;
    if (userClip && !shader.userClipPlane)
        return fail(error, "user_clip_plane_unpinned");
    if (shader.userClipPlane && !userClip)
        return fail(error, "user_clip_plane_unexpected");
    if (!screenSpace && ((!userClip && !required(0x2204, 0x80000)) || !required(0x2206, 0x43F)))
        return false;
    for (const auto [index, value] : {
             std::pair<std::size_t, std::uint32_t>{ 0x2302, 4 },
             { 0x2307, 0xFF000 },
             { 0x2308, 0xFF100 },
         })
        if (!required(index, value))
            return false;
    if (r[0x2208] != 4 && r[0x2208] != 5)
        return fail(error, "unsupported gameplay draw color/depth mode");
    // A draw without a pixel shader is modeled in depth-only mode with every
    // color channel masked.
    if (shader.depthOnly && (r[0x2208] != 5 || r[0x2104] != 0))
        return fail(error, "depth_only_color_writes");

    if (!required(0x2180, shader.shaderProgram))
        return false;
    // SQ_PROGRAM_CNTL.vs_export_mode: the raw translator exports one position
    // vector; kill, edge and sprite exports and multipass (no raster) are not modeled.
    if (((r[0x2180] >> 24) & 7U) != 0)
        return fail(error, "vertex_export_mode_unsupported");
    if (!required(0x2181, shader.shaderContext))
        return false;
    if (!NativeGameplayInterpolatorsMatch(shader, r[0x2182]))
        return fail(error, "gameplay shader interpolation state does not match its specialization");
    if (shader.generatedPixelParameters != bool(r[0x2180] & (1U << 18)))
        return fail(error, "gameplay shader generated pixel parameter specialization does not match SQ_PROGRAM_CNTL");

    const auto depthControl = r[0x2200];
    if ((depthControl & 8U) != 0)
        return fail(error, "early depth testing is outside the replay state contract");
    const auto depthFunction = (depthControl >> 4) & 7U;
    recipe.stencil.enabled   = (depthControl & 1U) != 0;
    if (recipe.stencil.enabled)
    {
        const auto mask = r[0x210D];
        if ((mask & 0xFF000000U) != 0 ||
            ((depthControl & 0x80U) != 0 && r[0x210C] != mask))
            return fail(error, "per-face stencil references or masks are unsupported");
        recipe.stencil.reference = mask & 0xFFU;
        recipe.stencil.readMask  = (mask >> 8) & 0xFFU;
        recipe.stencil.writeMask = (mask >> 16) & 0xFFU;
        const auto decodeFace    = [](std::uint32_t control)
        {
            return NativeDrawReplayStencilState::Face{
                static_cast<std::uint8_t>((control & 7U) + 1),
                static_cast<std::uint8_t>(((control >> 3) & 7U) + 1),
                static_cast<std::uint8_t>(((control >> 9) & 7U) + 1),
                static_cast<std::uint8_t>(((control >> 6) & 7U) + 1)
            };
        };
        recipe.stencil.front = decodeFace(depthControl >> 8);
        recipe.stencil.back  = (depthControl & 0x80U) ? decodeFace(depthControl >> 20) : recipe.stencil.front;
    }

    const auto rasterControl = r[0x2205];
    if ((rasterControl & 0x18000U) != 0x18000U || (rasterControl & ~0x218007U) != 0)
        return fail(error, "unsupported gameplay polygon or rasterizer state");
    const bool cullFront = (rasterControl & 1U) != 0;
    const bool cullBack  = (rasterControl & 2U) != 0;
    if (cullFront && cullBack)
        return fail(error, "simultaneous front and back face culling is unsupported");

    const auto colorControl = r[0x2202];
    if ((colorControl >> 24) != 0x87U || (colorControl & 0x00FFFFE0U) != 0)
        return fail(error, "unsupported gameplay alpha-to-coverage offsets");
    // In RB_MODECONTROL 5 the SDK binds no guest PS (command_processor.cpp:5697)
    // and alpha tests only in a translated PS (dxbc_translator_om.cpp:2625-2645),
    // so a pixel-less entry's enabled test is a no-op.
    const bool alphaIgnored = shader.depthOnly && r[0x2208] == 5;
    const bool alphaEnabled = (colorControl & 8U) != 0 && !alphaIgnored;
    // Keep the guest Xenos compare encoding (greater=4) in the interface
    // descriptor; shader generation maps it to its host comparison enum.
    const auto alphaCompare = colorControl & 7U;
    if (alphaEnabled != shader.alphaTestEnabled ||
        (alphaEnabled && shader.alphaTestCompare != alphaCompare))
        return fail(error, "translated pixel shader alpha-test specialization does not match RB_COLORCONTROL");
    // The SDK dithers alpha to mask with the RB_COLORCONTROL offsets in the
    // pixel shader; host alpha to coverage uses another pattern.
    if ((colorControl & 0x10U) != 0)
        return fail(error, "alpha_to_mask_unsupported");
    // In RB_MODECONTROL depth-only mode the SDK runs no pixel shader, so alpha
    // test and kill instructions cannot discard samples there.
    if (r[0x2208] == 5 && alphaEnabled && alphaCompare != 7)
        return fail(error, "depth_only_alpha_test");
    if (r[0x2208] == 5 && hasPixelKill(view.pixelMicrocode))
        return fail(error, "depth_only_pixel_kill");

    const auto blendControl = r[0x2201];
    if ((blendControl & 0xE000E000U) != 0 || (r[0x2208] == 4 && (r[0x2104] & ~0xFU) != 0))
        return fail(error, "unsupported gameplay blend or color-mask state");
    if (!decodeBlendFactor(blendControl & 0x1FU, false, recipe.blend.sourceColor) ||
        !decodeBlendOp((blendControl >> 5) & 7U, recipe.blend.colorOp) ||
        !decodeBlendFactor((blendControl >> 8) & 0x1FU, false, recipe.blend.destinationColor) ||
        !decodeBlendFactor((blendControl >> 16) & 0x1FU, true, recipe.blend.sourceAlpha) ||
        !decodeBlendOp((blendControl >> 21) & 7U, recipe.blend.alphaOp) ||
        !decodeBlendFactor((blendControl >> 24) & 0x1FU, true, recipe.blend.destinationAlpha))
        return fail(error, "unsupported gameplay blend factor or operation");

    NativeDrawReplayViewport decodedViewport;
    NativeDrawReplayScissor  decodedScissor;
    if (!decodeViewportAndScissor(view.registers, decodedViewport, decodedScissor, error))
        return false;
    if (!equalViewport(decodedViewport, target.viewport) || !equalScissor(decodedScissor, target.scissor))
        return fail(error, "gameplay viewport or scissor does not match the supplied target");
    // With the viewport transform off the SDK ignores PA_CL_VPORT_* and uses a
    // 0..1 depth range; the screen-space VS maps pixels to a 1280x720 viewport.
    if (screenSpace && !equalViewport(target.viewport, { 0, 0, 1280, 720, 0, 1 }))
        return fail(error, "screen_space_viewport");
    // The pinned vtx_window_offset_enable makes the SDK shift vertices by
    // PA_SC_WINDOW_OFFSET (draw.cpp:320); the screen-space VS does not.
    if (screenSpace && (packedSigned15(r[0x2080], 0) != 0 || packedSigned15(r[0x2080], 16) != 0))
        return fail(error, "screen_space_window_offset");
    const bool emptyScissor = target.scissor.left == target.scissor.right || target.scissor.top == target.scissor.bottom;
    if (target.width == 0 || target.height == 0 || target.viewport.width <= 0.0F ||
        target.viewport.height <= 0.0F ||
        (!emptyScissor && (target.viewport.x + target.viewport.width < 0.0F ||
                           target.viewport.y + target.viewport.height < 0.0F)) ||
        target.scissor.left < 0 ||
        target.scissor.top < 0 || target.scissor.right > static_cast<std::int32_t>(target.width) ||
        target.scissor.bottom > static_cast<std::int32_t>(target.height))
        return fail(error, "gameplay target viewport or scissor is outside its extent");

    recipe.schemaVersion                    = 2;
    recipe.width                            = target.width;
    recipe.height                           = target.height;
    recipe.sampleCount                      = target.sampleCount;
    recipe.sampleMask                       = target.sampleMask;
    recipe.targetFormat                     = target.format;
    recipe.clearColor                       = { 0, 0, 0, 0 };
    recipe.viewport                         = target.viewport;
    recipe.scissor                          = target.scissor;
    recipe.depth.enabled                    = (depthControl & 2U) != 0;
    recipe.depth.writeEnabled               = recipe.depth.enabled && (depthControl & 4U) != 0;
    recipe.depth.compare                    = depthFunction + 1U;
    recipe.depth.initialClear               = 1.0F;
    recipe.rasterizer.cull                  = cullFront ? 1U : cullBack ? 2U
                                                                        : 0U;
    recipe.rasterizer.frontCounterClockwise = (rasterControl & 4U) == 0;
    // pipeline_cache.cpp: depth_clip = !PA_CL_CLIP_CNTL.clip_disable.
    recipe.rasterizer.depthClip = (r[0x2204] & 0x10000U) == 0;
    recipe.blend.enabled        = true;
    recipe.blend.writeMask      = r[0x2208] == 5 ? 0 : static_cast<std::uint8_t>(r[0x2104] & 0xFU);

    // Stage constants retain their exact little-endian register bits.
    if constexpr (std::endian::native == std::endian::little)
    {
        const auto* vertexBytes = reinterpret_cast<const std::uint8_t*>(r.data() + 0x4000);
        const auto* pixelBytes  = reinterpret_cast<const std::uint8_t*>(r.data() + 0x4400);
        recipe.vertexConstants.assign(vertexBytes, vertexBytes + 256U * 16U);
        recipe.pixelConstants.assign(pixelBytes, pixelBytes + 256U * 16U);
    }
    else
    {
        recipe.vertexConstants.resize(256U * 16U);
        recipe.pixelConstants.resize(256U * 16U);
        for (std::size_t index = 0; index < 256U * 4U; ++index)
        {
            store(recipe.vertexConstants, index * 4, r[0x4000 + index]);
            store(recipe.pixelConstants, index * 4, r[0x4400 + index]);
        }
    }
    // Loop pairs read the live loop constants i0..i31 (0x4908..0x4927) at
    // bytes 336..463, after the bools, as the SDK binds its bool/loop buffer.
    recipe.sharedConstants.resize(NativeGameplaySharedConstantBytes(view.vertexMicrocode, view.pixelMicrocode, shader.userClipPlane));
    for (std::size_t index = 0; index < 8; ++index)
        store(recipe.sharedConstants, 256 + index * 4, r[0x4900 + index]);
    for (std::size_t index = 0; 336 + index * 4 < recipe.sharedConstants.size(); ++index)
        store(recipe.sharedConstants, 336 + index * 4, r[0x4908 + index]);
    // The adapter adds this W-scaled NDC offset in the vertex shader. With
    // D3D's inverted viewport Y it moves geometry +0.5 guest pixels per axis.
    store(recipe.sharedConstants, 292, std::bit_cast<std::uint32_t>(1.0F / target.viewport.width));
    store(recipe.sharedConstants, 296, std::bit_cast<std::uint32_t>(-1.0F / target.viewport.height));
    if (alphaEnabled)
        store(recipe.sharedConstants, 300, r[0x210E]);
    // PA_CL_UCP_0_X..W (0x2388..0x238B) as the SDK's user_clip_planes[0].
    if (shader.userClipPlane)
        for (std::size_t index = 0; index < 4; ++index)
            store(recipe.sharedConstants, kNativeGameplayUserClipPlaneOffset + index * 4, r[0x2388 + index]);
    return true;
}

bool buildRecipe(NativeGameplayShaderValidationCache* shaderValidation,
                 NativeGameplayGeometryCache*         geometryCache,
                 const NativeGameplayDrawView&        view,
                 const NativeGameplayShaderInterface& shader,
                 const NativeGameplayDrawTarget&      target,
                 NativeDrawReplayRecipe&              recipe,
                 std::string&                         error)
{
    resetRecipe(recipe);
    error.clear();
    if (view.registers.size() != kRegisterCount)
        return fail(error, "gameplay register snapshot is incomplete");
    if (!(shaderValidation ? shaderValidation->Validate(view, shader, error)
                           : validateShaderBytes(view, shader, error)))
        return false;
    if (shader.vertexAttributes.empty() || shader.vertexAttributes.size() > kMaxVertexAttributes)
        return fail(error, "gameplay shader interface has no bounded vertex attributes");
    if (!shader.rasterizedDepth)
        return fail(error, "gameplay shader must preserve rasterized depth");
    if (!shader.independentSamplers)
        return fail(error, "gameplay shader must use independent samplers");
    if (view.primitive != 4 && view.primitive != 6)
        return fail(error, "unsupported gameplay primitive topology");
    // VGT_OUTPUT_PATH_CNTL tessellation applies in the explicit major mode of
    // VGT_DRAW_INITIATOR; triangle lists and strips are explicit only by it.
    if ((view.registers[0x2284] & 3U) == 1U && ((view.registers[0x21FC] >> 8) & 3U) != 0)
        return fail(error, "tessellation_unsupported");
    // PA_SC_VIZ_QUERY with kill_pix_post_hi_z makes the SDK drop the draw.
    if ((view.registers[0x2293] & 0x81U) == 0x81U)
        return fail(error, "viz_query_kills_pixels");
    if (!view.indexCount || view.indexCount > kMaxGeometrySize / 2)
        return fail(error, "gameplay index count is outside the geometry bound");
    if ((view.primitive == 4 && view.indexCount > kMaxGeometrySize / 4) ||
        (view.primitive == 6 && view.indexCount > kMaxGeometrySize / 12))
        return fail(error, "expanded gameplay index stream exceeds the geometry bound");
    const std::size_t bytesPerIndex = view.indexFormat == 0 ? 2 : view.indexFormat == 1 ? 4
                                                                                        : 0;
    if (view.indexed)
    {
        if (!bytesPerIndex || (view.indexFormat == 0 && view.indexEndian != 1) ||
            (view.indexFormat == 1 && view.indexEndian != 2) ||
            view.indexBytes.size() != std::size_t(view.indexCount) * bytesPerIndex)
            return fail(error, "gameplay draw has an unsupported or incomplete indexed stream");
    }
    else if (!view.indexBytes.empty())
        return fail(error, "nonindexed gameplay draw supplied an index stream");

    const auto registers = view.registers;
    // Like the SDK, reset applies only to DMA indices. The SDK ignores it for
    // lists, where this converter would drop the triangle, so a hit is refused.
    const bool primitiveRestart = view.indexed && (registers[0x2205] & (1U << 21)) != 0;
    const auto restartIndex     = registers[0x2103] & 0xFFFFFFU;
    const auto minimumIndex     = registers[0x2101] & 0xFFFFFFU;
    const auto maximumIndex     = registers[0x2100] & 0xFFFFFFU;
    const auto indexOffset      = registers[0x2102] & 0xFFFFFFU;
    if (minimumIndex > maximumIndex || (registers[0x2100] & 0xFF000000U) != 0 ||
        (registers[0x2101] & 0xFF000000U) != 0 || (registers[0x2102] & 0xFF000000U) != 0 ||
        (registers[0x2103] & 0xFF000000U) != 0)
        return fail(error, "gameplay vertex index bounds are malformed");

    std::uint32_t maximum  = 0;
    const auto*   prepared = view.decodedGeometry;
    if (prepared)
    {
        if (!prepared->indices)
            return fail(error, "decoded gameplay geometry has no indices");
        recipe.sharedIndices = prepared->indices;
        maximum              = prepared->indices->Maximum();
    }
    const bool reusedIndices = prepared || (geometryCache && geometryCache->LookupIndices(view, recipe.indices, maximum));
    if (!reusedIndices)
    {
        constexpr std::uint32_t    kRestart = std::numeric_limits<std::uint32_t>::max();
        std::vector<std::uint32_t> stripIndices;
        if (view.primitive == 4)
            recipe.indices.resize(view.indexCount - view.indexCount % 3);
        else
            stripIndices.resize(view.indexCount);
        auto decoded = std::span<std::uint32_t>(view.primitive == 4 ? recipe.indices : stripIndices);
        // Keep the independent endian/offset conversion contiguous. Triangle
        // lists already have their final layout.
        for (std::size_t index = 0; index < decoded.size(); ++index)
        {
            std::uint32_t value = 0;
            if (view.indexed)
                value = view.indexFormat == 0 ? big16(view.indexBytes.data() + index * 2)
                                              : big32(view.indexBytes.data() + index * 4);
            else
                value = static_cast<std::uint32_t>(index);
            value &= view.indexed && view.indexFormat == 0 ? 0xFFFFU : 0xFFFFFFU;
            if (primitiveRestart && value == restartIndex)
            {
                if (view.primitive == 4)
                    return fail(error, "primitive_reset_in_list");
                decoded[index] = kRestart;
            }
            else
                decoded[index] = std::clamp((value + indexOffset) & 0xFFFFFFU, minimumIndex, maximumIndex);
        }

        if (view.primitive == 6)
        {
            recipe.indices.reserve(view.indexCount * 3);
            std::uint32_t a = 0, b = 0;
            std::size_t   stripIndex = 0;
            for (const auto c : decoded)
            {
                if (c == kRestart)
                {
                    stripIndex = 0;
                    continue;
                }
                if (stripIndex >= 2)
                {
                    if ((stripIndex & 1U) == 0)
                        recipe.indices.insert(recipe.indices.end(), { a, b, c });
                    else
                        recipe.indices.insert(recipe.indices.end(), { b, a, c });
                }
                a = b;
                b = c;
                ++stripIndex;
            }
        }
    }
    if (recipe.indexSpan().empty() || (recipe.indexSpan().size() % 3) != 0)
    {
        return fail(error, "gameplay topology did not produce a triangle list");
    }
    if (!reusedIndices)
    {
        for (const auto index : recipe.indices)
        {
            maximum = std::max(maximum, index);
        }
    }

    std::array<const NativeGameplayVertexFetchSpan*, 96> fetches{};
    std::size_t                                          totalFetchBytes = 0;
    for (const auto& fetch : view.vertexFetches)
    {
        if (fetch.fetchConstant >= fetches.size() || fetches[fetch.fetchConstant] ||
            fetch.bytes.empty() || fetch.bytes.size() > kMaxGeometrySize)
            return fail(error, "gameplay vertex fetch spans are duplicate, empty, or oversized");
        if (fetch.bytes.size() > kMaxGeometrySize - totalFetchBytes)
            return fail(error, "gameplay vertex fetch spans exceed the geometry bound");
        totalFetchBytes += fetch.bytes.size();
        const auto registerIndex = 0x4800U + fetch.fetchConstant * 2U;
        if ((view.registers[registerIndex] & 3U) != 3U ||
            (view.registers[registerIndex] & ~3U) != fetch.guestBase ||
            (view.registers[registerIndex + 1] & 3U) != 2U ||
            std::size_t((view.registers[registerIndex + 1] >> 2) & 0xFFFFFFU) * 4 != fetch.bytes.size())
            return fail(error, "gameplay vertex bytes do not match their guest fetch constant");
        fetches[fetch.fetchConstant] = &fetch;
    }
    if (view.vertexFetches.empty())
        return fail(error, "gameplay draw has no guest vertex fetch spans");

    const auto vertexCount      = std::uint64_t(maximum) + 1;
    recipe.vertexAttributeCount = static_cast<std::uint32_t>(shader.vertexAttributes.size());
    recipe.vertexStrideBytes    = recipe.vertexAttributeCount * 16U;
    if (vertexCount > kMaxGeometrySize / recipe.vertexStrideBytes ||
        vertexCount * recipe.vertexStrideBytes > kMaxGeometrySize)
        return fail(error, "decoded gameplay vertices exceed the geometry bound");

    std::array<const std::uint8_t*, kMaxVertexAttributes> vertexSources{};
    for (std::size_t index = 0; index < shader.vertexAttributes.size(); ++index)
    {
        const auto& attribute = shader.vertexAttributes[index];
        const auto  bytes     = formatBytes(attribute.format);
        if (!bytes || attribute.fetchConstant >= fetches.size() || attribute.strideBytes == 0 || attribute.offsetBytes < 0 ||
            std::uint64_t(attribute.offsetBytes) + bytes > attribute.strideBytes || !fetches[attribute.fetchConstant])
            return fail(error, "gameplay vertex attribute has an unsupported fetch layout");
        const auto& fetch = *fetches[attribute.fetchConstant];
        // Positive strides make the maximum indexed vertex the furthest read.
        // Validate each complete attribute range before allocating or decoding.
        const auto lastOffset = std::uint64_t(maximum) * attribute.strideBytes +
                                static_cast<std::uint32_t>(attribute.offsetBytes);
        if (lastOffset + bytes > fetch.bytes.size())
            return fail(error, "gameplay vertex fetch span is shorter than its indexed attributes");
        vertexSources[index] = fetch.bytes.data() + attribute.offsetBytes;
    }
    if (prepared)
    {
        if (!prepared->vertices || prepared->vertices->size() != vertexCount * recipe.vertexStrideBytes)
            return fail(error, "decoded gameplay geometry does not match its draw");
        recipe.sharedVertexData = prepared->vertices;
    }
    const bool reused = prepared || (geometryCache && geometryCache->Lookup(shader.vertexAttributes, view.vertexFetches, vertexCount, recipe.sharedVertexData, view, recipe.indices));
    if (!reused)
    {
        recipe.vertexData.resize(static_cast<std::size_t>(vertexCount * recipe.vertexStrideBytes));
        for (std::uint32_t vertex = 0; vertex < vertexCount; ++vertex)
            for (std::size_t attributeIndex = 0; attributeIndex < shader.vertexAttributes.size(); ++attributeIndex)
            {
                const auto&          attribute    = shader.vertexAttributes[attributeIndex];
                const auto           sourceOffset = std::size_t(vertex) * attribute.strideBytes;
                std::array<float, 4> values;
                if (!decodeAttribute(attribute, vertexSources[attributeIndex] + sourceOffset, values, error))
                    return false;
                const auto destination = std::size_t(vertex) * recipe.vertexStrideBytes + attributeIndex * 16;
                if constexpr (std::endian::native == std::endian::little)
                    std::memcpy(recipe.vertexData.data() + destination, values.data(), values.size() * sizeof(float));
                else
                    for (unsigned component = 0; component < 4; ++component)
                        store(recipe.vertexData, destination + component * 4, std::bit_cast<std::uint32_t>(values[component]));
            }

        if (geometryCache)
            geometryCache->Remember(shader.vertexAttributes, view.vertexFetches, vertexCount, recipe.vertexData, view, recipe.indices);
    }
    recipe.indexCount = static_cast<std::uint32_t>(recipe.indexSpan().size());

    if (!buildState(view, shader, target, recipe, error))
        return false;

    recipe.vertexShaderHash = shader.vertexShaderHash;
    recipe.pixelShaderHash  = shader.pixelShaderHash;
    recipe.depthOnly        = shader.depthOnly;
    if (shaderValidation)
    {
        recipe.sharedVertexShaderDxil = shaderValidation->VertexBytecode();
        recipe.sharedPixelShaderDxil  = shaderValidation->PixelBytecode();
        recipe.shaderVersion          = shaderValidation->Version();
    }
    else
    {
        recipe.vertexShaderDxil = shader.vertexShaderDxil;
        recipe.pixelShaderDxil  = shader.pixelShaderDxil;
    }
    if (shader.textureFetchSlots.size() != view.textureBindings.size() ||
        shader.textureDimensions.size() != shader.textureFetchSlots.size() ||
        shader.textureFetchSlots.size() > kNativeDrawReplayTextureSlots)
        return fail(error, "gameplay texture fetch interface and bindings have different sizes");
    recipe.textureMask = shader.textureFetchSlots.empty()
                             ? 0U
                         : shader.textureFetchSlots.size() == 32
                             ? 0xFFFFFFFFU
                             : (1U << shader.textureFetchSlots.size()) - 1U;
    recipe.samplers.clear();
    recipe.samplers.reserve(view.textureBindings.size());
    std::uint32_t seenTextureSlots = 0;
    for (std::size_t index = 0; index < view.textureBindings.size(); ++index)
    {
        const auto& binding = view.textureBindings[index];
        if (binding.guestTextureSlot != shader.textureFetchSlots[index] ||
            binding.guestTextureSlot >= kNativeDrawReplayTextureSlots || !binding.texture ||
            !binding.sampler || binding.texture->dimension != shader.textureDimensions[index] ||
            (seenTextureSlots & (1U << binding.guestTextureSlot)) != 0)
            return fail(error, "gameplay texture binding does not match the shader fetch interface");
        seenTextureSlots |= 1U << binding.guestTextureSlot;
        recipe.textures[index] = *binding.texture;
        recipe.samplers.push_back(*binding.sampler);
    }
    if (!recipe.samplers.empty())
        recipe.sampler = recipe.samplers.front();
    return true;
}

} // namespace

namespace
{

std::size_t geometrySourcePrefix(std::span<const NativeGameplayVertexAttribute> attributes,
                                 const NativeGameplayVertexFetchSpan&           fetch,
                                 std::uint64_t                                  vertexCount)
{
    std::uint64_t prefix = 0;
    for (const auto& attribute : attributes)
        if (attribute.fetchConstant == fetch.fetchConstant)
        {
            const auto bytes = formatBytes(attribute.format);
            if (!vertexCount || !bytes || attribute.offsetBytes < 0 || !attribute.strideBytes)
                throw std::runtime_error("geometry reuse requires validated attribute ranges");
            const auto end = (vertexCount - 1) * attribute.strideBytes +
                             static_cast<std::uint32_t>(attribute.offsetBytes) + bytes;
            if (end > fetch.bytes.size())
                throw std::runtime_error("geometry reuse prefix exceeds the validated source");
            prefix = std::max(prefix, end);
        }
    return static_cast<std::size_t>(prefix);
}

std::uint64_t geometryKey(std::span<const NativeGameplayVertexAttribute> attributes,
                          std::span<const NativeGameplayVertexFetchSpan> fetches,
                          std::uint64_t                                  vertexCount)
{
    std::vector<std::uint64_t> words;
    words.reserve(3 + attributes.size() * 6 + fetches.size() * 4);
    words.insert(words.end(), { vertexCount, attributes.size(), fetches.size() });
    for (const auto& attr : attributes)
        words.insert(words.end(), { attr.fetchConstant, attr.strideBytes, static_cast<std::uint32_t>(attr.offsetBytes), attr.format, attr.isSigned, attr.normalized });
    for (const auto& fetch : fetches)
    {
        // All source extents were checked above. Only this prefix can affect
        // the original decoder; unused tails remain outside the memoized input.
        const auto prefix = geometrySourcePrefix(attributes, fetch, vertexCount);
        words.insert(words.end(), { fetch.fetchConstant, fetch.guestBase, fetch.bytes.size(), XXH3_64bits(fetch.bytes.data(), prefix) });
    }
    return XXH3_64bits(words.data(), words.size() * sizeof(words[0]));
}

} // namespace

std::size_t NativeGameplayGeometryCache::EntryBytes(const Entry& entry)
{
    auto bytes = entry.attributes.capacity() * sizeof(NativeGameplayVertexAttribute) +
                 entry.sources.capacity() * sizeof(Source) + entry.decoded->capacity() + sizeof(std::vector<std::uint8_t>) +
                 entry.indexSource.capacity() + entry.indices.capacity() * sizeof(std::uint32_t);
    for (const auto& source : entry.sources)
        bytes += source.bytes.capacity();
    return bytes;
}

std::size_t NativeGameplayVertexFormatBytes(std::uint32_t format) noexcept
{
    return formatBytes(format);
}

std::size_t NativeGameplaySharedConstantBytes(std::span<const std::uint32_t> vertexMicrocode,
                                              std::span<const std::uint32_t> pixelMicrocode,
                                              bool                           userClipPlane)
{
    if (userClipPlane)
        return kNativeGameplayUserClipPlaneOffset + 16;
    return hasLoops(vertexMicrocode) || hasLoops(pixelMicrocode) ? 464 : 336;
}

std::size_t NativeGameplayGeometryCache::EntrySupplements(const Entry& entry) noexcept
{
    using namespace preparation;
    auto bytes = SharedBytes(entry.decoded->capacity()) - entry.decoded->capacity() - sizeof(std::vector<std::uint8_t>);
    bytes      = Add(bytes, Vector(entry.attributes) - entry.attributes.capacity() * sizeof(NativeGameplayVertexAttribute));
    bytes      = Add(bytes, Vector(entry.sources) - entry.sources.capacity() * sizeof(Source));
    for (const auto& source : entry.sources)
        bytes = Add(bytes, Vector(source.bytes) - source.bytes.capacity());
    bytes = Add(bytes, Vector(entry.indexSource) - entry.indexSource.capacity());
    bytes = Add(bytes, Vector(entry.indices) - entry.indices.capacity() * sizeof(std::uint32_t));
    return bytes;
}

std::size_t NativeGameplayGeometryCache::ManagedBytes() const noexcept
{
    return preparation::Add(ownedBytes_, supplements_);
}

preparation::Forecast NativeGameplayGeometryCache::ForecastRemember(std::size_t attributes, std::size_t fetches, std::size_t decoded, std::size_t sourceBytes, std::size_t indexCount) const noexcept
{
    using namespace preparation;
    Forecast forecast{ true };
    forecast.Allocate(Allocation<Entry>(NativeRenderBatchCommands));
    forecast.Allocate(Allocation<NativeGameplayVertexAttribute>(attributes));
    forecast.Allocate(Allocation<Source>(fetches));
    forecast.Allocate(SharedBytes(decoded));
    forecast.Allocate(Add(sourceBytes, Multiply(fetches, 32 - 1 + sizeof(void*))));
    forecast.Allocate(Allocation<std::uint8_t>(Multiply(indexCount, 4)));
    forecast.Allocate(Allocation<std::uint32_t>(Multiply(indexCount, 3)));
    // Each possible new buffer may cross the held manual-alignment threshold.
    forecast.cacheSupplements = Add(SharedBytes(0) - sizeof(std::vector<std::uint8_t>), Multiply(Add(fetches, 6), 32 - 1 + sizeof(void*)));
    return forecast;
}

std::size_t NativeGameplayShaderValidationCache::ManagedBytes() const noexcept
{
    using namespace preparation;
    auto bytes = Add(Vector(vertexMicrocode_), Vector(pixelMicrocode_));
    for (const auto* digest : { &vertexGuestWordDigest_, &pixelGuestWordDigest_, &vertexDxilDigest_, &pixelDxilDigest_ })
        bytes = Add(bytes, String(*digest));
    for (const auto* payload : { &vertexShaderDxil_, &pixelShaderDxil_ })
        if (*payload)
            bytes = Add(bytes, SharedBytes((*payload)->capacity()));
    if (version_)
        bytes = Add(bytes, Separate<NativeDrawReplayShaderVersion>());
    return bytes;
}

std::size_t NativeGameplayShaderValidationCache::ForecastChanges(std::size_t vertexWords, std::size_t pixelWords, const NativeGameplayShaderInterface& shader) const noexcept
{
    using namespace preparation;
    auto bytes = Add(vertexWords > vertexMicrocode_.capacity() ? Allocation<std::uint32_t>(Grow(vertexMicrocode_.capacity(), vertexWords)) : 0,
                     pixelWords > pixelMicrocode_.capacity() ? Allocation<std::uint32_t>(Grow(pixelMicrocode_.capacity(), pixelWords)) : 0);
    bytes      = Add(bytes, Add(SharedBytes(shader.vertexShaderDxil.size()), SharedBytes(shader.pixelShaderDxil.size())));
    bytes      = Add(bytes, Separate<NativeDrawReplayShaderVersion>());
    for (const auto* digest : { &vertexGuestWordDigest_, &pixelGuestWordDigest_, &vertexDxilDigest_, &pixelDxilDigest_ })
        bytes = Add(bytes, StringGrowth<char>(digest->capacity(), 64));
    return Add(bytes, Multiply(4, StringGrowth<char>(15, 64)));
}

void NativeGameplayGeometryCache::BeginFrame()
{
    // Keep the preceding frame's working set. Recipes retain their immutable
    // decoded owners independently when an unused cache entry is retired.
    std::erase_if(entries_, [](const Entry& entry)
                  {
                      return !entry.used;
                  });
    ownedBytes_  = entries_.capacity() * sizeof(Entry);
    supplements_ = preparation::Vector(entries_) - ownedBytes_;
    for (auto& entry : entries_)
    {
        entry.used = false;
        ownedBytes_ += EntryBytes(entry);
        supplements_ = preparation::Add(supplements_, EntrySupplements(entry));
    }
}

void NativeGameplayGeometryCache::SetBudget(std::size_t budget)
{
    budget_ = budget;
    if (ownedBytes_ > budget_)
    {
        std::vector<Entry>().swap(entries_);
        ownedBytes_  = 0;
        supplements_ = 0;
    }
}

std::array<std::uint32_t, 10> NativeGameplayGeometryCache::IndexControls(const NativeGameplayDrawView& view)
{
    return { view.primitive, view.indexed, view.indexCount, view.indexFormat, view.indexEndian, view.registers[0x2100], view.registers[0x2101], view.registers[0x2102], view.registers[0x2103], view.registers[0x2205] };
}

bool NativeGameplayGeometryCache::LookupIndices(const NativeGameplayDrawView& view, std::vector<std::uint32_t>& indices, std::uint32_t& maximum)
{
    if (view.registers.size() != kRegisterCount)
    {
        return false;
    }
    const auto controls = IndexControls(view);
    for (const auto& entry : entries_)
    {
        if (entry.indices.empty() || entry.indexControls != controls || !std::ranges::equal(entry.indexSource, view.indexBytes))
        {
            continue;
        }
        indices.assign(entry.indices.begin(), entry.indices.end());
        maximum = static_cast<std::uint32_t>(entry.vertexCount - 1);
        return true;
    }
    return false;
}

void NativeGameplayGeometryCache::RememberIndices(Entry& entry, const NativeGameplayDrawView& view, std::span<const std::uint32_t> indices)
{
    if (view.registers.size() != kRegisterCount || !entry.vertexCount || indices.empty())
    {
        return;
    }
    const auto controls = IndexControls(view);
    if (!entry.indices.empty() && entry.indexControls == controls && std::ranges::equal(entry.indexSource, view.indexBytes))
    {
        return;
    }
    const auto estimate = view.indexBytes.size() + indices.size_bytes();
    if (ownedBytes_ > budget_ || estimate > budget_ - ownedBytes_)
    {
        return;
    }
    std::vector<std::uint8_t>  source(view.indexBytes.begin(), view.indexBytes.end());
    std::vector<std::uint32_t> converted(indices.begin(), indices.end());
    const auto                 added = source.capacity() + converted.capacity() * sizeof(std::uint32_t);
    if (added > budget_ - ownedBytes_)
    {
        return;
    }
    const auto previous            = entry.indexSource.capacity() + entry.indices.capacity() * sizeof(std::uint32_t);
    const auto previousSupplements = preparation::Add(preparation::Vector(entry.indexSource) - entry.indexSource.capacity(),
                                                      preparation::Vector(entry.indices) - entry.indices.capacity() * sizeof(std::uint32_t));
    const auto nextSupplements     = preparation::Add(preparation::Vector(source) - source.capacity(),
                                                      preparation::Vector(converted) - converted.capacity() * sizeof(std::uint32_t));
    entry.indexControls            = controls;
    entry.indexSource              = std::move(source);
    entry.indices                  = std::move(converted);
    ownedBytes_                    = ownedBytes_ - previous + added;
    supplements_                   = preparation::Add(supplements_ - previousSupplements, nextSupplements);
}

bool NativeGameplayGeometryCache::Lookup(std::span<const NativeGameplayVertexAttribute>    attributes,
                                         std::span<const NativeGameplayVertexFetchSpan>    fetches,
                                         std::uint64_t                                     vertexCount,
                                         std::shared_ptr<const std::vector<std::uint8_t>>& decoded,
                                         const NativeGameplayDrawView&                     view,
                                         std::span<const std::uint32_t>                    indices)
{
    const auto key = geometryKey(attributes, fetches, vertexCount);
    for (auto& entry : entries_)
    {
        if (entry.hash != key || entry.vertexCount != vertexCount || entry.sources.size() != fetches.size() ||
            !std::ranges::equal(entry.attributes, attributes, [](const auto& a, const auto& b)
                                {
                                    return a.fetchConstant == b.fetchConstant && a.strideBytes == b.strideBytes &&
                                           a.offsetBytes == b.offsetBytes && a.format == b.format &&
                                           a.isSigned == b.isSigned && a.normalized == b.normalized;
                                }))
            continue;
        bool same = true;
        for (std::size_t index = 0; index < fetches.size(); ++index)
        {
            const auto& source = entry.sources[index];
            const auto& fetch  = fetches[index];
            const auto  prefix = geometrySourcePrefix(attributes, fetch, vertexCount);
            if (source.slot != fetch.fetchConstant || source.base != fetch.guestBase || source.extent != fetch.bytes.size() ||
                !std::ranges::equal(source.bytes, fetch.bytes.first(prefix)))
            {
                same = false;
                break;
            }
        }
        if (!same)
            continue;
        entry.used = true;
        decoded    = entry.decoded;
        RememberIndices(entry, view, indices);
        return true;
    }
    return false;
}

void NativeGameplayGeometryCache::Remember(std::span<const NativeGameplayVertexAttribute> attributes,
                                           std::span<const NativeGameplayVertexFetchSpan> fetches,
                                           std::uint64_t                                  vertexCount,
                                           const std::vector<std::uint8_t>&               decoded,
                                           const NativeGameplayDrawView&                  view,
                                           std::span<const std::uint32_t>                 indices)
{
    if (entries_.size() == NativeRenderBatchCommands)
        return;
    if (!entries_.capacity())
    {
        if (NativeRenderBatchCommands * sizeof(Entry) > budget_)
            return;
        entries_.reserve(NativeRenderBatchCommands);
        ownedBytes_  = entries_.capacity() * sizeof(Entry);
        supplements_ = preparation::Vector(entries_) - ownedBytes_;
        if (ownedBytes_ > budget_)
        {
            std::vector<Entry>().swap(entries_);
            ownedBytes_  = 0;
            supplements_ = 0;
            return;
        }
    }
    auto estimate = attributes.size_bytes() + fetches.size() * sizeof(Source) + decoded.size() + sizeof(std::vector<std::uint8_t>);
    for (const auto& fetch : fetches)
    {
        const auto prefix = geometrySourcePrefix(attributes, fetch, vertexCount);
        if (prefix > budget_ - ownedBytes_ || estimate > budget_ - ownedBytes_ - prefix)
            return;
        estimate += prefix;
    }
    if (estimate > budget_ - ownedBytes_)
        return;
    Entry entry;
    entry.hash        = geometryKey(attributes, fetches, vertexCount);
    entry.vertexCount = vertexCount;
    entry.attributes.assign(attributes.begin(), attributes.end());
    entry.sources.reserve(fetches.size());
    entry.decoded = std::make_shared<const std::vector<std::uint8_t>>(decoded);
    for (const auto& fetch : fetches)
    {
        const auto prefix = geometrySourcePrefix(attributes, fetch, vertexCount);
        entry.sources.push_back({ fetch.fetchConstant, fetch.guestBase, fetch.bytes.size(), { fetch.bytes.begin(), fetch.bytes.begin() + prefix } });
    }
    const auto added = EntryBytes(entry);
    if (added > budget_ - ownedBytes_)
        return;
    ownedBytes_ += added;
    supplements_ = preparation::Add(supplements_, EntrySupplements(entry));
    entries_.push_back(std::move(entry));
    RememberIndices(entries_.back(), view, indices);
}

bool NativeGameplayShaderValidationCache::Validate(const NativeGameplayDrawView&        view,
                                                   const NativeGameplayShaderInterface& shader,
                                                   std::string&                         error)
{
    error.clear();
    // By default compare owned bytes on every hit so even in-place caller
    // mutations are checked. Only the byte validation is cached; draw state
    // is checked anew.
    if (shader.vertexShaderHash && bool(shader.pixelShaderHash) != shader.depthOnly && vertexShaderDxil_ && pixelShaderDxil_ && !vertexMicrocode_.empty() &&
        vertexGuestWordDigest_ == shader.vertexGuestWordDigest &&
        pixelGuestWordDigest_ == shader.pixelGuestWordDigest &&
        vertexDxilDigest_ == shader.vertexDxilDigest && pixelDxilDigest_ == shader.pixelDxilDigest &&
        (hit_ == Hit::Digests ||
         (std::ranges::equal(vertexMicrocode_, view.vertexMicrocode) &&
          std::ranges::equal(pixelMicrocode_, view.pixelMicrocode) &&
          *vertexShaderDxil_ == shader.vertexShaderDxil && *pixelShaderDxil_ == shader.pixelShaderDxil)))
        return true;
    if (!validateShaderBytes(view, shader, error))
        return false;
    vertexMicrocode_.assign(view.vertexMicrocode.begin(), view.vertexMicrocode.end());
    pixelMicrocode_.assign(view.pixelMicrocode.begin(), view.pixelMicrocode.end());
    vertexShaderDxil_      = std::make_shared<const std::vector<std::uint8_t>>(shader.vertexShaderDxil);
    pixelShaderDxil_       = std::make_shared<const std::vector<std::uint8_t>>(shader.pixelShaderDxil);
    version_               = std::shared_ptr<const NativeDrawReplayShaderVersion>(new NativeDrawReplayShaderVersion(vertexShaderDxil_, pixelShaderDxil_));
    vertexGuestWordDigest_ = shader.vertexGuestWordDigest;
    pixelGuestWordDigest_  = shader.pixelGuestWordDigest;
    vertexDxilDigest_      = shader.vertexDxilDigest;
    pixelDxilDigest_       = shader.pixelDxilDigest;
    return true;
}

bool NativeGameplayInterpolatorsMatch(const NativeGameplayShaderInterface& shader,
                                      std::uint32_t                        interpolators)
{
    std::uint32_t used = 0xFFFF;
    // This pinned PS exports max(r0, r0), with no other interpolator reads.
    // Draw conversion verifies the captured microcode digest before execution.
    if (!shader.generatedPixelParameters && shader.pixelShaderHash == 0xA4A965C189287B99ULL &&
        shader.pixelGuestWordDigest == "f73f655ea80c22bde4bc93575f87664e81056b6f4714bef3a4defaa18994d18a")
        used = 1;
    // Without a pixel shader the SDK passes no interpolators (command_processor.cpp:5742)
    // and reads SQ_INTERPOLATOR_CNTL only for one (pipeline_cache.cpp:859-863).
    if (shader.depthOnly)
        used = 0;
    const auto control = (shader.shaderContext >> 2) & 3U;
    if (control == 3)
        return false;
    // SQ_INTERPOLATOR_CNTL.sampling_pattern is consulted only for mixed
    // centroid/center sampling. Single-sample draws always sample the center.
    const auto mask = used | (shader.sampleCount != 1 && control == 2 ? used << 16 : 0);
    return ((interpolators ^ shader.shaderInterpolators) & mask) == 0;
}

bool DecodeNativeGameplayViewportAndScissor(std::span<const std::uint32_t> registers,
                                            NativeDrawReplayViewport&      viewport,
                                            NativeDrawReplayScissor&       scissor,
                                            std::string&                   error)
{
    if (registers.size() != kRegisterCount)
        return fail(error, "gameplay register snapshot is incomplete");
    return decodeViewportAndScissor(registers, viewport, scissor, error);
}

bool DecodeNativeOverlayDisplayRectangle(std::span<const std::uint32_t> registers,
                                         std::string_view               prefix,
                                         NativeGameplayDrawTarget&      target,
                                         std::string&                   error)
{
    if (registers.size() != kRegisterCount)
        return fail(error, "gameplay register snapshot is incomplete");
    const auto form = [&](std::string_view field, std::uint32_t index)
    {
        error = std::format("{}_{} {:#x}: {:#x}", prefix, field, index, registers[index]);
        return false;
    };
    // Reserved bits, the window offset disable bit, or an inverted scissor.
    const auto tl = registers[0x2081], br = registers[0x2082];
    if (tl & 0xC000C000U)
        return form("scissor_form", 0x2081);
    if (br & 0xC000C000U || packedUnsigned14(br, 0) < packedUnsigned14(tl, 0) || packedUnsigned14(br, 16) < packedUnsigned14(tl, 16))
        return form("scissor_form", 0x2082);
    target.width  = 1280;
    target.height = 720;
    if (!decodeViewportAndScissor(registers, target.viewport, target.scissor, error))
    {
        error = std::format("{}_viewport_form: {}", prefix, error);
        return false;
    }
    const auto& v = target.viewport;
    const auto& s = target.scissor;
    if (v.x != std::floor(v.x) || v.y != std::floor(v.y) || v.width != std::floor(v.width) || v.height != std::floor(v.height) || v.x < 0 || v.y < 0 ||
        v.x + v.width > float(target.width) || v.y + v.height > float(target.height))
    {
        error = std::format("{}_viewport_form: {},{} {}x{}", prefix, v.x, v.y, v.width, v.height);
        return false;
    }
    if (s.left == s.right || s.top == s.bottom)
        return form("scissor_empty", 0x2082);
    if (s.left < std::int32_t(v.x) || s.top < std::int32_t(v.y) || s.right > std::int32_t(v.x + v.width) || s.bottom > std::int32_t(v.y + v.height))
    {
        error = std::format("{}_scissor_beyond_viewport: scissor {},{},{},{} viewport {},{} {}x{}", prefix, s.left, s.top, s.right, s.bottom, v.x, v.y, v.width, v.height);
        return false;
    }
    return true;
}

bool BuildNativeGameplayDrawRecipe(const NativeGameplayDrawView&        view,
                                   const NativeGameplayShaderInterface& shader,
                                   const NativeGameplayDrawTarget&      target,
                                   NativeDrawReplayRecipe&              recipe,
                                   std::string&                         error,
                                   NativeGameplayShaderValidationCache* shaderValidation,
                                   NativeGameplayGeometryCache*         geometryCache)
{
    return buildRecipe(shaderValidation, geometryCache, view, shader, target, recipe, error) &&
           ValidateNativeDrawReplayRecipe(recipe, error);
}

bool BuildNativeGameplayDrawRecipe(const NativeGameplayDrawView&        view,
                                   const NativeGameplayShaderInterface& shader,
                                   NativeDrawReplayRecipe&              recipe,
                                   std::string&                         error)
{
    recipe = {};
    error.clear();
    if (view.registers.size() != kRegisterCount)
        return fail(error, "gameplay register snapshot is incomplete");
    const bool right = view.registers[0x2080] == 0x7D80U;
    if ((!right && view.registers[0x2080] != 0) ||
        view.registers[0x2081] != (right ? 640U : 0U) ||
        view.registers[0x2082] != (right ? 0x02D00500U : 0x02D00280U))
        return fail(error, "gameplay draw is outside the observed 640-pixel viewport halves");

    NativeGameplayDrawTarget target;
    target.viewport = { right ? -640.0F : 0.0F, 0.0F, 1280.0F, 720.0F, 0.0F, 1.0F };
    target.scissor  = { 0, 0, 640, 720 };
    return buildRecipe(nullptr, nullptr, view, shader, target, recipe, error) &&
           ValidateNativeDrawReplayRecipe(recipe, error);
}

bool BuildNativeGameplayDrawAdmission(const NativeGameplayDrawView&                     view,
                                      const NativeGameplayShaderInterface&              shader,
                                      const NativeGameplayDrawTarget&                   target,
                                      std::shared_ptr<const NativeDrawReplayAdmission>& admission,
                                      std::string&                                      error,
                                      NativeGameplayShaderValidationCache*              shaderValidation,
                                      NativeGameplayGeometryCache*                      geometryCache)
{
    admission.reset();
    NativeDrawReplayRecipe recipe;
    return buildRecipe(shaderValidation, geometryCache, view, shader, target, recipe, error) &&
           AdmitNativeDrawReplayRecipe(std::move(recipe), admission, error);
}

} // namespace rerevved::gpu
