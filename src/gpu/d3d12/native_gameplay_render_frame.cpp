#include "native_gameplay_render_frame.h"
#include "native_gameplay_clear.h"
#include "native_gameplay_frame.h"
#include "native_texture_upload.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_set>

#include <rex/graphics/xenos.h>

namespace rerevved::gpu
{

using rex::graphics::diagnostic::DrawCaptureExecutionStatus;
using rex::graphics::diagnostic::DrawCaptureFinalSwap;
using rex::graphics::diagnostic::DrawCaptureReason;

namespace
{

constexpr std::size_t   kRegisterCount         = 0x5003;
constexpr std::uint32_t kSurfaceMain           = 0x0A010280;
constexpr std::uint32_t kColorInfoRgba8        = 0;
constexpr std::uint32_t kCopyLeft              = 0x100340;
constexpr std::uint32_t kCopyRight             = 0x100040;
constexpr std::uint32_t kTiledHalfOffset       = 0x14000;
constexpr std::uint32_t kSceneWidth            = 640;
constexpr std::uint32_t kSceneHeight           = 720;
constexpr std::size_t   kMaxStaticTextureBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kCopyColorClearBit     = 1U << 8;
constexpr std::uint32_t kCopyDepthClearBit     = 1U << 9;
constexpr std::uint32_t kMainDepthBase         = 0x530;
constexpr std::uint64_t kNoEffectVertexHash    = 0xB6C9863F710683ECULL;
constexpr std::uint64_t kNoEffectPixelHash     = 0xA4A965C189287B99ULL;
constexpr std::uint32_t kRg16FloatFormat       = 31;
constexpr std::uint8_t  kRg16FloatSigns        = 0x55;
constexpr std::uint32_t kRg16FloatMapping      = 6984;
constexpr std::uint32_t kRg16FloatWidth        = 1280;
constexpr std::uint32_t kRg16FloatHeight       = 720;
constexpr std::uint32_t kRg16FloatPitch        = 40;
constexpr std::uint32_t kRg16FloatRawSwizzle   = 0xB48;
constexpr std::uint32_t kRg16FloatSurface      = 0x14000500;
constexpr std::uint32_t kRg16FloatColorInfoA   = 0x00040000;
constexpr std::uint32_t kRg16FloatColorInfoB   = 0x000402D0;
constexpr std::uint32_t kRg16FloatCopyControl  = 0x00100000;
constexpr std::uint32_t kRg16FloatDestInfo     = 0x0000EF82;

bool isPreparedCpuCube(const rex::graphics::diagnostic::DrawCaptureTexture&    source,
                       const rex::graphics::diagnostic::DrawCaptureTextureUse& use,
                       const NativeDrawReplayTexture&                          owner,
                       std::span<const std::uint32_t>                          registers)
{
    using namespace rex::graphics;
    using namespace rex::graphics::diagnostic;
    const bool bc3Size = source.guest_format == 20;
    const auto size    = bc3Size ? 128U : 64U;
    const auto pitch   = bc3Size ? 4U : 2U;
    const auto levels  = bc3Size ? 8U : 7U;
    if (use.dimension != 3 || use.fetch_constant >= 32 || registers.size() != kRegisterCount ||
        registers[0x4400] != 0x3FC00000 ||
        source.unavailable_reason == DrawCaptureReason::kTextureUnresolved ||
        !use.data_available || use.availability != DrawCaptureAvailability::kComplete ||
        use.unavailable_reason != DrawCaptureReason::kNone || source.guest_width != size || source.guest_height != size ||
        source.guest_depth_or_array_size != 6 || source.guest_pitch != pitch || source.guest_tiled || !source.guest_packed_mips ||
        source.mip_level != 0 || owner.frameTextureId || owner.width != size || owner.height != size || owner.mipLevels != levels ||
        owner.dimension != NativeDrawReplayTextureDimension::TextureCube || !owner.textureVersion)
        return false;
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, registers.data() + 0x4800 + use.fetch_constant * 6, sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture || fetch.dimension != xenos::DataDimension::kCube ||
        fetch.stacked || fetch.size_2d.stack_depth != 5 || fetch.size_2d.width != size - 1 || fetch.size_2d.height != size - 1 ||
        fetch.pitch != pitch || fetch.tiled || !fetch.packed_mips || fetch.mip_min_level != 0 || fetch.mip_max_level != levels - 1 ||
        fetch.exp_adjust || fetch.force_bc_w_to_max || !fetch.base_address || !fetch.mip_address ||
        fetch.base_address == fetch.mip_address || source.guest_base_page != fetch.base_address || source.guest_mip_page != fetch.mip_address)
        return false;
    const auto                          signs   = (fetch.dword_0 >> 2) & 255U;
    const bool                          rgba8   = source.guest_format == 6 && fetch.format == xenos::TextureFormat::k_8_8_8_8 &&
                                                  source.guest_endianness == 2 && fetch.endianness == xenos::Endian::k8in32 &&
                                                  fetch.swizzle == 0xA0A && !fetch.num_format && !signs && !use.is_signed &&
                                                  !use.swizzled_signs && use.component_mapping == 0x1A0A &&
                                                  owner.format == NativeDrawReplayTextureFormat::Rgba8 && owner.swizzle == std::array<std::uint8_t, 4>{ 2, 1, 0, 5 };
    const bool                          rgba16f = source.guest_format == 32 && fetch.format == xenos::TextureFormat::k_16_16_16_16_FLOAT &&
                                                  source.guest_endianness == 1 && fetch.endianness == xenos::Endian::k8in16 &&
                                                  fetch.swizzle == 0x688 && fetch.num_format == 1 && signs == 0x55 && use.is_signed &&
                                                  use.swizzled_signs == 0x55 && use.component_mapping == 0x1688 &&
                                                  owner.format == NativeDrawReplayTextureFormat::Rgba16Float && owner.swizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 };
    const bool                          bc3     = source.guest_format == 20 && fetch.format == xenos::TextureFormat::k_DXT4_5 &&
                                                  source.guest_endianness == 1 && fetch.endianness == xenos::Endian::k8in16 &&
                                                  fetch.swizzle == 0x688 && !fetch.num_format && !signs && !use.is_signed &&
                                                  !use.swizzled_signs && use.component_mapping == 0x1688 &&
                                                  owner.format == NativeDrawReplayTextureFormat::Bc3 && owner.swizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 };
    const NativeDrawReplayTextureLayout expected{ size, size, owner.format, levels, NativeDrawReplayTextureDimension::TextureCube };
    if ((!rgba8 && !rgba16f && !bc3) || owner.textureVersion->Layout() != expected ||
        owner.payloadBytes().size() != (bc3 ? 131232U : rgba8 ? 131064U
                                                              : 262128U) ||
        owner.textureVersion->Subresources().size() != 6 * levels)
        return false;
    std::size_t offset = 0;
    for (const auto& span : owner.textureVersion->Subresources())
    {
        const auto index  = &span - owner.textureVersion->Subresources().data();
        const auto extent = size >> (index % levels);
        const auto row    = bc3 ? std::max(1U, extent / 4U) * 16U : extent * (rgba8 ? 4U : 8U);
        const auto height = bc3 ? std::max(1U, extent / 4U) : extent;
        if (span.arraySlice != index / levels || span.mipLevel != index % levels || span.sourceOffset != offset ||
            span.rowBytes != row || span.rowCount != height)
            return false;
        offset += row * height;
    }
    return offset == owner.payloadBytes().size();
}

struct CopyWords
{
    std::uint32_t surface, colorInfo, sourceInfo, mode;
    std::uint32_t control, destinationBase, destinationPitch, destinationInfo;
    std::uint32_t destinationMask, depthClear, colorClearLow, colorClearHigh;

    static CopyWords Read(std::span<const std::uint32_t> registers)
    {
        return { registers[0x2000], registers[0x2001], registers[0x2205], registers[0x2208], registers[0x2318], registers[0x2319], registers[0x231A], registers[0x231B], registers[0x231C], registers[0x231D], registers[0x231E], registers[0x231F] };
    }

    friend bool operator==(const CopyWords&, const CopyWords&) = default;
};

struct NativeGameplayProducerProof
{
    std::uint64_t                    eventId          = 0;
    std::uint64_t                    frame            = 0;
    std::uint64_t                    submission       = 0;
    std::uint32_t                    destinationBase  = 0;
    std::uint32_t                    destinationPitch = 0;
    std::uint32_t                    destinationInfo  = 0;
    std::uint32_t                    sourceX          = 0;
    std::uint32_t                    sourceY          = 0;
    std::uint32_t                    sourceWidth      = 0;
    std::uint32_t                    sourceHeight     = 0;
    std::uint32_t                    destinationX     = 0;
    NativeGameplayGuestTextureLayout layout;
};

NativeGameplayGuestTextureLayout deriveGuestTextureLayout(const NativeGameplayRenderFrameCopy& copy,
                                                          std::span<const std::uint32_t>       registers)
{
    NativeGameplayGuestTextureLayout layout;
    if (registers.size() != kRegisterCount || copy.depth ||
        (copy.semantics != NativeGameplayRenderFrameCopy::Semantics::TiledMainColor &&
         copy.semantics != NativeGameplayRenderFrameCopy::Semantics::Explicit))
        return layout;

    const auto destinationBase  = registers[0x2319];
    const auto destinationPitch = registers[0x231A];
    const auto destinationInfo  = registers[0x231B];
    const auto source           = registers[0x2318] & 7U;
    const auto width            = destinationPitch & 0x3FFFU;
    const auto height           = (destinationPitch >> 16) & 0x3FFFU;
    const auto format           = (destinationInfo >> 7) & 0x3FU;
    if (!destinationBase || destinationBase & 0xFFFU || source != 0 ||
        (registers[0x2318] & (kCopyColorClearBit | kCopyDepthClearBit)) ||
        (destinationInfo & (1U << 3)) || (destinationInfo & 0x01000000U) || (format != 6 && format != kRg16FloatFormat) ||
        !width || !height || (width & 31U) ||
        (format == kRg16FloatFormat &&
         (registers[0x2000] != kRg16FloatSurface ||
          (registers[0x2001] != kRg16FloatColorInfoA && registers[0x2001] != kRg16FloatColorInfoB) ||
          registers[0x2318] != kRg16FloatCopyControl || destinationInfo != kRg16FloatDestInfo)))
        return layout;

    if (copy.semantics == NativeGameplayRenderFrameCopy::Semantics::Explicit &&
        (copy.sourceX || copy.sourceY || copy.destinationX || copy.sourceWidth != width || copy.sourceHeight != height))
        return layout;
    if (copy.semantics == NativeGameplayRenderFrameCopy::Semantics::TiledMainColor &&
        (!copy.tiledSceneBase || copy.sourceWidth != kSceneWidth || copy.sourceHeight != kSceneHeight || width != 2 * kSceneWidth || height != kSceneHeight))
        return layout;

    const auto base = copy.semantics == NativeGameplayRenderFrameCopy::Semantics::TiledMainColor ? copy.tiledSceneBase : destinationBase;
    if (!base || base & 0xFFFU)
        return {};
    layout.valid            = true;
    layout.basePage         = base / 0x1000U;
    layout.width            = width;
    layout.height           = height;
    layout.depthOrArraySize = 1;
    layout.pitch            = width / 32U;
    layout.format           = format;
    layout.endianness       = destinationInfo & 7U;
    // The SDK resolve address derivation uses the Xenos tiled helpers for every
    // supported non-array color resolve, including the normal Explicit form.
    layout.tiled = true;
    return layout;
}

struct ValidatedCopyState
{
    CopyWords                     words{};
    NativeRenderFramePass::Clear  clear;
    NativeDrawReplayTextureFormat destinationFormat = NativeDrawReplayTextureFormat::Rgba8;
    std::array<std::uint32_t, 2>  samples{};
    float                         valueScale = 1.0F;
    std::uint32_t                 source     = 0;
    bool                          valid = false, rgb10Color = false, mainColor = false, mainDepth = false;
    bool                          tiledSourceValid = false, colorClearValid = false, destinationValid = false, swapRedBlue = false;

    void Prepare(CopyWords changed)
    {
        *this             = {};
        words             = changed;
        source            = words.control & 7U;
        rgb10Color        = words.colorInfo == 0x00020000 && words.destinationInfo == 0x00000380;
        mainColor         = words.surface == kSurfaceMain &&
                            ((words.colorInfo == 0 && words.destinationInfo == 0x01000300) || rgb10Color) && source != 4;
        mainDepth         = words.surface == kSurfaceMain && words.colorInfo == 0 && source == 4 && words.destinationInfo == 0x00000302;
        tiledSourceValid  = words.sourceInfo == 0x10000 && words.mode == 6 && words.destinationMask == 0xFFF &&
                            words.depthClear == 0xFFFFFF00 && words.colorClearLow == 0 && words.colorClearHigh == 0 &&
                            words.destinationPitch == 0x02D00500;
        const auto format = (words.destinationInfo >> 7) & 0x3FU;
        destinationValid  = true;
        if (source == 4)
            destinationFormat = NativeDrawReplayTextureFormat::R32Float;
        else if (format == 6)
            destinationFormat = NativeDrawReplayTextureFormat::Rgba8;
        else if (format == 7 && source == 0)
            destinationFormat = NativeDrawReplayTextureFormat::Rgb10a2;
        else if (format == 31)
            destinationFormat = NativeDrawReplayTextureFormat::Rg16Float;
        else
            destinationValid = false;
        const auto msaa = (words.surface >> 16) & 3U;
        samples         = msaa ? std::array<std::uint32_t, 2>{ 0, 3 } : std::array<std::uint32_t, 2>{ 0, 0 };
        if (source == 4)
        {
            const auto selection = (words.control >> 4) & 7U;
            const auto sample    = msaa && (selection == 1 || selection == 3) ? 3U : 0U;
            samples              = { sample, sample };
        }
        swapRedBlue            = (words.destinationInfo & 0x01000000U) != 0;
        const auto colorFormat = (words.colorInfo >> 16) & 0xFU;
        valueScale             = colorFormat == 4 ? 32.0F : 1.0F;
        clear.color            = (words.control & kCopyColorClearBit) != 0;
        clear.depth = clear.stencil = (words.control & kCopyDepthClearBit) != 0;
        colorClearValid             = !clear.color || colorFormat == 0 || colorFormat == 2;
        const auto colorValue       = std::uint64_t(words.colorClearLow) | (std::uint64_t(words.colorClearHigh) << 32);
        if (clear.color && colorFormat == 0)
            for (std::size_t component = 0; component < 4; ++component)
                clear.colorValue[component] = static_cast<float>((colorValue >> (component * 8)) & 0xFFU) / 255.0F;
        else if (clear.color && colorFormat == 2)
        {
            for (std::size_t component = 0; component < 3; ++component)
                clear.colorValue[component] = static_cast<float>((colorValue >> (component * 10)) & 0x3FFU) / 1023.0F;
            clear.colorValue[3] = static_cast<float>((colorValue >> 30) & 3U) / 3.0F;
        }
        if (clear.depth)
        {
            clear.depthValue   = static_cast<float>((words.depthClear >> 8) & 0xFFFFFFU) / 16777215.0F;
            clear.stencilValue = static_cast<std::uint8_t>(words.depthClear & 0xFFU);
        }
        valid = true;
    }
};

bool fail(std::string& error, const char* message)
{
    error = message;
    return false;
}

bool failEvent(std::string& error, std::uint64_t eventId, const char* message)
{
    error = "gameplay event " + std::to_string(eventId) + ": " + message;
    return false;
}

bool prefixEvent(std::string& error, std::uint64_t eventId)
{
    error = "gameplay event " + std::to_string(eventId) + ": " +
            (error.empty() ? std::string("conversion failed") : error);
    return false;
}

struct AttachmentSignature
{
    std::uint32_t                width        = 0;
    std::uint32_t                height       = 0;
    std::uint32_t                sampleCount  = 0;
    std::uint32_t                sampleMask   = 0;
    NativeDrawReplayTargetFormat targetFormat = NativeDrawReplayTargetFormat::Rgba8;

    friend bool operator==(const AttachmentSignature&, const AttachmentSignature&) = default;
};

AttachmentSignature signature(const NativeDrawReplayRecipe& recipe)
{
    return { recipe.width, recipe.height, recipe.sampleCount, recipe.sampleMask, recipe.targetFormat };
}

AttachmentSignature signature(const NativeRenderFramePass::Target& target)
{
    return { target.width, target.height, target.sampleCount, target.sampleCount == 4 ? 9U : 1U, target.format };
}

bool checkTiledCopySource(std::span<const std::uint32_t> registers, std::string& error)
{
    for (const auto [index, expected] : { std::pair{ 0x2205U, 0x10000U }, { 0x2208U, 6U }, { 0x231CU, 0xFFFU }, { 0x231DU, 0xFFFFFF00U }, { 0x231EU, 0U }, { 0x231FU, 0U } })
        if (registers[index] != expected)
        {
            error = std::format("tiled copy register 0x{:04X} is 0x{:08X}, expected 0x{:08X}", index, registers[index], expected);
            return false;
        }
    return true;
}

bool isMainCopyState(const std::vector<std::uint32_t>& registers, std::string& error)
{
    if (registers.size() != kRegisterCount)
        return fail(error, "tiled gameplay copy requires the complete register snapshot");
    if (registers[0x2000] != kSurfaceMain ||
        (registers[0x2001] != kColorInfoRgba8 && registers[0x2001] != 0x00020000))
        return fail(error, "tiled gameplay copy has an unsupported surface or color format");
    if (!checkTiledCopySource(registers, error))
        return false;
    if (registers[0x231A] != 0x02D00500)
        return fail(error, "tiled gameplay copy has unsupported destination pitch");
    return true;
}

bool deriveTiledResolve(const NativeGameplayRenderFrameCopy& copy,
                        std::uint64_t                        tiledSceneBase,
                        NativeRenderFrameResolve&            resolve,
                        std::string&                         error)
{
    const auto anchor = copy.tiledSceneBase ? copy.tiledSceneBase : tiledSceneBase;
    if (!anchor || (anchor % 0x1000U) != 0)
        return fail(error, "tiled gameplay copy requires an aligned nonzero scene base");
    if (!isMainCopyState(copy.registers, error))
        return false;
    if (copy.sourceX != 0 || copy.sourceY != 0 || copy.sourceWidth != kSceneWidth ||
        copy.sourceHeight != kSceneHeight)
        return fail(error, "tiled gameplay copy source must be the captured 640x720 scene");
    if (!copy.destinationTextureId || copy.colorTarget != 0 || copy.depth || copy.valueScale != 1.0F)
        return fail(error, "tiled color copy has unsupported resolve options");
    const auto base = static_cast<std::uint64_t>(copy.registers[0x2319]);
    if (base < anchor)
        return fail(error, "tiled gameplay copy destination precedes the scene base");
    const auto delta = base - anchor;
    if (delta != 0 && delta != kTiledHalfOffset)
        return fail(error, "tiled gameplay copy destination is not a 0x14000 half");
    const bool rgb10           = copy.registers[0x2001] == 0x00020000;
    const auto expectedControl = delta == 0 && !rgb10 ? kCopyLeft : kCopyRight;
    const auto expectedFormat  = rgb10 ? 0x00000380U : 0x01000300U;
    if (copy.registers[0x2318] != expectedControl || copy.registers[0x231B] != expectedFormat)
        return fail(error, "tiled gameplay copy control does not match its destination half");
    if (copy.destinationX != delta / kTiledHalfOffset * kSceneWidth)
        return fail(error, "tiled gameplay copy destination X disagrees with its EDRAM base");

    resolve.textureId    = copy.destinationTextureId;
    resolve.depth        = false;
    resolve.colorTarget  = 0;
    resolve.sourceX      = copy.sourceX;
    resolve.sourceY      = copy.sourceY;
    resolve.width        = copy.sourceWidth;
    resolve.height       = copy.sourceHeight;
    resolve.destinationX = copy.destinationX;
    resolve.samples      = { 0, 3 };
    resolve.swapRedBlue  = (copy.registers[0x231B] & 0x01000000U) != 0;
    resolve.valueScale   = 1.0F;
    return true;
}

std::array<std::uint32_t, 2> depthCopySamples(std::span<const std::uint32_t> registers)
{
    // Xenos depth resolves select one sample even for an averaging selector.
    // In the supported 2X layout, guest sample 1 occupies host sample 3.
    const auto selection = (registers[0x2318] >> 4) & 7U;
    const auto msaa      = (registers[0x2000] >> 16) & 3U;
    const auto sample    = msaa && (selection == 1 || selection == 3) ? 3U : 0U;
    return { sample, sample };
}

bool deriveTiledDepthResolve(const NativeGameplayRenderFrameCopy& copy,
                             std::uint64_t                        tiledSceneBase,
                             NativeRenderFrameResolve&            resolve,
                             std::string&                         error)
{
    const auto anchor = copy.tiledSceneBase ? copy.tiledSceneBase : tiledSceneBase;
    if (!anchor || (anchor % 0x1000U) != 0)
        return fail(error, "tiled depth copy requires an aligned nonzero scene base");
    if (copy.registers.size() != kRegisterCount)
        return fail(error, "tiled depth copy requires the complete register snapshot");
    if (copy.registers[0x2000] != kSurfaceMain || copy.registers[0x2001] != kColorInfoRgba8)
        return fail(error, "tiled depth copy has an unsupported surface or color format");
    if (!checkTiledCopySource(copy.registers, error))
        return false;
    if (copy.registers[0x231A] != 0x02D00500 || copy.registers[0x231B] != 0x00000302)
        return fail(error, "tiled depth copy has unsupported destination format");
    if (copy.sourceX != 0 || copy.sourceY != 0 || copy.sourceWidth != kSceneWidth ||
        copy.sourceHeight != kSceneHeight)
        return fail(error, "tiled depth copy source must be the captured 640x720 scene");
    if (!copy.destinationTextureId || copy.colorTarget != 0 || !copy.depth || copy.swapRedBlue ||
        copy.valueScale != 1.0F)
        return fail(error, "tiled depth copy has unsupported resolve options");
    if ((copy.registers[0x2318] & 7U) != 4U)
        return fail(error, "tiled depth copy is missing the depth source selector");
    const auto base = static_cast<std::uint64_t>(copy.registers[0x2319]);
    if (base < anchor)
        return fail(error, "tiled depth copy destination precedes the scene base");
    const auto delta = base - anchor;
    if (delta != 0 && delta != kTiledHalfOffset)
        return fail(error, "tiled depth copy destination is not a 0x14000 half");
    const auto destinationX = static_cast<std::uint32_t>(delta / kTiledHalfOffset * kSceneWidth);
    if (copy.destinationX != destinationX)
        return fail(error, "tiled depth copy destination X disagrees with its EDRAM base");

    resolve.textureId    = copy.destinationTextureId;
    resolve.depth        = true;
    resolve.colorTarget  = 0;
    resolve.sourceX      = copy.sourceX;
    resolve.sourceY      = copy.sourceY;
    resolve.width        = copy.sourceWidth;
    resolve.height       = copy.sourceHeight;
    resolve.destinationX = copy.destinationX;
    resolve.samples      = depthCopySamples(copy.registers);
    resolve.swapRedBlue  = false;
    resolve.valueScale   = 1.0F;
    return true;
}

bool deriveExplicitResolve(const NativeGameplayRenderFrameCopy& copy,
                           NativeRenderFrameResolve&            resolve,
                           std::string&                         error)
{
    if (!copy.destinationTextureId || !copy.sourceWidth || !copy.sourceHeight)
        return fail(error, "explicit gameplay resolve requires source and destination geometry");
    if (copy.depth && (copy.colorTarget != 0 || copy.swapRedBlue || copy.valueScale != 1.0F))
        return fail(error, "explicit depth resolve requires color target zero and unit scale");
    resolve.textureId    = copy.destinationTextureId;
    resolve.depth        = copy.depth;
    resolve.colorTarget  = copy.colorTarget;
    resolve.sourceX      = copy.sourceX;
    resolve.sourceY      = copy.sourceY;
    resolve.width        = copy.sourceWidth;
    resolve.height       = copy.sourceHeight;
    resolve.destinationX = copy.destinationX;
    resolve.samples      = copy.samples;
    resolve.swapRedBlue  = copy.swapRedBlue;
    resolve.valueScale   = copy.valueScale;
    return true;
}

bool overwritesColor(const NativeGameplayRenderFrameDraw& draw, const NativeDrawReplayScissor& rectangle)
{
    const auto& r = draw.Recipe();
    // These vertex programs transform XYZ by c0-c3 and add the shared pixel
    // offset. Their pixel programs write RGBA without kill when alpha test is
    // disabled. Other programs need their own coverage proof.
    const bool knownPrograms = (r.vertexShaderHash == 0xDE0359DBD47D7FABULL &&
                                r.pixelShaderHash == 0x560C753DAD5555ADULL) ||
                               (r.vertexShaderHash == 0x7C1DD533708BB131ULL &&
                                r.pixelShaderHash == 0x818E4EAF9BA41B33ULL) ||
                               (r.vertexShaderHash == 0x38691F81BEA4AD28ULL &&
                                r.pixelShaderHash == 0xB8C1B55C7CE2E502ULL) ||
                               (r.vertexShaderHash == 0xF95D105E51CF1A19ULL &&
                                r.pixelShaderHash == 0x884C15F5AE654F89ULL);
    if (!knownPrograms || draw.alphaTestEnabled ||
        r.targetFormat != NativeDrawReplayTargetFormat::Rgb10a2 ||
        r.sampleCount != 4 || r.sampleMask != 9 || r.depth.enabled || r.stencil.enabled ||
        (r.rasterizer.cull && r.rasterizer.cull != (r.rasterizer.frontCounterClockwise ? 2U : 1U)) ||
        r.blend.alphaToCoverage || r.blend.logicOpEnabled ||
        r.blend.writeMask != 15 ||
        (r.blend.enabled && (r.blend.sourceColor != NativeDrawReplayBlendFactor::One ||
                             r.blend.destinationColor != NativeDrawReplayBlendFactor::Zero ||
                             r.blend.sourceAlpha != NativeDrawReplayBlendFactor::One ||
                             r.blend.destinationAlpha != NativeDrawReplayBlendFactor::Zero ||
                             r.blend.colorOp != NativeDrawReplayBlendOp::Add ||
                             r.blend.alphaOp != NativeDrawReplayBlendOp::Add)) ||
        rectangle.left < 0 || rectangle.top < 0 || rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top ||
        rectangle.right > static_cast<std::int32_t>(r.width) || rectangle.bottom > static_cast<std::int32_t>(r.height) ||
        r.scissor.left > rectangle.left || r.scissor.top > rectangle.top ||
        r.scissor.right < rectangle.right || r.scissor.bottom < rectangle.bottom ||
        r.vertexAttributeCount != 2 || r.vertexStrideBytes != 32 || r.vertexBytes().size() != 128 ||
        r.indexCount != 6 || !std::ranges::equal(r.indexSpan(), std::array<std::uint32_t, 6>{ 0, 1, 2, 0, 2, 3 }) ||
        r.vertexConstants.size() < 64 || r.sharedConstants.size() < 336 ||
        r.sharedConstants.size() > kNativeGameplayUserClipPlaneOffset)
        return false; // A user clip plane could cut the rectangle.
    const auto scalar = [](std::span<const std::uint8_t> bytes, std::size_t offset)
    {
        float value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    // Nonzero semantic swap masks would change the decoded XYZ inputs.
    for (std::size_t byte = 288; byte < 292; ++byte)
        if (r.sharedConstants[byte])
            return false;
    std::array<std::array<float, 2>, 4> corners;
    for (std::size_t vertex = 0; vertex < 4; ++vertex)
    {
        std::array<float, 4> position{ scalar(r.vertexBytes(), vertex * 32),
                                       scalar(r.vertexBytes(), vertex * 32 + 4),
                                       scalar(r.vertexBytes(), vertex * 32 + 8),
                                       1.0F };
        std::array<float, 4> clip{};
        for (std::size_t row = 0; row < 4; ++row)
            for (std::size_t column = 0; column < 4; ++column)
                clip[row] += position[column] * scalar(r.vertexConstants, row * 16 + column * 4);
        if (std::any_of(clip.begin(), clip.end(), [](float value)
                        {
                            return !std::isfinite(value);
                        }) ||
            clip[3] <= 0 || clip[2] < 0 || clip[2] > clip[3])
            return false;
        corners[vertex] = {
            r.viewport.x + (clip[0] / clip[3] + scalar(r.sharedConstants, 292) + 1.0F) * r.viewport.width * 0.5F,
            r.viewport.y + (1.0F - clip[1] / clip[3] - scalar(r.sharedConstants, 296)) * r.viewport.height * 0.5F
        };
    }
    const auto& a = corners[0];
    const auto& b = corners[1];
    const auto& c = corners[2];
    const auto& d = corners[3];
    // Standard 4X samples 0/3 lie at least 1/8 pixel inside an edge.
    // Requiring the rectangle to enclose that inset also tolerates the
    // projection's float rounding at nominal integer viewport boundaries.
    return a[0] == b[0] && c[0] == d[0] && a[1] == d[1] && b[1] == c[1] &&
           a[0] < static_cast<float>(rectangle.left) + 0.125F && a[1] < static_cast<float>(rectangle.top) + 0.125F &&
           c[0] > static_cast<float>(rectangle.right) - 0.125F &&
           c[1] > static_cast<float>(rectangle.bottom) - 0.125F;
}

bool discardsUnusedColor(const NativeGameplayRenderFrameInput& input, std::size_t index)
{
    const auto& pass = input.passes[index];
    if (pass.draws.empty())
        return false;
    const auto&                   first  = pass.draws.front();
    const auto                    target = signature(first.Recipe());
    const NativeDrawReplayScissor full{ 0, 0, static_cast<std::int32_t>(target.width), static_cast<std::int32_t>(target.height) };
    if (overwritesColor(first, full))
        return true;
    const auto& written = first.Recipe().scissor;
    if (!overwritesColor(first, written))
        return false;
    // A partial overwrite is sufficient only while every color use stays in
    // its initialized region, up to a full replacement of the same color view.
    const auto firstIndex = index;
    for (; index < input.passes.size(); ++index)
    {
        const auto& candidate       = input.passes[index];
        const auto  candidateTarget = candidate.draws.empty() ? signature(candidate.target)
                                                              : signature(candidate.draws.front().Recipe());
        if ((!candidate.attachmentAliasValid && !candidate.attachmentAlias) ||
            (candidate.attachmentAlias & 0xFFFU) != (pass.attachmentAlias & 0xFFFU) || candidateTarget != target)
            return false;
        for (const auto& clear : candidate.clears)
            if (clear.color && (clear.colorTargets & 1U) && clear.rectangle.left == 0 && clear.rectangle.top == 0 &&
                clear.rectangle.right == full.right && clear.rectangle.bottom == full.bottom)
                return true;
        if (index != firstIndex)
        {
            if (!candidate.draws.empty() && overwritesColor(candidate.draws.front(), full))
                return true;
            if (!candidate.preservePreviousAttachments &&
                (candidate.draws.empty() || !overwritesColor(candidate.draws.front(), written)))
                return false;
        }
        for (const auto& draw : candidate.draws)
            if (draw.Recipe().scissor.left < written.left || draw.Recipe().scissor.top < written.top ||
                draw.Recipe().scissor.right > written.right || draw.Recipe().scissor.bottom > written.bottom)
                return false;
        for (const auto& copy : candidate.copies)
            if (!copy.depth)
            {
                NativeRenderFrameResolve resolve;
                std::string              error;
                if (!DeriveNativeGameplayResolve(copy, input.tiledSceneBase, resolve, error) ||
                    resolve.colorTarget != 0 || resolve.sourceX < static_cast<std::uint32_t>(written.left) ||
                    resolve.sourceY < static_cast<std::uint32_t>(written.top) ||
                    std::uint64_t(resolve.sourceX) + resolve.width > static_cast<std::uint32_t>(written.right) ||
                    std::uint64_t(resolve.sourceY) + resolve.height > static_cast<std::uint32_t>(written.bottom))
                    return false;
            }
    }
    return true;
}

bool discardsUnusedAttachments(const NativeGameplayRenderFrameInput& input, std::size_t index)
{
    if (!discardsUnusedColor(input, index))
        return false;
    // Color writes do not initialize depth. Every later depth/stencil use must
    // follow a full explicit clear in its own preserved attachment chain.
    bool                depthInitialized = false;
    std::uint64_t       previousAlias    = 0;
    AttachmentSignature previousTarget;
    for (; index < input.passes.size(); ++index)
    {
        const auto& pass   = input.passes[index];
        const auto  target = pass.draws.empty() ? signature(pass.target) : signature(pass.draws.front().Recipe());
        if (!pass.preservePreviousAttachments || (!pass.attachmentAliasValid && !pass.attachmentAlias) ||
            pass.attachmentAlias != previousAlias || target != previousTarget)
            depthInitialized = false;
        for (const auto& clear : pass.clears)
            if (clear.depth && clear.stencil && clear.rectangle.left == 0 && clear.rectangle.top == 0 &&
                clear.rectangle.right == static_cast<std::int32_t>(target.width) &&
                clear.rectangle.bottom == static_cast<std::int32_t>(target.height))
                depthInitialized = true;
        if (!depthInitialized &&
            (std::any_of(pass.draws.begin(), pass.draws.end(), [](const auto& draw)
                         {
                             return draw.Recipe().depth.enabled || draw.Recipe().stencil.enabled;
                         }) ||
             std::any_of(pass.copies.begin(), pass.copies.end(), [](const auto& copy)
                         {
                             return copy.depth;
                         })))
            return false;
        previousAlias  = pass.attachmentAlias;
        previousTarget = target;
    }
    return true;
}

} // namespace

NativeGameplayResolveAdmission::NativeGameplayResolveAdmission(const NativeGameplayRenderFrameCopy& copy,
                                                               NativeRenderFrameResolve             resolve,
                                                               NativeRenderFramePass::Clear         clear,
                                                               std::array<std::uint8_t, 24>         rectangle,
                                                               std::uint64_t                        geometryEvent,
                                                               std::uint32_t                        geometryAddress)
: copy_(copy)
, resolve_(resolve)
, clear_(clear)
, rectangle_(rectangle)
, geometryEvent_(geometryEvent)
, geometryAddress_(geometryAddress)
{
}

bool NativeGameplayResolveAdmission::Validate(const NativeGameplayRenderFrameCopy& copy, std::string& error) const
{
    const auto  canonical = owner_.lock();
    const auto& owner     = copy.admittedOperation;
    if (!owner || canonical.get() != this || owner.get() != this || canonical.owner_before(owner) || owner.owner_before(canonical))
        return fail(error, "gameplay resolve admission has no canonical wrapper owner");
    if (!copy.registers.empty())
        return fail(error, "gameplay resolve mixes raw registers and an admitted operation");
    if (std::tie(copy.eventId, copy.triggerEventId, copy.sourceGeometryOwnedByCopy, copy.semantics, copy.sourceX, copy.sourceY, copy.sourceWidth, copy.sourceHeight, copy.destinationX, copy.destinationTextureId, copy.depth, copy.colorTarget, copy.samples, copy.swapRedBlue, copy.valueScale, copy.tiledSceneBase, copy.producerLayout) !=
        std::tie(copy_.eventId, copy_.triggerEventId, copy_.sourceGeometryOwnedByCopy, copy_.semantics, copy_.sourceX, copy_.sourceY, copy_.sourceWidth, copy_.sourceHeight, copy_.destinationX, copy_.destinationTextureId, copy_.depth, copy_.colorTarget, copy_.samples, copy_.swapRedBlue, copy_.valueScale, copy_.tiledSceneBase, copy_.producerLayout))
        return fail(error, "gameplay resolve fields disagree with the admitted operation");
    return true;
}

bool DeriveNativeGameplayResolve(const NativeGameplayRenderFrameCopy& copy,
                                 std::uint64_t                        tiledSceneBase,
                                 NativeRenderFrameResolve&            resolve,
                                 std::string&                         error)
{
    error.clear();
    if (copy.admittedOperation)
    {
        if (!copy.admittedOperation->Validate(copy, error))
            return false;
        resolve = copy.admittedOperation->Resolve();
        return true;
    }
    switch (copy.semantics)
    {
        case NativeGameplayRenderFrameCopy::Semantics::TiledMainColor:
            return deriveTiledResolve(copy, tiledSceneBase, resolve, error);
        case NativeGameplayRenderFrameCopy::Semantics::TiledMainDepth:
            return deriveTiledDepthResolve(copy, tiledSceneBase, resolve, error);
        case NativeGameplayRenderFrameCopy::Semantics::Explicit:
            return deriveExplicitResolve(copy, resolve, error);
    }
    return fail(error, "unknown gameplay resolve semantics");
}

namespace
{

template <typename Input>
bool buildGameplayRenderFrame(Input& input, NativeRenderFrame& frame, std::string& error)
{
    error.clear();
    frame = {};
    if (input.resources.empty() || input.passes.empty() || !input.outputTextureId)
        return fail(error, "complete gameplay frame requires resources, passes and an output resource");
    for (const auto& pass : input.passes)
    {
        bool raw = false, admitted = false;
        for (const auto& draw : pass.draws)
        {
            raw |= !draw.admittedRecipe;
            admitted |= bool(draw.admittedRecipe);
            if (draw.admittedRecipe && !draw.admittedRecipe->ValidateSharedPayloads(error))
                return false;
        }
        if (raw && admitted)
            return fail(error, "gameplay frame pass mixes raw and admitted draw storage");
        for (const auto& copy : pass.copies)
            if (copy.admittedOperation && !copy.admittedOperation->Validate(copy, error))
                return prefixEvent(error, copy.eventId);
    }

    std::map<std::uint64_t, NativeRenderFrameTexture> resources;
    for (const auto& resource : input.resources)
    {
        if (!resource.generated)
            return fail(error, "gameplay frame resources must be native-generated resolve destinations");
        if (!resource.texture.id || !resources.emplace(resource.texture.id, resource.texture).second)
            return fail(error, "gameplay frame resource identities must be unique and nonzero");
    }
    if (resources.find(input.outputTextureId) == resources.end())
        return fail(error, "gameplay frame output resource is not declared");

    frame.outputTextureId      = input.outputTextureId;
    frame.outputSwizzle        = input.outputSwizzle;
    frame.gammaTable           = input.gammaTable;
    frame.outputPath           = input.outputPath;
    frame.returnResolvedOutput = input.returnResolvedOutput;
    frame.retainOnly           = input.retainOnly;
    for (const auto& [id, resource] : resources)
        frame.textures.push_back(resource);

    std::map<std::uint64_t, AttachmentSignature> aliases;
    std::map<std::uint64_t, std::uint32_t>       producedResolveCount;
    std::unordered_set<std::uint64_t>            seenEvents;
    std::uint64_t                                previousEvent            = 0;
    bool                                         previousWasPostCopyClear = false;

    for (const auto& passInput : input.passes)
    {
        if (!passInput.firstEvent || !passInput.lastEvent || passInput.firstEvent > passInput.lastEvent ||
            (passInput.draws.empty() && passInput.copies.empty() && passInput.clears.empty()))
            return fail(error, "gameplay frame pass requires a bounded event range and native work");
        if ((!passInput.draws.empty() || !passInput.copies.empty()) && passInput.operations.empty())
            return fail(error, "gameplay frame draw/copy pass requires ordered operations");
        const bool postCopyBoundary = passInput.postCopyClear && passInput.draws.empty() &&
                                      passInput.copies.empty() && !passInput.clears.empty() &&
                                      passInput.firstEvent == passInput.lastEvent &&
                                      passInput.firstEvent == previousEvent;
        if ((passInput.postCopyClear && !postCopyBoundary) || passInput.lastEvent < passInput.firstEvent ||
            passInput.firstEvent < previousEvent ||
            (passInput.firstEvent == previousEvent && (!postCopyBoundary || previousWasPostCopyClear)))
            return fail(error, "gameplay frame event ranges are out of order");

        const auto passSignature = passInput.draws.empty() ? signature(passInput.target)
                                                           : signature(passInput.draws.front().Recipe());
        if (!passSignature.width || !passSignature.height || !passSignature.sampleCount)
            return fail(error, "gameplay frame pass has no canonical target");
        const bool    attachmentAliasValid = passInput.attachmentAliasValid || passInput.attachmentAlias != 0;
        std::uint32_t colorSourcePass      = 0;
        if (attachmentAliasValid)
        {
            const auto foundAlias = aliases.find(passInput.attachmentAlias);
            if (foundAlias == aliases.end())
                aliases.emplace(passInput.attachmentAlias, passSignature);
            else if (!(foundAlias->second == passSignature) && !passInput.clearAliasTransition &&
                     !discardsUnusedAttachments(input, frame.passes.size()))
            {
                // A single-sample color view can reinterpret the last writer's
                // 32-bit words. Walk physical tile ranges, not stale combined
                // color/depth identities; intervening overlaps invalidate it.
                if (passSignature.sampleCount != 1 || passSignature.targetFormat != NativeDrawReplayTargetFormat::Rgba8 ||
                    passInput.preservePreviousAttachments ||
                    std::any_of(passInput.draws.begin(), passInput.draws.end(), [](const auto& draw)
                                {
                                    return draw.Recipe().depth.enabled || draw.Recipe().stencil.enabled;
                                }))
                    return failEvent(error, passInput.firstEvent, "EDRAM attachment alias changes require an explicit clear transition");
                const auto tileCount = [](const NativeRenderFramePass::Target& target)
                {
                    // Native samples 0/3 represent guest 2X, whose EDRAM
                    // sample rows are doubled without doubling its pitch.
                    const auto     samples    = target.sampleCount == 4 ? rex::graphics::xenos::MsaaSamples::k2X
                                                                        : rex::graphics::xenos::MsaaSamples::k1X;
                    const auto     pitch      = rex::graphics::xenos::GetSurfacePitchTiles(target.width, samples, false);
                    const auto     rows       = target.height * (target.sampleCount == 4 ? 2U : 1U);
                    constexpr auto tileHeight = rex::graphics::xenos::kEdramTileHeightSamples;
                    return std::uint64_t(pitch) * ((rows + tileHeight - 1) / tileHeight);
                };
                const auto begin = passInput.attachmentAlias & 0xFFFU;
                const auto end   = begin + tileCount({ passSignature.width, passSignature.height, passSignature.sampleCount, passSignature.targetFormat });
                if (end > rex::graphics::xenos::kEdramTileCount)
                    return failEvent(error, passInput.firstEvent, "wrapped EDRAM color aliases are unsupported");
                for (std::size_t index = frame.passes.size(); index-- > 0;)
                {
                    const auto& candidate = input.passes[index];
                    if (!candidate.attachmentAliasValid && !candidate.attachmentAlias)
                        break;
                    const auto target   = GetNativeRenderFrameTarget(frame.passes[index]);
                    const auto tiles    = tileCount(target);
                    const auto overlaps = [&](std::uint64_t base)
                    {
                        return base < end && begin < base + tiles;
                    };
                    const bool writesDepth = std::any_of(candidate.clears.begin(), candidate.clears.end(), [](const auto& clear)
                                                         {
                                                             return clear.depth || clear.stencil;
                                                         }) ||
                                             std::any_of(candidate.draws.begin(), candidate.draws.end(), [](const auto& draw)
                                                         {
                                                             return draw.Recipe().depth.writeEnabled || draw.Recipe().stencil.enabled;
                                                         });
                    if (writesDepth && overlaps((candidate.attachmentAlias >> 12) & 0xFFFU))
                        break;
                    const auto sourceBase = candidate.attachmentAlias & 0xFFFU;
                    if (sourceBase + tiles > rex::graphics::xenos::kEdramTileCount)
                        break;
                    if (!overlaps(sourceBase))
                        continue;
                    if (sourceBase == begin && target.sampleCount == 1 && target.format == NativeDrawReplayTargetFormat::Rg16Snorm &&
                        target.width == passSignature.width &&
                        target.height == passSignature.height)
                        colorSourcePass = static_cast<std::uint32_t>(index + 1);
                    break;
                }
                if (!colorSourcePass)
                    return failEvent(error, passInput.firstEvent, "EDRAM attachment alias has no compatible native color writer");
                foundAlias->second = passSignature;
            }
            else if (!(foundAlias->second == passSignature))
                foundAlias->second = passSignature;
        }

        NativeRenderFramePass pass;
        pass.preservePreviousAttachments = passInput.preservePreviousAttachments;
        pass.target                      = passInput.target;
        if (!pass.target.width && !passInput.draws.empty())
            pass.target = { passSignature.width, passSignature.height, passSignature.sampleCount, passSignature.targetFormat };
        pass.clears                          = passInput.clears;
        pass.colorSourcePass                 = colorSourcePass;
        std::size_t   expectedDraw           = 0;
        std::size_t   expectedCopy           = 0;
        bool          sawCopy                = false;
        std::uint64_t previousOperationEvent = 0;
        for (const auto& operation : passInput.operations)
        {
            if (!operation.eventId || operation.eventId < passInput.firstEvent ||
                operation.eventId > passInput.lastEvent ||
                !seenEvents.insert(operation.eventId).second ||
                (previousOperationEvent && operation.eventId <= previousOperationEvent))
                return fail(error, "gameplay frame operations must have unique ordered event identities");
            previousOperationEvent = operation.eventId;
            if (operation.kind == NativeGameplayRenderFrameOperation::Kind::Draw)
            {
                if (sawCopy)
                    return fail(error, "gameplay draw-after-copy ordering requires a separate pass");
                if (operation.index != expectedDraw || operation.index >= passInput.draws.size() ||
                    passInput.draws[operation.index].eventId != operation.eventId)
                    return fail(error, "gameplay frame draw operations are not in capture order");
                ++expectedDraw;
            }
            else if (operation.kind == NativeGameplayRenderFrameOperation::Kind::Copy)
            {
                if (operation.index != expectedCopy || operation.index >= passInput.copies.size() ||
                    passInput.copies[operation.index].eventId != operation.eventId)
                    return fail(error, "gameplay frame copy operations are not in capture order");
                ++expectedCopy;
                sawCopy          = true;
                const auto& copy = passInput.copies[operation.index];
                if ((!copy.sourceGeometryOwnedByCopy &&
                     (!copy.triggerEventId || copy.triggerEventId >= copy.eventId)) ||
                    (copy.sourceGeometryOwnedByCopy && !copy.eventId))
                    return fail(error, "gameplay frame copy is missing a preceding trigger event");
                NativeRenderFrameResolve resolve;
                if (!DeriveNativeGameplayResolve(copy, input.tiledSceneBase, resolve, error))
                    return prefixEvent(error, copy.eventId);
                pass.resolves.push_back(resolve);
                ++producedResolveCount[resolve.textureId];
            }
            else
                return fail(error, "unknown gameplay frame operation kind");
        }
        if (expectedDraw != passInput.draws.size() || expectedCopy != passInput.copies.size())
            return fail(error, "gameplay frame operations do not cover every draw and copy record");

        previousEvent            = passInput.lastEvent;
        previousWasPostCopyClear = postCopyBoundary;
        frame.passes.push_back(std::move(pass));
    }

    for (const auto& resource : input.resources)
        if (!resource.texture.imported && !producedResolveCount.contains(resource.texture.id))
            return fail(error, "declared gameplay frame resource has no native producer");

    // Alias validation above still inspects earlier draws. Transfer payloads
    // only after those checks; const callers retain their owned input buffers.
    for (std::size_t index = 0; index < input.passes.size(); ++index)
    {
        auto& pass = frame.passes[index];
        if (!input.passes[index].draws.empty() && input.passes[index].draws.front().admittedRecipe)
            pass.admittedDraws.reserve(input.passes[index].draws.size());
        else
            pass.draws.reserve(input.passes[index].draws.size());
        for (auto& draw : input.passes[index].draws)
            if (draw.admittedRecipe)
                pass.admittedDraws.push_back(std::move(draw.admittedRecipe));
            else
                pass.draws.push_back(std::move(draw.recipe));
    }

    if (!BatchNativeRenderFrame(frame, error) || !ValidateNativeRenderFrame(frame, error))
        return false;
    return true;
}

} // namespace

bool BuildNativeGameplayRenderFrame(const NativeGameplayRenderFrameInput& input,
                                    NativeRenderFrame&                    frame,
                                    std::string&                          error)
{
    return buildGameplayRenderFrame(input, frame, error);
}

bool BuildNativeGameplayRenderFrame(NativeGameplayRenderFrameInput&& input,
                                    NativeRenderFrame&               frame,
                                    std::string&                     error)
{
    return buildGameplayRenderFrame(input, frame, error);
}

namespace
{

using rex::graphics::diagnostic::DrawCaptureAvailability;
using rex::graphics::diagnostic::DrawCaptureFrameEvent;
using rex::graphics::diagnostic::DrawCaptureTexture;
using rex::graphics::diagnostic::DrawCaptureTextureUse;

std::int32_t snapshotSigned15(std::uint32_t value, unsigned shift)
{
    const auto packed = (value >> shift) & 0x7FFFU;
    const auto sign   = 1U << 14;
    return static_cast<std::int32_t>((packed ^ sign) - sign);
}

std::uint32_t snapshotUnsigned14(std::uint32_t value, unsigned shift)
{
    return (value >> shift) & 0x3FFFU;
}

bool decodeSnapshotTarget(std::span<const std::uint32_t> registers,
                          NativeGameplayDrawTarget&      target,
                          std::uint32_t&                 colorFormat,
                          std::int32_t&                  colorExpBias,
                          std::string&                   error)
{
    if (registers.size() != kRegisterCount)
        return fail(error, "gameplay draw register snapshot is incomplete");
    const auto msaa = (registers[0x2000] >> 16) & 3U;
    if (msaa > 1)
        return fail(error, "unsupported gameplay surface sample state");
    target.sampleCount = msaa ? 4 : 1;
    target.sampleMask  = msaa ? 9 : 1;
    target.width       = registers[0x2000] & 0x3FFFU;
    if (!target.width)
        return fail(error, "gameplay surface has no render width");

    colorFormat            = (registers[0x2001] >> 16) & 0xFU;
    const auto encodedBias = (registers[0x2001] >> 20) & 0x3FU;
    colorExpBias           = static_cast<std::int32_t>((encodedBias ^ 0x20U) - 0x20U);
    switch (colorFormat)
    {
        case 0:
            target.format = NativeDrawReplayTargetFormat::Rgba8;
            break;
        case 2:
            target.format = NativeDrawReplayTargetFormat::Rgb10a2;
            break;
        case 4:
            target.format = NativeDrawReplayTargetFormat::Rg16Snorm;
            break;
        default:
            return fail(error, "unsupported gameplay color target format");
    }

    const float xScale  = std::bit_cast<float>(registers[0x210F]);
    const float xOffset = std::bit_cast<float>(registers[0x2110]);
    const float yScale  = std::bit_cast<float>(registers[0x2111]);
    const float yOffset = std::bit_cast<float>(registers[0x2112]);
    const float zScale  = std::bit_cast<float>(registers[0x2113]);
    const float zOffset = std::bit_cast<float>(registers[0x2114]);
    if (!std::isfinite(xScale) || !std::isfinite(xOffset) || !std::isfinite(yScale) ||
        !std::isfinite(yOffset) || !std::isfinite(zScale) || !std::isfinite(zOffset) ||
        !std::isfinite(zOffset + zScale) || xScale <= 0.0F || yScale >= 0.0F || zScale < 0.0F ||
        zOffset < 0.0F || zOffset + zScale > 1.0F)
        return fail(error, "unsupported gameplay viewport state");
    const auto windowX        = snapshotSigned15(registers[0x2080], 0);
    const auto windowY        = snapshotSigned15(registers[0x2080], 16);
    target.viewport           = { xOffset - xScale + static_cast<float>(windowX),
                                  yOffset + yScale + static_cast<float>(windowY),
                                  xScale * 2.0F,
                                  -yScale * 2.0F,
                                  zOffset,
                                  zOffset + zScale };
    const bool offsetDisabled = (registers[0x2081] >> 31) != 0;
    const auto offsetX        = offsetDisabled ? 0 : windowX;
    const auto offsetY        = offsetDisabled ? 0 : windowY;
    target.scissor            = {
        static_cast<std::int32_t>(snapshotUnsigned14(registers[0x2081], 0)) + offsetX,
        static_cast<std::int32_t>(snapshotUnsigned14(registers[0x2081], 16)) + offsetY,
        static_cast<std::int32_t>(snapshotUnsigned14(registers[0x2082], 0)) + offsetX,
        static_cast<std::int32_t>(snapshotUnsigned14(registers[0x2082], 16)) + offsetY,
    };
    if (target.scissor.right < target.scissor.left || target.scissor.bottom < target.scissor.top)
        return fail(error, "gameplay scissor is outside the render target");
    // An empty HUD window stays empty after a signed window offset moves it
    // outside the attachment. Keep the draw inputs and use an in-bounds empty rect.
    if (target.scissor.left == target.scissor.right || target.scissor.top == target.scissor.bottom)
        target.scissor = {};
    if (target.scissor.left < 0 || target.scissor.top < 0 ||
        static_cast<std::uint32_t>(target.scissor.right) > target.width)
        return fail(error, "gameplay scissor is outside the render target");
    // RB_SURFACE_INFO carries the color pitch but no color height. These are
    // the observed capture extents; the per-draw window scissor is allowed to
    // be a smaller viewport inside the attachment.
    switch (registers[0x2000])
    {
        case 0x020000A0:
        case 0x04000140:
            // The short strips resolve 128 or 256 pixels from padded 160- or
            // 320-pixel pitches, matching the corresponding clear attachments.
            target.height = 32;
            break;
        case 0x0A010280:
        case 0x14000500:
            target.height = 720;
            break;
        case 0x10000410:
            target.height = 1024;
            break;
        case 0x08000230:
            // The terrain-map pass resolves 512 rows from its 560-pixel pitch.
            target.height = 512;
            break;
        case 0x05010140:
            // The 320x180 viewport resolves an 184-row padded rectangle.
            target.height = 184;
            break;
        default:
            return fail(error, "gameplay surface extent has no measured height mapping");
    }
    if (static_cast<std::uint32_t>(target.scissor.bottom) > target.height)
        return fail(error, "gameplay scissor exceeds the measured attachment height");
    return true;
}

const NativeGameplayShaderInterface* findSnapshotShader(
    const DrawCaptureFrameEvent&                   event,
    std::span<const std::uint32_t>                 registers,
    std::span<const NativeGameplayShaderInterface> shaders,
    std::uint32_t                                  colorFormat,
    std::int32_t                                   colorExpBias,
    std::uint32_t                                  sampleCount,
    std::uint32_t                                  sampleMask)
{
    const auto& draw         = event.draw;
    const bool  alphaEnabled = (registers[0x2202] & 8U) != 0;
    const auto  alphaCompare = registers[0x2202] & 7U;
    for (const auto& shader : shaders)
        if (shader.vertexShaderHash == draw.guest_vertex_shader_hash &&
            shader.pixelShaderHash == draw.guest_pixel_shader_hash &&
            shader.shaderProgram == registers[0x2180] && shader.shaderContext == registers[0x2181] &&
            NativeGameplayInterpolatorsMatch(shader, registers[0x2182]) &&
            shader.alphaTestEnabled == alphaEnabled &&
            (!alphaEnabled || shader.alphaTestCompare == alphaCompare) &&
            shader.colorTargetFormat == colorFormat && shader.colorExpBias == colorExpBias &&
            shader.sampleCount == sampleCount && shader.sampleMask == sampleMask)
            return &shader;
    return nullptr;
}

bool validateTextureUse(const DrawCaptureTexture&    source,
                        const DrawCaptureTextureUse& use,
                        std::string&                 error)
{
    if (!use.data_available || use.availability != DrawCaptureAvailability::kComplete ||
        source.availability != DrawCaptureAvailability::kComplete || source.scaled_resolve ||
        source.mip_level != 0 || source.host_width == 0 || source.host_height == 0 ||
        source.host_width > 4096 || source.host_height > 4096 ||
        (use.dimension != 1 && use.dimension != 3) ||
        source.host_depth_or_array_size != (use.dimension == 3 ? 6U : 1U))
        return fail(error, "gameplay texture input is incomplete or unsupported");
    if (use.is_signed && source.host_format != 9 && source.host_format != 10 &&
        source.host_format != 33 && source.host_format != 34 && source.host_format != 39 &&
        source.host_format != 41)
        return fail(error, "signed gameplay texture interpretation is unsupported");
    if (use.dimension != 1 && use.dimension != 3)
        return fail(error, "gameplay texture dimension is unsupported");
    if ((use.component_mapping & ~0x1FFFU) || !(use.component_mapping & 0x1000U))
        return fail(error, "gameplay texture component mapping is invalid");
    for (std::size_t component = 0; component < 4; ++component)
    {
        if (((use.swizzled_signs >> (component * 2)) & 3U) > 1U ||
            ((use.component_mapping >> (component * 3)) & 7U) > 5U)
            return fail(error, "gameplay texture swizzle or sign interpretation is unsupported");
    }
    return true;
}

bool isSelectedNativeRg16Float(const DrawCaptureTexture&       source,
                               const DrawCaptureTextureUse&    use,
                               const NativeRenderFrameTexture& resource) noexcept
{
    return resource.format == NativeDrawReplayTextureFormat::Rg16Float && source.guest_format == kRg16FloatFormat &&
           source.guest_width == kRg16FloatWidth && source.guest_height == kRg16FloatHeight && source.guest_pitch == kRg16FloatPitch &&
           source.guest_tiled && !source.guest_packed_mips && source.guest_depth_or_array_size == 1 && source.mip_level == 0 &&
           source.host_width == kRg16FloatWidth && source.host_height == kRg16FloatHeight &&
           source.host_depth_or_array_size == 1 && source.host_mip_levels == 1 && source.host_format == 33 && use.dimension == 1 &&
           use.is_signed && use.swizzled_signs == kRg16FloatSigns && use.component_mapping == kRg16FloatMapping;
}

bool decodeSnapshotTexture(const DrawCaptureTexture&    source,
                           const DrawCaptureTextureUse& use,
                           NativeDrawReplayTexture&     texture,
                           std::string&                 error)
{
    if (!validateTextureUse(source, use, error))
        return false;
    NativeDrawReplayTextureFormat format;
    std::size_t                   bytesPerPixel = 0;
    std::size_t                   blockSize     = 1;
    switch (source.host_format)
    {
        case 27:
        case 28:
            format        = NativeDrawReplayTextureFormat::Rgba8;
            bytesPerPixel = 4;
            break;
        case 60:
        case 61:
            format        = NativeDrawReplayTextureFormat::R8;
            bytesPerPixel = 1;
            break;
        case 70:
        case 71:
            format        = NativeDrawReplayTextureFormat::Bc1;
            bytesPerPixel = 8;
            blockSize     = 4;
            break;
        case 73:
        case 74:
            format        = NativeDrawReplayTextureFormat::Bc2;
            bytesPerPixel = 16;
            blockSize     = 4;
            break;
        case 76:
        case 77:
            format        = NativeDrawReplayTextureFormat::Bc3;
            bytesPerPixel = 16;
            blockSize     = 4;
            break;
        case 9:
        case 10:
            format        = NativeDrawReplayTextureFormat::Rgba16Float;
            bytesPerPixel = 8;
            break;
        case 33:
        case 34:
            format        = NativeDrawReplayTextureFormat::Rg16Float;
            bytesPerPixel = 4;
            break;
        case 39:
        case 41:
            format        = NativeDrawReplayTextureFormat::R32Float;
            bytesPerPixel = 4;
            break;
        default:
            return fail(error, "unsupported static gameplay texture format");
    }
    if (!source.host_mip_levels || source.host_mip_levels > 13 ||
        (!source.subresources.empty() &&
         source.subresources.size() != std::size_t(source.host_depth_or_array_size) * source.host_mip_levels))
        return fail(error, "static gameplay texture subresource count is invalid");
    const auto unitsWide = (source.host_width + static_cast<std::uint32_t>(blockSize) - 1) /
                           static_cast<std::uint32_t>(blockSize);
    (void)unitsWide;
    texture.width     = source.host_width;
    texture.height    = source.host_height;
    texture.format    = format;
    texture.dimension = use.dimension == 3 ? NativeDrawReplayTextureDimension::TextureCube
                                           : NativeDrawReplayTextureDimension::Texture2D;
    texture.mipLevels = source.host_mip_levels;
    std::vector<std::span<const std::uint8_t>> payloadSources;
    payloadSources.reserve(std::size_t(source.host_depth_or_array_size) * source.host_mip_levels);
    std::size_t payloadBytes = 0;
    for (std::uint32_t slice = 0; slice < source.host_depth_or_array_size; ++slice)
        for (std::uint32_t mip = 0; mip < source.host_mip_levels; ++mip)
        {
            const auto                    width        = std::max(1U, source.host_width >> mip);
            const auto                    height       = std::max(1U, source.host_height >> mip);
            const auto                    mipUnitsWide = (width + static_cast<std::uint32_t>(blockSize) - 1) /
                                                         static_cast<std::uint32_t>(blockSize);
            const auto                    mipUnitsHigh = (height + static_cast<std::uint32_t>(blockSize) - 1) /
                                                         static_cast<std::uint32_t>(blockSize);
            const auto                    rowSize      = std::size_t(mipUnitsWide) * bytesPerPixel;
            const auto                    rowCount     = static_cast<std::uint32_t>(mipUnitsHigh);
            std::span<const std::uint8_t> bytes;
            if (source.subresources.empty())
            {
                if (source.host_depth_or_array_size != 1 || source.host_mip_levels != 1 ||
                    source.row_size != rowSize || source.row_count != rowCount)
                    return fail(error, "static gameplay texture has no complete mip payload");
                bytes = source.bytes;
            }
            else
            {
                const auto& subresource = source.subresources[slice * source.host_mip_levels + mip];
                if (subresource.mip_level != mip || subresource.array_slice != slice ||
                    subresource.host_width != width || subresource.host_height != height ||
                    subresource.row_size != rowSize || subresource.row_count != rowCount)
                    return fail(error, "static gameplay texture subresource layout mismatch");
                bytes = subresource.bytes;
                if (bytes.empty() && slice == 0 && mip == 0)
                    bytes = source.bytes;
            }
            const auto expectedBytes = rowSize * mipUnitsHigh;
            if (bytes.size() != expectedBytes || expectedBytes > kMaxStaticTextureBytes || payloadBytes > kMaxStaticTextureBytes - expectedBytes)
                return fail(error, "static gameplay texture payload is incomplete or oversized");
            payloadBytes += expectedBytes;
            payloadSources.push_back(bytes);
        }
    const NativeDrawReplayTextureLayout layout{ texture.width, texture.height, texture.format, texture.mipLevels, texture.dimension };
    if (!PackNativeDrawReplayTextureVersion(layout, payloadSources, texture, error))
        return false;
    for (std::size_t component = 0; component < 4; ++component)
        texture.swizzle[component] = (use.component_mapping >> (component * 3)) & 7U;
    return true;
}

bool resolveRectangle(const DrawCaptureFrameEvent&  event,
                      const DrawCaptureFrameEvent*  fallback,
                      std::uint32_t&                sourceX,
                      std::uint32_t&                sourceY,
                      std::uint32_t&                width,
                      std::uint32_t&                height,
                      std::array<std::uint8_t, 24>& ownedRectangle,
                      std::uint64_t&                geometryEvent,
                      std::uint32_t&                geometryAddress,
                      std::string&                  error)
{
    const rex::graphics::diagnostic::DrawCaptureVertexFetch* rectangle     = nullptr;
    const auto                                               findRectangle = [&](const DrawCaptureFrameEvent& source)
    {
        for (const auto& fetch : source.draw.draw.vertex_fetches)
            if (fetch.fetch_constant == 0)
            {
                if (rectangle || fetch.availability != DrawCaptureAvailability::kComplete || fetch.Payload().size() != 24)
                {
                    error = "gameplay resolve rectangle is incomplete or duplicated";
                    return false;
                }
                rectangle     = &fetch;
                geometryEvent = source.id;
            }
        return true;
    };
    if (!findRectangle(event))
        return false;
    if (!rectangle && fallback && !findRectangle(*fallback))
        return false;
    if (!rectangle)
        return fail(error, "gameplay resolve has no source rectangle");
    std::ranges::copy(rectangle->Payload(), ownedRectangle.begin());
    geometryAddress = rectangle->base;
    float values[6];
    for (std::size_t index = 0; index < 6; ++index)
    {
        const auto* bytes = ownedRectangle.data() + index * 4;
        const auto  word  = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
                            (std::uint32_t(bytes[2]) << 8) | bytes[3];
        values[index]     = std::bit_cast<float>(word);
        if (!std::isfinite(values[index]))
            return fail(error, "gameplay resolve rectangle has a non-finite coordinate");
    }
    const auto x              = std::lround(values[0] + 0.5F);
    const auto y              = std::lround(values[1] + 0.5F);
    const auto resolvedWidth  = std::lround(values[2] - values[0]);
    const auto resolvedHeight = std::lround(values[5] - values[1]);
    if (x < 0 || y < 0 || resolvedWidth <= 0 || resolvedHeight <= 0)
        return fail(error, "gameplay resolve rectangle has invalid extent");
    sourceX = static_cast<std::uint32_t>(x);
    sourceY = static_cast<std::uint32_t>(y);
    width   = static_cast<std::uint32_t>(resolvedWidth);
    height  = static_cast<std::uint32_t>(resolvedHeight);
    return true;
}

bool makeSnapshotDraw(NativeGameplayShaderValidationCache&                             shaderValidation,
                      NativeGameplayGeometryCache&                                     geometryCache,
                      const DrawCaptureFrameEvent&                                     event,
                      std::span<const std::uint32_t>                                   registers,
                      std::span<const NativeGameplayShaderInterface>                   shaders,
                      std::span<const DrawCaptureTexture>                              snapshotTextures,
                      const std::map<std::uint32_t, NativeDrawReplayTexture>&          cpuTextures,
                      std::map<std::uint32_t, NativeDrawReplayTexture>&                decodedTextures,
                      const std::map<std::uint32_t, std::uint64_t>&                    pageResources,
                      const std::map<std::uint64_t, NativeGameplayGuestTextureLayout>& resourceLayouts,
                      const std::map<std::uint64_t, NativeRenderFrameTexture>&         resources,
                      const std::map<std::uint64_t, NativeGameplayProducerProof>&      producerProofs,
                      NativeGameplayRenderFrameDraw&                                   output,
                      NativeGameplayDrawTarget&                                        target,
                      std::string&                                                     error)
{
    const auto&   capture      = event.draw;
    const auto&   draw         = capture.draw;
    std::uint32_t colorFormat  = 0;
    std::int32_t  colorExpBias = 0;
    if (!decodeSnapshotTarget(registers, target, colorFormat, colorExpBias, error))
        return false;
    const auto* shader = findSnapshotShader(event, registers, shaders, colorFormat, colorExpBias, target.sampleCount, target.sampleMask);
    if (!shader)
        return fail(error, "gameplay event has no shader specialization for its target state");
    output.alphaTestEnabled = shader->alphaTestEnabled;
    // A depth-only host draw may omit its pixel-stage binding list. A pinned
    // shader interface with no texture fetches needs no texture payloads.
    if (capture.acquisition != DrawCaptureAvailability::kComplete ||
        (!capture.texture_bindings_complete && !shader->textureFetchSlots.empty()))
        return fail(error, "gameplay draw capture acquisition or required texture bindings are incomplete");
    if (draw.index.present &&
        (draw.index.availability != DrawCaptureAvailability::kComplete || draw.index.bytes.empty()))
        return fail(error, "gameplay indexed draw input is incomplete");
    for (const auto& fetch : draw.vertex_fetches)
        if (fetch.availability != DrawCaptureAvailability::kComplete || fetch.Payload().empty())
            return fail(error, "gameplay draw vertex fetch payload is incomplete");

    std::vector<NativeGameplayVertexFetchSpan> fetches;
    fetches.reserve(draw.vertex_fetches.size());
    for (const auto& fetch : draw.vertex_fetches)
        fetches.push_back({ fetch.fetch_constant, fetch.base, fetch.Payload() });

    std::vector<NativeDrawReplayTexture>      textures;
    std::vector<NativeDrawReplaySampler>      samplers;
    std::vector<NativeGameplayTextureBinding> bindings;
    textures.reserve(shader->textureFetchSlots.size());
    samplers.reserve(shader->textureFetchSlots.size());
    bindings.reserve(shader->textureFetchSlots.size());
    for (std::size_t slotIndex = 0; slotIndex < shader->textureFetchSlots.size(); ++slotIndex)
    {
        const auto                   slot     = shader->textureFetchSlots[slotIndex];
        const DrawCaptureTextureUse* selected = nullptr;
        for (const auto& use : capture.texture_uses)
            if (use.fetch_constant == slot && use.required)
            {
                if (selected && (selected->texture_index != use.texture_index ||
                                 selected->component_mapping != use.component_mapping ||
                                 selected->sampler_value != use.sampler_value ||
                                 selected->dimension != use.dimension || selected->is_signed != use.is_signed))
                    return fail(error, "gameplay texture binding is ambiguous for its fetch slot");
                selected = &use;
            }
        if (!selected || selected->texture_index == DrawCaptureTextureUse::kUnavailableTexture ||
            selected->texture_index >= snapshotTextures.size())
            return fail(error, "gameplay draw has no required texture binding");

        NativeDrawReplayTexture texture;
        const auto              sourceIndex = selected->texture_index;
        const auto&             source      = snapshotTextures[sourceIndex];
        const auto              cpu         = cpuTextures.find(sourceIndex);
        const auto              pageIt      = pageResources.find(source.guest_base_page);
        const bool              nativeSource =
            !selected->data_available && selected->availability == DrawCaptureAvailability::kUnavailable &&
            selected->unavailable_reason == DrawCaptureReason::kTextureUnresolved &&
            source.availability == DrawCaptureAvailability::kUnavailable &&
            source.unavailable_reason == DrawCaptureReason::kTextureUnresolved;
        const auto resourceIt              = pageIt == pageResources.end() ? resources.end() : resources.find(pageIt->second);
        const bool selectedNativeRg16Float = nativeSource && resourceIt != resources.end() &&
                                             isSelectedNativeRg16Float(source, *selected, resourceIt->second);
        if (source.unavailable_reason == DrawCaptureReason::kTextureUnresolved && !nativeSource)
            return fail(error, "native gameplay texture candidate has inconsistent availability");
        if (nativeSource && cpu != cpuTextures.end())
            return fail(error, "native gameplay texture cannot use a stale CPU owner");
        if (nativeSource && pageIt == pageResources.end())
            return fail(error, "native gameplay texture has no retained producer page");
        // A later CPU rewrite supersedes an earlier resolve to the same page.
        // Owned CPU versions carry the SDK's point-of-use ownership decision.
        if (cpu == cpuTextures.end() && pageIt != pageResources.end())
        {
            if (resourceIt == resources.end())
                return fail(error, "generated gameplay texture has no resource descriptor");
            const auto producerIt        = producerProofs.find(resourceIt->second.id);
            const bool sameFrameProducer = selectedNativeRg16Float && producerIt != producerProofs.end() &&
                                           producerIt->second.eventId < event.id && producerIt->second.frame == event.frame &&
                                           producerIt->second.submission == event.submission;
            if (nativeSource && !selectedNativeRg16Float &&
                (!resourceIt->second.imported || !resourceIt->second.persistentKey || !resourceIt->second.producerVersion))
                return fail(error, "native gameplay texture has no completed producer proof");
            if (selectedNativeRg16Float && !sameFrameProducer)
                return fail(error, "native RG16 float texture has no ordered same-frame producer");
            if (nativeSource && source.guest_mip_page)
            {
                const auto mipIt = pageResources.find(source.guest_mip_page);
                if (mipIt == pageResources.end() || mipIt->second != pageIt->second)
                    return fail(error, "native gameplay texture aliases no longer name one resource");
            }
            const auto layoutIt = resourceLayouts.find(pageIt->second);
            if (nativeSource && (layoutIt == resourceLayouts.end() || !layoutIt->second.valid))
                return fail(error, "native gameplay texture has no retained producer layout");
            const auto fetchOffset    = 0x4800 + slot * 6;
            const auto fetchExtent    = registers[fetchOffset + 2];
            const bool rgb10          = resourceIt->second.format == NativeDrawReplayTextureFormat::Rgb10a2;
            const auto guestFormat    = registers[fetchOffset + 1] & 0x3FU;
            const bool matchingFormat = rgb10                     ? guestFormat == 7 || guestFormat == 54
                                        : selectedNativeRg16Float ? guestFormat == kRg16FloatFormat
                                                                  : guestFormat == 6;
            if (nativeSource)
            {
                NativeTextureFetch fetch{};
                std::copy(registers.begin() + fetchOffset, registers.begin() + fetchOffset + 6, fetch.begin());
                NativeTextureMemoryRanges ranges;
                std::string               rangeError;
                const bool                rawRg16FloatParity = ((fetch[0] >> 2) & 0xFFU) == kRg16FloatSigns &&
                                                               ((fetch[3] >> 1) & 0xFFFU) == kRg16FloatRawSwizzle;
                if (!GetNativeTextureMemoryRanges(fetch, ranges, rangeError) ||
                    source.guest_base_page != ranges.base.address / 0x1000U ||
                    source.guest_mip_page != ranges.mips.address / 0x1000U ||
                    source.guest_width != ((fetchExtent & 0x1FFFU) + 1) ||
                    source.guest_height != (((fetchExtent >> 13) & 0x1FFFU) + 1) ||
                    source.guest_pitch != ((fetch[0] >> 22) & 0x1FFU) ||
                    source.guest_format != guestFormat ||
                    source.guest_endianness != ((fetch[1] >> 6) & 3U) ||
                    source.guest_tiled != ((fetch[0] >> 31) & 1U) ||
                    source.guest_depth_or_array_size != 1 ||
                    selected->dimension != 1 ||
                    (selectedNativeRg16Float && !rawRg16FloatParity) ||
                    (selectedNativeRg16Float ? (!selected->is_signed || selected->swizzled_signs != kRg16FloatSigns ||
                                                selected->component_mapping != kRg16FloatMapping)
                                             : selected->is_signed) ||
                    (fetch[5] >> 12) ||
                    ((fetch[4] >> 2) & 0xFU) || ((fetch[4] >> 6) & 0xFU) ||
                    (fetch[5] & (1U << 11)))
                    return fail(error, rangeError.empty() ? "native gameplay texture source range or descriptor changed" : rangeError.c_str());
                if (selectedNativeRg16Float)
                {
                    const auto& proof = producerIt->second;
                    if (proof.destinationBase != ranges.base.address || ranges.mips.size || proof.destinationPitch != 0x02D00500 ||
                        proof.sourceX || proof.sourceY || proof.destinationX || proof.sourceWidth != kRg16FloatWidth ||
                        proof.sourceHeight != kRg16FloatHeight || proof.layout != layoutIt->second)
                        return fail(error, "native RG16 float producer does not cover the complete consumer range");
                }
                const auto& producer = layoutIt->second;
                if (producer.basePage != source.guest_base_page || producer.mipPage != source.guest_mip_page ||
                    producer.width != source.guest_width || producer.height != source.guest_height ||
                    producer.depthOrArraySize != source.guest_depth_or_array_size || producer.pitch != source.guest_pitch ||
                    producer.format != source.guest_format || producer.endianness != source.guest_endianness ||
                    producer.tiled != source.guest_tiled || producer.packedMips != source.guest_packed_mips ||
                    producer.mipMinLevel != 0 || producer.mipMaxLevel != 0 || source.mip_level != 0 || source.host_mip_levels != 1)
                    return fail(error, "native gameplay texture consumer layout differs from its producer");
            }
            if (((fetchExtent & 0x1FFFU) + 1) != resourceIt->second.width ||
                (((fetchExtent >> 13) & 0x1FFFU) + 1) != resourceIt->second.height ||
                ((!selectedNativeRg16Float && (resourceIt->second.imported || rgb10)) &&
                 (!matchingFormat ||
                  (registers[fetchOffset + 5] >> 12) || selected->dimension != 1 || selected->is_signed)))
                return fail(error, "native gameplay texture no longer matches its guest resource layout");
            texture.width          = resourceIt->second.width;
            texture.height         = resourceIt->second.height;
            texture.format         = resourceIt->second.format;
            texture.frameTextureId = resourceIt->second.id;
        }
        else
        {
            if (cpu != cpuTextures.end())
                texture = cpu->second;
            else
            {
                if (!validateTextureUse(source, *selected, error))
                {
                    error = "texture fetch " + std::to_string(slot) + " page " +
                            std::to_string(source.guest_base_page) + ": " + error;
                    return false;
                }
                const auto cached = decodedTextures.find(sourceIndex);
                if (cached != decodedTextures.end())
                    texture = cached->second;
                else
                {
                    if (!decodeSnapshotTexture(source, *selected, texture, error))
                        return false;
                    decodedTextures.emplace(sourceIndex, texture);
                }
            }
        }
        for (std::size_t component = 0; component < 4; ++component)
            texture.swizzle[component] = (selected->component_mapping >> (component * 3)) & 7U;
        NativeDrawReplaySampler sampler;
        if (!DecodeNativeGameplaySampler(selected->sampler_value, sampler, error))
            return false;
        textures.push_back(std::move(texture));
        samplers.push_back(sampler);
    }
    for (std::size_t index = 0; index < textures.size(); ++index)
        bindings.push_back({ shader->textureFetchSlots[index], &textures[index], &samplers[index] });

    NativeGameplayDrawView view;
    view.registers       = registers;
    view.vertexMicrocode = draw.vertex_ucode;
    view.pixelMicrocode  = draw.pixel_ucode;
    view.vertexFetches   = fetches;
    view.indexBytes      = draw.index.bytes;
    view.textureBindings = bindings;
    view.primitive       = draw.primitive_type;
    view.indexCount      = draw.requested_index_count;
    view.indexFormat     = draw.index.format;
    view.indexEndian     = draw.index.endianness;
    view.indexed         = draw.index.present;
    output.eventId       = event.id;
    if (!BuildNativeGameplayDrawAdmission(view, *shader, target, output.admittedRecipe, error, &shaderValidation, &geometryCache))
        return false;
    return true;
}

bool isProvenNoEffectDraw(const DrawCaptureFrameEvent& event, std::span<const std::uint32_t> registers)
{
    const auto& draw = event.draw.draw;
    // Host-issued no-input journal draws are omitted; any owned input keeps
    // normal shader/state and memexport validation.
    return event.kind == DrawCaptureFrameEvent::Kind::kDraw && event.host_issued &&
           event.draw.guest_vertex_shader_hash == kNoEffectVertexHash &&
           event.draw.guest_pixel_shader_hash == kNoEffectPixelHash &&
           registers.size() == kRegisterCount && (registers[0x2104] & 0xFU) == 0 &&
           (registers[0x2200] & 7U) == 0 && !draw.color_target_written &&
           !draw.index.present && draw.vertex_fetches.empty();
}

bool isPreparedMainLeftClearState(std::span<const std::uint32_t> registers)
{
    // Match the collector's exact captured form before touching feed or prefix state.
    return registers.size() == kRegisterCount && registers[0x2000] == kSurfaceMain && registers[0x2001] == 0 &&
           registers[0x2002] == kMainDepthBase && registers[0x2205] == 0x10000 && registers[0x2208] == 6 &&
           registers[0x2080] == 0 && registers[0x2081] == 0 && registers[0x2082] == 0x02D00280 &&
           registers[0x2318] == kCopyLeft && registers[0x2319] && registers[0x2319] < 0x20000000U &&
           (registers[0x2319] & 0xFFFU) == 0 &&
           registers[0x231A] == 0x02D00500 && registers[0x231B] == 0x01000300 && registers[0x231C] == 0xFFF &&
           registers[0x231D] == 0xFFFFFF00 && registers[0x231E] == 0 && registers[0x231F] == 0;
}

bool isPreparedMainLeftGeometry(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != 24)
        return false;
    const std::array<std::uint32_t, 6> expected = {
        std::bit_cast<std::uint32_t>(-0.5F),
        std::bit_cast<std::uint32_t>(-0.5F),
        std::bit_cast<std::uint32_t>(639.5F),
        std::bit_cast<std::uint32_t>(-0.5F),
        std::bit_cast<std::uint32_t>(639.5F),
        std::bit_cast<std::uint32_t>(719.5F),
    };
    for (std::size_t index = 0; index != expected.size(); ++index)
    {
        const auto word = (std::uint32_t(bytes[index * 4]) << 24) | (std::uint32_t(bytes[index * 4 + 1]) << 16) |
                          (std::uint32_t(bytes[index * 4 + 2]) << 8) | bytes[index * 4 + 3];
        if (word != expected[index])
            return false;
    }
    return true;
}

bool isPreparedMainLeftClearEvent(const DrawCaptureFrameEvent& event, std::span<const std::uint32_t> registers)
{
    if (!isPreparedMainLeftClearState(registers) || event.draw.draw.vertex_fetches.size() != 1)
        return false;
    const auto& fetch = event.draw.draw.vertex_fetches.front();
    return fetch.fetch_constant == 0 && fetch.availability == DrawCaptureAvailability::kComplete &&
           isPreparedMainLeftGeometry(fetch.Payload());
}

} // namespace

struct NativeGameplayCommandFeed::Impl
{
    NativeGameplayPreparationContext*              preparationContext = nullptr;
    bool                                           preparationFeed = false, preparationSucceeded = false, reservationActive = false;
    bool                                           preparationOwnersKnown = true;
    std::size_t                                    activeCacheBudget = 0, ownerBytes = 0, localBytes = 0, completedPassBytes = 0;
    std::span<const NativeGameplayShaderInterface> shaders;
    NativeGameplayGeometryCache*                   reusableGeometry;
    NativeGameplayShaderValidationCache            shaderValidation;
    NativeGameplayGeometryCache                    frameGeometry{ 512U * 1024U * 1024U };
    NativeGameplayGeometryCache&                   geometryCache;
    NativeGameplayRenderFrameInput                 input;
    PrefixConsumer                                 prefixConsumer;
    PrefixQueueConsumer                            prefixQueueConsumer;

    struct PendingPrefix
    {
        std::shared_future<NativeDrawReplayResult> ticket;
        std::size_t                                bytes;
        std::uint64_t                              eventId;
    };

    std::deque<PendingPrefix>                                                 pendingPrefixes;
    std::string                                                               prefixFailure;
    PrefixRecording                                                           prefixRecording = PrefixRecording::FirstPass;
    NativeRenderFramePass::Target                                             prefixTarget{};
    std::size_t                                                               emittedPasses        = 0;
    std::size_t                                                               emittedBatchedPasses = 0; // sent in prefix packets
    bool                                                                      prefixDeferred       = false;
    std::map<std::uint32_t, NativeDrawReplayTexture>                          decodedTextures;
    std::map<std::uint32_t, std::uint64_t>                                    pageResources;
    std::map<std::uint64_t, NativeGameplayGuestTextureLayout>                 resourceLayouts;
    std::map<std::uint64_t, NativeRenderFrameTexture>                         resources;
    std::map<std::uint64_t, NativeGameplayProducerProof>                      producerProofs;
    std::map<std::uint64_t, std::uint64_t>                                    tiledPairResources, tiledDepthAnchors, retainedVersions, copyVersions;
    std::vector<std::uint32_t>                                                requestedPages;
    std::vector<NativeGameplayRetainedTexture>                                retained;
    NativeGameplayRenderFramePass                                             current;
    NativeGameplayDrawTarget                                                  currentTarget;
    DrawCaptureFrameEvent                                                     pendingTrigger;
    NativeRenderFramePass::Target                                             pendingCopyTarget;
    std::vector<NativeRenderFramePass::Clear>                                 pendingCopyClears;
    std::unordered_set<std::shared_ptr<const std::vector<std::uint8_t>>>      planningPayloads;
    std::unordered_set<std::shared_ptr<const NativeDrawReplayShaderVersion>>  planningShaderVersions;
    std::unordered_set<std::shared_ptr<const NativeDrawReplayTextureVersion>> planningTextureVersions;
    std::uint64_t                                                             startFrame = 0, startSubmission = 0, maximumFrame = 0, maximumSubmission = 0;
    std::uint64_t                                                             previousEvent = 0, nextResourceId = 1, pendingCopyAlias = 0, pendingCopyEvent = 0;
    std::size_t                                                               planningBytes = 0;
    ValidatedCopyState                                                        copyState;
    bool                                                                      haveCurrent = false, havePendingTrigger = false, pendingCopyPreserve = false;
    bool                                                                      pendingCopyAliasValid = false, sawLeadingSwap = false, sawNonSwapEvent = false;
    bool                                                                      retainAll = true, trackRetained = true, recording = false;

    Impl(std::span<const NativeGameplayShaderInterface> shaders, NativeGameplayGeometryCache* geometry)
    : shaders(shaders)
    , reusableGeometry(geometry)
    , geometryCache(geometry ? *geometry : frameGeometry)
    {
    }

    static std::size_t passBuffers(const NativeGameplayRenderFramePass& pass) noexcept
    {
        using namespace preparation;
        return Add(Add(Vector(pass.clears), Vector(pass.draws)), Add(Vector(pass.copies), Vector(pass.operations)));
    }

    static std::size_t recipeBuffers(const NativeDrawReplayRecipe& recipe) noexcept
    {
        using namespace preparation;
        auto bytes = Add(Vector(recipe.indices), Vector(recipe.samplers));
        for (const auto* value : { &recipe.vertexData, &recipe.vertexShaderDxil, &recipe.pixelShaderDxil, &recipe.vertexConstants, &recipe.pixelConstants, &recipe.sharedConstants, &recipe.initialSample0, &recipe.initialSample1, &recipe.depth.initialSamples })
            bytes = Add(bytes, Vector(*value));
        for (const auto& texture : recipe.textures)
            bytes = Add(bytes, Vector(texture.bytes));
        return Add(bytes, Add(String(recipe.sourcePath.native()), String(recipe.outputPath.native())));
    }

    static std::size_t admissionBytes(const NativeDrawReplayAdmission& admission) noexcept
    {
        using namespace preparation;
        return Add(Separate<NativeDrawReplayAdmission>(), Add(Allocation<NativeDrawReplayTextureBinding>(admission.TextureBindingCapacity()), recipeBuffers(admission.Recipe())));
    }

    std::size_t fixedBytes() const noexcept
    {
        using namespace preparation;
        auto bytes = Add(sizeof(Impl), shaderValidation.ManagedBytes());
        bytes      = Add(bytes, Add(Tree(decodedTextures), Add(Tree(pageResources), Add(Tree(resourceLayouts), Add(Tree(resources), Tree(producerProofs))))));
        for (const auto* map : { &tiledPairResources, &tiledDepthAnchors, &retainedVersions, &copyVersions })
            bytes = Add(bytes, Tree(*map));
        bytes = Add(bytes, Add(Hash(planningPayloads), Add(Hash(planningShaderVersions), Hash(planningTextureVersions))));
        bytes = Add(bytes, Add(Vector(retained), Vector(requestedPages)));
        bytes = Add(bytes, Add(Vector(input.resources), Vector(input.passes)));
        bytes = Add(bytes, Add(completedPassBytes, passBuffers(current)));
        bytes = Add(bytes, Add(Vector(pendingCopyClears), Add(String(prefixFailure), String(input.outputPath.native()))));
        return bytes;
    }

    std::size_t managedBytes() const noexcept
    {
        return preparation::Add(fixedBytes(), preparation::Add(ownerBytes, localBytes));
    }

    static preparation::Forecast forecastConstruction() noexcept
    {
        using namespace preparation;
        Forecast forecast{ true };
        forecast.Allocate(sizeof(Impl));
        forecast.Allocate(Tree<typename decltype(decodedTextures)::value_type>(0));
        forecast.Allocate(Tree<typename decltype(pageResources)::value_type>(0));
        forecast.Allocate(Tree<typename decltype(resourceLayouts)::value_type>(0));
        forecast.Allocate(Tree<typename decltype(producerProofs)::value_type>(0));
        forecast.Allocate(Tree<typename decltype(resources)::value_type>(0));
        forecast.Allocate(Multiply(4, Tree<typename decltype(copyVersions)::value_type>(0)));
        forecast.Allocate(Hash<typename decltype(planningPayloads)::value_type>(0, 8));
        forecast.Allocate(Hash<typename decltype(planningShaderVersions)::value_type>(0, 8));
        forecast.Allocate(Hash<typename decltype(planningTextureVersions)::value_type>(0, 8));
        return forecast;
    }

    static std::size_t errorScratch(const std::string& error) noexcept
    {
        using namespace preparation;
        constexpr auto literal = sizeof("gameplay shader generated pixel parameter specialization does not match SQ_PROGRAM_CNTL") - 1;
        constexpr auto context = sizeof("gameplay event ") - 1 + 20 + sizeof(": texture fetch ") - 1 + 10 + sizeof(" page ") - 1 + 10 + sizeof(": ") - 1 + literal;
        auto           bytes   = Add(String(error), StringGrowth<char>(error.capacity(), literal));
        bytes                  = Add(bytes, Add(StringGrowth<char>(15, 20), Multiply(2, StringGrowth<char>(15, 10))));
        return Add(bytes, Multiply(8, StringGrowth<char>(15, context)));
    }

    // This function runs before any selected whole unit, including replacement
    // Impl allocation. Its callback owns no feed buffers or allocation capture.
    static bool fund(NativeGameplayPreparationContext* context, NativeGameplayGeometryCache* cache, std::size_t committed, const preparation::Forecast& forecast, std::size_t& cacheBudget, bool& active, std::string& error)
    {
        using namespace preparation;
        active = false;
        if (!context)
            return true;
        if (!context->Settle(false, error))
            return false;
        if (context->state == NativeGameplayPreparationStatus::Success)
            return true;
        auto total = Add(context->receipt.bytes, Add(committed, context->callerBankBytes));
        total      = Add(total, Add(forecast.logical, forecast.allocations));
        if (SupportedAbi && context->receipt.known && context->receipt.basis == Basis && forecast.known && total <= Budget)
        {
            const auto remaining  = Budget - total;
            const auto supplement = Add(cache ? cache->Supplements() : 0, forecast.cacheSupplements);
            cacheBudget           = supplement <= remaining ? remaining - supplement : 0;
            if (cache)
                cache->SetBudget(cacheBudget);
            if (!cache || cache->ManagedBytes() <= remaining)
            {
                active = true;
                return true;
            }
        }
        if (!context->Settle(true, error))
            return false;
        if (cache)
            cache->SetBudget(committed <= Budget ? Budget - committed : 0);
        return true;
    }

    bool fund(const preparation::Forecast& forecast, std::string& error)
    {
        const auto result = fund(preparationContext, &geometryCache, std::max(planningBytes, managedBytes()), forecast, activeCacheBudget, reservationActive, error);
        if (result && !reservationActive)
            geometryCache.SetBudget(preparation::Budget - planningBytes);
        return result;
    }

    preparation::Forecast forecastBegin(std::size_t count, std::size_t pages, const std::string& error) const noexcept
    {
        using namespace preparation;
        auto forecast  = forecastConstruction();
        forecast.known = count <= NativeRenderBatchCommands && pages <= NativeRenderBatchCommands && !prefixConsumer && !prefixQueueConsumer;
        forecast.Charge(sizeof(copyState));
        forecast.Allocate(Allocation<NativeGameplayRetainedTexture>(count));
        forecast.Allocate(Allocation<std::uint32_t>(pages));
        forecast.Allocate(Multiply(count, Tree<typename decltype(resources)::value_type>(0) + Tree<typename decltype(resourceLayouts)::value_type>(0) + Tree<typename decltype(retainedVersions)::value_type>(0) + Tree<typename decltype(pageResources)::value_type>(0)));
        forecast.Allocate(Add(Hash<std::uint32_t>(0, 8), HashInsertions<std::uint32_t>(0, 8, pages)));
        forecast.Allocate(errorScratch(error));
        return forecast;
    }

    preparation::Forecast forecastConsume(const DrawCaptureFrameEvent& event, std::span<const std::uint32_t> registers, std::span<const DrawCaptureTexture> textures, const std::map<std::uint32_t, NativeDrawReplayTexture>& cpu, const std::string& error, bool prepared) const noexcept
    {
        using namespace preparation;
        Forecast forecast{ registers.size() == kRegisterCount && !havePendingTrigger };
        forecast.Charge(sizeof(NativeGameplayRenderFramePass) + sizeof(NativeGameplayRenderFrameOperation));
        forecast.Allocate(errorScratch(error));
        if (!forecast.known)
            return forecast;
        if (!prepared && event.kind == DrawCaptureFrameEvent::Kind::kDraw && !event.host_issued && registers[0x2208] == 6 && event.draw.draw.primitive_type == 8)
        {
            forecast.known = false;
            return forecast;
        }
        if (event.kind == DrawCaptureFrameEvent::Kind::kSwap || (event.kind != DrawCaptureFrameEvent::Kind::kCopy && ((!prepared && !event.host_issued) || isProvenNoEffectDraw(event, registers))))
            return forecast;
        forecast.Allocate(Pushes(input.passes, 2));
        if (event.kind == DrawCaptureFrameEvent::Kind::kCopy)
        {
            std::size_t rectangles = 0;
            for (const auto& fetch : event.draw.draw.vertex_fetches)
                if (fetch.fetch_constant == 0)
                {
                    ++rectangles;
                    if (fetch.availability != DrawCaptureAvailability::kComplete || fetch.Payload().size() != 24)
                        forecast.known = false;
                }
            forecast.known = forecast.known && event.copy_mode == "resolve" && rectangles == 1;
            forecast.Charge(sizeof(NativeGameplayRenderFrameCopy) + sizeof(NativeGameplayResolveAdmission));
            forecast.Allocate(Separate<NativeGameplayResolveAdmission>());
            forecast.Allocate(Tree<typename decltype(resources)::value_type>(0));
            forecast.Allocate(Tree<typename decltype(resourceLayouts)::value_type>(0));
            forecast.Allocate(Tree<typename decltype(producerProofs)::value_type>(0));
            forecast.Allocate(Multiply(3, Tree<typename decltype(pageResources)::value_type>(0)));
            forecast.Allocate(Multiply(3, Tree<typename decltype(copyVersions)::value_type>(0)));
            forecast.Allocate(Pushes(input.resources, 1));
            forecast.Allocate(Add(Pushes(current.copies, 1), Allocation<NativeGameplayRenderFrameCopy>(1)));
            forecast.Allocate(Add(Pushes(current.operations, 1), Allocation<NativeGameplayRenderFrameOperation>(1)));
            forecast.Allocate(Add(Pushes(pendingCopyClears, 1), Allocation<NativeRenderFramePass::Clear>(1)));
            return forecast;
        }
        const auto& draw = event.draw.draw;
        if (event.draw.guest_vertex_shader_hash == 0x1E6883FCCDE1F688ULL && event.draw.guest_pixel_shader_hash == 0xA4A965C189287B99ULL)
        {
            forecast.known = draw.vertex_ucode.size() == 27 && draw.pixel_ucode.size() == 9 && draw.requested_index_count && draw.requested_index_count <= 4096 && draw.requested_index_count % 3 == 0 && draw.vertex_fetches.size() == 1;
            forecast.Allocate(Add(StringGrowth<char>(15, 108), StringGrowth<char>(15, 36)));
            forecast.Allocate(Multiply(2, StringGrowth<char>(15, 64)));
            forecast.Allocate(Pushes<NativeRenderFramePass::Clear>(0, 0, draw.requested_index_count / 3));
            return forecast;
        }
        const auto  msaa   = (registers[0x2000] >> 16) & 3U;
        const auto  bias   = (registers[0x2001] >> 20) & 0x3FU;
        const auto* shader = findSnapshotShader(event, registers, shaders, (registers[0x2001] >> 16) & 0xFU, static_cast<std::int32_t>((bias ^ 0x20U) - 0x20U), msaa ? 4 : 1, msaa ? 9 : 1);
        if (!shader || msaa > 1 || shader->vertexAttributes.empty() || shader->vertexAttributes.size() > 16 || shader->textureFetchSlots.size() > 32 ||
            registers[0x2204] != 0x80000 || registers[0x2206] != 0x43F || registers[0x2302] != 4 || registers[0x2307] != 0xFF000 || registers[0x2308] != 0xFF100 ||
            shader->vertexShaderDxil.size() > 4U * 1024U * 1024U || shader->pixelShaderDxil.size() > 4U * 1024U * 1024U || draw.vertex_ucode.size() > 1024U * 1024U || draw.pixel_ucode.size() > 1024U * 1024U ||
            shader->vertexGuestWordDigest.size() != 64 || shader->pixelGuestWordDigest.size() != 64 || shader->vertexDxilDigest.size() != 64 || shader->pixelDxilDigest.size() != 64 ||
            (draw.primitive_type != 4 && draw.primitive_type != 6) || !draw.requested_index_count || draw.requested_index_count > (16U * 1024U * 1024U) / (draw.primitive_type == 4 ? 4 : 12) || draw.vertex_fetches.empty() || draw.vertex_fetches.size() > 96)
        {
            forecast.known = false;
            return forecast;
        }
        std::array<const rex::graphics::diagnostic::DrawCaptureVertexFetch*, 96> fetches{};
        std::size_t                                                              sourceBytes = 0;
        for (const auto& fetch : draw.vertex_fetches)
        {
            if (fetch.fetch_constant >= fetches.size() || fetches[fetch.fetch_constant] || fetch.availability != DrawCaptureAvailability::kComplete || fetch.Payload().empty())
            {
                forecast.known = false;
                return forecast;
            }
            fetches[fetch.fetch_constant] = &fetch;
            sourceBytes                   = Add(sourceBytes, fetch.Payload().size());
        }
        if (sourceBytes > 16U * 1024U * 1024U)
        {
            forecast.known = false;
            return forecast;
        }
        std::size_t vertices = (registers[0x2100] & 0xFFFFFFU) + 1U;
        for (const auto& attribute : shader->vertexAttributes)
        {
            const auto width = NativeGameplayVertexFormatBytes(attribute.format);
            if (!width || !attribute.strideBytes || attribute.offsetBytes < 0 || attribute.fetchConstant >= fetches.size() || !fetches[attribute.fetchConstant] || Add(attribute.offsetBytes, width) > fetches[attribute.fetchConstant]->Payload().size())
            {
                forecast.known = false;
                return forecast;
            }
            vertices = std::min(vertices, (fetches[attribute.fetchConstant]->Payload().size() - attribute.offsetBytes - width) / attribute.strideBytes + 1);
        }
        const auto decoded = std::min(Multiply(vertices, 16 * shader->vertexAttributes.size()), std::size_t(16U * 1024U * 1024U));
        const auto slots = shader->textureFetchSlots.size(), count = std::size_t(draw.requested_index_count), fetchCount = draw.vertex_fetches.size();
        forecast.Allocate(Allocation<NativeGameplayVertexFetchSpan>(fetchCount));
        forecast.Allocate(Allocation<NativeDrawReplayTexture>(slots));
        forecast.Allocate(Allocation<NativeDrawReplaySampler>(slots));
        forecast.Allocate(Allocation<NativeGameplayTextureBinding>(slots));
        forecast.Allocate(shaderValidation.ForecastChanges(draw.vertex_ucode.size(), draw.pixel_ucode.size(), *shader));
        forecast.Allocate(Add(Allocation<std::uint32_t>(count), Allocation<std::uint32_t>(Multiply(3, count))));
        forecast.Allocate(Allocation<std::uint8_t>(decoded));
        forecast.Allocate(Multiply(2, Allocation<std::uint64_t>(3 + 6 * shader->vertexAttributes.size() + 4 * fetchCount)));
        const auto cache = geometryCache.ForecastRemember(shader->vertexAttributes.size(), fetchCount, decoded, sourceBytes, count);
        forecast.Allocate(cache.allocations);
        forecast.cacheSupplements = cache.cacheSupplements;
        const auto shared         = NativeGameplaySharedConstantBytes(draw.vertex_ucode, draw.pixel_ucode, shader->userClipPlane);
        forecast.Allocate(Add(Multiply(2, Allocation<std::uint8_t>(4096)), Allocation<std::uint8_t>(shared)));
        forecast.Allocate(Allocation<NativeDrawReplaySampler>(slots));
        forecast.Allocate(Add(Separate<NativeDrawReplayAdmission>(), Allocation<NativeDrawReplayTextureBinding>(slots)));
        forecast.Allocate(Multiply(slots, Tree<typename decltype(decodedTextures)::value_type>(0)));
        forecast.Allocate(HashInsertions(planningPayloads, 3 + slots));
        forecast.Allocate(HashInsertions(planningShaderVersions, 1));
        forecast.Allocate(HashInsertions(planningTextureVersions, slots));
        forecast.Allocate(Add(Pushes(current.draws, 1), Allocation<NativeGameplayRenderFrameDraw>(1)));
        forecast.Allocate(Add(Pushes(current.operations, 1), Allocation<NativeGameplayRenderFrameOperation>(1)));
        forecast.Charge(sizeof(NativeGameplayRenderFrameDraw) + sizeof(NativeDrawReplayAdmission) + slots * sizeof(NativeDrawReplayTextureBinding) + sizeof(NativeDrawReplayShaderVersion));
        forecast.Charge(Add(Add(shader->vertexShaderDxil.size(), shader->pixelShaderDxil.size()), Add(decoded, Add(Multiply(12, count), slots * sizeof(NativeDrawReplaySampler) + 8192 + shared))));
        for (const auto slot : shader->textureFetchSlots)
        {
            const DrawCaptureTextureUse* use = nullptr;
            for (const auto& candidate : event.draw.texture_uses)
                if (candidate.fetch_constant == slot && candidate.required)
                    use = &candidate;
            if (!use || use->texture_index >= textures.size())
            {
                forecast.known = false;
                return forecast;
            }
            const auto& source = textures[use->texture_index];
            const auto  owned  = cpu.find(use->texture_index);
            if (owned == cpu.end() && source.availability != DrawCaptureAvailability::kComplete && pageResources.contains(source.guest_base_page))
                continue;
            NativeDrawReplayTextureLayout layout;
            std::size_t                   metadata = 0, payload = 0, subresources = 0;
            if (owned != cpu.end())
            {
                const auto& texture = owned->second;
                if (!texture.bytes.empty() || !texture.textureVersion || !texture.textureVersion->MatchesOwner(texture.textureVersion) || texture.sharedBytes != texture.textureVersion->Payload() || texture.sharedBytes.owner_before(texture.textureVersion->Payload()) || texture.textureVersion->Payload().owner_before(texture.sharedBytes))
                {
                    forecast.known = false;
                    return forecast;
                }
                layout   = texture.textureVersion->Layout();
                metadata = texture.textureVersion->MetadataBytes();
                // The admitted owner may retain capacity above its logical size.
                forecast.Allocate(Add(SharedBytes(texture.sharedBytes->capacity()), Add(Separate<NativeDrawReplayTextureVersion>(), Allocation<NativeDrawReplayTextureSubresource>(texture.textureVersion->SubresourceCapacity()))));
            }
            else
            {
                layout = { source.host_width, source.host_height, NativeDrawReplayTextureFormat::Rgba8, source.host_mip_levels, use->dimension == 3 ? NativeDrawReplayTextureDimension::TextureCube : NativeDrawReplayTextureDimension::Texture2D };
                switch (source.host_format)
                {
                    case 27:
                    case 28:
                        break;
                    case 60:
                    case 61:
                        layout.format = NativeDrawReplayTextureFormat::R8;
                        break;
                    case 70:
                    case 71:
                        layout.format = NativeDrawReplayTextureFormat::Bc1;
                        break;
                    case 73:
                    case 74:
                        layout.format = NativeDrawReplayTextureFormat::Bc2;
                        break;
                    case 76:
                    case 77:
                        layout.format = NativeDrawReplayTextureFormat::Bc3;
                        break;
                    case 9:
                    case 10:
                        layout.format = NativeDrawReplayTextureFormat::Rgba16Float;
                        break;
                    case 33:
                    case 34:
                        layout.format = NativeDrawReplayTextureFormat::Rg16Float;
                        break;
                    case 39:
                    case 41:
                        layout.format = NativeDrawReplayTextureFormat::R32Float;
                        break;
                    default:
                        forecast.known = false;
                        return forecast;
                }
                if (source.host_depth_or_array_size != (use->dimension == 3 ? 6U : 1U) || (use->dimension != 1 && use->dimension != 3))
                {
                    forecast.known = false;
                    return forecast;
                }
            }
            if (!SizeNativeDrawReplayTextureLayout(layout, payload, subresources))
            {
                forecast.known = false;
                return forecast;
            }
            forecast.Allocate(Allocation<std::span<const std::uint8_t>>(subresources));
            forecast.Allocate(Multiply(2, Allocation<NativeDrawReplayTextureSubresource>(subresources)));
            forecast.Allocate(Add(SharedBytes(payload), Separate<NativeDrawReplayTextureVersion>()));
            forecast.Charge(Add(payload, std::max(metadata, sizeof(NativeDrawReplayTextureVersion) + subresources * sizeof(NativeDrawReplayTextureSubresource))));
        }
        return forecast;
    }

    void retireBytes(std::size_t bytes)
    {
        planningBytes -= bytes;
        geometryCache.SetBudget(reservationActive ? activeCacheBudget : 512U * 1024U * 1024U - planningBytes);
    }

    bool retirePrefixes(std::string& error, bool wait = false)
    {
        while (!pendingPrefixes.empty())
        {
            const auto& pending = pendingPrefixes.front();
            if (pending.ticket.valid() && !wait && pending.ticket.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                break;
            std::string failure;
            try
            {
                if (!pending.ticket.valid())
                    failure = "native prefix queue returned an invalid recording ticket";
                else
                {
                    const auto& result = pending.ticket.get();
                    if (!result.success)
                        failure = result.error.empty() ? "native prefix recording failed" : result.error;
                }
            }
            catch (const std::exception& exception)
            {
                failure = exception.what();
            }
            catch (...)
            {
                failure = "native prefix recording raised an unknown exception";
            }
            if (!failure.empty() && prefixFailure.empty())
            {
                prefixEvent(failure, pending.eventId);
                prefixFailure = std::move(failure);
            }
            // Retirement refunds only this request. Later callback preparation
            // remains charged in the producer's current planning total.
            retireBytes(pending.bytes);
            pendingPrefixes.pop_front();
        }
        if (!prefixFailure.empty())
        {
            error = prefixFailure;
            return false;
        }
        return true;
    }

    bool flush(std::string& error)
    {
        if (!haveCurrent)
            return true;
        if (current.draws.empty() && current.clears.empty() && current.copies.empty())
            return fail(error, "gameplay pass has no native work");
        completedPassBytes = preparation::Add(completedPassBytes, passBuffers(current));
        input.passes.push_back(std::move(current));
        current     = {};
        haveCurrent = false;
        return emitPass(error);
    }

    bool emitPass(std::string& error)
    {
        if (recording && (prefixConsumer || prefixQueueConsumer) && !prefixDeferred)
        {
            if (!retirePrefixes(error))
                return false;
            const auto& first  = input.passes.back();
            const auto  target = passTarget(first);
            if (input.passes.size() != emittedPasses + 1 ||
                (emittedPasses && prefixRecording == PrefixRecording::FirstPass) ||
                target.format == NativeDrawReplayTargetFormat::Rg16Snorm ||
                (!emittedPasses && first.preservePreviousAttachments) ||
                (emittedPasses && prefixRecording == PrefixRecording::CompatibleChain && target != prefixTarget))
            {
                prefixDeferred = true;
                return true;
            }
            std::size_t prefixBytes = sizeof(NativeRenderFrame) + resources.size() * sizeof(NativeRenderFrameTexture) +
                                      sizeof(NativeRenderFramePass) + first.clears.size() * sizeof(NativeRenderFramePass::Clear) +
                                      first.copies.size() * sizeof(NativeRenderFrameResolve);
            prefixBytes += first.draws.size() * sizeof(std::shared_ptr<const NativeDrawReplayAdmission>);
            if (prefixQueueConsumer)
                prefixBytes += sizeof(PendingPrefix);
            const auto commands = first.clears.size() + first.draws.size() + first.copies.size();
            prefixBytes += ((commands - 1) / NativeRenderBatchCommands) * sizeof(NativeRenderFramePass);
            constexpr std::size_t maximum = 512U * 1024U * 1024U;
            if (prefixBytes > maximum - planningBytes)
            {
                prefixDeferred = true;
                return true;
            }

            struct RetireCopy
            {
                Impl&       owner;
                std::size_t bytes;

                ~RetireCopy()
                {
                    owner.retireBytes(bytes);
                }
            } retire{ *this, 0 };

            charge(prefixBytes);
            retire.bytes = prefixBytes;
            NativeRenderFrame prefix;
            for (const auto& [id, resource] : resources)
            {
                auto descriptor = resource;
                // Successive strips can change the final producer key. Only
                // the terminal graph publishes a completed generated version.
                if (!descriptor.imported)
                    descriptor.persistentKey = 0;
                prefix.textures.push_back(descriptor);
            }
            NativeRenderFramePass pass;
            pass.target                      = target;
            pass.clears                      = first.clears;
            pass.preservePreviousAttachments = first.preservePreviousAttachments;
            for (const auto& draw : first.draws)
                pass.admittedDraws.push_back(draw.admittedRecipe);
            for (const auto& copy : first.copies)
            {
                NativeRenderFrameResolve resolve;
                if (!DeriveNativeGameplayResolve(copy, input.tiledSceneBase, resolve, error))
                    return prefixEvent(error, copy.eventId);
                pass.resolves.push_back(resolve);
            }
            prefix.passes.push_back(std::move(pass));
            if (!BatchNativeRenderFrame(prefix, error))
                return false;
            const auto batched = prefix.passes.size();
            if (prefixQueueConsumer)
            {
                // Allocate the ticket before admission transfers the request's
                // operation handles to the recording thread.
                pendingPrefixes.push_back({ {}, prefixBytes, first.lastEvent });
                retire.bytes = 0;
                if (!prefixQueueConsumer(std::move(prefix), pendingPrefixes.back().ticket, error))
                {
                    retireBytes(pendingPrefixes.back().bytes);
                    pendingPrefixes.pop_back();
                    return prefixEvent(error, first.lastEvent);
                }
                if (!retirePrefixes(error))
                    return false;
            }
            else if (!prefixConsumer(std::move(prefix), error))
                return false;
            prefixTarget = target;
            ++emittedPasses;
            emittedBatchedPasses += batched;
        }
        return true;
    }

    std::uint64_t addResource(std::uint32_t width, std::uint32_t height, NativeDrawReplayTextureFormat format)
    {
        const auto               id = nextResourceId++;
        NativeRenderFrameTexture resource{ id, width, height, format };
        resources.emplace(id, resource);
        input.resources.push_back({ resource, true });
        return id;
    }

    static std::uint64_t attachmentAlias(std::span<const std::uint32_t> registers)
    {
        const auto colorInfo = registers[0x2001];
        const auto colorBase = std::uint64_t(colorInfo) & 0xFFFU;
        // Depth-disabled composition identifies the attachment from the active
        // color base only.
        if (((colorInfo >> 16) & 0xFU) == 4U)
            return colorBase;
        return colorBase | ((std::uint64_t(registers[0x2002]) & 0xFFFU) << 12);
    }

    static NativeGameplayDrawTarget clearTarget(const NativeRenderFramePass::Target& target)
    {
        NativeGameplayDrawTarget result;
        result.width       = target.width;
        result.height      = target.height;
        result.sampleCount = target.sampleCount;
        result.sampleMask  = target.sampleCount == 4 ? 9U : 1U;
        result.format      = target.format;
        return result;
    }

    static NativeRenderFramePass::Target passTarget(const NativeGameplayRenderFramePass& pass)
    {
        if (pass.target.width || pass.draws.empty())
            return pass.target;
        const auto& recipe = pass.draws.front().Recipe();
        return NativeRenderFramePass::Target{ recipe.width, recipe.height, recipe.sampleCount, recipe.targetFormat };
    }

    bool validatePreparedMainLeftClearAttachment(std::span<const std::uint32_t> registers, std::string& error) const
    {
        if (!haveCurrent || !pendingCopyClears.empty())
            return fail(error, "prepared main-left clear has no current attachment");
        const auto target = passTarget(current);
        if (target != NativeRenderFramePass::Target{ kSceneWidth, kSceneHeight, 4, NativeDrawReplayTargetFormat::Rgba8 })
            return fail(error, "prepared main-left clear attachment target is unsupported");
        if (!current.attachmentAliasValid || current.attachmentAlias != (std::uint64_t(kMainDepthBase) << 12))
            return fail(error, "prepared main-left clear attachment alias is unsupported");
        if (attachmentAlias(registers) != current.attachmentAlias)
            return fail(error, "prepared main-left clear attachment alias does not match the copy");
        return true;
    }

    bool materializePendingCopyClear(std::string& error)
    {
        if (pendingCopyClears.empty())
            return true;
        if (!pendingCopyPreserve || !pendingCopyTarget.width || !pendingCopyEvent)
            return fail(error, "gameplay copy clear has no preceding attachment");
        NativeGameplayRenderFramePass clearPass;
        clearPass.firstEvent                  = pendingCopyEvent;
        clearPass.lastEvent                   = clearPass.firstEvent;
        clearPass.attachmentAlias             = pendingCopyAlias;
        clearPass.attachmentAliasValid        = pendingCopyAliasValid;
        clearPass.clearAliasTransition        = true;
        clearPass.preservePreviousAttachments = true;
        clearPass.target                      = pendingCopyTarget;
        clearPass.clears                      = std::move(pendingCopyClears);
        clearPass.postCopyClear               = true;
        completedPassBytes                    = preparation::Add(completedPassBytes, passBuffers(clearPass));
        input.passes.push_back(std::move(clearPass));
        pendingCopyClears.clear();
        pendingCopyPreserve   = false;
        pendingCopyTarget     = {};
        pendingCopyAlias      = 0;
        pendingCopyAliasValid = false;
        pendingCopyEvent      = 0;
        return emitPass(error);
    }

    void charge(std::size_t bytes)
    {
        constexpr std::size_t maximum = 512U * 1024U * 1024U;
        std::string           error;
        if (!retirePrefixes(error))
            throw std::runtime_error(error);
        if (bytes > maximum - planningBytes && !pendingPrefixes.empty() && !retirePrefixes(error, true))
            throw std::runtime_error(error);
        if (bytes > maximum - planningBytes)
            throw std::runtime_error("gameplay planning inputs exceed the owned byte bound");
        planningBytes += bytes;
        geometryCache.SetBudget(reservationActive ? activeCacheBudget : maximum - planningBytes);
    }

    void retirePage(std::uint32_t page)
    {
        if (const auto found = pageResources.find(page); found != pageResources.end())
        {
            const auto id = found->second;
            std::erase_if(pageResources, [id](const auto& entry)
                          {
                              return entry.second == id;
                          });
        }
    }

    bool begin(std::string& error, std::span<const std::uint32_t> retainPages)
    {
        geometryCache.BeginFrame();
        charge(sizeof(copyState));
        input.returnResolvedOutput = true;
        for (const auto& prior : retained)
        {
            auto texture = prior.texture;
            if (!prior.guestBasePage || prior.guestBasePage >= 0x20000 || !texture.persistentKey || !texture.imported)
                return fail(error, "gameplay retained texture has no completed producer identity");
            const auto version = retainedVersions.find(texture.persistentKey);
            if (version == retainedVersions.end())
            {
                texture.id = nextResourceId++;
                resources.emplace(texture.id, texture);
                retainedVersions.emplace(texture.persistentKey, texture.id);
                if (prior.guestLayout.valid)
                    resourceLayouts.emplace(texture.id, prior.guestLayout);
            }
            else
            {
                // Tiled left/right pages may name the same completed resolve.
                // Import one resource, retaining both checked page identities.
                const auto& shared = resources.at(version->second);
                if (texture.width != shared.width || texture.height != shared.height || texture.format != shared.format)
                    return fail(error, "gameplay retained aliases have conflicting descriptors");
                texture.id        = shared.id;
                const auto layout = resourceLayouts.find(texture.id);
                if (prior.guestLayout.valid != (layout != resourceLayouts.end()) ||
                    (prior.guestLayout.valid && layout->second != prior.guestLayout))
                    return fail(error, "gameplay retained aliases have conflicting guest layouts");
            }
            if (!pageResources.emplace(prior.guestBasePage, texture.id).second)
                return fail(error, "gameplay retained texture pages must be unique");
        }

        std::unordered_set<std::uint32_t> uniquePages;
        for (const auto page : retainPages)
            if (!page || page >= 0x20000 || !uniquePages.insert(page).second)
                return fail(error, "gameplay retention pages must be valid and unique");
        requestedPages.assign(retainPages.begin(), retainPages.end());
        return true;
    }

    bool consume(const DrawCaptureFrameEvent&                            event,
                 std::span<const std::uint32_t>                          registers,
                 std::span<const DrawCaptureTexture>                     snapshotTextures,
                 const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                 std::string&                                            error,
                 bool                                                    prepared)
    {
        charge(sizeof(NativeGameplayRenderFramePass) + sizeof(NativeGameplayRenderFrameOperation));
        if (!event.id || event.id <= previousEvent || event.frame < startFrame ||
            event.submission < startSubmission || (!prepared && (!event.success || (event.execution_status != rex::graphics::diagnostic::DrawCaptureExecutionStatus::kIssued && event.execution_status != rex::graphics::diagnostic::DrawCaptureExecutionStatus::kNoEffect))) ||
            registers.size() != kRegisterCount)
            return fail(error, "invalid gameplay event order, completion or ownership boundary");
        previousEvent     = event.id;
        maximumFrame      = std::max(maximumFrame, event.frame);
        maximumSubmission = std::max(maximumSubmission, event.submission);
        if (event.kind == DrawCaptureFrameEvent::Kind::kSwap)
        {
            // The capture window may begin with a preceding present and
            // end with the owned frame present. Only the latter bounds
            // the native graph.
            if (sawLeadingSwap || sawNonSwapEvent)
                return failEvent(error, event.id, "only one leading non-final swap is allowed");
            sawLeadingSwap = true;
            return true;
        }
        sawNonSwapEvent = true;

        if (havePendingTrigger)
        {
            if (event.kind != DrawCaptureFrameEvent::Kind::kCopy)
                return failEvent(error, event.id, "legacy resolve trigger must be immediately followed by its copy");
            if (!std::ranges::equal(registers, pendingTrigger.draw.draw.registers))
                return failEvent(error, event.id, "legacy resolve trigger and copy state do not match");
        }

        if (!prepared && event.kind == DrawCaptureFrameEvent::Kind::kDraw && !event.host_issued &&
            (registers[0x2208] == 6 && event.draw.draw.primitive_type == 8))
        {
            if (havePendingTrigger)
                return fail(error, "gameplay resolve trigger has no copy event");
            pendingTrigger = event;
            pendingTrigger.draw.draw.registers.assign(registers.begin(), registers.end());
            havePendingTrigger = true;
            return true;
        }
        if (event.kind == DrawCaptureFrameEvent::Kind::kCopy)
        {
            if (event.copy_mode != "resolve")
                return fail(error, "gameplay copy has an unsupported copy mode");
            NativeGameplayRenderFrameCopy copy;
            charge(sizeof(copy) + sizeof(NativeGameplayResolveAdmission));
            copy.eventId                   = event.id;
            copy.triggerEventId            = havePendingTrigger ? pendingTrigger.id : 0;
            copy.sourceGeometryOwnedByCopy = !havePendingTrigger;
            const auto changed             = CopyWords::Read(registers);
            if (!copyState.valid || copyState.words != changed)
                copyState.Prepare(changed);
            std::array<std::uint8_t, 24> rectangle;
            std::uint64_t                geometryEvent   = 0;
            std::uint32_t                geometryAddress = 0;
            if (!resolveRectangle(event, havePendingTrigger ? &pendingTrigger : nullptr, copy.sourceX, copy.sourceY, copy.sourceWidth, copy.sourceHeight, rectangle, geometryEvent, geometryAddress, error))
                return prefixEvent(error, event.id);
            const auto&                   words             = copyState.words;
            const auto                    surface           = words.surface;
            const auto                    source            = copyState.source;
            const auto                    destInfo          = words.destinationInfo;
            const bool                    rgb10Color        = copyState.rgb10Color;
            const bool                    mainColor         = copyState.mainColor && copy.sourceWidth == kSceneWidth && copy.sourceHeight == kSceneHeight;
            const bool                    mainDepth         = copyState.mainDepth && copy.sourceWidth == kSceneWidth && copy.sourceHeight == kSceneHeight;
            NativeDrawReplayTextureFormat destinationFormat = NativeDrawReplayTextureFormat::Rgba8;
            std::uint64_t                 destinationId     = 0;
            std::uint64_t                 tiledAnchor       = 0;
            if (mainColor || mainDepth)
            {
                if (!copyState.tiledSourceValid)
                {
                    if (!checkTiledCopySource(registers, error))
                        return prefixEvent(error, event.id);
                    return failEvent(error, event.id, "tiled gameplay copy has unsupported destination pitch");
                }
                const auto base    = static_cast<std::uint64_t>(words.destinationBase);
                const auto control = words.control;
                if (!base || base % 0x1000U)
                    return failEvent(error, event.id, "tiled gameplay copy requires an aligned nonzero scene base");
                if (!mainDepth && (control != kCopyLeft && control != kCopyRight || rgb10Color && control != kCopyRight))
                {
                    return fail(error, "tiled gameplay color copy has unsupported control state");
                }
                if (!mainDepth && !rgb10Color && control == kCopyRight && base < kTiledHalfOffset)
                    return fail(error, "tiled gameplay copy right half precedes its scene base");
                std::uint64_t anchor = base;
                if (mainDepth)
                {
                    if (source != 4U)
                        return fail(error, "tiled gameplay depth copy is missing its source selector");
                    const auto exact = tiledDepthAnchors.find(base);
                    if (exact != tiledDepthAnchors.end())
                        anchor = exact->second;
                    else if (base >= kTiledHalfOffset)
                    {
                        const auto candidate = base - kTiledHalfOffset;
                        const auto previous  = tiledDepthAnchors.find(candidate);
                        if (previous != tiledDepthAnchors.end())
                            anchor = previous->second;
                    }
                }
                else if (rgb10Color)
                {
                    // Both RGB10 halves use the no-clear resolve control.
                    // The captured source rectangle identifies the half.
                    if ((copy.sourceX != 0 && copy.sourceX != kSceneWidth) ||
                        (copy.sourceX != 0 && base < kTiledHalfOffset))
                        return fail(error, "RGB10 tiled copy has an invalid source half");
                    anchor = base - (copy.sourceX ? kTiledHalfOffset : 0);
                }
                else
                    anchor = (control == kCopyRight) ? base - kTiledHalfOffset : base;
                if (!anchor || anchor % 0x1000U)
                    return failEvent(error, event.id, "tiled gameplay copy requires an aligned nonzero scene base");
                if (mainDepth)
                    tiledDepthAnchors.emplace(anchor, anchor);
                const auto delta           = base - anchor;
                const auto expectedSourceX = delta == 0 ? 0U : kSceneWidth;
                if (delta != 0 && delta != kTiledHalfOffset)
                    return fail(error, "tiled gameplay copy destination is not a 0x14000 half");
                if (copy.sourceX != expectedSourceX || copy.sourceY != 0)
                    return fail(error, "tiled gameplay copy source rectangle does not match its destination half");
                if (expectedSourceX != 0 &&
                    (words.control & (kCopyColorClearBit | kCopyDepthClearBit)) != 0)
                    return fail(error, "tiled gameplay right-half copy clear is unsupported");
                // CopyView uses full 1280-pixel coordinates; native resolve
                // uses a 640-pixel attachment. Validate source/destination
                // relation before normalizing right-half source coordinates.
                copy.sourceX         = 0;
                const auto pairKey   = anchor | (mainDepth ? (std::uint64_t(1) << 63) : 0);
                tiledAnchor          = anchor;
                const auto foundPair = tiledPairResources.find(pairKey);
                if (foundPair != tiledPairResources.end())
                    destinationId = foundPair->second;
                else
                {
                    destinationFormat = mainDepth    ? NativeDrawReplayTextureFormat::R32Float
                                        : rgb10Color ? NativeDrawReplayTextureFormat::Rgb10a2
                                                     : NativeDrawReplayTextureFormat::Rgba8;
                    destinationId     = addResource(1280, 720, destinationFormat);
                    tiledPairResources.emplace(pairKey, destinationId);
                }
                copy.destinationX         = expectedSourceX;
                copy.destinationTextureId = destinationId;
                copy.semantics            = mainDepth ? NativeGameplayRenderFrameCopy::Semantics::TiledMainDepth
                                                      : NativeGameplayRenderFrameCopy::Semantics::TiledMainColor;
                copy.tiledSceneBase       = anchor;
                copy.depth                = mainDepth;
                copy.samples              = mainDepth ? copyState.samples : std::array<std::uint32_t, 2>{ 0, 3 };
                copy.swapRedBlue          = !mainDepth && copyState.swapRedBlue;
            }
            else
            {
                // The 320x180 descriptor and fetch exclude four padding rows
                // from the native SRV.
                if (surface == 0x05010140 && words.destinationPitch == 0x00B40140 &&
                    source == 0 && destInfo == 0x01000300 &&
                    copy.sourceX == 0 && copy.sourceY == 0 &&
                    copy.sourceWidth == 320 && copy.sourceHeight == 184)
                    copy.sourceHeight = 180;
                copy.semantics            = NativeGameplayRenderFrameCopy::Semantics::Explicit;
                copy.destinationX         = 0;
                copy.destinationTextureId = 0;
                if (!copyState.destinationValid)
                    return fail(error, "gameplay copy destination format is unsupported");
                destinationFormat         = copyState.destinationFormat;
                destinationId             = addResource(copy.sourceWidth, copy.sourceHeight, destinationFormat);
                copy.destinationTextureId = destinationId;
                copy.depth                = source == 4;
                copy.colorTarget          = 0;
                copy.samples              = copyState.samples;
                copy.swapRedBlue          = copyState.swapRedBlue;
                copy.valueScale           = copyState.valueScale;
            }
            if (!haveCurrent && (!pendingCopyPreserve || !pendingCopyTarget.width))
                return fail(error, "gameplay copy phase has no preceding native draw pass");
            const auto copyTarget = haveCurrent ? passTarget(current) : pendingCopyTarget;
            if (prepared && (copy.sourceX > copyTarget.width || copy.sourceWidth > copyTarget.width - copy.sourceX ||
                             copy.sourceY > copyTarget.height || copy.sourceHeight > copyTarget.height - copy.sourceY))
                return failEvent(error, event.id, "prepared gameplay copy rectangle is outside its attachment");
            const bool selectedRg16FloatCopy = copyState.destinationFormat == NativeDrawReplayTextureFormat::Rg16Float;
            if (selectedRg16FloatCopy)
            {
                const auto expectedAlias    = attachmentAlias(registers);
                const auto sourceAlias      = haveCurrent ? current.attachmentAlias : pendingCopyAlias;
                const bool sourceAliasValid = haveCurrent ? (current.attachmentAliasValid || current.attachmentAlias != 0) : (pendingCopyAliasValid || pendingCopyAlias != 0);
                if (copyTarget != NativeRenderFramePass::Target{ kRg16FloatWidth, kRg16FloatHeight, 1, NativeDrawReplayTargetFormat::Rg16Snorm })
                    return failEvent(error, event.id, "selected RG16 float copy source target is unsupported");
                if (!sourceAliasValid || sourceAlias != expectedAlias)
                    return failEvent(error, event.id, "selected RG16 float copy source attachment alias does not match");
            }
            auto       copyClear    = copyState.clear;
            const bool hasCopyClear = copyClear.color || copyClear.depth;
            if (hasCopyClear)
            {
                if (copy.sourceX > copyTarget.width || copy.sourceWidth > copyTarget.width - copy.sourceX ||
                    copy.sourceY > copyTarget.height || copy.sourceHeight > copyTarget.height - copy.sourceY)
                    return failEvent(error, event.id, "gameplay copy clear rectangle is outside its attachment");
                if (copy.depth && copyClear.depth)
                    return failEvent(error, event.id, "depth resolve cannot clear the depth attachment");
                if (!copyState.colorClearValid)
                    return failEvent(error, event.id, "gameplay copy color clear format is unsupported");
                copyClear.rectangle = { static_cast<std::int32_t>(copy.sourceX), static_cast<std::int32_t>(copy.sourceY), static_cast<std::int32_t>(copy.sourceX + copy.sourceWidth), static_cast<std::int32_t>(copy.sourceY + copy.sourceHeight) };
            }
            NativeRenderFrameResolve resolve;
            if (!deriveExplicitResolve(copy, resolve, error))
                return prefixEvent(error, event.id);
            copy.producerLayout = deriveGuestTextureLayout(copy, registers);
            if (selectedRg16FloatCopy && !copy.producerLayout.valid)
                return failEvent(error, event.id, "selected RG16 float copy has an unsupported producer form");
            if (copy.producerLayout.valid)
            {
                const auto existing = resourceLayouts.find(destinationId);
                if (existing != resourceLayouts.end() && existing->second != copy.producerLayout)
                    return failEvent(error, event.id, "resolve producer guest layout changed for an existing resource");
                resourceLayouts[destinationId] = copy.producerLayout;
            }
            else
                resourceLayouts.erase(destinationId);
            if (copy.producerLayout.valid)
                producerProofs[destinationId] = { event.id, event.frame, event.submission, words.destinationBase, words.destinationPitch, words.destinationInfo, copy.sourceX, copy.sourceY, copy.sourceWidth, copy.sourceHeight, copy.destinationX, copy.producerLayout };
            else
                producerProofs.erase(destinationId);
            // A later resolve supersedes a page only for later-converted draws;
            // earlier recipes retain their resource ID.
            const auto basePage = words.destinationBase / 0x1000U;
            if (trackRetained)
                if (const auto prior = pageResources.find(basePage); prior != pageResources.end() && prior->second != destinationId)
                    retirePage(basePage);
            pageResources[basePage] = destinationId;
            if (std::any_of(retained.begin(), retained.end(), [&](const auto& prior)
                            {
                                return prior.guestBasePage == basePage;
                            }))
                resources.at(destinationId).persistentKey = event.id;
            copyVersions[destinationId] = event.id;
            if (mainColor || mainDepth)
            {
                pageResources[tiledAnchor / 0x1000U]                      = destinationId;
                pageResources[(tiledAnchor + kTiledHalfOffset) / 0x1000U] = destinationId;
            }
            if (!pendingCopyClears.empty())
            {
                const auto priorCopyEvent = pendingCopyEvent;
                if (!materializePendingCopyClear(error))
                    return false;
                pendingCopyPreserve = true;
                pendingCopyTarget   = input.passes.back().target;
                if (!pendingCopyTarget.width)
                    pendingCopyTarget = passTarget(input.passes.back());
                pendingCopyAlias      = input.passes.back().attachmentAlias;
                pendingCopyAliasValid = input.passes.back().attachmentAliasValid || pendingCopyAlias != 0;
                pendingCopyEvent      = priorCopyEvent;
            }
            if (!haveCurrent)
            {
                if (!pendingCopyPreserve || !pendingCopyTarget.width)
                    return fail(error, "gameplay copy phase has no preceding native draw pass");
                current                             = {};
                current.firstEvent                  = event.id;
                current.lastEvent                   = event.id;
                current.attachmentAlias             = pendingCopyAlias;
                current.attachmentAliasValid        = pendingCopyAliasValid;
                current.preservePreviousAttachments = true;
                current.target                      = pendingCopyTarget;
                currentTarget                       = clearTarget(current.target);
                haveCurrent                         = true;
                pendingCopyPreserve                 = false;
                pendingCopyTarget                   = {};
                pendingCopyAlias                    = 0;
                pendingCopyAliasValid               = false;
                pendingCopyEvent                    = 0;
            }
            auto admission                = std::shared_ptr<NativeGameplayResolveAdmission>(new NativeGameplayResolveAdmission(copy, resolve, copyClear, rectangle, geometryEvent, geometryAddress));
            admission->owner_             = admission;
            copy.admittedOperation        = admission;
            localBytes                    = preparation::Add(localBytes, preparation::Separate<NativeGameplayResolveAdmission>());
            const auto preserveTarget     = copyTarget;
            const auto preserveAlias      = current.attachmentAlias;
            const auto preserveAliasValid = current.attachmentAliasValid;
            current.copies.push_back(std::move(copy));
            current.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Copy, event.id, static_cast<std::uint32_t>(current.copies.size() - 1) });
            current.lastEvent = event.id;
            // A copy ends its pass; the next pass preserves attachments and
            // carries the post-resolve Xenos clear.
            if (!flush(error))
                return false;
            pendingCopyPreserve   = true;
            pendingCopyTarget     = preserveTarget;
            pendingCopyAlias      = preserveAlias;
            pendingCopyAliasValid = preserveAliasValid;
            pendingCopyEvent      = event.id;
            if (hasCopyClear)
                pendingCopyClears.push_back(admission->Clear());
            havePendingTrigger = false;
            return true;
        }
        if (event.kind != DrawCaptureFrameEvent::Kind::kDraw || (!prepared && !event.host_issued))
            return true;

        const bool clearShader = event.draw.guest_vertex_shader_hash == 0x1E6883FCCDE1F688ULL &&
                                 event.draw.guest_pixel_shader_hash == 0xA4A965C189287B99ULL;
        if (clearShader)
        {
            NativeRenderFramePass clearPass;
            if (prepared)
            {
                // Reject prepared clear inputs before sealing a prior prefix.
                if (!BuildNativeGameplayClearPass(event.draw.draw, registers, clearPass, error))
                    return prefixEvent(error, event.id);
                if (!clearPass.draws.empty() || !clearPass.resolves.empty() || clearPass.clears.empty())
                    return fail(error, "gameplay clear decoder returned a non-clear pass");
            }
            if (!pendingCopyClears.empty() && !materializePendingCopyClear(error))
                return false;
            if (!flush(error))
                return false;
            if (!prepared && !BuildNativeGameplayClearPass(event.draw.draw, registers, clearPass, error))
                return prefixEvent(error, event.id);
            if (!prepared && (!clearPass.draws.empty() || !clearPass.resolves.empty() || clearPass.clears.empty()))
                return fail(error, "gameplay clear decoder returned a non-clear pass");
            current                      = {};
            current.firstEvent           = event.id;
            current.lastEvent            = event.id;
            current.attachmentAlias      = attachmentAlias(registers);
            current.attachmentAliasValid = true;
            current.clearAliasTransition = true;
            current.target               = clearPass.target;
            current.clears               = std::move(clearPass.clears);
            if (!input.passes.empty() &&
                passTarget(input.passes.back()) == current.target &&
                (input.passes.back().attachmentAliasValid || input.passes.back().attachmentAlias != 0) &&
                input.passes.back().attachmentAlias == current.attachmentAlias)
                current.preservePreviousAttachments = true;
            currentTarget         = clearTarget(current.target);
            haveCurrent           = true;
            pendingCopyPreserve   = false;
            pendingCopyTarget     = {};
            pendingCopyAlias      = 0;
            pendingCopyAliasValid = false;
            pendingCopyEvent      = 0;
            return true;
        }

        if (isProvenNoEffectDraw(event, registers))
            return true;

        NativeGameplayRenderFrameDraw nativeDraw;
        NativeGameplayDrawTarget      target;
        if (trackRetained)
        {
            // Recorded CPU pixels and the live decoded map both identify
            // owned CPU versions. Retire native pages at their point of use.
            for (const auto& use : event.draw.texture_uses)
                if (use.required && use.texture_index < snapshotTextures.size())
                {
                    const auto& source = snapshotTextures[use.texture_index];
                    if (cpuTextures.contains(use.texture_index) || source.availability == DrawCaptureAvailability::kComplete)
                        retirePage(source.guest_base_page);
                }
        }
        if (!makeSnapshotDraw(shaderValidation, geometryCache, event, registers, shaders, snapshotTextures, cpuTextures, decodedTextures, pageResources, resourceLayouts, resources, producerProofs, nativeDraw, target, error))
            return prefixEvent(error, event.id);
        const auto& recipe = nativeDraw.Recipe();
        localBytes         = preparation::Add(localBytes, admissionBytes(*nativeDraw.admittedRecipe));
        charge(sizeof(nativeDraw) + nativeDraw.admittedRecipe->MetadataBytes());
        if (recipe.shaderVersion && planningShaderVersions.insert(recipe.shaderVersion).second)
        {
            ownerBytes = preparation::Add(ownerBytes, preparation::Separate<NativeDrawReplayShaderVersion>());
            charge(sizeof(NativeDrawReplayShaderVersion));
        }
        const auto chargePayload = [&](const auto& owner, std::size_t bytes)
        {
            if (!owner || planningPayloads.insert(owner).second)
            {
                if (owner)
                    ownerBytes = preparation::Add(ownerBytes, preparation::SharedBytes(owner->capacity()));
                charge(bytes);
            }
        };
        chargePayload(recipe.sharedVertexShaderDxil, recipe.vertexShaderBytes().size());
        chargePayload(recipe.sharedPixelShaderDxil, recipe.pixelShaderBytes().size());
        chargePayload(recipe.sharedVertexData, recipe.vertexBytes().size());
        charge(recipe.samplers.size() * sizeof(NativeDrawReplaySampler));
        for (const auto bytes : { recipe.indexSpan().size() * sizeof(std::uint32_t), recipe.vertexConstants.size(), recipe.pixelConstants.size(), recipe.sharedConstants.size() })
            charge(bytes);
        for (const auto& texture : recipe.textures)
            if (!texture.frameTextureId)
            {
                // Generic shared payloads remain valid sequential inputs, but
                // their arbitrary control blocks have no bounded factory proof.
                if (texture.sharedBytes && !texture.textureVersion)
                    preparationOwnersKnown = false;
                if (texture.textureVersion && planningTextureVersions.insert(texture.textureVersion).second)
                {
                    ownerBytes = preparation::Add(ownerBytes, preparation::Add(preparation::Separate<NativeDrawReplayTextureVersion>(), preparation::Allocation<NativeDrawReplayTextureSubresource>(texture.textureVersion->SubresourceCapacity())));
                    charge(texture.textureVersion->MetadataBytes());
                }
                chargePayload(texture.sharedBytes, texture.payloadBytes().size());
            }
        const auto     newAlias        = attachmentAlias(registers);
        constexpr bool newAliasValid   = true;
        const auto     targetAsPass    = NativeRenderFramePass::Target{ target.width, target.height, target.sampleCount, target.format };
        const bool     samePendingCopy = pendingCopyPreserve && pendingCopyTarget == targetAsPass &&
                                         pendingCopyAliasValid && pendingCopyAlias == newAlias;
        if (!pendingCopyClears.empty() && !samePendingCopy && !materializePendingCopyClear(error))
            return false;
        if (!haveCurrent)
        {
            current                             = {};
            current.firstEvent                  = event.id;
            current.attachmentAlias             = newAlias;
            current.attachmentAliasValid        = newAliasValid;
            current.preservePreviousAttachments = samePendingCopy;
            current.target                      = samePendingCopy ? pendingCopyTarget : NativeRenderFramePass::Target{};
            if (samePendingCopy)
            {
                current.clears               = std::move(pendingCopyClears);
                current.clearAliasTransition = true;
                pendingCopyClears.clear();
            }
            current.draws.push_back(std::move(nativeDraw));
            current.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, event.id, 0 });
            currentTarget         = target;
            current.lastEvent     = event.id;
            haveCurrent           = true;
            pendingCopyPreserve   = false;
            pendingCopyTarget     = {};
            pendingCopyAlias      = 0;
            pendingCopyAliasValid = false;
            pendingCopyEvent      = 0;
            return true;
        }
        const auto changed = currentTarget.width != target.width || currentTarget.height != target.height ||
                             currentTarget.sampleCount != target.sampleCount ||
                             currentTarget.sampleMask != target.sampleMask || currentTarget.format != target.format ||
                             (current.attachmentAliasValid && newAliasValid && current.attachmentAlias != newAlias);
        if (changed)
        {
            if (!flush(error))
                return false;
            current                      = {};
            current.firstEvent           = event.id;
            current.attachmentAlias      = newAlias;
            current.attachmentAliasValid = newAliasValid;
            current.draws.push_back(std::move(nativeDraw));
            current.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, event.id, 0 });
            currentTarget     = target;
            current.lastEvent = event.id;
            haveCurrent       = true;
        }
        else
        {
            const auto drawIndex = static_cast<std::uint32_t>(current.draws.size());
            current.draws.push_back(std::move(nativeDraw));
            current.operations.push_back({ NativeGameplayRenderFrameOperation::Kind::Draw, event.id, drawIndex });
            current.lastEvent = event.id;
        }
        return true;
    }

    bool finishPasses(std::string& error)
    {
        if (havePendingTrigger)
            return fail(error, "gameplay resolve trigger has no copy event");
        if (!flush(error))
            return false;
        return pendingCopyClears.empty() || materializePendingCopyClear(error);
    }

    bool finish(const DrawCaptureFrameEvent& terminal, const DrawCaptureFinalSwap& swap, NativeRenderFrame& frame, std::vector<NativeGameplayRetainedTexture>& nextRetained, std::string& error)
    {
        if (!finishPasses(error))
            return false;
        std::copy(swap.gamma.gamma_256.begin(), swap.gamma.gamma_256.end(), input.gammaTable.begin());
        const auto& finalRegisters = terminal.draw.draw.registers;
        if (finalRegisters.size() != kRegisterCount)
            return fail(error, "final swap has no complete texture fetch state");
        const auto fetchBase   = std::uint64_t(finalRegisters[0x4801] & 0xFFFFF000U);
        const auto fetchFormat = finalRegisters[0x4801] & 0x3FU;
        const auto fetchWidth  = (finalRegisters[0x4802] & 0x1FFFU) + 1U;
        const auto fetchHeight = ((finalRegisters[0x4802] >> 13) & 0x1FFFU) + 1U;
        if ((finalRegisters[0x4800] & 3U) != 2U || fetchFormat != 6 || fetchBase != swap.frontbuffer_ptr ||
            fetchWidth != swap.output_width ||
            fetchHeight != swap.output_height)
            return fail(error, "final swap texture fetch does not match the presented RGBA8 resource");
        for (std::size_t component = 0; component < input.outputSwizzle.size(); ++component)
            input.outputSwizzle[component] = static_cast<std::uint8_t>((finalRegisters[0x4803] >> (1U + 3U * component)) & 7U);
        if (std::any_of(input.outputSwizzle.begin(), input.outputSwizzle.end(), [](std::uint8_t component)
                        {
                            return component > 5;
                        }))
            return fail(error, "final swap texture fetch has an unsupported swizzle");
        const auto outputPage     = swap.frontbuffer_ptr / 0x1000U;
        const auto outputResource = pageResources.find(static_cast<std::uint32_t>(outputPage));
        if (outputResource == pageResources.end())
            return fail(error, "final swap frontbuffer has no native resolve producer");
        const auto resource = resources.find(outputResource->second);
        if (resource == resources.end() || resource->second.width != swap.output_width ||
            resource->second.height != swap.output_height ||
            resource->second.format != NativeDrawReplayTextureFormat::Rgba8)
            return fail(error, "final swap frontbuffer producer has incompatible native dimensions or format");
        input.outputTextureId = outputResource->second;
        std::vector<NativeGameplayRetainedTexture> completed;
        if (trackRetained)
        {
            const auto retain = [&](std::uint32_t page, std::uint64_t id)
            {
                auto& texture = resources.at(id);
                if (!texture.imported)
                    texture.persistentKey = copyVersions.at(texture.id);
                auto imported     = texture;
                imported.imported = true;
                const auto layout = resourceLayouts.find(id);
                completed.push_back({ page, imported, layout == resourceLayouts.end() ? NativeGameplayGuestTextureLayout{} : layout->second });
            };
            if (retainAll)
            {
                if (pageResources.size() > NativeRenderBatchCommands)
                    return fail(error, "gameplay retention exceeds the resource identity bound");
                for (const auto& [page, id] : pageResources)
                    retain(page, id);
            }
            else
                for (const auto page : requestedPages)
                    if (const auto found = pageResources.find(page); found != pageResources.end())
                        retain(page, found->second);
        }
        input.resources.clear();
        for (const auto& [id, resource] : resources)
            input.resources.push_back({ resource, true });
        if (!BuildNativeGameplayRenderFrame(std::move(input), frame, error) || !continuePrefixChain(frame, error))
            return false;
        nextRetained = std::move(completed);
        return true;
    }

    // The renderer holds the batched passes the prefix packets recorded, so
    // the terminal continues that chain by count. Its full resource list
    // seals the keys the packets left zero.
    bool continuePrefixChain(NativeRenderFrame& frame, std::string& error) const
    {
        return !emittedBatchedPasses || SplitNativeRenderFrameTail(frame, emittedBatchedPasses, error);
    }

    bool finishProducer(std::uint32_t producerPage, std::uint64_t producerEvent, NativeRenderFrame& frame, std::string& error)
    {
        if (!finishPasses(error))
            return false;
        const auto found = pageResources.find(producerPage);
        if (found == pageResources.end())
            return fail(error, "producer snapshot did not create the requested texture");
        if (!copyVersions.contains(found->second) || copyVersions.at(found->second) != producerEvent)
            return fail(error, "producer snapshot has no resolve to the requested page");
        input.outputTextureId                     = found->second;
        resources.at(found->second).persistentKey = producerEvent;
        input.retainOnly                          = true;
        input.returnResolvedOutput                = false;
        input.resources.clear();
        for (const auto& [id, resource] : resources)
            input.resources.push_back({ resource, true });
        return BuildNativeGameplayRenderFrame(std::move(input), frame, error) && continuePrefixChain(frame, error);
    }
};

NativeGameplayCommandFeed::NativeGameplayCommandFeed(std::span<const NativeGameplayShaderInterface> shaders,
                                                     NativeGameplayGeometryCache*                   geometryCache)
: impl(std::make_unique<Impl>(shaders, geometryCache))
{
}

NativeGameplayCommandFeed::NativeGameplayCommandFeed(std::span<const NativeGameplayShaderInterface> shaders, NativeGameplayGeometryCache* geometryCache, NativeGameplayPreparationContext& context)
{
    const bool  deferred = context.state == NativeGameplayPreparationStatus::Pending;
    std::string error;
    std::size_t cacheBudget = 0;
    bool        active      = false;
    if (!Impl::fund(&context, geometryCache, 0, Impl::forecastConstruction(), cacheBudget, active, error))
        throw std::runtime_error(error);
    impl                     = std::make_unique<Impl>(shaders, geometryCache);
    impl->preparationContext = &context;
    impl->preparationFeed    = true;
    impl->prefixDeferred     = deferred;
}

NativeGameplayCommandFeed::~NativeGameplayCommandFeed()
{
    std::string error;
    impl->retirePrefixes(error, true);
}

void NativeGameplayCommandFeed::SetPrefixConsumer(PrefixConsumer consumer, PrefixRecording mode)
{
    if (impl->preparationFeed && impl->prefixDeferred && (impl->recording || !impl->preparationSucceeded))
        consumer = {};
    impl->prefixConsumer      = std::move(consumer);
    impl->prefixQueueConsumer = {};
    impl->prefixRecording     = mode;
}

void NativeGameplayCommandFeed::SetPrefixQueueConsumer(PrefixQueueConsumer consumer, PrefixRecording mode)
{
    if (impl->preparationFeed && impl->prefixDeferred && (impl->recording || !impl->preparationSucceeded))
        consumer = {};
    impl->prefixQueueConsumer = std::move(consumer);
    impl->prefixConsumer      = {};
    impl->prefixRecording     = mode;
}

bool NativeGameplayCommandFeed::Begin(std::uint64_t startFrame, std::uint64_t startSubmission, std::span<const NativeGameplayRetainedTexture> retained, std::string& error, std::span<const std::uint32_t> retainPages, bool retainAll, bool trackRetained)
{
    error.clear();
    if (!impl->retirePrefixes(error, true))
        return false;
    if (impl->recording)
        return fail(error, "native command feed still owns an unfinished frame");
    try
    {
        if (impl->preparationContext && !impl->fund(impl->forecastBegin(retained.size(), retainPages.size(), error), error))
            return false;
        auto next                 = std::make_unique<Impl>(impl->shaders, impl->reusableGeometry);
        next->preparationContext  = impl->preparationContext;
        next->preparationFeed     = impl->preparationFeed;
        next->prefixDeferred      = impl->preparationFeed && impl->prefixDeferred;
        next->reservationActive   = impl->reservationActive;
        next->activeCacheBudget   = impl->activeCacheBudget;
        next->shaderValidation    = std::move(impl->shaderValidation);
        next->copyState           = impl->copyState;
        next->prefixConsumer      = std::move(impl->prefixConsumer);
        next->prefixQueueConsumer = std::move(impl->prefixQueueConsumer);
        next->prefixRecording     = impl->prefixRecording;
        impl                      = std::move(next);
        if (retained.size() > NativeRenderBatchCommands || retainPages.size() > NativeRenderBatchCommands)
            return fail(error, "gameplay retention exceeds the resource identity bound");
        impl->startFrame = impl->maximumFrame = startFrame;
        impl->startSubmission = impl->maximumSubmission = startSubmission;
        impl->retained.assign(retained.begin(), retained.end());
        impl->retainAll         = retainAll;
        impl->trackRetained     = trackRetained;
        impl->recording         = impl->begin(error, retainPages);
        impl->reservationActive = false;
        return impl->recording;
    }
    catch (const std::exception& exception)
    {
        impl->recording = false;
        error           = exception.what();
        return false;
    }
}

bool NativeGameplayCommandFeed::Begin(std::uint64_t startFrame, std::uint64_t startSubmission, std::span<const NativeGameplayRetainedTexture> retained, std::string& error, NativeGameplayPreparationContext& context)
{
    impl->preparationContext = &context;
    impl->preparationFeed    = true;
    impl->prefixDeferred     = context.state == NativeGameplayPreparationStatus::Pending;
    if (impl->prefixDeferred)
    {
        impl->prefixConsumer      = {};
        impl->prefixQueueConsumer = {};
    }
    return Begin(startFrame, startSubmission, retained, error);
}

std::size_t NativeGameplayCommandFeed::CopyStateBytes() noexcept
{
    return sizeof(ValidatedCopyState);
}

const NativeGameplayRenderFrameCopy* NativeGameplayCommandFeed::Copy(std::uint64_t eventId) const noexcept
{
    for (const auto& pass : impl->input.passes)
        for (const auto& copy : pass.copies)
            if (copy.eventId == eventId)
                return &copy;
    for (const auto& copy : impl->current.copies)
        if (copy.eventId == eventId)
            return &copy;
    return nullptr;
}

const NativeGameplayRenderFrameDraw* NativeGameplayCommandFeed::Draw(std::uint64_t eventId) const noexcept
{
    for (const auto& pass : impl->input.passes)
        for (const auto& draw : pass.draws)
            if (draw.eventId == eventId)
                return &draw;
    for (const auto& draw : impl->current.draws)
        if (draw.eventId == eventId)
            return &draw;
    return nullptr;
}

bool NativeGameplayCommandFeed::Consume(const DrawCaptureFrameEvent&                            event,
                                        std::span<const DrawCaptureTexture>                     textures,
                                        const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                                        std::string&                                            error)
{
    return Consume(event, event.draw.draw.registers, textures, cpuTextures, error);
}

bool NativeGameplayCommandFeed::Consume(const DrawCaptureFrameEvent&                            event,
                                        std::span<const std::uint32_t>                          registers,
                                        std::span<const DrawCaptureTexture>                     textures,
                                        const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                                        std::string&                                            error)
{
    return ConsumeEvent(event, registers, textures, cpuTextures, error, false);
}

bool NativeGameplayCommandFeed::ConsumePreparedDraw(const DrawCaptureFrameEvent&                            event,
                                                    std::span<const DrawCaptureTexture>                     textures,
                                                    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                                                    std::string&                                            error)
{
    return ConsumePreparedDraw(event, event.draw.draw.registers, textures, cpuTextures, error);
}

bool NativeGameplayCommandFeed::ConsumePreparedDraw(const DrawCaptureFrameEvent&                            event,
                                                    std::span<const std::uint32_t>                          registers,
                                                    std::span<const DrawCaptureTexture>                     textures,
                                                    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                                                    std::string&                                            error)
{
    return ConsumeEvent(event, registers, textures, cpuTextures, error, true);
}

bool NativeGameplayCommandFeed::ConsumePreparedCopy(const DrawCaptureFrameEvent&                            event,
                                                    std::span<const DrawCaptureTexture>                     textures,
                                                    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                                                    std::string&                                            error)
{
    return ConsumePreparedCopy(event, event.draw.draw.registers, textures, cpuTextures, error);
}

bool NativeGameplayCommandFeed::ConsumePreparedCopy(const DrawCaptureFrameEvent&                            event,
                                                    std::span<const std::uint32_t>                          registers,
                                                    std::span<const DrawCaptureTexture>                     textures,
                                                    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                                                    std::string&                                            error)
{
    return ConsumeEvent(event, registers, textures, cpuTextures, error, true, true);
}

bool NativeGameplayCommandFeed::ConsumePreparedBoundary(const DrawCaptureFrameEvent&   event,
                                                        std::span<const std::uint32_t> registers,
                                                        std::string&                   error)
{
    return ConsumeEvent(event, registers, {}, {}, error, true, false, true);
}

bool NativeGameplayCommandFeed::ConsumeEvent(const DrawCaptureFrameEvent&                            event,
                                             std::span<const std::uint32_t>                          registers,
                                             std::span<const DrawCaptureTexture>                     textures,
                                             const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
                                             std::string&                                            error,
                                             bool                                                    prepared,
                                             bool                                                    preparedCopy,
                                             bool                                                    preparedBoundary)
{
    error.clear();
    if (!impl->recording)
    {
        if (!impl->prefixFailure.empty())
        {
            error = impl->prefixFailure;
            return false;
        }
        return fail(error, "native command feed has no recording frame");
    }
    try
    {
        const bool preparedMainLeftClear = preparedCopy && isPreparedMainLeftClearEvent(event, registers);
        if (prepared && (event.kind != (preparedBoundary ? DrawCaptureFrameEvent::Kind::kSwap : preparedCopy ? DrawCaptureFrameEvent::Kind::kCopy
                                                                                                             : DrawCaptureFrameEvent::Kind::kDraw) ||
                         event.success || event.host_issued ||
                         event.execution_status != DrawCaptureExecutionStatus::kUnfinished || event.execution_reason != DrawCaptureReason::kNone ||
                         event.acquisition != DrawCaptureAvailability::kComplete || event.acquisition_reason != DrawCaptureReason::kNone ||
                         event.draw.acquisition != DrawCaptureAvailability::kComplete || event.draw.acquisition_reason != DrawCaptureReason::kNone ||
                         !event.draw.texture_bindings_complete ||
                         (preparedBoundary ? !event.copy_mode.empty() || !event.draw.texture_uses.empty() || !event.draw.draw.vertex_fetches.empty()
                          : preparedCopy   ? event.copy_mode != "resolve" || !event.draw.texture_uses.empty() || event.draw.draw.vertex_fetches.size() != 1 ||
                                                 event.draw.draw.vertex_fetches[0].fetch_constant != 0 || event.draw.draw.vertex_fetches[0].availability != DrawCaptureAvailability::kComplete ||
                                                 event.draw.draw.vertex_fetches[0].Payload().size() != 24 ||
                                                 std::uint64_t(event.draw.draw.vertex_fetches[0].base) + 24 > 0x20000000ULL ||
                                                 (registers.size() == kRegisterCount && (registers[0x2208] != 6 || !registers[0x2319] || registers[0x2319] >= 0x20000000U ||
                                                                                         ((registers[0x2318] >> 20) & 3U) > 1 ||
                                                                                         ((registers[0x2318] & 7U) != 0 && (registers[0x2318] & 7U) != 4) ||
                                                                                         (registers[0x231B] & (1U << 3)) ||
                                                                                         ((registers[0x2318] & (kCopyColorClearBit | kCopyDepthClearBit)) && !preparedMainLeftClear)))
                                           : !event.copy_mode.empty() || (registers.size() == kRegisterCount && registers[0x2208] == 6))))
        {
            impl->recording = false;
            return fail(error, "prepared gameplay event has invalid provenance, copy mode or inputs");
        }
        if (preparedMainLeftClear && !impl->validatePreparedMainLeftClearAttachment(registers, error))
        {
            impl->recording = false;
            return false;
        }
        if (prepared && !preparedCopy && !preparedBoundary)
            for (const auto& use : event.draw.texture_uses)
                if (use.required)
                {
                    const bool nativeSource =
                        !use.data_available && use.availability == DrawCaptureAvailability::kUnavailable &&
                        use.unavailable_reason == DrawCaptureReason::kTextureUnresolved &&
                        use.texture_index < textures.size() &&
                        textures[use.texture_index].availability == DrawCaptureAvailability::kUnavailable &&
                        textures[use.texture_index].unavailable_reason == DrawCaptureReason::kTextureUnresolved;
                    const bool validIndex = use.texture_index < textures.size();
                    if (validIndex && !nativeSource && cpuTextures.contains(use.texture_index) &&
                        isPreparedCpuCube(textures[use.texture_index], use, cpuTextures.at(use.texture_index), registers))
                        continue;
                    const bool selectedFloat = validIndex && use.fetch_constant < 32 && registers.size() == kRegisterCount &&
                                               textures[use.texture_index].guest_format == kRg16FloatFormat &&
                                               textures[use.texture_index].guest_width == kRg16FloatWidth &&
                                               textures[use.texture_index].guest_height == kRg16FloatHeight &&
                                               textures[use.texture_index].guest_pitch == kRg16FloatPitch &&
                                               textures[use.texture_index].guest_tiled && !textures[use.texture_index].guest_packed_mips &&
                                               textures[use.texture_index].guest_depth_or_array_size == 1 &&
                                               textures[use.texture_index].mip_level == 0 && textures[use.texture_index].host_mip_levels == 1 &&
                                               textures[use.texture_index].host_width == kRg16FloatWidth &&
                                               textures[use.texture_index].host_height == kRg16FloatHeight &&
                                               textures[use.texture_index].host_depth_or_array_size == 1 &&
                                               textures[use.texture_index].host_format == 33 && use.is_signed &&
                                               use.swizzled_signs == kRg16FloatSigns && use.component_mapping == kRg16FloatMapping &&
                                               ((registers[0x4800 + use.fetch_constant * 6] >> 2) & 0xFFU) == kRg16FloatSigns &&
                                               ((registers[0x4803 + use.fetch_constant * 6] >> 1) & 0xFFFU) == kRg16FloatRawSwizzle;
                    const bool invalid =
                        !validIndex || use.dimension != 1 || use.fetch_constant >= 32 ||
                        registers.size() != kRegisterCount ||
                        (registers.size() == kRegisterCount &&
                         ((registers[0x4800 + use.fetch_constant * 6] & 3U) != 2 ||
                          ((registers[0x4800 + use.fetch_constant * 6 + 5] >> 9) & 3U) != 1 ||
                          (registers[0x4800 + use.fetch_constant * 6 + 1] & (1U << 10)) ||
                          (registers[0x4800 + use.fetch_constant * 6 + 2] >> 26))) ||
                        (validIndex && (nativeSource ? false
                                                     : (!use.data_available || use.availability != DrawCaptureAvailability::kComplete ||
                                                        use.unavailable_reason != DrawCaptureReason::kNone ||
                                                        !cpuTextures.contains(use.texture_index) ||
                                                        cpuTextures.at(use.texture_index).frameTextureId ||
                                                        textures[use.texture_index].guest_depth_or_array_size != 1 ||
                                                        cpuTextures.at(use.texture_index).dimension != NativeDrawReplayTextureDimension::Texture2D ||
                                                        (cpuTextures.at(use.texture_index).textureVersion &&
                                                         cpuTextures.at(use.texture_index).textureVersion->Layout().dimension != NativeDrawReplayTextureDimension::Texture2D) ||
                                                        cpuTextures.at(use.texture_index).payloadBytes().empty()))) ||
                        (validIndex && nativeSource && (textures[use.texture_index].guest_depth_or_array_size != 1 || (!selectedFloat && (use.is_signed || use.swizzled_signs != 0)) || cpuTextures.contains(use.texture_index))) ||
                        (validIndex && textures[use.texture_index].unavailable_reason == DrawCaptureReason::kTextureUnresolved &&
                         !nativeSource);
                    if (invalid)
                    {
                        impl->recording = false;
                        return fail(error, nativeSource ? "prepared gameplay draw has no exact native texture binding" : "prepared gameplay draw has no complete CPU texture binding");
                    }
                }
        if (impl->preparationContext && !impl->fund(impl->forecastConsume(event, registers, textures, cpuTextures, error, prepared), error))
        {
            impl->recording = false;
            return false;
        }
        impl->recording         = impl->consume(event, registers, textures, cpuTextures, error, prepared);
        impl->reservationActive = false;
        return impl->recording;
    }
    catch (const std::exception& exception)
    {
        impl->recording = false;
        error           = exception.what();
        return false;
    }
}

bool NativeGameplayCommandFeed::Finish(const DrawCaptureFrameEvent&                terminal,
                                       const DrawCaptureFinalSwap&                 swap,
                                       std::uint64_t                               endFrame,
                                       std::uint64_t                               endSubmission,
                                       NativeRenderFrame&                          frame,
                                       std::vector<NativeGameplayRetainedTexture>& nextRetained,
                                       std::string&                                error)
{
    return FinishFrame(terminal, swap, endFrame, endSubmission, frame, nextRetained, error, false);
}

bool NativeGameplayCommandFeed::FinishPrepared(const DrawCaptureFrameEvent&                terminal,
                                               const DrawCaptureFinalSwap&                 swap,
                                               std::uint64_t                               endFrame,
                                               std::uint64_t                               endSubmission,
                                               NativeRenderFrame&                          frame,
                                               std::vector<NativeGameplayRetainedTexture>& nextRetained,
                                               std::string&                                error)
{
    return FinishFrame(terminal, swap, endFrame, endSubmission, frame, nextRetained, error, true);
}

bool NativeGameplayCommandFeed::FinishFrame(const DrawCaptureFrameEvent&                terminal,
                                            const DrawCaptureFinalSwap&                 swap,
                                            std::uint64_t                               endFrame,
                                            std::uint64_t                               endSubmission,
                                            NativeRenderFrame&                          frame,
                                            std::vector<NativeGameplayRetainedTexture>& nextRetained,
                                            std::string&                                error,
                                            bool                                        prepared)
{
    error.clear();
    frame = {};
    nextRetained.clear();
    impl->preparationSucceeded = false;
    if (impl->preparationContext && !impl->preparationContext->Settle(true, error))
    {
        impl->recording = false;
        return false;
    }
    if (!impl->retirePrefixes(error, true))
    {
        impl->recording = false;
        return false;
    }
    if (!impl->recording)
        return fail(error, "native command feed has no recording frame");
    impl->recording = false;
    try
    {
        if (impl->startFrame >= endFrame || impl->maximumFrame >= endFrame ||
            impl->maximumSubmission > endSubmission || impl->startSubmission > endSubmission ||
            terminal.kind != DrawCaptureFrameEvent::Kind::kSwap || !terminal.id || terminal.id <= impl->previousEvent ||
            terminal.frame < impl->startFrame || terminal.frame >= endFrame ||
            terminal.submission < impl->startSubmission || terminal.submission > endSubmission ||
            (prepared ? terminal.success || terminal.host_issued || terminal.execution_status != DrawCaptureExecutionStatus::kUnfinished ||
                            terminal.execution_reason != DrawCaptureReason::kNone || terminal.acquisition != DrawCaptureAvailability::kComplete ||
                            terminal.acquisition_reason != DrawCaptureReason::kNone || terminal.draw.acquisition != DrawCaptureAvailability::kComplete ||
                            terminal.draw.acquisition_reason != DrawCaptureReason::kNone || !terminal.draw.texture_bindings_complete ||
                            !terminal.copy_mode.empty() || !terminal.draw.texture_uses.empty() || !terminal.draw.draw.vertex_fetches.empty()
                      : !terminal.success || (terminal.execution_status != DrawCaptureExecutionStatus::kIssued && terminal.execution_status != DrawCaptureExecutionStatus::kNoEffect)) ||
            terminal.draw.draw.registers.size() != kRegisterCount ||
            terminal.frontbuffer_ptr != swap.frontbuffer_ptr || swap.gamma.gamma_256.size() != 256 ||
            swap.gamma.use_pwl_gamma_ramp || swap.gamma.use_fxaa || !swap.gamma.is_8bpc ||
            (swap.output_format != 0 && swap.output_format != 24) ||
            swap.output_width != 1280 || swap.output_height != 720)
            return fail(error, "incomplete gameplay frame boundary or unsupported presentation state");
        impl->charge(sizeof(NativeGameplayRenderFramePass) + sizeof(NativeGameplayRenderFrameOperation));
        impl->preparationSucceeded = impl->finish(terminal, swap, frame, nextRetained, error);
        if (!impl->preparationSucceeded)
        {
            frame = {};
            nextRetained.clear();
        }
        return impl->preparationSucceeded;
    }
    catch (const std::exception& exception)
    {
        frame = {};
        nextRetained.clear();
        error = exception.what();
        return false;
    }
}

bool NativeGameplayCommandFeed::FinishProducer(std::uint32_t page, std::uint64_t producerEvent, NativeRenderFrame& frame, std::string& error)
{
    error.clear();
    if (impl->preparationContext && !impl->preparationContext->Settle(true, error))
    {
        impl->recording = false;
        return false;
    }
    frame = {};
    if (!impl->retirePrefixes(error, true))
    {
        impl->recording = false;
        return false;
    }
    if (!impl->recording)
        return fail(error, "native command feed has no recording frame");
    impl->recording = false;
    try
    {
        if (!page || page >= 0x20000 || !producerEvent || producerEvent != impl->previousEvent)
            return fail(error, "producer snapshot has no resolve to the requested page");
        return impl->finishProducer(page, producerEvent, frame, error);
    }
    catch (const std::exception& exception)
    {
        frame = {};
        error = exception.what();
        return false;
    }
}

NativeGameplayPreparationReceipt NativeGameplayCommandFeed::PreparationReceipt(const NativeRenderFrame& frame, const std::vector<NativeGameplayRetainedTexture>& nextRetained)
{
    using namespace preparation;
    if (!impl->preparationSucceeded)
        return {};
    if (!SupportedAbi || impl->pendingTrigger.id || !impl->preparationOwnersKnown)
        return { false, 0, Vector(nextRetained), Basis };
    impl->prefixConsumer      = {};
    impl->prefixQueueConsumer = {};
    std::deque<Impl::PendingPrefix>().swap(impl->pendingPrefixes);
    std::size_t owners = 0;
    for (const auto& payload : impl->planningPayloads)
        owners = Add(owners, SharedBytes(payload->capacity()));
    for (const auto& version : impl->planningShaderVersions)
        owners = Add(owners, Separate<NativeDrawReplayShaderVersion>());
    for (const auto& version : impl->planningTextureVersions)
        owners = Add(owners, Add(Separate<NativeDrawReplayTextureVersion>(), Allocation<NativeDrawReplayTextureSubresource>(version->SubresourceCapacity())));
    impl->ownerBytes         = owners;
    impl->completedPassBytes = 0;
    std::size_t locals       = 0;
    for (const auto& pass : impl->input.passes)
    {
        impl->completedPassBytes = Add(impl->completedPassBytes, Impl::passBuffers(pass));
        for (const auto& copy : pass.copies)
            locals = Add(locals, Add(copy.admittedOperation ? Separate<NativeGameplayResolveAdmission>() : 0, Vector(copy.registers)));
    }
    auto terminal = Add(sizeof(frame), Add(Vector(frame.textures), Add(Vector(frame.passes), String(frame.outputPath.native()))));
    for (const auto& pass : frame.passes)
    {
        terminal = Add(terminal, Add(Vector(pass.clears), Add(Vector(pass.resolves), Add(Vector(pass.draws), Vector(pass.admittedDraws)))));
        for (const auto& admission : pass.admittedDraws)
            locals = Add(locals, Impl::admissionBytes(*admission));
        for (const auto& draw : pass.draws)
            locals = Add(locals, Impl::recipeBuffers(draw));
    }
    impl->localBytes   = locals;
    const auto bank    = Vector(nextRetained);
    const auto managed = Add(impl->managedBytes(), Add(terminal, bank));
    const auto bytes   = Add(std::max(impl->planningBytes, managed), impl->geometryCache.ManagedBytes());
    return { bytes != Unknown, bytes, bank, Basis };
}

template <class Snapshot>
static bool buildSnapshotRenderFrame(
    const Snapshot&                                         snapshot,
    std::span<const NativeGameplayShaderInterface>          shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
    NativeRenderFrame&                                      frame,
    std::string&                                            error,
    std::span<const NativeGameplayRetainedTexture>          retained,
    std::uint32_t                                           producerPage,
    std::span<const std::uint32_t>                          retainPages        = {},
    std::vector<NativeGameplayRetainedTexture>*             nextRetained       = nullptr,
    bool                                                    retainAll          = false,
    NativeGameplayGeometryCache*                            reusableGeometry   = nullptr,
    NativeGameplayCommandFeed::PrefixConsumer               prefixConsumer     = {},
    NativeGameplayCommandFeed::PrefixRecording              prefixRecording    = NativeGameplayCommandFeed::PrefixRecording::FirstPass,
    NativeGameplayCommandFeed::PrefixQueueConsumer          queueConsumer      = {},
    NativeGameplayPreparationContext*                       preparationContext = nullptr,
    NativeGameplayPreparationReceipt*                       receipt            = nullptr)
{
    error.clear();
    frame = {};
    try
    {
        const auto& swap = snapshot.final_swap;
        if (snapshot.schema_version != rex::graphics::diagnostic::kDrawCaptureFrameSchemaVersion ||
            snapshot.events.empty() || snapshot.start_frame >= snapshot.end_frame ||
            snapshot.start_submission > snapshot.end_submission || swap.gamma.gamma_256.size() != 256 ||
            swap.gamma.use_pwl_gamma_ramp || swap.gamma.use_fxaa || !swap.gamma.is_8bpc ||
            (swap.output_format != 0 && swap.output_format != 24) || swap.output_width != 1280 ||
            swap.output_height != 720 ||
            snapshot.events.back().kind != DrawCaptureFrameEvent::Kind::kSwap ||
            snapshot.events.back().frontbuffer_ptr != swap.frontbuffer_ptr)
            return fail(error, "incomplete gameplay frame boundary or unsupported presentation state");
        std::uint64_t producerEvent = 0;
        if (producerPage)
        {
            for (const auto& event : snapshot.events)
                if (event.kind == DrawCaptureFrameEvent::Kind::kCopy && event.copy_mode == "resolve" &&
                    event.draw.draw.registers.size() == kRegisterCount &&
                    event.draw.draw.registers[0x2319] == producerPage * 0x1000U)
                    producerEvent = event.id;
            if (!producerEvent)
                return fail(error, "producer snapshot has no resolve to the requested page");
        }
        // Unresolved feeds never construct allocating prefix captures.
        if (preparationContext && preparationContext->state == NativeGameplayPreparationStatus::Pending)
        {
            prefixConsumer = {};
            queueConsumer  = {};
        }
        std::optional<NativeGameplayCommandFeed> ownedFeed;
        if (preparationContext)
            ownedFeed.emplace(shaders, reusableGeometry, *preparationContext);
        else
            ownedFeed.emplace(shaders, reusableGeometry);
        auto& feed = *ownedFeed;
        if (queueConsumer)
            feed.SetPrefixQueueConsumer(std::move(queueConsumer), prefixRecording);
        else
            feed.SetPrefixConsumer(std::move(prefixConsumer), prefixRecording);
        if (!feed.Begin(snapshot.start_frame, snapshot.start_submission, retained, error, retainPages, retainAll, bool(nextRetained)))
            return false;
        const auto& finalEvent = snapshot.events.back();
        std::size_t ordinal    = 0;
        for (const auto& event : snapshot.events)
        {
            if (++ordinal == snapshot.events.size())
                break;
            if (event.frame >= snapshot.end_frame || event.submission > snapshot.end_submission)
                return fail(error, "invalid gameplay event order, completion or ownership boundary");
            if (!feed.Consume(event, snapshot.textures, cpuTextures, error))
                return false;
            if (event.id == producerEvent)
                return feed.FinishProducer(producerPage, producerEvent, frame, error);
        }
        std::vector<NativeGameplayRetainedTexture> completed;
        if (!feed.Finish(finalEvent, swap, snapshot.end_frame, snapshot.end_submission, frame, completed, error))
            return false;
        if (receipt)
            *receipt = feed.PreparationReceipt(frame, completed);
        if (nextRetained)
            *nextRetained = std::move(completed);
        return true;
    }
    catch (const std::exception& exception)
    {
        frame = {};
        error = exception.what();
        return false;
    }
}

bool BuildNativeGameplaySequenceFrame(
    const NativeGameplaySnapshot&                  snapshot,
    std::span<const NativeGameplayShaderInterface> shaders,
    std::span<const NativeGameplayRetainedTexture> retained,
    std::span<const std::uint32_t>                 retainPages,
    NativeRenderFrame&                             frame,
    std::vector<NativeGameplayRetainedTexture>&    nextRetained,
    std::string&                                   error)
{
    nextRetained.clear();
    return buildSnapshotRenderFrame(snapshot, shaders, {}, frame, error, retained, 0, retainPages, &nextRetained);
}

bool BuildNativeGameplayLiveFrame(
    const NativeGameplaySnapshot&                           snapshot,
    std::span<const NativeGameplayShaderInterface>          shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
    std::span<const NativeGameplayRetainedTexture>          retained,
    NativeRenderFrame&                                      frame,
    std::vector<NativeGameplayRetainedTexture>&             nextRetained,
    std::string&                                            error,
    NativeGameplayGeometryCache*                            geometryCache,
    NativeGameplayCommandFeed::PrefixConsumer               prefixConsumer,
    NativeGameplayCommandFeed::PrefixRecording              prefixRecording,
    NativeGameplayCommandFeed::PrefixQueueConsumer          queueConsumer)
{
    nextRetained.clear();
    return buildSnapshotRenderFrame(snapshot, shaders, cpuTextures, frame, error, retained, 0, {}, &nextRetained, true, geometryCache, std::move(prefixConsumer), prefixRecording, std::move(queueConsumer));
}

bool BuildNativeGameplayLiveFrame(const NativeGameplaySnapshot& snapshot, std::span<const NativeGameplayShaderInterface> shaders, const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures, std::span<const NativeGameplayRetainedTexture> retained, NativeRenderFrame& frame, std::vector<NativeGameplayRetainedTexture>& nextRetained, std::string& error, NativeGameplayPreparationContext& context, NativeGameplayPreparationReceipt& receipt, NativeGameplayGeometryCache* geometryCache, NativeGameplayCommandFeed::PrefixConsumer prefixConsumer, NativeGameplayCommandFeed::PrefixRecording prefixRecording, NativeGameplayCommandFeed::PrefixQueueConsumer queueConsumer)
{
    receipt = {};
    return buildSnapshotRenderFrame(snapshot, shaders, cpuTextures, frame, error, retained, 0, {}, &nextRetained, true, geometryCache, std::move(prefixConsumer), prefixRecording, std::move(queueConsumer), &context, &receipt);
}

bool BuildNativeGameplayRenderFrame(
    const rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot,
    std::span<const NativeGameplayShaderInterface>             shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>&    cpuTextures,
    NativeRenderFrame&                                         frame,
    std::string&                                               error,
    const NativeGameplayRetainedTexture*                       retained,
    NativeGameplayRetainedTexture*                             nextRetained)
{
    if (!buildSnapshotRenderFrame(snapshot, shaders, cpuTextures, frame, error, { retained, retained ? 1U : 0U }, 0))
        return false;
    if (nextRetained && retained)
    {
        *nextRetained            = *retained;
        std::uint64_t newestCopy = 0;
        for (const auto& texture : frame.textures)
            if (!texture.imported && texture.persistentKey > newestCopy)
            {
                newestCopy                     = texture.persistentKey;
                nextRetained->texture          = texture;
                nextRetained->texture.imported = true;
            }
    }
    return true;
}

bool BuildNativeGameplayProducer(
    const rex::graphics::diagnostic::DrawCaptureFrameSnapshot& snapshot,
    std::span<const NativeGameplayShaderInterface>             shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>&    cpuTextures,
    std::uint32_t                                              page,
    NativeRenderFrame&                                         frame,
    NativeGameplayRetainedTexture&                             retained,
    std::string&                                               error)
{
    retained = {};
    if (!page || page >= 0x20000 || !buildSnapshotRenderFrame(snapshot, shaders, cpuTextures, frame, error, {}, page))
        return false;
    const auto found          = std::find_if(frame.textures.begin(), frame.textures.end(), [&](const auto& texture)
                                             {
                                        return texture.id == frame.outputTextureId;
                                             });
    retained                  = { page, *found };
    retained.texture.imported = true;
    return true;
}

bool BuildNativeGameplayRenderFrame(
    const NativeGameplaySnapshot&                           snapshot,
    std::span<const NativeGameplayShaderInterface>          shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
    NativeRenderFrame&                                      frame,
    std::string&                                            error,
    const NativeGameplayRetainedTexture*                    retained,
    NativeGameplayRetainedTexture*                          nextRetained)
{
    if (!buildSnapshotRenderFrame(snapshot, shaders, cpuTextures, frame, error, { retained, retained ? 1U : 0U }, 0))
        return false;
    if (nextRetained && retained)
    {
        *nextRetained            = *retained;
        std::uint64_t newestCopy = 0;
        for (const auto& texture : frame.textures)
            if (!texture.imported && texture.persistentKey > newestCopy)
            {
                newestCopy                     = texture.persistentKey;
                nextRetained->texture          = texture;
                nextRetained->texture.imported = true;
            }
    }
    return true;
}

bool BuildNativeGameplayProducer(
    const NativeGameplaySnapshot&                           snapshot,
    std::span<const NativeGameplayShaderInterface>          shaders,
    const std::map<std::uint32_t, NativeDrawReplayTexture>& cpuTextures,
    std::uint32_t                                           page,
    NativeRenderFrame&                                      frame,
    NativeGameplayRetainedTexture&                          retained,
    std::string&                                            error)
{
    retained = {};
    if (!page || page >= 0x20000 || !buildSnapshotRenderFrame(snapshot, shaders, cpuTextures, frame, error, {}, page))
        return false;
    const auto found          = std::find_if(frame.textures.begin(), frame.textures.end(), [&](const auto& texture)
                                             {
                                        return texture.id == frame.outputTextureId;
                                             });
    retained                  = { page, *found };
    retained.texture.imported = true;
    return true;
}

} // namespace rerevved::gpu
