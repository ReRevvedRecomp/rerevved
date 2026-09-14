#include "native_menu_panel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <rex/crypto/sha256.h>

namespace rerevved::gpu
{
namespace
{

constexpr std::size_t   kMaxGeometryBytes = 16U * 1024U * 1024U;
constexpr std::size_t   kMaxEdramBytes    = 10U * 1024U * 1024U;
constexpr std::size_t   kEdramTileBytes   = 80U * 16U * 4U;
constexpr std::size_t   kEdramTileCount   = 2048U;
constexpr std::uint32_t kOutputWidth      = 640U;
constexpr std::uint32_t kOutputHeight     = 720U;
constexpr std::size_t   kOutputPlaneBytes =
    static_cast<std::size_t>(kOutputWidth) * kOutputHeight * 4U;
constexpr std::size_t kOutputCombinedBytes = 2U * kOutputPlaneBytes;

constexpr std::uint64_t kPanelVertexHash = 0x11213E38D7154104ULL;
constexpr std::uint64_t kPanelPixelHash  = 0x3A92D78FE55C7B83ULL;

constexpr std::string_view kVertexUcodeSha256 =
    "7a8473e246709b45e6895b7e20ebff38949e02da014ac760cfc6caa71d13cb77";
constexpr std::string_view kPixelUcodeSha256 =
    "f15792acab07e97384dbaff573da53b2641cedd0e4ecbf1c5f3e1b34d2171921";
constexpr std::string_view kVertexDxilSha256 =
    "3eee5d345eb1f1b858d073d77cb00e8296d0462eef164f44cac62c67018abd26";
constexpr std::string_view kPixelCoverageDxilSha256 =
    "f92f9c5b3d9c98bb399fbf2e92cc5d950a0fbd40de95f49c594e70b36f7d8b9a";

constexpr std::uint32_t kSurfaceInfoIndex       = 0x2000U;
constexpr std::uint32_t kColorInfoIndex         = 0x2001U;
constexpr std::uint32_t kWindowOffsetIndex      = 0x2080U;
constexpr std::uint32_t kWindowScissorTlIndex   = 0x2081U;
constexpr std::uint32_t kWindowScissorBrIndex   = 0x2082U;
constexpr std::uint32_t kScreenScissorTlIndex   = 0x200EU;
constexpr std::uint32_t kScreenScissorBrIndex   = 0x200FU;
constexpr std::uint32_t kVertexMaxIndex         = 0x2100U;
constexpr std::uint32_t kVertexMinIndex         = 0x2101U;
constexpr std::uint32_t kIndexOffsetIndex       = 0x2102U;
constexpr std::uint32_t kColorMaskIndex         = 0x2104U;
constexpr std::uint32_t kViewportXScaleIndex    = 0x210FU;
constexpr std::uint32_t kViewportXOffsetIndex   = 0x2110U;
constexpr std::uint32_t kViewportYScaleIndex    = 0x2111U;
constexpr std::uint32_t kViewportYOffsetIndex   = 0x2112U;
constexpr std::uint32_t kViewportZScaleIndex    = 0x2113U;
constexpr std::uint32_t kViewportZOffsetIndex   = 0x2114U;
constexpr std::uint32_t kProgramIndex           = 0x2180U;
constexpr std::uint32_t kContextMiscIndex       = 0x2181U;
constexpr std::uint32_t kInterpolatorIndex      = 0x2182U;
constexpr std::uint32_t kDmaSizeIndex           = 0x21FBU;
constexpr std::uint32_t kDrawInitiatorIndex     = 0x21FCU;
constexpr std::uint32_t kDepthControlIndex      = 0x2200U;
constexpr std::uint32_t kBlendControlIndex      = 0x2201U;
constexpr std::uint32_t kColorControlIndex      = 0x2202U;
constexpr std::uint32_t kClipControlIndex       = 0x2204U;
constexpr std::uint32_t kRasterControlIndex     = 0x2205U;
constexpr std::uint32_t kVteControlIndex        = 0x2206U;
constexpr std::uint32_t kEdramModeIndex         = 0x2208U;
constexpr std::uint32_t kPaSuVtxIndex           = 0x2302U;
constexpr std::uint32_t kVsConstantControlIndex = 0x2307U;
constexpr std::uint32_t kPsConstantControlIndex = 0x2308U;
constexpr std::uint32_t kFetchConstant95Index   = 0x48BEU;
constexpr std::uint32_t kSharedConstantsIndex   = 0x4900U;
constexpr std::uint32_t kVertexConstantIndex    = 0x4000U;
constexpr std::uint32_t kPixelConstantIndex     = 0x4400U;

constexpr std::uint32_t kExpectedSurfaceInfo   = 0x0A010280U;
constexpr std::uint32_t kExpectedColorControl  = 0x87000004U;
constexpr std::uint32_t kExpectedBlendControl  = 0x07060706U;
constexpr std::uint32_t kExpectedClipControl   = 0x00080000U;
constexpr std::uint32_t kExpectedRasterControl = 0x00018000U;
constexpr std::uint32_t kExpectedVteControl    = 0x0000043FU;
constexpr std::uint32_t kExpectedProgram       = 0x10010001U;
constexpr std::uint32_t kExpectedPaSuVtx       = 0x00000004U;
constexpr std::uint32_t kExpectedVsConstants   = 0x000FF000U;
constexpr std::uint32_t kExpectedPsConstants   = 0x000FF100U;

void fail(std::string& error, std::string_view message)
{
    error.assign(message.data(), message.size());
}

bool require(bool condition, std::string& error, std::string_view message)
{
    if (!condition)
    {
        fail(error, message);
        return false;
    }
    return true;
}

std::int32_t signExtend(std::uint32_t value, unsigned bits)
{
    const std::uint32_t sign = std::uint32_t{ 1 } << (bits - 1U);
    value &= (sign << 1U) - 1U;
    return static_cast<std::int32_t>((value ^ sign) - sign);
}

std::int32_t readSignedPair(std::uint32_t value, unsigned shift)
{
    return signExtend((value >> shift) & 0x7FFFU, 15U);
}

constexpr std::uint32_t floatBits(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

void appendLittleEndian(std::string& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<char>(value & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

std::string sha256Bytes(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
    {
        return rex::crypto::sha256(std::string_view{});
    }
    return rex::crypto::sha256(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string sha256LittleEndianDwords(std::span<const std::uint32_t> dwords)
{
    std::string bytes;
    bytes.reserve(dwords.size() * sizeof(std::uint32_t));
    for (const auto value : dwords)
    {
        appendLittleEndian(bytes, value);
    }
    return rex::crypto::sha256(bytes);
}

bool validateRegisterFile(std::span<const std::uint32_t> registers,
                          std::string&                   error)
{
    return require(registers.size() == kNativeMenuPanelRegisterCount,
                   error,
                   "menu panel register file must contain exactly 0x5003 dwords");
}

bool validateEdramRegisters(std::span<const std::uint32_t> registers,
                            std::string&                   error)
{
    if (!validateRegisterFile(registers, error))
    {
        return false;
    }

    const auto surface = registers[kSurfaceInfoIndex];
    if (!require(surface == kExpectedSurfaceInfo,
                 error,
                 "unsupported menu panel surface or guest MSAA state"))
    {
        return false;
    }

    const auto color = registers[kColorInfoIndex];
    if (!require((color & ~0x7FFU) == 0,
                 error,
                 "unsupported menu panel EDRAM color format or base encoding"))
    {
        return false;
    }

    return require(registers[kEdramModeIndex] == 4U,
                   error,
                   "menu panel color comparison requires ROV EDRAM mode");
}

struct PanelScissor
{
    std::int32_t left   = 0;
    std::int32_t top    = 0;
    std::int32_t right  = 0;
    std::int32_t bottom = 0;
};

bool deriveScissor(std::span<const std::uint32_t> registers,
                   std::int32_t                   windowX,
                   std::int32_t                   windowY,
                   PanelScissor&                  result,
                   std::string&                   error)
{
    const auto windowTl = registers[kWindowScissorTlIndex];
    const auto windowBr = registers[kWindowScissorBrIndex];
    if (!require((windowTl & 0xC000C000U) == 0 && (windowBr & 0xC000C000U) == 0,
                 error,
                 "menu panel window scissor has unsupported reserved bits"))
    {
        return false;
    }
    if (!require((windowTl & (1U << 31U)) == 0,
                 error,
                 "menu panel window scissor offset must be enabled"))
    {
        return false;
    }

    const auto screenTl = registers[kScreenScissorTlIndex];
    const auto screenBr = registers[kScreenScissorBrIndex];
    if (!require((screenTl & 0x80008000U) == 0 && (screenBr & 0x80008000U) == 0,
                 error,
                 "menu panel screen scissor has unsupported reserved bits"))
    {
        return false;
    }

    const auto windowLeft  = static_cast<std::int64_t>(windowTl & 0x3FFFU) + windowX;
    const auto windowTop   = static_cast<std::int64_t>((windowTl >> 16U) & 0x3FFFU) + windowY;
    const auto windowRight = static_cast<std::int64_t>(windowBr & 0x3FFFU) + windowX;
    const auto windowBottom =
        static_cast<std::int64_t>((windowBr >> 16U) & 0x3FFFU) + windowY;
    const auto screenLeft   = static_cast<std::int64_t>(readSignedPair(screenTl, 0));
    const auto screenTop    = static_cast<std::int64_t>(readSignedPair(screenTl, 16));
    const auto screenRight  = static_cast<std::int64_t>(readSignedPair(screenBr, 0));
    const auto screenBottom = static_cast<std::int64_t>(readSignedPair(screenBr, 16));

    const auto left   = std::max<std::int64_t>({ windowLeft, screenLeft, 0 });
    const auto top    = std::max<std::int64_t>({ windowTop, screenTop, 0 });
    const auto right  = std::min<std::int64_t>({ windowRight, screenRight, static_cast<std::int64_t>(kOutputWidth) });
    const auto bottom = std::min<std::int64_t>({ windowBottom, screenBottom, static_cast<std::int64_t>(kOutputHeight) });
    if (!require(left == 0 && top == 0 && right == kOutputWidth &&
                     bottom == kOutputHeight,
                 error,
                 "menu panel scissor does not cover the demonstrated 640x720 output"))
    {
        return false;
    }

    result.left   = static_cast<std::int32_t>(left);
    result.top    = static_cast<std::int32_t>(top);
    result.right  = static_cast<std::int32_t>(right);
    result.bottom = static_cast<std::int32_t>(bottom);
    return true;
}

bool validatePanelRegisters(std::span<const std::uint32_t> registers,
                            std::uint32_t                  indexCount,
                            PanelScissor&                  scissor,
                            std::string&                   error)
{
    if (!validateEdramRegisters(registers, error))
    {
        return false;
    }

    if (!require(registers[kColorMaskIndex] == 0xFU,
                 error,
                 "menu panel color write mask must be RGBA"))
    {
        return false;
    }
    if (!require(registers[kDepthControlIndex] == 0x24F00770U,
                 error,
                 "menu panel depth and stencil state is unsupported"))
    {
        return false;
    }
    if (!require(registers[kBlendControlIndex] == kExpectedBlendControl &&
                     registers[kColorControlIndex] == kExpectedColorControl,
                 error,
                 "menu panel blend or alpha test state is unsupported"))
    {
        return false;
    }
    if (!require(registers[kClipControlIndex] == kExpectedClipControl &&
                     registers[kRasterControlIndex] == kExpectedRasterControl &&
                     registers[kVteControlIndex] == kExpectedVteControl,
                 error,
                 "menu panel clip, cull, or viewport transform state is unsupported"))
    {
        return false;
    }
    if (!require(registers[kProgramIndex] == kExpectedProgram &&
                     registers[kContextMiscIndex] == 0U &&
                     registers[kInterpolatorIndex] == 1U &&
                     registers[kPaSuVtxIndex] == kExpectedPaSuVtx,
                 error,
                 "menu panel shader interpolation state is unsupported"))
    {
        return false;
    }
    if (!require(registers[kIndexOffsetIndex] == 0U,
                 error,
                 "menu panel indexed draw has an unsupported index offset"))
    {
        return false;
    }
    if (!require(registers[kVertexMinIndex] == 0U &&
                     registers[kVertexMaxIndex] == 0x00FFFFFFU,
                 error,
                 "menu panel vertex range state is unsupported"))
    {
        return false;
    }

    const auto draw = registers[kDrawInitiatorIndex];
    if (!require((draw & 0x0000FFFFU) == 4U && (draw >> 16U) == indexCount,
                 error,
                 "menu panel draw must be an indexed triangle list"))
    {
        return false;
    }
    const auto dma = registers[kDmaSizeIndex];
    if (!require((dma & 0xFF000000U) == 0x40000000U &&
                     (dma & 0x00FFFFFFU) == indexCount,
                 error,
                 "menu panel index DMA endian or count is unsupported"))
    {
        return false;
    }

    const auto fetch     = registers[kFetchConstant95Index];
    const auto fetchInfo = registers[kFetchConstant95Index + 1U];
    if (!require((fetch & 3U) == 3U && (fetchInfo & 3U) == 2U,
                 error,
                 "menu panel vertex fetch type or endian is unsupported"))
    {
        return false;
    }
    if (!require(registers[kVsConstantControlIndex] == kExpectedVsConstants &&
                     registers[kPsConstantControlIndex] == kExpectedPsConstants,
                 error,
                 "menu panel constant register windows are unsupported"))
    {
        return false;
    }

    constexpr std::array<std::uint32_t, 6> kViewportBits = {
        floatBits(640.0F),
        floatBits(640.0F),
        floatBits(-360.0F),
        floatBits(360.0F),
        floatBits(0.0F),
        floatBits(0.0F),
    };
    const std::array<std::uint32_t, 6> viewport = {
        registers[kViewportXScaleIndex],
        registers[kViewportXOffsetIndex],
        registers[kViewportYScaleIndex],
        registers[kViewportYOffsetIndex],
        registers[kViewportZScaleIndex],
        registers[kViewportZOffsetIndex],
    };
    if (!require(viewport == kViewportBits,
                 error,
                 "menu panel raw viewport does not match the demonstrated transform"))
    {
        return false;
    }

    const auto windowOffset = registers[kWindowOffsetIndex];
    if (!require((windowOffset & 0x80008000U) == 0,
                 error,
                 "menu panel window offset has unsupported reserved bits"))
    {
        return false;
    }
    const auto windowX = readSignedPair(windowOffset, 0);
    const auto windowY = readSignedPair(windowOffset, 16);
    if (!require(windowY == 0 && (windowX == 0 || windowX == -640),
                 error,
                 "menu panel window offset is outside the admitted positions"))
    {
        return false;
    }

    return deriveScissor(registers, windowX, windowY, scissor, error);
}

bool decodeIndexStream(const NativeMenuPanelView&  view,
                       std::vector<std::uint32_t>& indices,
                       std::uint32_t&              maxIndex,
                       std::string&                error)
{
    if (!require(view.indexFormat == 0U && view.indexEndian == 1U,
                 error,
                 "menu panel indices must be big endian uint16 values"))
    {
        return false;
    }
    if (!require(view.indexCount >= 3U && (view.indexCount % 3U) == 0U,
                 error,
                 "menu panel index count must be a triangle-list multiple"))
    {
        return false;
    }
    const auto indexBytes = static_cast<std::uint64_t>(view.indexCount) * 2ULL;
    if (!require(indexBytes <= kMaxGeometryBytes && view.indexBytes.size() == indexBytes,
                 error,
                 "menu panel index bytes are missing or exceed the geometry bound"))
    {
        return false;
    }

    indices.resize(view.indexCount);
    maxIndex = 0;
    for (std::uint32_t index = 0; index < view.indexCount; ++index)
    {
        const auto offset = static_cast<std::size_t>(index) * 2U;
        const auto value  = (static_cast<std::uint32_t>(view.indexBytes[offset]) << 8U) |
                            view.indexBytes[offset + 1U];
        indices[index]    = value;
        maxIndex          = std::max(maxIndex, value);
    }
    return true;
}

void writeLittleEndian(std::vector<std::uint8_t>& bytes,
                       std::size_t                offset,
                       std::uint32_t              value)
{
    bytes[offset]      = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    bytes[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

bool decodeVertexStream(const NativeMenuPanelView& view,
                        std::uint32_t              maxIndex,
                        std::vector<std::uint8_t>& vertexData,
                        std::string&               error)
{
    const auto fetch       = view.registers[kFetchConstant95Index];
    const auto fetchInfo   = view.registers[kFetchConstant95Index + 1U];
    const auto guestBase   = static_cast<std::uint64_t>(fetch >> 2U) * 4ULL;
    const auto sourceBytes = static_cast<std::uint64_t>((fetchInfo >> 2U) & 0xFFFFFFU) * 4ULL;
    if (!require(view.vertexGuestBase == guestBase,
                 error,
                 "menu panel vertex guest base does not match fetch constant 95"))
    {
        return false;
    }
    if (!require(sourceBytes == view.vertexBytes.size() && sourceBytes <= kMaxGeometryBytes,
                 error,
                 "menu panel vertex fetch range is missing or exceeds the geometry bound"))
    {
        return false;
    }

    const auto vertexCount = static_cast<std::uint64_t>(maxIndex) + 1ULL;
    const auto sourceEnd   = (vertexCount - 1ULL) * 8ULL + 8ULL;
    const auto outputBytes = vertexCount * 2ULL * 16ULL;
    if (!require(sourceEnd <= sourceBytes && outputBytes <= kMaxGeometryBytes,
                 error,
                 "menu panel index references a vertex outside the fetched range"))
    {
        return false;
    }

    vertexData.resize(static_cast<std::size_t>(outputBytes));
    for (std::uint64_t vertex = 0; vertex < vertexCount; ++vertex)
    {
        for (std::uint32_t attribute = 0; attribute < 2U; ++attribute)
        {
            const auto           sourceOffset = static_cast<std::size_t>(vertex * 8ULL + attribute * 4ULL);
            const auto           swapped      = (static_cast<std::uint32_t>(view.vertexBytes[sourceOffset]) << 24U) |
                                                (static_cast<std::uint32_t>(view.vertexBytes[sourceOffset + 1U]) << 16U) |
                                                (static_cast<std::uint32_t>(view.vertexBytes[sourceOffset + 2U]) << 8U) |
                                                view.vertexBytes[sourceOffset + 3U];
            std::array<float, 4> values       = {};
            if (attribute == 0U)
            {
                values[0] = static_cast<float>(signExtend(swapped & 0xFFFFU, 16U));
                values[1] = static_cast<float>(signExtend((swapped >> 16U) & 0xFFFFU, 16U));
            }
            else
            {
                values[0] = static_cast<float>((swapped >> 0U) & 0xFFU) / 255.0F;
                values[1] = static_cast<float>((swapped >> 8U) & 0xFFU) / 255.0F;
                values[2] = static_cast<float>((swapped >> 16U) & 0xFFU) / 255.0F;
                values[3] = static_cast<float>((swapped >> 24U) & 0xFFU) / 255.0F;
            }
            const auto destinationOffset =
                static_cast<std::size_t>((vertex * 2ULL + attribute) * 16ULL);
            for (std::size_t component = 0; component < values.size(); ++component)
            {
                writeLittleEndian(vertexData,
                                  destinationOffset + component * sizeof(std::uint32_t),
                                  floatBits(values[component]));
            }
        }
    }
    return true;
}

void copyRegisterBytes(std::span<const std::uint32_t> registers,
                       std::uint32_t                  first,
                       std::size_t                    count,
                       std::vector<std::uint8_t>&     output,
                       std::size_t                    destinationOffset = 0)
{
    const auto requiredBytes = destinationOffset + count * sizeof(std::uint32_t);
    if (output.size() < requiredBytes)
    {
        output.resize(requiredBytes);
    }
    for (std::size_t index = 0; index < count; ++index)
    {
        writeLittleEndian(output,
                          destinationOffset + index * sizeof(std::uint32_t),
                          registers[first + index]);
    }
}

} // namespace

bool ValidateNativeMenuPanelShaders(std::span<const std::uint8_t> vertexDxil,
                                    std::span<const std::uint8_t> pixelDxil,
                                    std::string&                  outError)
{
    outError.clear();
    if (!require(!vertexDxil.empty() && !pixelDxil.empty() &&
                     vertexDxil.size() <= 4U * 1024U * 1024U &&
                     pixelDxil.size() <= 4U * 1024U * 1024U &&
                     (vertexDxil.size() % 4U) == 0U && (pixelDxil.size() % 4U) == 0U,
                 outError,
                 "menu panel DXIL is empty, unaligned, or exceeds the shader bound"))
    {
        return false;
    }
    if (!require(sha256Bytes(vertexDxil) == kVertexDxilSha256,
                 outError,
                 "menu panel vertex DXIL hash is unsupported"))
    {
        return false;
    }
    return require(sha256Bytes(pixelDxil) == kPixelCoverageDxilSha256,
                   outError,
                   "menu panel coverage pixel DXIL hash is unsupported");
}

bool DecodeNativeMenuPanelGeometry(const NativeMenuPanelView&  view,
                                   std::vector<std::uint8_t>&  outVertexData,
                                   std::vector<std::uint32_t>& outIndices,
                                   std::string&                outError)
{
    outVertexData.clear();
    outIndices.clear();
    outError.clear();
    if (!validateRegisterFile(view.registers, outError))
    {
        return false;
    }
    std::uint32_t maxIndex = 0;
    if (!decodeIndexStream(view, outIndices, maxIndex, outError))
    {
        return false;
    }
    if (!decodeVertexStream(view, maxIndex, outVertexData, outError))
    {
        outIndices.clear();
        return false;
    }
    return true;
}

bool DecodeNativeMenuPanelColor(std::span<const std::uint8_t>  edram,
                                std::span<const std::uint32_t> registers,
                                std::vector<std::uint8_t>&     outCombinedSamples,
                                std::string&                   outError)
{
    outCombinedSamples.clear();
    outError.clear();
    if (!validateEdramRegisters(registers, outError))
    {
        return false;
    }
    if (!require(edram.size() <= kMaxEdramBytes && edram.size() >= kEdramTileBytes,
                 outError,
                 "menu panel EDRAM snapshot is outside the bounded byte range"))
    {
        return false;
    }

    const auto surface     = registers[kSurfaceInfoIndex];
    const auto pitch       = surface & 0x3FFFU;
    const auto tilesPerRow = (pitch + 79U) / 80U;
    const auto base        = registers[kColorInfoIndex] & 0x7FFU;
    outCombinedSamples.resize(kOutputCombinedBytes);
    for (std::uint32_t sample = 0; sample < 2U; ++sample)
    {
        for (std::uint32_t y = 0; y < kOutputHeight; ++y)
        {
            const auto physicalY = y * 2U + sample;
            for (std::uint32_t x = 0; x < kOutputWidth; ++x)
            {
                const auto tile = (base + (physicalY / 16U) * tilesPerRow + x / 80U) %
                                  kEdramTileCount;
                const auto offset =
                    static_cast<std::size_t>(tile) * kEdramTileBytes +
                    static_cast<std::size_t>(physicalY % 16U) * 80U * 4U +
                    static_cast<std::size_t>(x % 80U) * 4U;
                if (!require(offset <= edram.size() - 4U,
                             outError,
                             "menu panel EDRAM mapping exceeds the supplied snapshot"))
                {
                    outCombinedSamples.clear();
                    return false;
                }
                const auto outputOffset =
                    (static_cast<std::size_t>(sample) * kOutputWidth * kOutputHeight +
                     static_cast<std::size_t>(y) * kOutputWidth + x) *
                    4U;
                std::copy_n(edram.data() + offset, 4U, outCombinedSamples.data() + outputOffset);
            }
        }
    }
    return true;
}

bool BuildNativeMenuPanelRecipe(const NativeMenuPanelView&    view,
                                std::span<const std::uint8_t> vertexDxil,
                                std::span<const std::uint8_t> pixelDxil,
                                std::span<const std::uint8_t> edramBefore,
                                NativeDrawReplayRecipe&       outRecipe,
                                std::string&                  outError)
{
    outRecipe = {};
    outError.clear();

    if (!require(view.vertexShaderHash == kPanelVertexHash &&
                     view.pixelShaderHash == kPanelPixelHash,
                 outError,
                 "menu panel shader hashes are unsupported"))
    {
        return false;
    }
    if (!require(view.vertexUcodeDwords.size() == 33U &&
                     view.pixelUcodeDwords.size() == 9U &&
                     sha256LittleEndianDwords(view.vertexUcodeDwords) == kVertexUcodeSha256 &&
                     sha256LittleEndianDwords(view.pixelUcodeDwords) == kPixelUcodeSha256,
                 outError,
                 "menu panel ucode hash or length is unsupported"))
    {
        return false;
    }
    if (!ValidateNativeMenuPanelShaders(vertexDxil, pixelDxil, outError))
    {
        return false;
    }
    if (!validateRegisterFile(view.registers, outError))
    {
        return false;
    }

    PanelScissor scissor;
    if (!validatePanelRegisters(view.registers, view.indexCount, scissor, outError))
    {
        return false;
    }

    std::vector<std::uint8_t>  vertexData;
    std::vector<std::uint32_t> indices;
    if (!DecodeNativeMenuPanelGeometry(view, vertexData, indices, outError))
    {
        return false;
    }

    std::vector<std::uint8_t> combinedSamples;
    if (!DecodeNativeMenuPanelColor(edramBefore, view.registers, combinedSamples, outError))
    {
        return false;
    }

    outRecipe.schemaVersion = 2;
    outRecipe.vertexShaderDxil.assign(vertexDxil.begin(), vertexDxil.end());
    outRecipe.pixelShaderDxil.assign(pixelDxil.begin(), pixelDxil.end());
    outRecipe.vertexData = std::move(vertexData);
    outRecipe.indices    = std::move(indices);
    copyRegisterBytes(view.registers, kVertexConstantIndex, 256U * 4U, outRecipe.vertexConstants);
    copyRegisterBytes(view.registers, kPixelConstantIndex, 224U * 4U, outRecipe.pixelConstants);
    outRecipe.sharedConstants.assign(336U, 0U);
    copyRegisterBytes(view.registers,
                      kSharedConstantsIndex,
                      8U,
                      outRecipe.sharedConstants,
                      256U);
    writeLittleEndian(outRecipe.sharedConstants, 292U, floatBits(1.0F / 1280.0F));
    writeLittleEndian(outRecipe.sharedConstants, 296U, floatBits(-1.0F / 720.0F));
    outRecipe.initialSample0.assign(combinedSamples.begin(),
                                    combinedSamples.begin() + kOutputPlaneBytes);
    outRecipe.initialSample1.assign(combinedSamples.begin() + kOutputPlaneBytes,
                                    combinedSamples.end());

    outRecipe.clearColor             = { 0, 0, 0, 0 };
    const auto windowOffset          = view.registers[kWindowOffsetIndex];
    outRecipe.viewport               = { static_cast<float>(readSignedPair(windowOffset, 0)),
                                         static_cast<float>(readSignedPair(windowOffset, 16)),
                                         1280.0F,
                                         720.0F,
                                         0.0F,
                                         0.0F };
    outRecipe.scissor                = { scissor.left, scissor.top, scissor.right, scissor.bottom };
    outRecipe.blend.enabled          = true;
    outRecipe.blend.alphaToCoverage  = false;
    outRecipe.blend.logicOpEnabled   = false;
    outRecipe.blend.sourceColor      = NativeDrawReplayBlendFactor::SourceAlpha;
    outRecipe.blend.destinationColor = NativeDrawReplayBlendFactor::InverseSourceAlpha;
    outRecipe.blend.colorOp          = NativeDrawReplayBlendOp::Add;
    outRecipe.blend.sourceAlpha      = NativeDrawReplayBlendFactor::SourceAlpha;
    outRecipe.blend.destinationAlpha = NativeDrawReplayBlendFactor::InverseSourceAlpha;
    outRecipe.blend.alphaOp          = NativeDrawReplayBlendOp::Add;
    outRecipe.blend.writeMask        = 0x0F;
    outRecipe.depth                  = {};
    outRecipe.rasterizer             = {};
    outRecipe.targetFormat           = NativeDrawReplayTargetFormat::Rgba8;
    outRecipe.width                  = kOutputWidth;
    outRecipe.height                 = kOutputHeight;
    outRecipe.sampleCount            = 4U;
    outRecipe.sampleMask             = 0x0FU;
    outRecipe.vertexStrideBytes      = 32U;
    outRecipe.vertexAttributeCount   = 2U;
    outRecipe.indexCount             = view.indexCount;
    outRecipe.textureMask            = 0U;
    outRecipe.vertexShaderHash       = view.vertexShaderHash;
    outRecipe.pixelShaderHash        = view.pixelShaderHash;

    if (!ValidateNativeDrawReplayRecipe(outRecipe, outError))
    {
        outRecipe = {};
        return false;
    }
    return true;
}

} // namespace rerevved::gpu
