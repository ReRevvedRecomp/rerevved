#include "main_menu_logo_asset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include <rex/logging.h>

namespace rerevved::main_menu_logo
{

namespace
{

constexpr uint32_t kDdsWidth  = 1024;
constexpr uint32_t kDdsHeight = 256;
constexpr size_t   kDdsHeader = 128;

std::shared_mutex selectionMutex;
Payload           selectedLogo;

uint32_t readLittleEndianU32(const uint8_t* value)
{
    return uint32_t{ value[0] } | (uint32_t{ value[1] } << 8) |
           (uint32_t{ value[2] } << 16) | (uint32_t{ value[3] } << 24);
}

void clearSelection()
{
    std::unique_lock lock(selectionMutex);
    selectedLogo = {};
}

void logResolutionProvenance(const rex::system::AssetOverlayResolution& resolved)
{
    REXLOG_INFO("Selected main-menu logo asset from package '{}'",
                resolved.package_id);
    for (const auto& shadowed : resolved.shadowed_package_ids)
    {
        REXLOG_INFO("Main-menu logo asset shadowed package '{}'", shadowed);
    }
}

} // namespace

bool IsValidLogoDds(std::span<const uint8_t> data)
{
    if (data.data() == nullptr || data.size() != kLogoDdsSize ||
        std::memcmp(data.data(), "DDS ", 4) != 0)
    {
        return false;
    }

    const auto* bytes = data.data();
    if (readLittleEndianU32(bytes + 4) != 124 ||
        readLittleEndianU32(bytes + 12) != kDdsHeight ||
        readLittleEndianU32(bytes + 16) != kDdsWidth ||
        readLittleEndianU32(bytes + 24) != 0 ||
        readLittleEndianU32(bytes + 28) != 0)
    {
        return false;
    }

    // A nonzero FourCC includes the DX10 extension and every compressed DDS
    // format. This title path accepts only the legacy uncompressed payload.
    if (readLittleEndianU32(bytes + 76) != 32 ||
        readLittleEndianU32(bytes + 80) != 0x41 ||
        readLittleEndianU32(bytes + 84) != 0 ||
        readLittleEndianU32(bytes + 88) != 32 ||
        readLittleEndianU32(bytes + 92) != 0x00ff0000 ||
        readLittleEndianU32(bytes + 96) != 0x0000ff00 ||
        readLittleEndianU32(bytes + 100) != 0x000000ff ||
        readLittleEndianU32(bytes + 104) != 0xff000000)
    {
        return false;
    }

    constexpr uint32_t kDdsCapsComplex = 0x00000008;
    constexpr uint32_t kDdsCapsMipmap  = 0x00400000;
    return (readLittleEndianU32(bytes + 108) &
            (kDdsCapsComplex | kDdsCapsMipmap)) == 0 &&
           readLittleEndianU32(bytes + 112) == 0;
}

std::vector<uint8_t> ScaleLogo(std::span<const uint8_t> dds, float scale)
{
    std::vector<uint8_t> scaled(dds.begin(), dds.end());
    if (!IsValidLogoDds(dds) || !(scale > 0.0f && scale < 1.0f))
    {
        return scaled;
    }

    const uint8_t* source = dds.data() + kDdsHeader;
    uint8_t*       target = scaled.data() + kDdsHeader;
    // Color weighted by alpha, so transparent texels do not darken the edges.
    const auto texel = [source](int32_t x, int32_t y) -> std::array<float, 4>
    {
        if (x < 0 || y < 0 || x >= static_cast<int32_t>(kDdsWidth) || y >= static_cast<int32_t>(kDdsHeight))
        {
            return {};
        }
        const uint8_t* pixel = source + (size_t{ kDdsWidth } * y + x) * 4;
        const float    alpha = pixel[3] / 255.0f;
        return { pixel[0] * alpha, pixel[1] * alpha, pixel[2] * alpha, alpha };
    };
    const float centerX = kDdsWidth / 2.0f;
    const float centerY = kDdsHeight / 2.0f;
    for (uint32_t y = 0; y < kDdsHeight; ++y)
    {
        const float   sourceY = centerY + (y + 0.5f - centerY) / scale - 0.5f;
        const float   floorY  = std::floor(sourceY);
        const float   weightY = sourceY - floorY;
        const int32_t y0      = static_cast<int32_t>(floorY);
        for (uint32_t x = 0; x < kDdsWidth; ++x)
        {
            const float          sourceX = centerX + (x + 0.5f - centerX) / scale - 0.5f;
            const float          floorX  = std::floor(sourceX);
            const float          weightX = sourceX - floorX;
            const int32_t        x0      = static_cast<int32_t>(floorX);
            const auto           a       = texel(x0, y0);
            const auto           b       = texel(x0 + 1, y0);
            const auto           c       = texel(x0, y0 + 1);
            const auto           d       = texel(x0 + 1, y0 + 1);
            uint8_t*             pixel   = target + (size_t{ kDdsWidth } * y + x) * 4;
            std::array<float, 4> mixed{};
            for (size_t channel = 0; channel < 4; ++channel)
            {
                const float top    = a[channel] + (b[channel] - a[channel]) * weightX;
                const float bottom = c[channel] + (d[channel] - c[channel]) * weightX;
                mixed[channel]     = top + (bottom - top) * weightY;
            }
            const float alpha = mixed[3];
            for (size_t channel = 0; channel < 3; ++channel)
            {
                const float color = alpha > 0.0f ? mixed[channel] / alpha : 0.0f;
                pixel[channel]    = static_cast<uint8_t>(std::clamp(std::lround(color), 0l, 255l));
            }
            pixel[3] = static_cast<uint8_t>(std::clamp(std::lround(alpha * 255.0f), 0l, 255l));
        }
    }
    return scaled;
}

bool ResolveSelectedLogo(
    std::span<const rex::system::AssetOverlayPackage> packages,
    Resolver                                          resolver,
    float                                             scale)
{
    clearSelection();
    if (!resolver)
    {
        resolver = &rex::system::ResolveAssetOverlay;
    }

    const auto result = resolver(packages, kAssetKey, kLogoDdsSize);
    if (!result)
    {
        if (result.error().category != rex::ErrorCategory::NotFound)
        {
            REXLOG_ERROR("Main-menu logo asset rejected: {}", result.error().what());
        }
        return false;
    }

    const auto& resolved = *result;
    logResolutionProvenance(resolved);
    if (!IsValidLogoDds(resolved.bytes))
    {
        REXLOG_ERROR("Main-menu logo asset from package '{}' is malformed; using retail asset",
                     resolved.package_id);
        return false;
    }

    auto payload = std::make_shared<const std::vector<uint8_t>>(ScaleLogo(resolved.bytes, scale));
    {
        std::unique_lock lock(selectionMutex);
        selectedLogo = std::move(payload);
    }

    return true;
}

bool TryGetPayload(Payload& payload)
{
    std::shared_lock lock(selectionMutex);
    payload = selectedLogo;
    return static_cast<bool>(payload);
}

void ResetForTests()
{
    clearSelection();
}

} // namespace rerevved::main_menu_logo
