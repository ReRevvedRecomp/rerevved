#include "native_prepared_bc2_fixture.h"

#include "gpu/d3d12/native_gameplay_render_frame.h"
#include "gpu/d3d12/native_gameplay_shaders.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "helper_commands.hpp"
#include "native_argv.hpp"
#include "native_gameplay_fixture_inputs.h"
#include "native_render_frame_test_access.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <rex/crypto/sha256.h>
#include <rex/graphics/format/ucode.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <wrl/client.h>

using namespace rerevved::gpu;
using rex::graphics::testing::ExpectedNativePreparedBc2Pixels;
using rex::graphics::testing::MakeNativePreparedBc2Fixture;
using rex::graphics::testing::NativePreparedBc2CopyRecord;
using rex::graphics::testing::NativePreparedBc2DrawRecord;
using rex::graphics::testing::NativePreparedBc2Fixture;
using rex::graphics::testing::NativePreparedBc2OriginalParams;
using rex::graphics::testing::NativePreparedBc2OriginalResult;
using rex::graphics::testing::RunNativePreparedBc2Original;

namespace
{

constexpr std::uint32_t kMipCount     = rex::graphics::testing::kNativePreparedBc2MipCount;
constexpr std::uint32_t kWidth        = rex::graphics::testing::kNativePreparedBc2Width;
constexpr std::uint32_t kHeight       = rex::graphics::testing::kNativePreparedBc2Height;
constexpr std::uint32_t kOutputWidth  = rex::graphics::testing::kNativePreparedBc2OutputWidth;
constexpr std::uint32_t kOutputHeight = rex::graphics::testing::kNativePreparedBc2OutputHeight;
constexpr std::size_t   kPlane        = static_cast<std::size_t>(kOutputWidth) * kOutputHeight * 4;

// Test-owned endpoint reference. Each level keeps green constant at 0 or
// full-scale; red and blue remain exact 565 endpoints. This qualifies the
// observed green precision case without claiming general BC2 byte conformance.
constexpr std::array<std::array<std::uint16_t, 2>, kMipCount> kExpectedBc2Endpoints = {
    { { 0x0000U, 0xF800U }, { 0x0000U, 0x001FU }, { 0x0000U, 0xF81FU }, { 0x001FU, 0xF800U }, { 0x001FU, 0xF81FU }, { 0xF800U, 0xF81FU }, { 0x07E0U, 0xFFFFU } }
};

std::size_t outputGuestExtent()
{
    // The callback range is the tiled guest allocation, not the linear RGBA8
    // capture plane retained in the output file.
    const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
        rex::graphics::xenos::DataDimension::k2DOrStacked,
        kOutputWidth / 32U,
        kOutputWidth,
        kOutputHeight,
        1U,
        true,
        rex::graphics::xenos::TextureFormat::k_8_8_8_8,
        false,
        true,
        0U);
    return layout.base.level_data_extent_bytes;
}

struct DenyGuestMemoryRange
{
    rex::graphics::diagnostic::DrawCaptureGuestMemoryReader upstream;
    std::uint32_t                                           deny_begin = 0;
    std::uint32_t                                           deny_end   = 0;

    static bool Copy(void* context, std::uint32_t address, std::span<std::uint8_t> destination) noexcept
    {
        const auto* self = static_cast<const DenyGuestMemoryRange*>(context);
        const auto  end  = std::uint64_t(address) + destination.size();
        if (address < self->deny_end && std::uint64_t(self->deny_begin) < end)
            return false;
        return self->upstream.Copy(address, destination);
    }
};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        fail(message);
}

struct AdapterIdentity
{
    LUID                             luid{};
    Microsoft::WRL::ComPtr<IUnknown> canonical;
};

bool sameLuid(const LUID& left, const LUID& right)
{
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

AdapterIdentity describeAdapter(ID3D12Device* device, IDXGIAdapter1* adapter)
{
    require(device != nullptr && adapter != nullptr, "BC2 adapter identity requires a device and adapter");
    DXGI_ADAPTER_DESC1 description{};
    require(SUCCEEDED(adapter->GetDesc1(&description)), "BC2 adapter identity could not read the DXGI description");
    AdapterIdentity result;
    result.luid = device->GetAdapterLuid();
    require(sameLuid(result.luid, description.AdapterLuid), "BC2 adapter identity LUID did not match the device");
    require(SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&result.canonical))),
            "BC2 adapter identity could not query canonical IUnknown");
    return result;
}

AdapterIdentity describeProviderAdapter(const rex::ui::d3d12::D3D12Provider& provider)
{
    const auto luid    = provider.GetDevice()->GetAdapterLuid();
    auto*      factory = provider.GetDXGIFactory();
    require(factory != nullptr, "BC2 main provider did not retain its DXGI factory");
    for (std::uint32_t index = 0;; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto                            result = factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf());
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        require(SUCCEEDED(result), "BC2 main provider adapter enumeration");
        DXGI_ADAPTER_DESC1 description{};
        require(SUCCEEDED(adapter->GetDesc1(&description)), "BC2 main provider adapter description");
        if (sameLuid(luid, description.AdapterLuid))
            return describeAdapter(provider.GetDevice(), adapter.Get());
    }
    fail("BC2 main provider adapter LUID was not present in its factory");
}

struct DedicatedWarpDevice
{
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>  device;
    AdapterIdentity                       identity;
};

DedicatedWarpDevice createDedicatedWarpDevice()
{
    DedicatedWarpDevice result;
    require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&result.factory))),
            "BC2 dedicated WARP factory creation");
    Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    require(SUCCEEDED(result.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),
            "BC2 dedicated WARP adapter enumeration");
    require(SUCCEEDED(warp.As(&result.adapter)), "BC2 dedicated WARP adapter query");
    require(SUCCEEDED(D3D12CreateDevice(result.adapter.Get(),
                                        D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&result.device))),
            "BC2 dedicated WARP device creation");
    result.identity = describeAdapter(result.device.Get(), result.adapter.Get());
    return result;
}

void phase(std::uint32_t pairIndex, const char* name)
{
    std::cerr << "native_prepared_bc2 pair=" << pairIndex << " phase=" << name << std::endl;
}

struct HarnessLogging
{
    HarnessLogging()
    {
        rex::LogConfig config;
        config.log_to_console = true;
        rex::InitLogging(config);
    }

    ~HarnessLogging()
    {
        rex::ShutdownLogging();
    }
};

class PreparedCollector final : public rex::graphics::diagnostic::DrawCaptureFrameInputObserver
{
public:
    using CopyView = rex::graphics::diagnostic::DrawCaptureFrameInputCopyView;
    using DrawView = rex::graphics::diagnostic::DrawCaptureFrameInputDrawView;
    using SwapView = rex::graphics::diagnostic::DrawCaptureFrameInputSwapView;

    struct NegativeProbe
    {
        std::string name;
        std::string error;
        bool        rejected          = false;
        bool        no_admitted_event = false;
    };

    bool WantsPreparedDraws() const noexcept override
    {
        return true;
    }

    bool WantsPreparedCopies() const noexcept override
    {
        return true;
    }

    bool WantsPreparedSwaps() const noexcept override
    {
        return true;
    }

    bool OnFrameBegin(const SwapView& boundary) override
    {
        if (frameCount != 0U)
        {
            // Each completed PM4 stream has its own Prepared owner. Retain the
            // prior owner before beginning the next stream from its boundary.
            inputs   = rerevved::gpu::diagnostics::NativeGameplayLiveInputs{};
            finished = false;
        }
        probeBoundary = boundary;
        ++frameCount;
        initialBoundary = boundary.id;
        return inputs.BeginPrepared(boundary, error);
    }

    bool OnDrawPrepared(const DrawView& draw) override
    {
        ++preparedDrawCount;
        try
        {
            runNegativeProbes(draw);
        }
        catch (const std::exception& exception)
        {
            error = std::string("selected Prepared negative probe failed: ") + exception.what();
            return false;
        }
        if (!inputs.PreparedDraw(draw, error))
            return false;
        const auto texture = inputs.Textures().find(0U);
        if (texture != inputs.Textures().end() && texture->second.textureVersion &&
            (preparedOwners.empty() || preparedOwners.back().textureVersion != texture->second.textureVersion))
        {
            preparedOwners.push_back(texture->second);
            const auto payload = texture->second.payloadBytes();
            preparedPayloads.emplace_back(payload.begin(), payload.end());
        }
        return true;
    }

    void runNegativeProbes(const DrawView& draw)
    {
        if (negativeProbesRan)
            return;
        require(!draw.textures.empty(), "actual Prepared BC2 draw had no texture view for negative probes");

        const auto reject = [&](const DrawView& candidate, std::string name)
        {
            rerevved::gpu::diagnostics::NativeGameplayLiveInputs probe;
            std::string                                          probeError;
            require(probe.BeginPrepared(probeBoundary, probeError),
                    "negative Prepared probe boundary was not accepted: " + probeError);
            const bool accepted = probe.PreparedDraw(candidate, probeError);
            negativeProbes.push_back({ std::move(name), probeError, !accepted, probe.Snapshot().events.size() == 1U });
            require(!accepted, "negative Prepared probe was admitted: " + negativeProbes.back().name);
            require(negativeProbes.back().no_admitted_event,
                    "negative Prepared probe admitted an event: " + negativeProbes.back().name);
        };

        auto incomplete               = draw;
        incomplete.cpu_data_complete  = false;
        incomplete.unavailable_reason = rex::graphics::diagnostic::DrawCaptureReason::kNone;
        reject(incomplete, "incomplete_cpu_inputs");

        auto memexport               = draw;
        memexport.unavailable_reason = rex::graphics::diagnostic::DrawCaptureReason::kMemexport;
        reject(memexport, "memexport_provenance");

        auto dirtyTextures = std::vector<rex::graphics::diagnostic::DrawCaptureFrameInputTextureView>(
            draw.textures.begin(), draw.textures.end());
        const auto requiredTexture = std::ranges::find_if(dirtyTextures, [](const auto& texture)
                                                          {
                                                              return texture.required;
                                                          });
        require(requiredTexture != dirtyTextures.end(),
                "actual Prepared BC2 draw had no required texture for negative probes");
        requiredTexture->gpu_written = true;
        auto dirty                   = draw;
        dirty.textures               = dirtyTextures;
        reject(dirty, "gpu_dirty_texture");

        auto       truncatedTextures  = dirtyTextures;
        const auto truncatedTexture   = std::ranges::find_if(truncatedTextures, [](const auto& texture)
                                                             {
                                                               return texture.required;
                                                             });
        truncatedTexture->gpu_written = false;
        truncatedTexture->mip_size    = 1U;
        auto truncated                = draw;
        truncated.textures            = truncatedTextures;
        reject(truncated, "truncated_mip_metadata");

        // Keep the actual footprint metadata intact while the borrowed reader
        // rejects only the packed tail interval, exercising guarded copying.
        auto guardedTextures = std::vector<rex::graphics::diagnostic::DrawCaptureFrameInputTextureView>(
            draw.textures.begin(), draw.textures.end());
        const auto           guardedTexture = std::ranges::find_if(guardedTextures, [](const auto& texture)
                                                                   {
                                                             return texture.required;
                                                                   });
        DenyGuestMemoryRange guardedMemory{
            guardedTexture->memory,
            guardedTexture->mip_address + 0x4000U,
            guardedTexture->mip_address + 0x4680U
        };
        guardedTexture->memory = { &guardedMemory, &DenyGuestMemoryRange::Copy };
        auto guarded           = draw;
        guarded.textures       = guardedTextures;
        reject(guarded, "guarded_unreadable_mip_tail");

        rerevved::gpu::diagnostics::NativeGameplayLiveInputs budgetProbe;
        std::string                                          budgetError;
        require(budgetProbe.BeginPrepared(probeBoundary, budgetError),
                "budget Prepared probe boundary was not accepted: " + budgetError);
        const auto fixedBudget = rerevved::gpu::diagnostics::NativeGameplayLiveInputs::BoundaryByteBound();
        budgetProbe.SetOwnedByteBudget(fixedBudget, [fixedBudget](std::size_t)
                                       {
                                           return fixedBudget;
                                       });
        const bool budgetAccepted = budgetProbe.PreparedDraw(draw, budgetError);
        negativeProbes.push_back({ "owned_byte_budget", budgetError, !budgetAccepted, budgetProbe.Snapshot().events.size() == 1U });
        require(!budgetAccepted, "owned byte budget probe was admitted");
        require(negativeProbes.back().no_admitted_event, "owned byte budget probe admitted an event");
        negativeProbesRan = true;
    }

    bool OnCopyPrepared(const CopyView& copy) override
    {
        ++preparedCopyCount;
        return inputs.PreparedCopy(copy, error);
    }

    bool OnSwapPrepared(const SwapView& swap) override
    {
        if (swap.id == initialBoundary)
            return true;
        if (finished)
        {
            // The SDK emits the next stream's leading Prepared swap without
            // another frame-begin callback. Advance from the owned completed
            // boundary before accepting that stream's first Prepared draw.
            if (!inputs.BeginAfterCompletion(error))
                return false;
            finished = false;
            ++afterCompletionCount;
            return true;
        }
        finished = inputs.PreparedSwap(swap, error);
        return finished;
    }

    bool OnDraw(const DrawView&) override
    {
        return true;
    }

    bool OnCopy(const CopyView&) override
    {
        return true;
    }

    bool OnSwap(const SwapView&) override
    {
        return true;
    }

    bool OnFrameEnd(std::uint64_t, std::uint64_t, bool) override
    {
        return true;
    }

    rerevved::gpu::diagnostics::NativeGameplayLiveInputs inputs;
    std::uint64_t                                        initialBoundary      = 0;
    std::size_t                                          preparedDrawCount    = 0;
    std::size_t                                          preparedCopyCount    = 0;
    std::size_t                                          frameCount           = 0;
    std::size_t                                          afterCompletionCount = 0;
    std::vector<NativeDrawReplayTexture>                 preparedOwners;
    std::vector<std::vector<std::uint8_t>>               preparedPayloads;
    std::vector<NegativeProbe>                           negativeProbes;
    SwapView                                             probeBoundary;
    bool                                                 finished          = false;
    bool                                                 negativeProbesRan = false;
    std::string                                          error;
};

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "could not open " + path.string());
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    require(size >= 0, "could not size " + path.string());
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(input.good() || input.eof(), "could not read " + path.string());
    return bytes;
}

void writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.good(), "could not create " + path.string());
    if (!bytes.empty())
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    require(output.good(), "could not write " + path.string());
}

std::vector<std::uint8_t> wordsBytes(std::span<const std::uint32_t> words)
{
    std::vector<std::uint8_t> bytes(words.size() * 4);
    for (std::size_t index = 0; index < words.size(); ++index)
    {
        bytes[index * 4]     = static_cast<std::uint8_t>(words[index]);
        bytes[index * 4 + 1] = static_cast<std::uint8_t>(words[index] >> 8);
        bytes[index * 4 + 2] = static_cast<std::uint8_t>(words[index] >> 16);
        bytes[index * 4 + 3] = static_cast<std::uint8_t>(words[index] >> 24);
    }
    return bytes;
}

std::string wordDigest(std::span<const std::uint32_t> words)
{
    const auto bytes = wordsBytes(words);
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::size_t hostMipOffset(std::uint32_t level)
{
    std::size_t offset = 0;
    for (std::uint32_t index = 0; index < level; ++index)
    {
        const auto width  = std::max(1U, kWidth >> index);
        const auto height = std::max(1U, kHeight >> index);
        offset += static_cast<std::size_t>(std::max(1U, (width + 3U) / 4U)) *
                  std::max(1U, (height + 3U) / 4U) * 16U;
    }
    return offset;
}

NativeDrawReplayTexture mutableTexture(const NativeDrawReplayTexture& source)
{
    NativeDrawReplayTexture result  = source;
    const auto              payload = source.payloadBytes();
    result.textureVersion.reset();
    result.sharedBytes.reset();
    result.bytes.assign(payload.begin(), payload.end());
    return result;
}

void MutateBc2ColorMip(NativeDrawReplayTexture& texture, std::uint32_t level)
{
    require(level < texture.mipLevels, "BC2 color mutation level is outside the replay texture");
    const auto width      = std::max(1U, texture.width >> level);
    const auto height     = std::max(1U, texture.height >> level);
    const auto levelBytes = static_cast<std::size_t>(std::max(1U, (width + 3U) / 4U)) *
                            std::max(1U, (height + 3U) / 4U) * 16U;
    const auto begin      = hostMipOffset(level);
    require(begin + levelBytes <= texture.bytes.size(), "BC2 color mutation level exceeds its owned payload");
    for (std::size_t offset = begin; offset < begin + levelBytes; offset += 16U)
    {
        // Use a green payload distinct from the Original magenta update so
        // this mutation always changes a prepared owner, including after a
        // combined CPU update.
        texture.bytes[offset + 8U]  = 0xE0U;
        texture.bytes[offset + 9U]  = 0x07U;
        texture.bytes[offset + 10U] = 0xE0U;
        texture.bytes[offset + 11U] = 0x07U;
        std::fill(texture.bytes.begin() + static_cast<std::ptrdiff_t>(offset + 12U),
                  texture.bytes.begin() + static_cast<std::ptrdiff_t>(offset + 16U),
                  0U);
    }
}

void MutateBc2AlphaMip(NativeDrawReplayTexture& texture, std::uint32_t level)
{
    require(level < texture.mipLevels, "BC2 alpha mutation level is outside the replay texture");
    const auto width      = std::max(1U, texture.width >> level);
    const auto height     = std::max(1U, texture.height >> level);
    const auto levelBytes = static_cast<std::size_t>(std::max(1U, (width + 3U) / 4U)) *
                            std::max(1U, (height + 3U) / 4U) * 16U;
    const auto begin      = hostMipOffset(level);
    require(begin + levelBytes <= texture.bytes.size(), "BC2 alpha mutation level exceeds its owned payload");
    for (std::size_t offset = begin; offset < begin + levelBytes; offset += 16U)
        std::fill(texture.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                  texture.bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8U),
                  0x00U);
}

NativeDrawReplayTexture omitBc2Mip(const NativeDrawReplayTexture& source, std::uint32_t level)
{
    require(level < source.mipLevels, "BC2 omission level is outside the replay texture");
    auto       result     = mutableTexture(source);
    const auto width      = std::max(1U, result.width >> level);
    const auto height     = std::max(1U, result.height >> level);
    const auto levelBytes = static_cast<std::size_t>(std::max(1U, (width + 3U) / 4U)) *
                            std::max(1U, (height + 3U) / 4U) * 16U;
    const auto begin      = hostMipOffset(level);
    require(begin + levelBytes <= result.bytes.size(), "BC2 omission level exceeds its owned payload");
    result.bytes.erase(result.bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                       result.bytes.begin() + static_cast<std::ptrdiff_t>(begin + levelBytes));
    return result;
}

NativeDrawReplayTexture decodeControlTexture(const NativePreparedBc2Fixture&         fixture,
                                             std::vector<std::vector<std::uint8_t>>* levelsOut = nullptr)
{
    require(fixture.texture.base_guest_bytes.size() >= 0x2000U && fixture.texture.mip_guest_bytes.size() >= 0x4680U,
            "fixture raw BC2 storage is shorter than its declared decoder footprint");
    std::vector<std::vector<std::uint8_t>> levels;
    levels.reserve(kMipCount);
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto& mip        = fixture.texture.mips[level];
        const auto  width      = rex::graphics::testing::kNativePreparedBc2Widths[level];
        const auto  height     = rex::graphics::testing::kNativePreparedBc2Heights[level];
        const auto  blocksWide = std::max(1U, (width + 3U) / 4U);
        const auto  blocksHigh = std::max(1U, (height + 3U) / 4U);
        const auto  rowBytes   = static_cast<std::size_t>(blocksWide) * 16U;
        require(mip.level == level && mip.width == width && mip.height == height &&
                    mip.guest_offset == rex::graphics::testing::kNativePreparedBc2GuestOffsets[level] &&
                    mip.row_pitch == rex::graphics::testing::kNativePreparedBc2RowPitches[level],
                "control BC2 oracle metadata differs from the fixed selected layout at mip " + std::to_string(level));
        const auto solidColor = [&](std::uint8_t red, std::uint8_t green, std::uint8_t blue)
        {
            return mip.linear_rgba8.size() == static_cast<std::size_t>(width) * height * 4U &&
                   std::ranges::all_of(std::views::iota(0U, width * height), [&](std::uint32_t pixel)
                                       {
                                           const auto at = static_cast<std::size_t>(pixel) * 4U;
                                           return mip.linear_rgba8[at] == red && mip.linear_rgba8[at + 1U] == green &&
                                                  mip.linear_rgba8[at + 2U] == blue;
                                       });
        };
        const auto updatedMagenta = solidColor(255U, 0U, 255U);
        const auto updatedGreen   = solidColor(0U, 255U, 0U);
        require(!(updatedMagenta && updatedGreen), "independent BC2 fixture reports conflicting updated RGB states");
        levels.emplace_back(rowBytes * blocksHigh, 0U);
        const auto source = level == 0U ? std::span<const std::uint8_t>(fixture.texture.base_guest_bytes)
                                        : std::span<const std::uint8_t>(fixture.texture.mip_guest_bytes);
        for (std::uint32_t blockY = 0; blockY < blocksHigh; ++blockY)
        {
            for (std::uint32_t blockX = 0; blockX < blocksWide; ++blockX)
            {
                const auto sourceOffset = static_cast<std::size_t>(rex::graphics::testing::kNativePreparedBc2GuestOffsets[level]) +
                                          static_cast<std::size_t>(blockY) * rex::graphics::testing::kNativePreparedBc2RowPitches[level] +
                                          static_cast<std::size_t>(blockX) * 16U;
                require(sourceOffset + 16U <= source.size(), "control BC2 subresource exceeds its selected guest range");
                // Decode k8in16 directly so this control route does not share
                // the production block-swap helper or layout calculation.
                const auto destinationOffset = static_cast<std::size_t>(blockY) * rowBytes +
                                               static_cast<std::size_t>(blockX) * 16U;
                for (std::size_t pair = 0; pair < 8U; ++pair)
                {
                    levels.back()[destinationOffset + pair * 2U]      = source[sourceOffset + pair * 2U + 1U];
                    levels.back()[destinationOffset + pair * 2U + 1U] = source[sourceOffset + pair * 2U];
                }
                const auto read16 = [&](std::size_t at)
                {
                    return static_cast<std::uint16_t>(levels.back()[destinationOffset + at] |
                                                      (levels.back()[destinationOffset + at + 1U] << 8U));
                };
                const auto color0       = read16(8U);
                const auto color1       = read16(10U);
                const auto colorIndices = static_cast<std::uint32_t>(read16(12U)) |
                                          (static_cast<std::uint32_t>(read16(14U)) << 16U);
                if (blockX == 0U && blockY == 0U)
                {
                    const auto baselineEndpoints  = color0 == kExpectedBc2Endpoints[level][0] && color1 == kExpectedBc2Endpoints[level][1];
                    const auto combinedEndpoints  = color0 == 0xF81FU && color1 == 0xF81FU;
                    const auto colorOnlyEndpoints = color0 == 0x07E0U && color1 == 0x07E0U;
                    require(static_cast<unsigned>(baselineEndpoints) + static_cast<unsigned>(combinedEndpoints) +
                                    static_cast<unsigned>(colorOnlyEndpoints) ==
                                1U,
                            "independent BC2 endpoint reference contains an unknown RGB update state at mip " + std::to_string(level));
                    require(combinedEndpoints == updatedMagenta && colorOnlyEndpoints == updatedGreen,
                            "independent BC2 endpoint state does not match its exact updated RGB pixels at mip " + std::to_string(level));
                }
                // Keep the control decoder independent, promote the endpoint
                // channels to UNORM8 before interpolation, and use BC2's four
                // color entries even when color0 <= color1.
                const auto decodeColor = [&](std::uint32_t index, std::uint32_t channel)
                {
                    const auto a5    = std::array<std::uint32_t, 3>{ (color0 >> 11U) & 0x1FU, (color0 >> 5U) & 0x3FU, color0 & 0x1FU };
                    const auto b5    = std::array<std::uint32_t, 3>{ (color1 >> 11U) & 0x1FU, (color1 >> 5U) & 0x3FU, color1 & 0x1FU };
                    const auto a     = std::array<std::uint32_t, 3>{ (a5[0] << 3U) | (a5[0] >> 2U),
                                                                     (a5[1] << 2U) | (a5[1] >> 4U),
                                                                     (a5[2] << 3U) | (a5[2] >> 2U) };
                    const auto b     = std::array<std::uint32_t, 3>{ (b5[0] << 3U) | (b5[0] >> 2U),
                                                                     (b5[1] << 2U) | (b5[1] >> 4U),
                                                                     (b5[2] << 3U) | (b5[2] >> 2U) };
                    const auto value = index == 0U ? a[channel] : index == 1U ? b[channel]
                                                              : index == 2U   ? (2U * a[channel] + b[channel] + 1U) / 3U
                                                                              : (a[channel] + 2U * b[channel] + 1U) / 3U;
                    return value;
                };
                for (std::uint32_t texel = 0; texel < 16U; ++texel)
                {
                    const auto alphaNibble = (levels.back()[destinationOffset + texel / 2U] >> ((texel & 1U) * 4U)) & 0xFU;
                    const auto colorIndex  = (colorIndices >> (texel * 2U)) & 3U;
                    const auto x           = blockX * 4U + (texel & 3U);
                    const auto y           = blockY * 4U + (texel / 4U);
                    if (x < mip.width && y < mip.height)
                    {
                        const auto expected = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
                        require(mip.linear_rgba8[expected + 0U] == decodeColor(colorIndex, 0U) &&
                                    mip.linear_rgba8[expected + 1U] == decodeColor(colorIndex, 1U) &&
                                    mip.linear_rgba8[expected + 2U] == decodeColor(colorIndex, 2U) &&
                                    mip.linear_rgba8[expected + 3U] == alphaNibble * 17U,
                                "independent BC2 color interpolation or explicit alpha decode differs at mip " + std::to_string(level));
                    }
                }
            }
        }
    }
    std::vector<std::span<const std::uint8_t>> levelSpans;
    levelSpans.reserve(levels.size());
    for (const auto& level : levels)
        levelSpans.emplace_back(level);
    NativeDrawReplayTextureLayout layout;
    layout.width     = kWidth;
    layout.height    = kHeight;
    layout.format    = NativeDrawReplayTextureFormat::Bc2;
    layout.mipLevels = kMipCount;
    layout.dimension = NativeDrawReplayTextureDimension::Texture2D;
    NativeDrawReplayTexture texture;
    std::string             error;
    require(PackNativeDrawReplayTextureVersion(layout, levelSpans, texture, error),
            "independent BC2 control packing failed: " + error);
    if (levelsOut)
        *levelsOut = std::move(levels);
    return texture;
}

std::string tomlString(std::string_view value)
{
    std::string result = "\"";
    for (const auto character : value)
    {
        if (character == '\\' || character == '"')
            result.push_back('\\');
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

void setBigEndianFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    const auto bits = std::bit_cast<std::uint32_t>(value);
    require(offset + 4 <= bytes.size(), "fixture vertex scalar offset exceeds owned input");
    bytes[offset]     = static_cast<std::uint8_t>(bits >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(bits >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(bits >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(bits);
}

float bigEndianFloat(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    require(offset + 4U <= bytes.size(), "fixture UV scalar offset exceeds owned input");
    const auto bits = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
                      (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
                      (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
                      static_cast<std::uint32_t>(bytes[offset + 3U]);
    return std::bit_cast<float>(bits);
}

NativePreparedBc2Fixture fixtureForPair(std::uint32_t first, std::uint32_t second)
{
    auto fixture                     = MakeNativePreparedBc2Fixture();
    fixture.selected_lods            = { first, second };
    fixture.expected_selected_pixels = ExpectedNativePreparedBc2Pixels(fixture, first, second);
    require(!fixture.expected_selected_pixels.empty(), "fixture did not provide expected pair pixels");
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        const auto base = vertex * fixture.vertex_stride;
        setBigEndianFloat(fixture.vertex_bytes, base + 24, static_cast<float>(first));
        setBigEndianFloat(fixture.vertex_bytes, base + 28, static_cast<float>(second));
    }
    return fixture;
}

bool sameAttributes(std::span<const NativeGameplayVertexAttribute> left,
                    std::span<const NativeGameplayVertexAttribute> right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.fetchConstant != b.fetchConstant || a.strideBytes != b.strideBytes || a.offsetBytes != b.offsetBytes ||
            a.format != b.format || a.isSigned != b.isSigned || a.normalized != b.normalized)
            return false;
    }
    return true;
}

NativeGameplayShaderInterface loadShader(const std::filesystem::path&   catalog,
                                         std::span<const std::uint32_t> vertexWords,
                                         std::span<const std::uint32_t> pixelWords,
                                         std::string_view               role)
{
    std::vector<NativeGameplayShaderInterface> shaders;
    std::string                                error;
    const bool                                 loaded = LoadNativeGameplayShaders(catalog, shaders, error);
    require(loaded, std::string(role) + " catalog: " + error);
    require(shaders.size() == 1, std::string(role) + " catalog must contain one shader pair");
    auto shader = std::move(shaders.front());
    require(shader.vertexGuestWordDigest == wordDigest(vertexWords) && shader.pixelGuestWordDigest == wordDigest(pixelWords),
            std::string(role) + " catalog is not bound to the exact fixture words");
    require(shader.textureFetchSlots.size() == 1 && shader.textureFetchSlots[0] == 0 &&
                shader.textureDimensions[0] == NativeDrawReplayTextureDimension::Texture2D,
            std::string(role) + " catalog must bind one guest slot-0 Texture2D");
    require(!shader.vertexAttributes.empty(), std::string(role) + " catalog has no vertex attributes");
    return shader;
}

struct DrawInputs
{
    std::vector<std::uint32_t>    registers;
    std::vector<std::uint8_t>     vertices;
    std::vector<std::uint8_t>     indices;
    NativeDrawReplayTexture       texture;
    NativeDrawReplaySampler       sampler;
    NativeGameplayVertexFetchSpan fetch;
    NativeGameplayTextureBinding  binding;
    NativeGameplayDrawView        view;
};

DrawInputs makeInputs(const NativePreparedBc2Fixture& fixture,
                      NativeDrawReplayTexture         preparedTexture,
                      std::span<const std::uint32_t>  sourceRegisters,
                      std::uint32_t                   vertexGuestBase,
                      std::uint32_t                   mipLevels = kMipCount)
{
    DrawInputs result;
    require(!sourceRegisters.empty(), "native replay requires the actual original source register bank");
    result.registers = std::vector<std::uint32_t>(sourceRegisters.begin(), sourceRegisters.end());
    result.vertices  = fixture.vertex_bytes;
    result.indices   = fixture.index_bytes;
    result.texture   = std::move(preparedTexture);
    require(result.texture.width == fixture.width && result.texture.height == fixture.height &&
                result.texture.format == NativeDrawReplayTextureFormat::Bc2 &&
                result.texture.mipLevels == mipLevels &&
                result.texture.dimension == NativeDrawReplayTextureDimension::Texture2D,
            "prepared collector did not supply the complete BC2 replay texture");
    result.sampler.minLinear = false;
    result.sampler.magLinear = false;
    result.sampler.mipLinear = false;
    result.sampler.address   = { 3, 3, 3 };
    result.sampler.minLod    = 0.0F;
    result.sampler.maxLod    = 6.0F;
    result.view.primitive    = 4;
    result.view.indexCount   = 3;
    result.view.indexFormat  = 0;
    result.view.indexEndian  = 1;
    result.view.indexed      = true;
    require(result.registers.size() == rex::graphics::testing::kNativePreparedBc2RegisterCount,
            "fixture register bank has the wrong extent");
    return result;
}

NativeDrawReplayRecipe buildRecipe(const NativePreparedBc2Fixture&      fixture,
                                   const NativeGameplayShaderInterface& shader,
                                   const std::filesystem::path&         output,
                                   NativeDrawReplayTexture              preparedTexture,
                                   std::uint32_t                        mipLevels       = kMipCount,
                                   std::span<const std::uint32_t>       sourceRegisters = {},
                                   std::uint32_t                        vertexGuestBase = 0,
                                   std::uint32_t                        targetWidth     = kOutputWidth,
                                   std::uint32_t                        targetHeight    = kOutputHeight)
{
    auto inputs = makeInputs(fixture, std::move(preparedTexture), sourceRegisters, vertexGuestBase, mipLevels);
    // makeInputs owns the vectors, but its returned view may have been moved.
    // Rebind every span and pointer to the final local owner before conversion.
    inputs.fetch                = { 95, vertexGuestBase, inputs.vertices };
    inputs.binding              = { 0, &inputs.texture, &inputs.sampler };
    inputs.view.registers       = inputs.registers;
    inputs.view.vertexMicrocode = fixture.vertex_shader_words;
    inputs.view.pixelMicrocode  = fixture.pixel_shader_words;
    inputs.view.vertexFetches   = std::span<const NativeGameplayVertexFetchSpan>(&inputs.fetch, 1);
    inputs.view.indexBytes      = inputs.indices;
    inputs.view.textureBindings = std::span<const NativeGameplayTextureBinding>(&inputs.binding, 1);
    NativeGameplayDrawTarget target;
    target.width       = targetWidth;
    target.height      = targetHeight;
    target.sampleCount = 1;
    target.sampleMask  = 1;
    target.format      = NativeDrawReplayTargetFormat::Rgba8;
    const auto zScale  = std::bit_cast<float>(inputs.registers[0x2113U]);
    const auto zOffset = std::bit_cast<float>(inputs.registers[0x2114U]);
    target.viewport    = { 0.0F, 0.0F, static_cast<float>(kWidth), static_cast<float>(kHeight), zOffset, zOffset + zScale };
    target.scissor     = { 0, 0, static_cast<std::int32_t>(kWidth), static_cast<std::int32_t>(kHeight) };
    NativeDrawReplayRecipe recipe;
    std::string            error;
    const bool             built = BuildNativeGameplayDrawRecipe(inputs.view, shader, target, recipe, error);
    require(built, error);
    // Presenter capture normalizes the untouched canvas to opaque alpha.
    recipe.clearColor = { 0, 0, 0, 255 };
    recipe.outputPath = output;
    return recipe;
}

std::vector<std::uint8_t> readNativeOutput(const std::filesystem::path& path)
{
    const auto bytes = readBytes(path);
    require(bytes.size() == 2 * kPlane, "native output must contain two complete RGBA8 planes");
    return bytes;
}

void compare(std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected, std::string_view label)
{
    require(actual.size() == expected.size(), std::string(label) + " size differs");
    const auto mismatch = std::ranges::mismatch(actual, expected);
    if (mismatch.in1 != actual.end())
        fail(std::string(label) + " differs at byte " + std::to_string(mismatch.in1 - actual.begin()));
}

std::vector<std::uint8_t> duplicatePlane(std::span<const std::uint8_t> plane)
{
    require(plane.size() == kPlane, "original terminal must be one complete RGBA8 plane");
    std::vector<std::uint8_t> result;
    result.insert(result.end(), plane.begin(), plane.end());
    result.insert(result.end(), plane.begin(), plane.end());
    return result;
}

std::uint32_t sourceVertexGuestBase(const NativePreparedBc2OriginalResult& original)
{
    require(original.source_vertex_fetch_words[0] != 0, "original source vertex fetch was absent");
    require((original.source_vertex_fetch_words[0] & 3U) == static_cast<std::uint32_t>(rex::graphics::xenos::FetchConstantType::kVertex),
            "original source vertex fetch lost its guest vertex type");
    return original.source_vertex_fetch_words[0] & ~3U;
}

std::array<std::uint32_t, 6> fetchWords(std::span<const std::uint32_t> registers, std::uint32_t slot)
{
    const auto address = 0x4800U + slot * 2U;
    require(registers.size() >= address + 6U, "source register bank lacks a fetch descriptor");
    std::array<std::uint32_t, 6> result{};
    std::copy_n(registers.begin() + address, result.size(), result.begin());
    return result;
}

void assertOriginalSource(const NativePreparedBc2Fixture&        expected,
                          const NativePreparedBc2OriginalResult& original)
{
    constexpr std::uint32_t kCpRbWptr         = 0x1C5U;
    constexpr std::uint32_t kVgtDmaBase       = 0x21FAU;
    constexpr std::uint32_t kVgtDmaSize       = 0x21FBU;
    constexpr std::uint32_t kVgtDrawInitiator = 0x21FCU;
    constexpr std::uint32_t kTextureFetch     = 0x4800U;
    constexpr std::uint32_t kVertexFetch      = kTextureFetch + 2U * 95U;

    require(original.source_fixture.vertex_shader_words == expected.vertex_shader_words &&
                original.source_fixture.pixel_shader_words == expected.pixel_shader_words,
            "original source fixture changed its raw shader words");
    require(original.source_fixture.vertex_bytes == expected.vertex_bytes &&
                original.source_fixture.index_bytes == expected.index_bytes &&
                original.source_fixture.texture.base_guest_bytes == original.new_base_bytes &&
                original.source_fixture.texture.mip_guest_bytes == original.new_mip_bytes,
            "original source fixture did not retain the updated raw input bytes");
    require(original.source_registers.size() == rex::graphics::testing::kNativePreparedBc2RegisterCount,
            "original source draw did not retain the full register bank");
    require(original.source_fixture.register_image == original.source_registers,
            "original source fixture did not retain the issued register image");
    require(original.source_vertex_fetch_words == fetchWords(original.source_registers, 95),
            "original source vertex fetch identity was not retained");
    require(original.source_texture_fetch_words == fetchWords(original.source_registers, 0),
            "original source texture fetch identity was not retained");
    require(original.source_fixture.texture.fetch_words == original.source_texture_fetch_words &&
                original.source_fixture.vertex_fetch_words == original.source_vertex_fetch_words,
            "original source fixture did not retain the issued fetch descriptors");

    const auto expectedTexture = fetchWords(expected.register_image, 0U);
    const auto expectedVertex  = fetchWords(expected.register_image, 95U);
    require((expectedTexture[1] & 0xFFFFF000U) == 0U && (expectedTexture[5] & 0xFFFFF000U) == 0U,
            "fixture texture fetch address fields changed from their guest-image placeholders");
    require(original.published_word_count != 0, "original source did not retain its published ring word count");
    require(original.source_index_guest_base != 0, "original source did not retain its index guest base");
    require(original.source_texture_fetch_words[1] != 0 && original.source_texture_fetch_words[5] != 0,
            "original source texture fetch did not retain physical base and mip addresses");
    require((original.source_vertex_fetch_words[0] & ~3U) != 0,
            "original source vertex fetch did not retain its physical address");

    // The CPU ring progress, DMA index address, and three physical fetch
    // address words are the only per-run values. Every other register and
    // every non-address fetch field must retain the fixture's exact state.
    std::size_t mismatch_count = 0;
    std::string mismatch_details;
    for (std::uint32_t index = 0; index < original.source_registers.size(); ++index)
    {
        const auto actual = original.source_registers[index];
        if (index == kCpRbWptr)
        {
            require(actual == original.published_word_count,
                    "original CP ring write pointer did not match the published word count");
            continue;
        }
        if (index == kVgtDmaBase)
        {
            require(actual == original.source_index_guest_base,
                    "original DMA base did not match the issued index guest base");
            continue;
        }
        if (index == kVgtDmaSize)
        {
            require(expected.register_image[index] == 0x40000003U && actual == expected.register_image[index],
                    "original DMA size changed from the authored index contract");
            continue;
        }
        if (index == kVgtDrawInitiator)
        {
            require(expected.register_image[index] == 0x30004U && actual == expected.register_image[index],
                    "original draw initiator changed from the authored index contract");
            continue;
        }
        if (index == kTextureFetch + 1U || index == kTextureFetch + 5U)
        {
            const auto word = index - kTextureFetch;
            require((actual & 0xFFFFF000U) != 0U && (actual & 0xFFFU) == (expectedTexture[word] & 0xFFFU),
                    "original texture physical address or descriptor flags changed");
            continue;
        }
        if (index == kVertexFetch)
        {
            require((actual & 3U) == (expectedVertex[0] & 3U),
                    "original vertex fetch changed its non-address type bits");
            require((actual & ~3U) == (original.source_vertex_fetch_words[0] & ~3U),
                    "original vertex physical address was not retained in the source descriptor");
            continue;
        }
        if (actual != expected.register_image[index])
        {
            if (mismatch_count < 32U)
            {
                if (!mismatch_details.empty())
                    mismatch_details += ", ";
                mismatch_details += std::to_string(index) + " actual=";
                mismatch_details += std::to_string(actual);
                mismatch_details += " expected=";
                mismatch_details += std::to_string(expected.register_image[index]);
            }
            ++mismatch_count;
        }
    }
    require(mismatch_count == 0,
            "original source register state differs from the exact fixture state at " + mismatch_details);

    const auto sameShaderWords = [&](const auto& item)
    {
        return item.vertex_shader_words == expected.vertex_shader_words &&
               item.pixel_shader_words == expected.pixel_shader_words;
    };
    const auto baselineDraw = std::ranges::find_if(original.draws, sameShaderWords);
    const auto finalDraw    = std::ranges::find_if(original.draws | std::views::reverse, sameShaderWords);
    require(baselineDraw != original.draws.end() && finalDraw != std::ranges::reverse_view(original.draws).end() &&
                baselineDraw->event_ordinal != finalDraw->event_ordinal,
            "original source baseline and updated draw identities were not both observed");
    require(finalDraw->registers == original.source_registers && finalDraw->original_host_issued &&
                finalDraw->cpu_data_complete && finalDraw->execution_status == 1U &&
                finalDraw->execution_reason == 0U && finalDraw->primitive_type == 4U &&
                finalDraw->requested_index_count == 3U && original.source_draw_issued,
            "original source draw did not retain the issued DMA triangle contract");
    rex::graphics::xenos::xe_gpu_texture_fetch_t sourceFetch{};
    std::memcpy(&sourceFetch, original.source_texture_fetch_words.data(), sizeof(sourceFetch));

    const auto assertPreparedJoin = [&](const NativePreparedBc2DrawRecord& draw,
                                        std::span<const std::uint8_t>      expectedBase,
                                        std::span<const std::uint8_t>      expectedMip,
                                        std::string_view                   label)
    {
        const auto matches = std::ranges::count_if(
            original.prepared_draws,
            [&](const auto& item)
            {
                return item.id == draw.id && item.frame == draw.frame && item.submission == draw.submission &&
                       item.registers == draw.registers && item.vertex_shader_words == draw.vertex_shader_words &&
                       item.pixel_shader_words == draw.pixel_shader_words;
            });
        require(matches == 1U, std::string(label) + " did not join exactly one Prepared callback");
        const auto prepared = std::ranges::find_if(
            original.prepared_draws,
            [&](const auto& item)
            {
                return item.id == draw.id && item.frame == draw.frame && item.submission == draw.submission &&
                       item.registers == draw.registers && item.vertex_shader_words == draw.vertex_shader_words &&
                       item.pixel_shader_words == draw.pixel_shader_words;
            });
        require(prepared != original.prepared_draws.end() && !prepared->original_host_issued &&
                    prepared->execution_status == 0U && prepared->event_ordinal != 0U &&
                    prepared->event_ordinal < draw.event_ordinal && !prepared->textures.empty(),
                std::string(label) + " was not retained as an unfinished pre-issue view");
        const auto selected = std::ranges::find_if(
            prepared->textures,
            [](const auto& texture)
            {
                return texture.format == static_cast<std::uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT2_3) &&
                       texture.fetch_constant == 0U && !texture.is_signed && texture.required && texture.available;
            });
        require(selected != prepared->textures.end() &&
                    std::ranges::count_if(prepared->textures, [](const auto& texture)
                                          {
                                              return texture.format == static_cast<std::uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT2_3) &&
                                                     texture.fetch_constant == 0U && !texture.is_signed && texture.required && texture.available;
                                          }) == 1U,
                std::string(label) + " did not retain exactly one required available unsigned BC2 view");
        const auto& texture          = *selected;
        std::size_t signedCompanions = 0;
        for (const auto& companion : prepared->textures)
        {
            if (!companion.is_signed)
                continue;
            ++signedCompanions;
            require(companion.fetch_constant == texture.fetch_constant && companion.stage == texture.stage &&
                        companion.shader_binding_index == texture.shader_binding_index + 1U && !companion.required &&
                        companion.fetch_words == texture.fetch_words &&
                        !companion.available && !companion.base_bytes_copied && !companion.mip_bytes_copied &&
                        companion.base_bytes.empty() && companion.mip_bytes.empty() &&
                        !companion.memory_watch_supplied &&
                        companion.unavailable_reason ==
                            static_cast<std::uint32_t>(rex::graphics::diagnostic::DrawCaptureReason::kTextureSource),
                    std::string(label) + " signed BC2 companion was admitted or retained raw bytes");
        }
        require(signedCompanions != 0U, std::string(label) + " did not retain the SDK signed BC2 companion");
        require(texture.endianness == static_cast<std::uint32_t>(rex::graphics::xenos::Endian::k8in16) &&
                    texture.width == kWidth && texture.height == kHeight && texture.depth == 1U && texture.pitch == 4U &&
                    texture.mip_min_level == 0U && texture.mip_max_level == kMipCount - 1U && texture.packed_mips &&
                    !texture.tiled && texture.base_address != 0U && texture.mip_address != 0U &&
                    texture.base_address == (sourceFetch.base_address << 12U) &&
                    texture.mip_address == (sourceFetch.mip_address << 12U) &&
                    texture.base_size == 0x2000U && texture.mip_size == 0x4680U && texture.base_bytes_copied &&
                    texture.mip_bytes_copied && texture.base_bytes.size() == texture.base_size &&
                    texture.mip_bytes.size() == texture.mip_size && !texture.memory_watch_stale,
                std::string(label) + " did not retain a complete current CPU-owned raw BC2 view");
        require(expectedBase.size() >= texture.base_bytes.size() && expectedMip.size() >= texture.mip_bytes.size() &&
                    std::ranges::equal(texture.base_bytes, expectedBase.first(texture.base_bytes.size())) &&
                    std::ranges::equal(texture.mip_bytes, expectedMip.first(texture.mip_bytes.size())),
                std::string(label) + " raw bytes differ from its admitted allocation");
    };
    assertPreparedJoin(*baselineDraw, original.old_base_bytes, original.old_mip_bytes, "baseline Prepared source");
    assertPreparedJoin(*finalDraw, original.new_base_bytes, original.new_mip_bytes, "updated Prepared source");
}

void writeOriginalDiagnostics(const std::filesystem::path&           outputRoot,
                              std::uint32_t                          pairIndex,
                              std::uint32_t                          first,
                              std::uint32_t                          second,
                              const NativePreparedBc2OriginalResult& original,
                              std::string_view                       preparedCollectorError = {})
{
    const auto terminalPath = outputRoot / ("original-" + std::to_string(pairIndex) + "-terminal.rgba8.bin");
    const auto baselinePath = outputRoot / ("original-" + std::to_string(pairIndex) + "-baseline-terminal.rgba8.bin");
    const auto baselineExpectedPath =
        outputRoot / ("original-" + std::to_string(pairIndex) + "-baseline-expected.rgba8.bin");
    const auto expectedPath       = outputRoot / ("original-" + std::to_string(pairIndex) + "-expected.rgba8.bin");
    const auto oldBasePath        = outputRoot / ("original-" + std::to_string(pairIndex) + "-old-base.bc2.bin");
    const auto oldMipPath         = outputRoot / ("original-" + std::to_string(pairIndex) + "-old-mips.bc2.bin");
    const auto newBasePath        = outputRoot / ("original-" + std::to_string(pairIndex) + "-new-base.bc2.bin");
    const auto newMipPath         = outputRoot / ("original-" + std::to_string(pairIndex) + "-new-mips.bc2.bin");
    const auto sourceRegisterPath = outputRoot / ("original-" + std::to_string(pairIndex) + "-source-registers.le.bin");
    writeBytes(terminalPath, original.terminal_pixels);
    writeBytes(baselinePath, original.baseline_terminal_pixels);
    writeBytes(baselineExpectedPath, original.baseline_expected_pixels);
    writeBytes(expectedPath, original.expected_pixels);
    writeBytes(oldBasePath, original.old_base_bytes);
    writeBytes(oldMipPath, original.old_mip_bytes);
    writeBytes(newBasePath, original.new_base_bytes);
    writeBytes(newMipPath, original.new_mip_bytes);
    writeBytes(sourceRegisterPath, wordsBytes(original.source_registers));
    const auto  comparable          = original.baseline_terminal_pixels.size() == original.baseline_expected_pixels.size();
    std::size_t firstMismatchOffset = std::numeric_limits<std::size_t>::max();
    std::size_t mismatchBytes       = 0U;
    std::size_t mismatchPixels      = 0U;
    std::size_t firstMismatchPixel  = std::numeric_limits<std::size_t>::max();
    if (comparable)
    {
        for (std::size_t offset = 0U; offset < original.baseline_expected_pixels.size(); ++offset)
        {
            if (original.baseline_terminal_pixels[offset] == original.baseline_expected_pixels[offset])
                continue;
            if (firstMismatchOffset == std::numeric_limits<std::size_t>::max())
                firstMismatchOffset = offset;
            ++mismatchBytes;
        }
        if (original.baseline_expected_pixels.size() % 4U == 0U)
        {
            for (std::size_t offset = 0U; offset < original.baseline_expected_pixels.size(); offset += 4U)
            {
                if (!std::equal(original.baseline_terminal_pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                                original.baseline_terminal_pixels.begin() + static_cast<std::ptrdiff_t>(offset + 4U),
                                original.baseline_expected_pixels.begin() + static_cast<std::ptrdiff_t>(offset)))
                {
                    if (firstMismatchPixel == std::numeric_limits<std::size_t>::max())
                        firstMismatchPixel = offset / 4U;
                    ++mismatchPixels;
                }
            }
        }
    }
    const auto firstMismatchX = firstMismatchPixel == std::numeric_limits<std::size_t>::max()
                                    ? 0U
                                    : static_cast<std::uint32_t>(firstMismatchPixel % kOutputWidth);
    const auto firstMismatchY = firstMismatchPixel == std::numeric_limits<std::size_t>::max()
                                    ? 0U
                                    : static_cast<std::uint32_t>(firstMismatchPixel / kOutputWidth);
    const auto sourceDraws    = std::views::reverse(original.draws);
    const auto sourceDraw     = std::ranges::find_if(
        sourceDraws,
        [&](const auto& item)
        {
            return item.vertex_shader_words == original.source_fixture.vertex_shader_words &&
                   item.pixel_shader_words == original.source_fixture.pixel_shader_words;
        });
    const bool                         sourceDrawObserved = sourceDraw != sourceDraws.end();
    const NativePreparedBc2DrawRecord* sourceDrawRecord   = sourceDrawObserved ? &*sourceDraw : nullptr;
    std::ofstream                      report(outputRoot / ("original-" + std::to_string(pairIndex) + ".toml"),
                                              std::ios::binary | std::ios::trunc);
    require(report.good(), "could not create original prepared BC2 diagnostics");
    report << "schema_version = 1\nuv_bias_base_texels = 0.25\ntexel_edge_guard = true\npair_index = " << pairIndex << "\nfirst_lod = " << first
           << "\nsecond_lod = " << second << "\nprogressed = " << (original.progressed ? "true" : "false")
           << "\ncompletion_observed = " << (original.completion_observed ? "true" : "false")
           << "\nbaseline_completion_observed = " << (original.baseline_completion_observed ? "true" : "false")
           << "\nbaseline_terminal_capture = " << (original.baseline_terminal_capture ? "true" : "false")
           << "\ncpu_update_observed = " << (original.cpu_update_observed ? "true" : "false")
           << "\nterminal_capture = " << (original.terminal_capture ? "true" : "false")
           << "\nactual_readback_owner = " << (original.actual_readback_owner ? "true" : "false")
           << "\ncpu_partial_overlap_writes = " << (original.cpu_partial_overlap_writes ? "true" : "false")
           << "\nsource_raw_bytes_frozen = " << (original.source_raw_bytes_frozen ? "true" : "false")
           << "\ncpu_update_level = " << original.cpu_update_level
           << "\ncpu_update_kind = " << static_cast<std::uint32_t>(original.update_kind)
           << "\nprepared_collector_error = " << tomlString(preparedCollectorError)
           << "\nfailure = " << tomlString(original.failure)
           << "\nsource_draw_issued = " << (original.source_draw_issued ? "true" : "false")
           << "\nsource_draw_observed = " << (sourceDrawObserved ? "true" : "false")
           << "\nsource_draw_event_order = " << (sourceDrawRecord ? sourceDrawRecord->event_ordinal : 0)
           << "\nsource_draw_id = " << (sourceDrawRecord ? sourceDrawRecord->id : 0)
           << "\nsource_draw_frame = " << (sourceDrawRecord ? sourceDrawRecord->frame : 0)
           << "\nsource_draw_submission = " << (sourceDrawRecord ? sourceDrawRecord->submission : 0)
           << "\nsource_original_host_issued = "
           << (sourceDrawRecord && sourceDrawRecord->original_host_issued ? "true" : "false")
           << "\nsource_cpu_data_complete = "
           << (sourceDrawRecord && sourceDrawRecord->cpu_data_complete ? "true" : "false")
           << "\nsource_execution_status = " << (sourceDrawRecord ? sourceDrawRecord->execution_status : 0)
           << "\nsource_execution_reason = " << (sourceDrawRecord ? sourceDrawRecord->execution_reason : 0)
           << "\nsource_draw_primitive_type = " << (sourceDrawRecord ? sourceDrawRecord->primitive_type : 0)
           << "\nsource_draw_requested_index_count = "
           << (sourceDrawRecord ? sourceDrawRecord->requested_index_count : 0)
           << "\ncpu_update_guest_offset = " << original.cpu_update_guest_offset
           << "\ncpu_update_guest_length = " << original.cpu_update_guest_length
           << "\ncpu_update_overlap_written = " << (original.cpu_update_overlap_written ? "true" : "false")
           << "\ncpu_update_outside_unchanged = "
           << (original.cpu_update_outside_unchanged ? "true" : "false")
           << "\ncpu_update_opposite_unchanged = "
           << (original.cpu_update_opposite_unchanged ? "true" : "false")
           << "\nsource_index_guest_base = " << original.source_index_guest_base
           << "\npublished_word_count = " << original.published_word_count
           << "\nruntime_module_path = " << tomlString(original.runtime_module_path)
           << "\ngpu_module_path = " << tomlString(original.gpu_module_path)
           << "\nterminal_pixels_match_expected = " << (original.terminal_pixels_match_expected ? "true" : "false")
           << "\nterminal_width = " << original.terminal_width << "\nterminal_height = " << original.terminal_height
           << "\nterminal_stride = " << original.terminal_stride
           << "\nsource_register_count = " << original.source_registers.size()
           << "\ncompleted_fence = " << original.completed_fence
           << "\ncompletion_copy_id = " << original.completion_copy_id
           << "\ncompletion_swap_id = " << original.completion_swap_id
           << "\ncompletion_frame = " << original.completion_frame
           << "\ncompletion_submission = " << original.completion_submission
           << "\nbaseline_copy_id = " << original.baseline_copy_id
           << "\nbaseline_swap_id = " << original.baseline_swap_id
           << "\nbaseline_frame = " << original.baseline_frame
           << "\nbaseline_submission = " << original.baseline_submission
           << "\ncompletion_fence_reported = false\nbaseline_expected_comparable = "
           << (comparable ? "true" : "false") << "\nbaseline_expected_mismatch_bytes = "
           << mismatchBytes << "\nbaseline_expected_mismatch_pixels = " << mismatchPixels
           << "\nbaseline_expected_first_mismatch_offset = "
           << (firstMismatchOffset == std::numeric_limits<std::size_t>::max() ? 0U : firstMismatchOffset)
           << "\nbaseline_expected_first_mismatch_x = " << firstMismatchX
           << "\nbaseline_expected_first_mismatch_y = " << firstMismatchY
           << "\nsource_vertex_word_sha256 = \""
           << wordDigest(original.source_fixture.vertex_shader_words) << "\"\nsource_pixel_word_sha256 = \""
           << wordDigest(original.source_fixture.pixel_shader_words) << "\"\nsource_texture_fetch_words = [";
    for (std::size_t index = 0; index < original.source_texture_fetch_words.size(); ++index)
        report << (index ? ", " : "") << original.source_texture_fetch_words[index];
    report << "]\nsource_vertex_fetch_words = [";
    for (std::size_t index = 0; index < original.source_vertex_fetch_words.size(); ++index)
        report << (index ? ", " : "") << original.source_vertex_fetch_words[index];
    report << "]\nreadback_owner = " << tomlString(original.readback_owner)
           << "\nbaseline_terminal_file = \"" << baselinePath.filename().string()
           << "\"\nbaseline_expected_file = \"" << baselineExpectedPath.filename().string()
           << "\"\nold_base_file = \"" << oldBasePath.filename().string()
           << "\"\nold_mips_file = \"" << oldMipPath.filename().string()
           << "\"\nnew_base_file = \"" << newBasePath.filename().string()
           << "\"\nnew_mips_file = \"" << newMipPath.filename().string()
           << "\"\nsource_register_file = \""
           << sourceRegisterPath.filename().string() << "\"\nexpected_file = \""
           << expectedPath.filename().string() << "\"\nterminal_file = \""
           << terminalPath.filename().string() << "\"\nresolve_readback_readback_owner = false\n";

    const auto writeCopies = [&](std::string_view label, const auto& records)
    {
        report << label << "_count = " << records.size() << "\n";
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const auto& record = records[index];
            const auto  key    = std::string(label) + "_" + std::to_string(index);
            report << key << "_event_order = " << record.event_ordinal << "\n"
                   << key << "_id = " << record.id << "\n"
                   << key << "_frame = " << record.frame << "\n"
                   << key << "_submission = " << record.submission << "\n"
                   << key << "_mode = " << tomlString(record.mode) << "\n"
                   << key << "_mode_is_resolve_readback = "
                   << (record.mode == "resolve_readback" ? "true" : "false") << "\n"
                   << key << "_original_succeeded = " << (record.original_succeeded ? "true" : "false") << "\n"
                   << key << "_cpu_data_complete = " << (record.cpu_data_complete ? "true" : "false") << "\n"
                   << key << "_unavailable_reason = " << record.unavailable_reason << "\n"
                   << key << "_source_rect_present = " << (record.source_rect_present ? "true" : "false") << "\n"
                   << key << "_source_x = " << record.source_x << "\n"
                   << key << "_source_y = " << record.source_y << "\n"
                   << key << "_source_width = " << record.source_width << "\n"
                   << key << "_source_height = " << record.source_height << "\n"
                   << key << "_source_geometry_fetch_constant = "
                   << record.source_geometry_fetch_constant << "\n"
                   << key << "_source_geometry_address = " << record.source_geometry_address << "\n"
                   << key << "_source_geometry_size = " << record.source_geometry_size << "\n"
                   << key << "_written_address = " << record.written_address << "\n"
                   << key << "_written_length = " << record.written_length << "\n"
                   << key << "_planned_address = " << record.planned_address << "\n"
                   << key << "_planned_length = " << record.planned_length << "\n"
                   << key << "_register_count = " << record.registers.size() << "\n"
                   << key << "_readback_owner = false\n";
        }
    };
    const auto writeDraws = [&](std::string_view label, const auto& records)
    {
        report << label << "_count = " << records.size() << "\n";
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const auto& record = records[index];
            const auto  key    = std::string(label) + "_" + std::to_string(index);
            report << key << "_event_order = " << record.event_ordinal << "\n"
                   << key << "_id = " << record.id << "\n"
                   << key << "_frame = " << record.frame << "\n"
                   << key << "_submission = " << record.submission << "\n"
                   << key << "_original_host_issued = "
                   << (record.original_host_issued ? "true" : "false") << "\n"
                   << key << "_cpu_data_complete = "
                   << (record.cpu_data_complete ? "true" : "false") << "\n"
                   << key << "_execution_status = " << record.execution_status << "\n"
                   << key << "_execution_reason = " << record.execution_reason << "\n"
                   << key << "_primitive_type = " << record.primitive_type << "\n"
                   << key << "_requested_index_count = " << record.requested_index_count << "\n"
                   << key << "_register_count = " << record.registers.size() << "\n"
                   << key << "_vertex_shader_sha256 = \"" << wordDigest(record.vertex_shader_words)
                   << "\"\n"
                   << key << "_pixel_shader_sha256 = \"" << wordDigest(record.pixel_shader_words)
                   << "\"\n"
                   << key << "_texture_count = " << record.textures.size() << "\n";
            for (std::size_t textureIndex = 0; textureIndex < record.textures.size(); ++textureIndex)
            {
                const auto& texture    = record.textures[textureIndex];
                const auto  textureKey = key + "_texture_" + std::to_string(textureIndex);
                report << textureKey << "_fetch_constant = " << texture.fetch_constant << "\n"
                       << textureKey << "_format = " << texture.format << "\n"
                       << textureKey << "_endianness = " << texture.endianness << "\n"
                       << textureKey << "_dimension = " << texture.dimension << "\n"
                       << textureKey << "_width = " << texture.width << "\n"
                       << textureKey << "_height = " << texture.height << "\n"
                       << textureKey << "_depth = " << texture.depth << "\n"
                       << textureKey << "_pitch = " << texture.pitch << "\n"
                       << textureKey << "_mip_min_level = " << texture.mip_min_level << "\n"
                       << textureKey << "_mip_max_level = " << texture.mip_max_level << "\n"
                       << textureKey << "_fetch_word_count = " << texture.fetch_words.size() << "\n"
                       << textureKey << "_tiled = " << (texture.tiled ? "true" : "false") << "\n"
                       << textureKey << "_packed_mips = " << (texture.packed_mips ? "true" : "false") << "\n"
                       << textureKey << "_available = " << (texture.available ? "true" : "false") << "\n"
                       << textureKey << "_gpu_written = " << (texture.gpu_written ? "true" : "false") << "\n"
                       << textureKey << "_unavailable_reason = " << texture.unavailable_reason << "\n"
                       << textureKey << "_base_address = " << texture.base_address << "\n"
                       << textureKey << "_base_size = " << texture.base_size << "\n"
                       << textureKey << "_mip_address = " << texture.mip_address << "\n"
                       << textureKey << "_mip_size = " << texture.mip_size << "\n"
                       << textureKey << "_stage = " << texture.stage << "\n"
                       << textureKey << "_shader_binding_index = " << texture.shader_binding_index << "\n"
                       << textureKey << "_sampler_value = " << texture.sampler_value << "\n"
                       << textureKey << "_component_mapping = " << texture.component_mapping << "\n"
                       << textureKey << "_swizzled_signs = " << static_cast<unsigned>(texture.swizzled_signs)
                       << "\n"
                       << textureKey << "_is_signed = " << (texture.is_signed ? "true" : "false") << "\n"
                       << textureKey << "_required = " << (texture.required ? "true" : "false") << "\n"
                       << textureKey << "_base_bytes_copied = " << (texture.base_bytes_copied ? "true" : "false")
                       << "\n"
                       << textureKey << "_mip_bytes_copied = " << (texture.mip_bytes_copied ? "true" : "false")
                       << "\n"
                       << textureKey << "_base_bytes_length = " << texture.base_bytes.size() << "\n"
                       << textureKey << "_mip_bytes_length = " << texture.mip_bytes.size() << "\n"
                       << textureKey << "_memory_watch_supplied = "
                       << (texture.memory_watch_supplied ? "true" : "false") << "\n"
                       << textureKey << "_memory_watch_identity = " << texture.memory_watch_identity << "\n"
                       << textureKey << "_memory_watch_generation = " << texture.memory_watch_generation << "\n"
                       << textureKey << "_memory_current_before = "
                       << (texture.memory_current_before ? "true" : "false") << "\n"
                       << textureKey << "_memory_current_after = "
                       << (texture.memory_current_after ? "true" : "false") << "\n"
                       << textureKey << "_memory_watch_stale = "
                       << (texture.memory_watch_stale ? "true" : "false") << "\n";
            }
        }
    };
    const auto writeSwaps = [&](std::string_view label, const auto& records)
    {
        report << label << "_count = " << records.size() << "\n";
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const auto& record = records[index];
            const auto  key    = std::string(label) + "_" + std::to_string(index);
            report << key << "_event_order = " << record.event_ordinal << "\n"
                   << key << "_id = " << record.id << "\n"
                   << key << "_frame = " << record.frame << "\n"
                   << key << "_submission = " << record.submission << "\n"
                   << key << "_frontbuffer_ptr = " << record.frontbuffer_ptr << "\n"
                   << key << "_width = " << record.width << "\n"
                   << key << "_height = " << record.height << "\n"
                   << key << "_original_succeeded = " << (record.original_succeeded ? "true" : "false") << "\n";
        }
    };
    const auto writeFrameEnds = [&](const auto& records)
    {
        report << "frame_end_count = " << records.size() << "\n";
        for (std::size_t index = 0; index < records.size(); ++index)
        {
            const auto& record = records[index];
            const auto  key    = std::string("frame_end_") + std::to_string(index);
            report << key << "_event_order = " << record.event_ordinal << "\n"
                   << key << "_frame = " << record.frame << "\n"
                   << key << "_submission = " << record.submission << "\n"
                   << key << "_original_succeeded = " << (record.original_succeeded ? "true" : "false") << "\n";
        }
    };
    writeCopies("copy", original.copies);
    writeCopies("prepared_copy", original.prepared_copies);
    writeDraws("draw", original.draws);
    writeDraws("prepared_draw", original.prepared_draws);
    writeSwaps("swap", original.swaps);
    writeSwaps("prepared_swap", original.prepared_swaps);
    writeFrameEnds(original.frame_ends);
    report.flush();
    require(report.good(), "could not flush original prepared BC2 diagnostics");
}

void assertOriginalCompletion(const NativePreparedBc2OriginalResult& original)
{
    require(original.completion_copy_id != 0 && original.completion_swap_id != 0,
            "original completion did not retain copy and swap identities");
    const auto copy = std::ranges::find_if(
        original.copies,
        [&](const auto& record)
        {
            return record.id == original.completion_copy_id;
        });
    const auto swap = std::ranges::find_if(
        original.swaps,
        [&](const auto& record)
        {
            return record.id == original.completion_swap_id;
        });
    require(copy != original.copies.end() && swap != original.swaps.end(),
            "original completion identities were not retained in callback records");
    require(copy->original_succeeded && copy->written_length != 0 && copy->event_ordinal < swap->event_ordinal &&
                copy->frame == swap->frame && copy->submission == swap->submission && swap->frontbuffer_ptr != 0,
            "original completion copy and swap were not ordered in one successful submission");
    require(copy->frame == original.completion_frame && copy->submission == original.completion_submission,
            "original completion frame identity differs from the matched copy");
    const auto frameEnd = std::ranges::find_if(
        original.frame_ends,
        [&](const auto& record)
        {
            return record.frame == swap->frame && record.submission == swap->submission &&
                   record.event_ordinal > swap->event_ordinal && record.original_succeeded;
        });
    require(frameEnd != original.frame_ends.end(),
            "original completion lacks a successful frame end after the matched swap");
}

void assertOriginalCopyExtent(const NativePreparedBc2CopyRecord& copy, std::string_view label)
{
    require(copy.source_rect_present && copy.source_x == 0U && copy.source_y == 0U &&
                copy.source_width == kOutputWidth && copy.source_height == kOutputHeight,
            std::string(label) + " did not retain the full actual copy source rectangle");
    require(copy.registers.size() == rex::graphics::testing::kNativePreparedBc2RegisterCount &&
                copy.registers[0x2319U] == copy.written_address &&
                copy.registers[0x231AU] == (kOutputWidth | (kOutputHeight << 16U)) &&
                copy.written_address == copy.planned_address && copy.written_length == outputGuestExtent() &&
                copy.written_length == copy.planned_length,
            std::string(label) + " did not retain the actual full-copy register plan");
}

void assertOriginalPreparedCopyExtent(const NativePreparedBc2OriginalResult& original,
                                      const NativePreparedBc2CopyRecord&     copy,
                                      std::string_view                       label)
{
    const auto prepared = std::ranges::find_if(
        original.prepared_copies,
        [&](const auto& candidate)
        {
            return candidate.id == copy.id && candidate.frame == copy.frame && candidate.submission == copy.submission;
        });
    require(prepared != original.prepared_copies.end(), std::string(label) + " lacked its Prepared copy join");
    require(prepared->event_ordinal < copy.event_ordinal && !prepared->original_succeeded &&
                !prepared->written_address && !prepared->written_length && prepared->planned_address == copy.planned_address &&
                prepared->planned_length == outputGuestExtent() && prepared->planned_length == copy.planned_length &&
                prepared->source_rect_present && prepared->source_x == 0U && prepared->source_y == 0U &&
                prepared->source_width == kOutputWidth && prepared->source_height == kOutputHeight,
            std::string(label) + " did not retain the full Prepared copy plan before consumption");
}

void assertCandidateSource(const std::filesystem::path& source)
{
    const auto bytes       = readBytes(source);
    const auto text        = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto setter      = text.find("registerLod = r0.z;");
    const auto second      = text.find("registerLod = r0.w;");
    const auto firstFetch  = text.find("= tfetch2DRegisterLod");
    const auto secondFetch = firstFetch == std::string::npos
                                 ? std::string::npos
                                 : text.find("= tfetch2DRegisterLod", firstFetch + 1);
    const auto thirdFetch  = secondFetch == std::string::npos
                                 ? std::string::npos
                                 : text.find("= tfetch2DRegisterLod", secondFetch + 1);
    require(setter != std::string::npos && second != std::string::npos && firstFetch != std::string::npos &&
                secondFetch != std::string::npos && thirdFetch == std::string::npos && setter < firstFetch &&
                firstFetch < second && second < secondFetch,
            "candidate source does not retain two ordered non-x register LOD pairs");
    require(text.find("SampleLevel") != std::string::npos, "candidate source lacks explicit SampleLevel");
    require(text.find("= tfetch2D(") == std::string::npos,
            "candidate source retained an implicit computed-LOD fetch");
    const auto vertexBytes = readBytes(source.parent_path() / "vs_D1A5A8B7C6E50123.hlsl");
    const auto vertexText  = std::string(reinterpret_cast<const char*>(vertexBytes.data()), vertexBytes.size());
    require(vertexText.find("r3.xyzw = sintTexcoord") != std::string::npos &&
                vertexText.find("r0.xyzw = sintTexcoord") == std::string::npos &&
                vertexText.find("r0.y = dot") != std::string::npos &&
                vertexText.find("oTexCoord0.xyzw = max(r3.xyzw, r3.xyzw)") != std::string::npos,
            "candidate source did not preserve fetched UVs in r3 through the interpolator export");
}

void assertFixtureGuards()
{
    const auto                                   fixture = MakeNativePreparedBc2Fixture();
    rex::graphics::ucode::VertexFetchInstruction uvFetch{};
    rex::graphics::ucode::AluInstruction         interpolatorExport{};
    require(fixture.vertex_shader_words.size() == 45U, "fixture vertex shader word extent changed");
    std::memcpy(&uvFetch, fixture.vertex_shader_words.data() + 12U, sizeof(uvFetch));
    std::memcpy(&interpolatorExport, fixture.vertex_shader_words.data() + 39U, sizeof(interpolatorExport));
    require(uvFetch.dest() == 3U && uvFetch.fetch_constant_index() == 95U &&
                uvFetch.dest_swizzle() == 0x688U,
            "fixture UV fetch did not move to temporary r3 with its exact fetch fields");
    require(interpolatorExport.is_export() && interpolatorExport.src_is_temp(1U) &&
                interpolatorExport.src_is_temp(2U) && interpolatorExport.src_reg(1U) == 3U &&
                interpolatorExport.src_reg(2U) == 3U &&
                fixture.vertex_shader_words[41U] == 0xE2030300U,
            "fixture interpolator export did not read r3/r3 with its exact raw field");
    rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fixture.texture.fetch_words.data(), sizeof(fetch));
    require(fixture.width == kWidth && fixture.height == kHeight && fixture.terminal_stride == kOutputWidth * 4U &&
                fixture.vertex_stride == 56,
            "fixture dimensions or vertex stride changed");
    require(fixture.register_image.size() == rex::graphics::testing::kNativePreparedBc2RegisterCount,
            "fixture register image extent changed");
    require(fixture.register_image[0x2000U] == 0x14000500U &&
                fixture.register_image[0x2082U] == (kWidth | (kHeight << 16U)) &&
                fixture.register_image[0x210FU] == std::bit_cast<std::uint32_t>(32.0F) &&
                fixture.register_image[0x2110U] == std::bit_cast<std::uint32_t>(32.0F) &&
                fixture.register_image[0x2111U] == std::bit_cast<std::uint32_t>(-32.0F) &&
                fixture.register_image[0x2112U] == std::bit_cast<std::uint32_t>(32.0F) &&
                fixture.register_image[0x2201U] == 0x01000006U,
            "fixture render canvas, source scissor, or alpha-visible blend changed");
    require(fixture.vertex_bytes.size() == 3 * 56 && fixture.index_bytes.size() == 6,
            "fixture geometry extent changed");
    const auto expectedUv = std::array<std::array<float, 2>, 3>{ { { 0.25F / kWidth, 1.0F + 0.25F / kHeight }, { 0.25F / kWidth, -1.0F + 0.25F / kHeight }, { 2.0F + 0.25F / kWidth, 1.0F + 0.25F / kHeight } } };
    for (std::size_t vertex = 0; vertex < expectedUv.size(); ++vertex)
    {
        const auto base = vertex * fixture.vertex_stride;
        require(bigEndianFloat(fixture.vertex_bytes, base + 16U) == expectedUv[vertex][0] &&
                    bigEndianFloat(fixture.vertex_bytes, base + 20U) == expectedUv[vertex][1],
                "fixture UV triangle does not cover the patterned source texels");
    }
    // Original's +0.5 geometry shift interpolates at integer positions.
    // SDK dxbc_translator_fetch.cpp adds 1.5/1024 of a base texel.
    for (const auto& mip : fixture.texture.mips)
        for (const auto axis : std::array<std::array<std::uint32_t, 2>, 2>{ { { kWidth, mip.width }, { kHeight, mip.height } } })
        {
            const double  margin         = (1.5 / 1024.0) * axis[1] / axis[0];
            std::uint32_t lastTexel      = 0U;
            std::uint32_t distinctTexels = 0U;
            for (std::uint32_t pixel = 0U; pixel < axis[0]; ++pixel)
            {
                const double original = (pixel + 0.25) * axis[1] / axis[0];
                const double center   = (pixel + 0.75) * axis[1] / axis[0];
                const auto   texel    = static_cast<std::uint32_t>(original);
                require(texel == static_cast<std::uint32_t>(center) &&
                            original > texel + margin && original < texel + 1.0 - margin &&
                            center > texel + margin && center < texel + 1.0 - margin &&
                            original < axis[1] && center < axis[1],
                        "pixel-center conventions differ, hit a texel edge, or leave the level");
                if (pixel == 0U || texel != lastTexel)
                    ++distinctTexels;
                lastTexel = texel;
            }
            require(distinctTexels == axis[1] && lastTexel / 4U == (axis[1] - 1U) / 4U,
                    "biased viewport lost distinct texels or the last block");
        }
    require(fixture.texture.mips.size() == kMipCount && fixture.expected_by_lod_pair.size() == kMipCount * kMipCount,
            "fixture mip or expected-pair extent changed");
    require(fixture.texture.format == static_cast<std::uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT2_3) &&
                fixture.texture.pitch == 4U && fixture.texture.packed_mips && !fixture.texture.tiled &&
                fixture.texture.base_guest_bytes.size() >= 0x2000U && fixture.texture.mip_guest_bytes.size() >= 0x4680U,
            "fixture does not retain the exact packed BC2 guest footprint");
    require(fixture.texture.base_guest_bytes.size() >= 0x2000U && fixture.texture.mip_guest_bytes.size() >= 0x4680U,
            "fixture packed BC2 allocation extents changed");
    require(fetch.format == rex::graphics::xenos::TextureFormat::k_DXT2_3 &&
                fetch.endianness == rex::graphics::xenos::Endian::k8in16 && fetch.pitch == 4U &&
                fetch.packed_mips && fetch.swizzle == 0x688U && fetch.mip_min_level == 0U &&
                fetch.mip_max_level == 6U,
            "fixture fetch words do not retain the exact BC2 interpretation");
    const auto unpack565 = [](std::uint16_t color)
    {
        return std::array<std::uint32_t, 3>{ (color >> 11U) & 0x1FU, (color >> 5U) & 0x3FU, color & 0x1FU };
    };
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto endpoint0 = unpack565(kExpectedBc2Endpoints[level][0]);
        const auto endpoint1 = unpack565(kExpectedBc2Endpoints[level][1]);
        require(kExpectedBc2Endpoints[level][0] < kExpectedBc2Endpoints[level][1],
                "BC2 endpoint reference does not retain strict four-color ordering at mip " + std::to_string(level));
        require(endpoint0[1] == endpoint1[1] && (endpoint0[1] == 0U || endpoint0[1] == 63U),
                "BC2 endpoint reference does not retain constant 0/full-scale green at mip " + std::to_string(level));
        for (std::size_t channel = 0; channel < endpoint0.size(); ++channel)
        {
            const auto fullScale = channel == 1U ? 63U : 31U;
            require((endpoint0[channel] == 0U || endpoint0[channel] == fullScale) &&
                        (endpoint1[channel] == 0U || endpoint1[channel] == fullScale),
                    "BC2 endpoint reference uses a non-representable 565 component at mip " + std::to_string(level));
            const auto promoted0  = channel == 1U ? (endpoint0[channel] << 2U) | (endpoint0[channel] >> 4U)
                                                  : (endpoint0[channel] << 3U) | (endpoint0[channel] >> 2U);
            const auto promoted1  = channel == 1U ? (endpoint1[channel] << 2U) | (endpoint1[channel] >> 4U)
                                                  : (endpoint1[channel] << 3U) | (endpoint1[channel] >> 2U);
            const auto difference = promoted0 > promoted1 ? promoted0 - promoted1 : promoted1 - promoted0;
            require(difference % 3U == 0U,
                    "BC2 endpoint interpolation difference is not divisible by three at mip " + std::to_string(level));
        }
    }
    for (std::uint32_t first = 0; first < kMipCount; ++first)
        for (std::uint32_t second = first + 1U; second < kMipCount; ++second)
            require(kExpectedBc2Endpoints[first] != kExpectedBc2Endpoints[second],
                    "BC2 endpoint reference unexpectedly reuses a level pair");
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto& mip = fixture.texture.mips[level];
        require(mip.level == level && mip.width == rex::graphics::testing::kNativePreparedBc2Widths[level] &&
                    mip.height == rex::graphics::testing::kNativePreparedBc2Heights[level] &&
                    mip.guest_offset == rex::graphics::testing::kNativePreparedBc2GuestOffsets[level] &&
                    mip.row_pitch == rex::graphics::testing::kNativePreparedBc2RowPitches[level] &&
                    mip.linear_rgba8.size() == static_cast<std::size_t>(mip.width) * mip.height * 4U &&
                    mip.guest_extent != 0U,
                "fixture BC2 mip dimensions, offsets, pitch, or independently decoded pixels are incomplete");
    }
    for (std::uint32_t first = 0; first < kMipCount; ++first)
        for (std::uint32_t second = 0; second < kMipCount; ++second)
            require(ExpectedNativePreparedBc2Pixels(fixture, first, second).size() == kPlane,
                    "fixture expected-pair pixels are incomplete");
    const auto expected         = ExpectedNativePreparedBc2Pixels(fixture, 0U, 1U);
    const auto background_alpha = ((static_cast<std::size_t>(kOutputHeight) - 1U) * kOutputWidth + (kOutputWidth - 1U)) * 4U + 3U;
    require(expected.size() == kPlane && expected[3U] == 255U && expected[background_alpha] == 255U,
            "fixture expected canvas does not retain presenter-normalized alpha 255");
}

void emitFixture(const std::filesystem::path& output)
{
    require(!std::filesystem::exists(output), "fixture output must be fresh");
    std::filesystem::create_directories(output);
    const auto fixture = MakeNativePreparedBc2Fixture();
    writeBytes(output / "registers.le.bin", wordsBytes(fixture.register_image));
    writeBytes(output / "vertex-ucode.le.bin", wordsBytes(fixture.vertex_shader_words));
    writeBytes(output / "pixel-ucode.le.bin", wordsBytes(fixture.pixel_shader_words));
    writeBytes(output / "vertices.bin", fixture.vertex_bytes);
    writeBytes(output / "indices.bin", fixture.index_bytes);
    writeBytes(output / "texture-base.bc2.bin", fixture.texture.base_guest_bytes);
    writeBytes(output / "texture-mips.bc2.bin", fixture.texture.mip_guest_bytes);
    std::ofstream metadata(output / "fixture.json", std::ios::binary | std::ios::trunc);
    require(metadata.good(), "could not create fixture metadata");
    metadata.precision(17);
    metadata << "{\n  \"schema_version\": 1,\n  \"uv_bias_base_texels\": 0.25,\n  \"width\": 64,\n  \"height\": 64,\n  \"output_width\": 1280,\n  \"output_height\": 720,\n"
             << "  \"mip_levels\": 7,\n  \"format\": \"k_DXT2_3\",\n  \"format_value\": 19,\n"
             << "  \"endianness\": \"k8in16\",\n  \"swizzle\": " << fixture.texture.swizzle << ",\n  \"pitch\": 4,\n"
             << "  \"packed_mips\": true,\n  \"base_extent\": 8192,\n  \"mip_extent\": 18048,\n  \"packed_tail_offset\": 16384,\n  \"packed_tail_extent\": 1664,\n  \"guest_offsets\": [0, 0, 16448, 16416, 16400, 17408, 16896],\n  \"row_pitch\": 512,\n"
             << "  \"raw_storage\": \"guest_bc2\",\n  \"vertex_fetch_constant\": 95,\n"
             << "  \"vertex_stride_bytes\": 56,\n  \"lod_source_register\": 0,\n"
             << "  \"lod_source_swizzles\": [\"z\", \"w\"],\n"
             << "  \"computed_lod_enabled\": false,\n  \"register_lod_enabled\": true,\n"
             << "  \"setter_fetch_pairs\": 2,\n  \"vertex_word_sha256\": \""
             << wordDigest(fixture.vertex_shader_words) << "\",\n  \"pixel_word_sha256\": \""
             << wordDigest(fixture.pixel_shader_words) << "\"\n}\n";
    metadata.flush();
    require(metadata.good(), "could not flush fixture metadata");
    std::cout << output / "fixture.json" << '\n';
}

NativePreparedBc2OriginalResult runOriginal(std::uint32_t                                       first,
                                            std::uint32_t                                       second,
                                            NativePreparedBc2Fixture                            fixture,
                                            const std::shared_ptr<PreparedCollector>&           collector,
                                            rex::graphics::testing::NativePreparedBc2UpdateKind updateKind =
                                                rex::graphics::testing::NativePreparedBc2UpdateKind::kCombined)
{
    NativePreparedBc2OriginalParams params;
    params.fixture     = std::move(fixture);
    params.first_lod   = first;
    params.second_lod  = second;
    params.update_kind = updateKind;
    NativePreparedBc2OriginalResult result;
    const auto                      fallbackFixture  = params.fixture;
    const auto                      fallbackExpected = ExpectedNativePreparedBc2Pixels(params.fixture, first, second);
    std::string                     error;
    try
    {
        const auto status = RunNativePreparedBc2Original(params, collector, result, error);
        if (result.source_fixture.vertex_shader_words.empty())
            result.source_fixture = fallbackFixture;
        if (result.expected_pixels.empty())
            result.expected_pixels = fallbackExpected;
        if (status != 0 && result.failure.empty())
            result.failure = error.empty() ? "original runner failed without an error" : error;
    }
    catch (const std::exception& exception)
    {
        result.source_fixture  = fallbackFixture;
        result.expected_pixels = fallbackExpected;
        result.failure         = exception.what();
    }
    return result;
}

int run(const std::filesystem::path& candidateCatalog,
        const std::filesystem::path& controlCatalog,
        const std::filesystem::path& candidateSource,
        const std::filesystem::path& outputRoot)
{
    require(!std::filesystem::exists(outputRoot), "native prepared BC2 output must be fresh");
    std::filesystem::create_directories(outputRoot);
    phase(0, "load_catalogs");
    const auto fixture = MakeNativePreparedBc2Fixture();
    assertFixtureGuards();
    const auto candidate = loadShader(candidateCatalog, fixture.vertex_shader_words, fixture.pixel_shader_words, "candidate");
    const auto control   = loadShader(controlCatalog, fixture.vertex_shader_words, fixture.pixel_shader_words, "control");
    require(sameAttributes(candidate.vertexAttributes, control.vertexAttributes), "candidate/control attributes differ");
    assertCandidateSource(candidateSource);

    HarnessLogging                                    harnessLogging;
    NativeRendererD3D12                               candidateRenderer;
    NativeRendererD3D12                               controlRenderer;
    const std::array<std::array<std::uint32_t, 2>, 7> pairs = {
        { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 }, { 4, 5 }, { 5, 6 }, { 6, 0 } }
    };

    struct PairSource
    {
        NativePreparedBc2Fixture                      fixture;
        std::vector<std::uint32_t>                    registers;
        std::uint32_t                                 vertexGuestBase = 0;
        std::vector<std::uint8_t>                     baselinePlane;
        std::vector<std::uint8_t>                     expectedPlane;
        NativeDrawReplayTexture                       preparedTexture;
        NativeDrawReplayTexture                       controlTexture;
        std::vector<std::uint8_t>                     oldBaseBytes;
        std::vector<std::uint8_t>                     oldMipBytes;
        std::vector<std::uint8_t>                     newBaseBytes;
        std::vector<std::uint8_t>                     newMipBytes;
        std::uint64_t                                 prepared_event_order       = 0;
        std::uint64_t                                 prepared_draw_id           = 0;
        std::uint64_t                                 prepared_frame             = 0;
        std::uint64_t                                 prepared_submission        = 0;
        std::uint32_t                                 prepared_base_address      = 0;
        std::uint32_t                                 prepared_base_extent       = 0;
        std::uint32_t                                 prepared_mip_address       = 0;
        std::uint32_t                                 prepared_mip_extent        = 0;
        std::uint32_t                                 prepared_format            = 0;
        std::uint32_t                                 prepared_endianness        = 0;
        std::uint32_t                                 prepared_pitch             = 0;
        std::uint32_t                                 prepared_mip_min           = 0;
        std::uint32_t                                 prepared_mip_max           = 0;
        std::uint32_t                                 prepared_component_mapping = 0;
        std::uint8_t                                  prepared_swizzled_signs    = 0;
        bool                                          prepared_packed_mips       = false;
        bool                                          prepared_tiled             = false;
        std::uint64_t                                 baseline_copy_id           = 0;
        std::uint64_t                                 baseline_swap_id           = 0;
        std::uint64_t                                 baseline_frame             = 0;
        std::uint64_t                                 baseline_submission        = 0;
        std::uint64_t                                 completion_copy_id         = 0;
        std::uint64_t                                 completion_swap_id         = 0;
        std::uint64_t                                 completion_frame           = 0;
        std::uint64_t                                 completion_submission      = 0;
        std::vector<PreparedCollector::NegativeProbe> prepared_negative_probes;
    };

    std::array<PairSource, 7> pairSources;
    std::uint32_t             pairIndex = 0;
    std::string               runtimeModulePath;
    std::string               gpuModulePath;
    for (const auto& pair : pairs)
    {
        phase(pairIndex, "before_original");
        auto       requested        = fixtureForPair(pair[0], pair[1]);
        const auto baselineExpected = ExpectedNativePreparedBc2Pixels(requested, pair[0], pair[1]);
        auto       collector        = std::make_shared<PreparedCollector>();
        auto       original         = runOriginal(pair[0], pair[1], requested, collector);
        phase(pairIndex, "original_returned");
        writeOriginalDiagnostics(outputRoot, pairIndex, pair[0], pair[1], original, collector->error);
        phase(pairIndex, "original_diagnostics");
        require(original.failure.empty(), "original runner failed: " + original.failure);
        if (pairIndex == 0U)
        {
            runtimeModulePath = original.runtime_module_path;
            gpuModulePath     = original.gpu_module_path;
        }
        else
        {
            require(original.runtime_module_path == runtimeModulePath && original.gpu_module_path == gpuModulePath,
                    "original producer module identity changed between BC2 submissions");
        }
        require(original.progressed && original.baseline_completion_observed && original.baseline_terminal_capture &&
                    original.completion_observed && original.terminal_capture && original.cpu_update_observed &&
                    original.actual_readback_owner && original.source_draw_issued &&
                    original.cpu_partial_overlap_writes && original.source_raw_bytes_frozen,
                "original runner did not report an issued completed terminal readback");
        require(original.baseline_terminal_pixels == baselineExpected,
                "original baseline pixels did not match the pre-update expected plane");
        require(original.terminal_pixels_match_expected,
                "original terminal pixels did not match its independent expected values");
        require(original.baseline_terminal_pixels != original.terminal_pixels,
                "same-allocation CPU BC2 update did not change completed output pixels");
        require(original.terminal_width == kOutputWidth && original.terminal_height == kOutputHeight &&
                    original.terminal_stride == kOutputWidth * 4,
                "original terminal dimensions are outside the bounded schema");
        require(collector->error.empty(), "prepared collector rejected the original callback: " + collector->error);
        require(collector->finished && collector->preparedDrawCount != 0 && collector->preparedOwners.size() == 2 &&
                    collector->preparedPayloads.size() == 2 && collector->afterCompletionCount == 1,
                "prepared collector did not retain exactly one BC2 owner per completed submission");
        require(collector->negativeProbes.size() == 6U,
                "prepared collector did not execute all selected negative probes");
        require(std::ranges::all_of(collector->negativeProbes, [](const auto& probe)
                                    {
                                        return probe.rejected && probe.no_admitted_event && !probe.error.empty();
                                    }),
                "selected Prepared negative probe was not rejected before admission");
        require(collector->preparedOwners.front().textureVersion && collector->preparedOwners.back().textureVersion &&
                    collector->preparedOwners.front().textureVersion != collector->preparedOwners.back().textureVersion &&
                    collector->preparedPayloads.front() != collector->preparedPayloads.back(),
                "prepared collector did not retain distinct changed immutable BC2 owners");
        const auto& preparedTexture = collector->preparedOwners.back();
        require(preparedTexture.format == NativeDrawReplayTextureFormat::Bc2 &&
                    preparedTexture.width == kWidth && preparedTexture.height == kHeight &&
                    preparedTexture.mipLevels == kMipCount && preparedTexture.textureVersion &&
                    preparedTexture.payloadBytes().size() != 0,
                "prepared collector output was not an owned complete BC2 texture version");
        require(collector->preparedPayloads.front() == std::vector<std::uint8_t>(
                                                           collector->preparedOwners.front().payloadBytes().begin(), collector->preparedOwners.front().payloadBytes().end()) &&
                    collector->preparedPayloads.back() == std::vector<std::uint8_t>(
                                                              preparedTexture.payloadBytes().begin(), preparedTexture.payloadBytes().end()),
                "prepared collector did not freeze borrowed payload bytes before callback expiry");
        assertOriginalCompletion(original);
        const auto baselineCopy   = std::ranges::find_if(original.copies, [&](const auto& copy)
                                                         {
                                                           return copy.id == original.baseline_copy_id;
                                                         });
        const auto completionCopy = std::ranges::find_if(original.copies, [&](const auto& copy)
                                                         {
                                                             return copy.id == original.completion_copy_id;
                                                         });
        require(baselineCopy != original.copies.end() && completionCopy != original.copies.end(),
                "original baseline or completion copy record was not retained");
        assertOriginalCopyExtent(*baselineCopy, "original baseline copy");
        assertOriginalCopyExtent(*completionCopy, "original completion copy");
        assertOriginalPreparedCopyExtent(original, *baselineCopy, "original baseline Prepared copy");
        assertOriginalPreparedCopyExtent(original, *completionCopy, "original completion Prepared copy");
        require(original.baseline_copy_id != 0 && original.baseline_swap_id != 0 &&
                    original.baseline_copy_id != original.completion_copy_id &&
                    original.baseline_swap_id != original.completion_swap_id,
                "original baseline and updated completions did not retain distinct identities");
        assertOriginalSource(requested, original);
        const auto finalSourceDraw = std::ranges::find_if(
            original.draws | std::views::reverse,
            [&](const auto& item)
            {
                return item.vertex_shader_words == requested.vertex_shader_words &&
                       item.pixel_shader_words == requested.pixel_shader_words && item.original_host_issued;
            });
        require(finalSourceDraw != std::ranges::reverse_view(original.draws).end(),
                "actual source draw was not available for the receipt join");
        const auto finalPreparedDraw = std::ranges::find_if(
            original.prepared_draws,
            [&](const auto& item)
            {
                return item.id == finalSourceDraw->id && item.frame == finalSourceDraw->frame &&
                       item.submission == finalSourceDraw->submission && item.registers == finalSourceDraw->registers &&
                       item.vertex_shader_words == finalSourceDraw->vertex_shader_words &&
                       item.pixel_shader_words == finalSourceDraw->pixel_shader_words;
            });
        require(finalPreparedDraw != original.prepared_draws.end(),
                "actual final source draw lacked a Prepared receipt join");
        const auto finalPreparedTexture = std::ranges::find_if(
            finalPreparedDraw->textures,
            [](const auto& texture)
            {
                return texture.fetch_constant == 0U &&
                       texture.format == static_cast<std::uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT2_3) &&
                       !texture.is_signed && texture.required && texture.available;
            });
        require(finalPreparedTexture != finalPreparedDraw->textures.end(),
                "actual final source draw lacked its required unsigned BC2 receipt");
        std::vector<std::vector<std::uint8_t>> baselineControlLevels;
        auto                                   baselineControlTexture = decodeControlTexture(requested, &baselineControlLevels);
        const auto                             compareLevels          = [&](const NativeDrawReplayTexture&                texture,
                                                                            const std::vector<std::vector<std::uint8_t>>& levels,
                                                                            const std::string&                            label)
        {
            const auto  payload = texture.payloadBytes();
            std::size_t offset  = 0;
            for (std::uint32_t level = 0; level < kMipCount; ++level)
            {
                require(offset + levels[level].size() <= payload.size(), label + " subresource exceeds retained payload");
                require(std::ranges::equal(payload.subspan(offset, levels[level].size()), levels[level]),
                        label + " subresource differs at mip " + std::to_string(level));
                offset += levels[level].size();
            }
            require(offset == payload.size(), label + " retained payload has trailing bytes");
        };
        compareLevels(collector->preparedOwners.front(), baselineControlLevels, "baseline Prepared BC2 owner");
        std::vector<std::vector<std::uint8_t>> controlLevels;
        auto                                   controlTexture = decodeControlTexture(original.source_fixture, &controlLevels);
        require(controlTexture.format == NativeDrawReplayTextureFormat::Bc2 &&
                    controlTexture.mipLevels == kMipCount &&
                    std::ranges::equal(controlTexture.payloadBytes(), preparedTexture.payloadBytes()),
                "independent BC2 control decode differs from the collector's actual prepared bytes");
        compareLevels(preparedTexture, controlLevels, "updated Prepared BC2 owner");
        const auto baselineExpectedOutput  = duplicatePlane(original.baseline_terminal_pixels);
        const auto baselineCandidatePath   = outputRoot / ("candidate-baseline-" + std::to_string(pairIndex) + ".bin");
        const auto baselineControlPath     = outputRoot / ("control-baseline-" + std::to_string(pairIndex) + ".bin");
        const auto baselineCandidateResult = candidateRenderer.ReplayOffscreen(buildRecipe(
            requested,
            candidate,
            baselineCandidatePath,
            collector->preparedOwners.front(),
            kMipCount,
            original.source_registers,
            sourceVertexGuestBase(original)));
        const auto baselineControlResult   = controlRenderer.ReplayOffscreen(buildRecipe(
            requested,
            control,
            baselineControlPath,
            std::move(baselineControlTexture),
            kMipCount,
            original.source_registers,
            sourceVertexGuestBase(original)));
        require(baselineCandidateResult.success && baselineCandidateResult.completionFenceValue == 0,
                "baseline candidate native replay failed: " + baselineCandidateResult.error);
        require(baselineControlResult.success && baselineControlResult.completionFenceValue == 0,
                "baseline control native replay failed: " + baselineControlResult.error);
        const auto baselineCandidateOutput = readNativeOutput(baselineCandidatePath);
        const auto baselineControlOutput   = readNativeOutput(baselineControlPath);
        compare(baselineCandidateOutput, baselineExpectedOutput, "baseline candidate pair " + std::to_string(pairIndex) + " vs original");
        compare(baselineControlOutput, baselineExpectedOutput, "baseline control pair " + std::to_string(pairIndex) + " vs original");
        compare(baselineCandidateOutput, baselineControlOutput, "baseline candidate pair " + std::to_string(pairIndex) + " vs control");
        phase(pairIndex, "original_validated");
        auto& source                      = pairSources[pairIndex];
        source.fixture                    = std::move(original.source_fixture);
        source.registers                  = std::move(original.source_registers);
        source.vertexGuestBase            = sourceVertexGuestBase(original);
        source.baselinePlane              = std::move(original.baseline_terminal_pixels);
        source.expectedPlane              = std::move(original.expected_pixels);
        source.preparedTexture            = preparedTexture;
        source.controlTexture             = std::move(controlTexture);
        source.oldBaseBytes               = original.old_base_bytes;
        source.oldMipBytes                = original.old_mip_bytes;
        source.newBaseBytes               = original.new_base_bytes;
        source.newMipBytes                = original.new_mip_bytes;
        source.prepared_event_order       = finalPreparedDraw->event_ordinal;
        source.prepared_draw_id           = finalPreparedDraw->id;
        source.prepared_frame             = finalPreparedDraw->frame;
        source.prepared_submission        = finalPreparedDraw->submission;
        source.prepared_base_address      = finalPreparedTexture->base_address;
        source.prepared_base_extent       = finalPreparedTexture->base_size;
        source.prepared_mip_address       = finalPreparedTexture->mip_address;
        source.prepared_mip_extent        = finalPreparedTexture->mip_size;
        source.prepared_format            = finalPreparedTexture->format;
        source.prepared_endianness        = finalPreparedTexture->endianness;
        source.prepared_pitch             = finalPreparedTexture->pitch;
        source.prepared_mip_min           = finalPreparedTexture->mip_min_level;
        source.prepared_mip_max           = finalPreparedTexture->mip_max_level;
        source.prepared_component_mapping = finalPreparedTexture->component_mapping;
        source.prepared_swizzled_signs    = finalPreparedTexture->swizzled_signs;
        source.prepared_packed_mips       = finalPreparedTexture->packed_mips;
        source.prepared_tiled             = finalPreparedTexture->tiled;
        source.baseline_copy_id           = original.baseline_copy_id;
        source.baseline_swap_id           = original.baseline_swap_id;
        source.baseline_frame             = original.baseline_frame;
        source.baseline_submission        = original.baseline_submission;
        source.completion_copy_id         = original.completion_copy_id;
        source.completion_swap_id         = original.completion_swap_id;
        source.completion_frame           = original.completion_frame;
        source.completion_submission      = original.completion_submission;
        source.prepared_negative_probes   = collector->negativeProbes;
        require(source.oldBaseBytes != source.newBaseBytes || source.oldMipBytes != source.newMipBytes,
                "guarded CPU BC2 update did not change the retained raw guest bytes");
        for (std::uint32_t prior = 0; prior < pairIndex; ++prior)
            require(!source.preparedTexture.textureVersion ||
                        source.preparedTexture.textureVersion != pairSources[prior].preparedTexture.textureVersion,
                    "prepared collector reused an earlier immutable texture version owner");
        require(source.expectedPlane.size() == kPlane, "original expected plane is incomplete");
        const auto expected      = duplicatePlane(original.terminal_pixels);
        const auto candidatePath = outputRoot / ("candidate-" + std::to_string(pairIndex) + ".bin");
        const auto controlPath   = outputRoot / ("control-" + std::to_string(pairIndex) + ".bin");
        phase(pairIndex, "candidate_recipe");
        const auto candidateResult = candidateRenderer.ReplayOffscreen(
            buildRecipe(source.fixture, candidate, candidatePath, source.preparedTexture, kMipCount, source.registers, source.vertexGuestBase));
        phase(pairIndex, "candidate_replay_returned");
        require(candidateResult.success && candidateResult.completionFenceValue == 0,
                "candidate native replay failed: " + candidateResult.error);
        const auto controlResult = controlRenderer.ReplayOffscreen(
            buildRecipe(source.fixture, control, controlPath, source.controlTexture, kMipCount, source.registers, source.vertexGuestBase));
        phase(pairIndex, "control_replay_returned");
        require(controlResult.success && controlResult.completionFenceValue == 0,
                "control native replay failed: " + controlResult.error);
        const auto candidateOutput = readNativeOutput(candidatePath);
        const auto controlOutput   = readNativeOutput(controlPath);
        compare(candidateOutput, expected, "candidate pair " + std::to_string(pairIndex) + " vs original");
        compare(controlOutput, expected, "control pair " + std::to_string(pairIndex) + " vs original");
        compare(candidateOutput, controlOutput, "candidate pair " + std::to_string(pairIndex) + " vs control");
        phase(pairIndex, "pair_complete");
        ++pairIndex;
    }

    const auto runCpuUpdateVariant = [&](std::uint32_t                                       level,
                                         rex::graphics::testing::NativePreparedBc2UpdateKind updateKind,
                                         std::string_view                                    label,
                                         std::uint32_t                                       diagnosticIndex)
    {
        const auto second    = (level + 1U) % kMipCount;
        auto       requested = fixtureForPair(level, second);
        auto       collector = std::make_shared<PreparedCollector>();
        auto       original  = runOriginal(level, second, requested, collector, updateKind);
        writeOriginalDiagnostics(outputRoot, diagnosticIndex, level, second, original, collector->error);
        require(original.failure.empty(), std::string(label) + " Original runner failed: " + original.failure);
        require(original.runtime_module_path == runtimeModulePath && original.gpu_module_path == gpuModulePath,
                std::string(label) + " Original producer module identity changed");
        require(original.progressed && original.baseline_completion_observed && original.baseline_terminal_capture &&
                    original.completion_observed && original.terminal_capture && original.cpu_update_observed &&
                    original.actual_readback_owner && original.source_draw_issued && original.cpu_partial_overlap_writes &&
                    original.source_raw_bytes_frozen && original.cpu_update_outside_unchanged &&
                    original.cpu_update_opposite_unchanged,
                std::string(label) + " Original completion or CPU update proof was incomplete");
        require(collector->error.empty() && collector->preparedOwners.size() == 2U &&
                    collector->preparedPayloads.size() == 2U && collector->finished &&
                    collector->afterCompletionCount == 1U,
                std::string(label) + " Prepared callbacks did not retain baseline and changed owners");
        const auto baselineExpected = ExpectedNativePreparedBc2Pixels(requested, level, second);
        require(original.baseline_terminal_pixels == baselineExpected,
                std::string(label) + " Original baseline pixels differed from the independent expected plane");
        require(original.terminal_pixels_match_expected && original.terminal_pixels == original.expected_pixels &&
                    original.terminal_pixels != original.baseline_terminal_pixels,
                std::string(label) + " Original changed pixels were incomplete or unchanged");
        assertOriginalSource(requested, original);

        std::vector<std::vector<std::uint8_t>> baselineLevels;
        auto                                   baselineControlTexture = decodeControlTexture(requested, &baselineLevels);
        std::vector<std::vector<std::uint8_t>> changedLevels;
        auto                                   changedControlTexture = decodeControlTexture(original.source_fixture, &changedLevels);
        const auto                             compareLevelsVariant  = [&](const NativeDrawReplayTexture&                texture,
                                                                           const std::vector<std::vector<std::uint8_t>>& levels,
                                                                           std::string_view                              route)
        {
            const auto  payload = texture.payloadBytes();
            std::size_t offset  = 0;
            for (std::uint32_t mip = 0; mip < kMipCount; ++mip)
            {
                require(offset + levels[mip].size() <= payload.size(),
                        std::string(label) + " " + std::string(route) + " mip exceeds payload");
                require(std::ranges::equal(payload.subspan(offset, levels[mip].size()), levels[mip]),
                        std::string(label) + " " + std::string(route) + " mip differs at level " + std::to_string(mip));
                offset += levels[mip].size();
            }
            require(offset == payload.size(), std::string(label) + " " + std::string(route) + " payload has trailing bytes");
        };
        compareLevelsVariant(collector->preparedOwners.front(), baselineLevels, "Prepared baseline");
        compareLevelsVariant(collector->preparedOwners.back(), changedLevels, "Prepared changed");
        require(std::ranges::equal(baselineControlTexture.payloadBytes(), collector->preparedOwners.front().payloadBytes()) &&
                    std::ranges::equal(changedControlTexture.payloadBytes(), collector->preparedOwners.back().payloadBytes()),
                std::string(label) + " independent control bytes differed from Prepared owners");

        const auto prefix                  = outputRoot / ("variant-" + std::string(label) + "-" + std::to_string(level));
        const auto baselineCandidatePath   = prefix.string() + "-candidate-baseline.bin";
        const auto baselineControlPath     = prefix.string() + "-control-baseline.bin";
        const auto changedCandidatePath    = prefix.string() + "-candidate-changed.bin";
        const auto changedControlPath      = prefix.string() + "-control-changed.bin";
        const auto baselineCandidateResult = candidateRenderer.ReplayOffscreen(buildRecipe(
            requested,
            candidate,
            baselineCandidatePath,
            collector->preparedOwners.front(),
            kMipCount,
            original.source_registers,
            sourceVertexGuestBase(original)));
        const auto baselineControlResult   = controlRenderer.ReplayOffscreen(buildRecipe(
            requested,
            control,
            baselineControlPath,
            std::move(baselineControlTexture),
            kMipCount,
            original.source_registers,
            sourceVertexGuestBase(original)));
        const auto changedCandidateResult  = candidateRenderer.ReplayOffscreen(buildRecipe(
            original.source_fixture,
            candidate,
            changedCandidatePath,
            collector->preparedOwners.back(),
            kMipCount,
            original.source_registers,
            sourceVertexGuestBase(original)));
        const auto changedControlResult    = controlRenderer.ReplayOffscreen(buildRecipe(
            original.source_fixture,
            control,
            changedControlPath,
            std::move(changedControlTexture),
            kMipCount,
            original.source_registers,
            sourceVertexGuestBase(original)));
        require(baselineCandidateResult.success && baselineControlResult.success && changedCandidateResult.success &&
                    changedControlResult.success && baselineCandidateResult.completionFenceValue == 0 &&
                    baselineControlResult.completionFenceValue == 0 && changedCandidateResult.completionFenceValue == 0 &&
                    changedControlResult.completionFenceValue == 0,
                std::string(label) + " native baseline or changed replay failed");
        const auto baselineCandidateOutput = readNativeOutput(baselineCandidatePath);
        const auto baselineControlOutput   = readNativeOutput(baselineControlPath);
        const auto changedCandidateOutput  = readNativeOutput(changedCandidatePath);
        const auto changedControlOutput    = readNativeOutput(changedControlPath);
        const auto baselineExpectedOutput  = duplicatePlane(original.baseline_terminal_pixels);
        const auto changedExpectedOutput   = duplicatePlane(original.terminal_pixels);
        compare(baselineCandidateOutput, baselineExpectedOutput, std::string(label) + " candidate baseline vs Original");
        compare(baselineControlOutput, baselineExpectedOutput, std::string(label) + " control baseline vs Original");
        compare(baselineCandidateOutput, baselineControlOutput, std::string(label) + " baseline candidate vs control");
        compare(changedCandidateOutput, changedExpectedOutput, std::string(label) + " candidate changed vs Original");
        compare(changedControlOutput, changedExpectedOutput, std::string(label) + " control changed vs Original");
        compare(changedCandidateOutput, changedControlOutput, std::string(label) + " changed candidate vs control");
        require(changedCandidateOutput != baselineCandidateOutput,
                std::string(label) + " changed native output did not differ from baseline");
    };
    const std::array<std::pair<rex::graphics::testing::NativePreparedBc2UpdateKind, std::string_view>, 2> updateVariants = {
        { { rex::graphics::testing::NativePreparedBc2UpdateKind::kColorOnly, "color-only" },
          { rex::graphics::testing::NativePreparedBc2UpdateKind::kAlphaOnly, "alpha-only" } }
    };
    std::uint32_t variantIndex = 7U;
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        for (const auto& [updateKind, label] : updateVariants)
            runCpuUpdateVariant(level, updateKind, label, variantIndex++);
    }
    const auto cpuUpdateVariantChecks = variantIndex - 7U;

    const auto& firstSource = pairSources.front();
    // Keep a payload-only copy for later rejection and sensitivity routes;
    // the lifetime proof below moves the actual collector owner into the
    // submitted frame so caller release can be observed independently.
    const auto firstSourceReplayTexture  = mutableTexture(firstSource.preparedTexture);
    bool       freshTextureVersionOwners = true;
    bool       changedCompletedOutputs   = true;
    for (std::size_t left = 0; left < pairSources.size(); ++left)
    {
        changedCompletedOutputs = changedCompletedOutputs && pairSources[left].baselinePlane != pairSources[left].expectedPlane;
        for (std::size_t right = left + 1; right < pairSources.size(); ++right)
            freshTextureVersionOwners = freshTextureVersionOwners &&
                                        pairSources[left].preparedTexture.textureVersion &&
                                        pairSources[right].preparedTexture.textureVersion &&
                                        pairSources[left].preparedTexture.textureVersion != pairSources[right].preparedTexture.textureVersion;
    }
    bool                                                lifetimeRecordingReady             = false;
    bool                                                lifetimeNativeCompletion           = false;
    bool                                                lifetimeSdkCopyRetired             = false;
    bool                                                lifetimeOwnerReleased              = false;
    bool                                                lifetimeOwnerHeldThroughCompletion = false;
    bool                                                lifetimeOwnerRetiredAfterDrain     = false;
    bool                                                deviceFailureRecordingReady        = false;
    bool                                                deviceFailureCompletionFailed      = false;
    bool                                                deviceFailureOwnerHeldBeforeRemove = false;
    bool                                                deviceFailureOwnerRetiredAfterStop = false;
    std::uint32_t                                       deviceFailurePublications          = 0;
    std::string                                         deviceFailureError;
    std::weak_ptr<const NativeDrawReplayTextureVersion> lifetimeOwnerWeak;
    AdapterIdentity                                     mainAdapterIdentity;
    {
        struct FenceGate
        {
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            bool                                released = false;

            ~FenceGate()
            {
                if (fence && !released)
                    (void)fence->Signal(1);
            }

            void Release()
            {
                require(fence && SUCCEEDED(fence->Signal(1)), "BC2 lifetime gate signal");
                released = true;
            }
        };

        auto provider = rex::ui::d3d12::D3D12Provider::Create();
        require(bool(provider), "BC2 lifetime proof provider");
        mainAdapterIdentity = describeProviderAdapter(*provider);
        auto presenter      = rex::ui::d3d12::D3D12Presenter::Create([](bool, bool)
                                                                     {
                                                                     },
                                                                     *provider);
        require(bool(presenter), "BC2 lifetime proof presenter");
        const auto                                            stream = presenter->CreateGpuGuestOutputStream();
        std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket> published;
        const auto                                            publish = [&](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
        {
            rex::ui::d3d12::D3D12Presenter::GpuGuestOutputFrame output;
            output.resource    = resource;
            output.ready_fence = fence;
            output.ready_value = value;
            output.width       = kOutputWidth;
            output.height      = kOutputHeight;
            published          = presenter->QueueGpuGuestOutputFrame(stream, std::move(output));
            return published;
        };

        NativeRenderFrame lifetimeFrame;
        lifetimeFrame.textures             = { { 1, kOutputWidth, kOutputHeight, NativeDrawReplayTextureFormat::Rgba8 } };
        lifetimeFrame.outputTextureId      = 1;
        lifetimeFrame.returnGpuOutput      = true;
        lifetimeFrame.returnResolvedOutput = false;
        NativeRenderFramePass lifetimePass;
        lifetimePass.target = { kOutputWidth, kOutputHeight, 1, NativeDrawReplayTargetFormat::Rgba8 };
        NativeRenderFramePass::Clear lifetimeClear;
        lifetimeClear.rectangle  = { 0, 0, static_cast<std::int32_t>(kOutputWidth), static_cast<std::int32_t>(kOutputHeight) };
        lifetimeClear.color      = true;
        lifetimeClear.colorValue = { 0.0F, 0.0F, 0.0F, 1.0F };
        lifetimePass.clears.push_back(lifetimeClear);
        auto lifetimeOwner = std::move(pairSources.front().preparedTexture);
        lifetimeOwnerWeak  = lifetimeOwner.textureVersion;
        lifetimePass.draws.push_back(buildRecipe(firstSource.fixture,
                                                 candidate,
                                                 {},
                                                 lifetimeOwner,
                                                 kMipCount,
                                                 firstSource.registers,
                                                 firstSource.vertexGuestBase,
                                                 kOutputWidth,
                                                 kOutputHeight));
        NativeRenderFrameResolve lifetimeResolve;
        lifetimeResolve.textureId = 1;
        lifetimeResolve.width     = kOutputWidth;
        lifetimeResolve.height    = kOutputHeight;
        lifetimeResolve.samples   = { 0, 0 };
        lifetimePass.resolves.push_back(lifetimeResolve);
        lifetimeFrame.passes.push_back(std::move(lifetimePass));
        std::string lifetimeError;
        require(ValidateNativeRenderFrame(lifetimeFrame, lifetimeError), lifetimeError);

        NativeRendererD3D12 lifetimeRenderer;
        FenceGate           nativeGate, copyGate;
        require(lifetimeRenderer.StartDrawStream(lifetimeError, provider->GetDevice(), publish), lifetimeError);
        require(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nativeGate.fence))),
                "BC2 native completion gate allocation");
        require(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyGate.fence))),
                "BC2 SDK copy gate allocation");
        auto* lifetimeQueue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(lifetimeRenderer);
        require(lifetimeQueue && SUCCEEDED(lifetimeQueue->Wait(nativeGate.fence.Get(), 1)),
                "BC2 native queue gate installation");
        std::shared_ptr<NativeRenderFrameTicket> lifetimeTicket;
        require(lifetimeRenderer.QueueRenderFrame(std::move(lifetimeFrame), lifetimeTicket, lifetimeError), lifetimeError);
        lifetimeOwner         = {};
        lifetimeOwnerReleased = true;
        require(lifetimeOwnerWeak.use_count() != 0U, "BC2 actual prepared owner retired before native completion");
        require(lifetimeTicket->Recording().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "BC2 Recording did not become ready behind the native gate");
        const auto recorded = lifetimeTicket->Recording().get();
        require(recorded.success && !published &&
                    lifetimeTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "BC2 Recording did not remain distinct from native completion");
        lifetimeRecordingReady = true;
        nativeGate.Release();
        require(lifetimeTicket->Completion().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "BC2 native completion did not retire after the real queue gate");
        const auto completed = lifetimeTicket->Completion().get();
        require(completed.success && completed.completionFenceValue && published && completed.gpuOutputTicket == published,
                completed.error);
        lifetimeOwnerHeldThroughCompletion = !lifetimeOwnerWeak.expired();
        require(lifetimeOwnerHeldThroughCompletion,
                "BC2 actual prepared owner retired before native completion result was consumed");
        lifetimeNativeCompletion = true;
        auto* sdkQueue           = rex::ui::d3d12::detail::GpuGuestOutputTestAccess::CopyQueue(*presenter);
        require(sdkQueue && SUCCEEDED(sdkQueue->Wait(copyGate.fence.Get(), 1)),
                "BC2 SDK consumer copy gate installation");
        Microsoft::WRL::ComPtr<ID3D12Resource> copied;
        require(presenter->RefreshGuestOutput(kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight, [&](rex::ui::Presenter::GuestOutputRefreshContext& context)
                                              {
                                                  copied = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
                                                  context.SetIs8bpc(true);
                                                  return true;
                                              }),
                "BC2 SDK consumer copy submission");
        std::uint64_t sdkCopyFence = 0;
        require(NativeRendererD3D12::PollGpuOutput(published, &sdkCopyFence) == NativeRendererD3D12::GpuOutputStatus::Pending &&
                    !sdkCopyFence,
                "BC2 native completion did not stay distinct from SDK copy retirement");
        copyGate.Release();
        require(NativeRendererD3D12::AwaitGpuOutput(published, lifetimeError, &sdkCopyFence) && sdkCopyFence,
                lifetimeError);
        require(copied != nullptr, "BC2 SDK copy retirement did not retain its consumer resource");
        lifetimeSdkCopyRetired = true;
        lifetimeRenderer.Shutdown();
    }
    lifetimeOwnerRetiredAfterDrain = lifetimeOwnerWeak.expired();
    require(lifetimeOwnerRetiredAfterDrain, "BC2 actual prepared owner was not retired after native and SDK drains");

    // Exercise a real pending BC2 owner on a dedicated removable device. The
    // device fault must fail the native completion before output publication,
    // then normal shutdown must release the actual frame owner.
    {
        auto warp = createDedicatedWarpDevice();
        require(mainAdapterIdentity.canonical && warp.identity.canonical,
                "BC2 device-removal adapter identities were unavailable");
        require(!sameLuid(mainAdapterIdentity.luid, warp.identity.luid),
                "BC2 device-removal proof requires a WARP adapter distinct from the main provider");
        require(mainAdapterIdentity.canonical.Get() != warp.identity.canonical.Get(),
                "BC2 device-removal proof requires distinct canonical adapter identities");
        std::atomic<std::uint32_t> publicationCalls{ 0 };
        const auto                 publish = [&](ID3D12Resource*, ID3D12Fence*, std::uint64_t)
        {
            publicationCalls.fetch_add(1, std::memory_order_relaxed);
            return std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>{};
        };

        NativeRendererD3D12 removedRenderer;
        std::string         removedError;
        require(removedRenderer.StartDrawStream(removedError, warp.device.Get(), publish), removedError);
        auto*                               removedQueue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(removedRenderer);
        Microsoft::WRL::ComPtr<ID3D12Fence> removedGate;
        require(removedQueue &&
                    SUCCEEDED(warp.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&removedGate))) &&
                    SUCCEEDED(removedQueue->Wait(removedGate.Get(), 1)),
                "BC2 device-removal native queue gate");

        const auto                                          deviceFailureReplayTexture = mutableTexture(pairSources[1].preparedTexture);
        auto                                                deviceFailureOwner         = std::move(pairSources[1].preparedTexture);
        std::weak_ptr<const NativeDrawReplayTextureVersion> deviceFailureOwnerWeak     = deviceFailureOwner.textureVersion;

        NativeRenderFrame deviceFailureFrame;
        deviceFailureFrame.textures             = { { 1, kOutputWidth, kOutputHeight, NativeDrawReplayTextureFormat::Rgba8 } };
        deviceFailureFrame.outputTextureId      = 1;
        deviceFailureFrame.returnGpuOutput      = true;
        deviceFailureFrame.returnResolvedOutput = false;
        NativeRenderFramePass deviceFailurePass;
        deviceFailurePass.target = { kOutputWidth, kOutputHeight, 1, NativeDrawReplayTargetFormat::Rgba8 };
        NativeRenderFramePass::Clear deviceFailureClear;
        deviceFailureClear.rectangle  = { 0, 0, static_cast<std::int32_t>(kOutputWidth), static_cast<std::int32_t>(kOutputHeight) };
        deviceFailureClear.color      = true;
        deviceFailureClear.colorValue = { 0.0F, 0.0F, 0.0F, 1.0F };
        deviceFailurePass.clears.push_back(deviceFailureClear);
        deviceFailurePass.draws.push_back(buildRecipe(pairSources[1].fixture,
                                                      candidate,
                                                      {},
                                                      std::move(deviceFailureOwner),
                                                      kMipCount,
                                                      pairSources[1].registers,
                                                      pairSources[1].vertexGuestBase,
                                                      kOutputWidth,
                                                      kOutputHeight));
        NativeRenderFrameResolve deviceFailureResolve;
        deviceFailureResolve.textureId = 1;
        deviceFailureResolve.width     = kOutputWidth;
        deviceFailureResolve.height    = kOutputHeight;
        deviceFailureResolve.samples   = { 0, 0 };
        deviceFailurePass.resolves.push_back(deviceFailureResolve);
        deviceFailureFrame.passes.push_back(std::move(deviceFailurePass));
        require(ValidateNativeRenderFrame(deviceFailureFrame, removedError), removedError);

        std::shared_ptr<NativeRenderFrameTicket> deviceFailureTicket;
        require(removedRenderer.QueueRenderFrame(std::move(deviceFailureFrame), deviceFailureTicket, removedError), removedError);
        require(!deviceFailureOwnerWeak.expired(), "BC2 device-removal owner retired before native recording");
        require(deviceFailureTicket->Recording().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "BC2 device-removal Recording did not become ready behind the native gate");
        const auto deviceFailureRecorded = deviceFailureTicket->Recording().get();
        require(deviceFailureRecorded.success &&
                    deviceFailureTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "BC2 device-removal Recording did not remain distinct from native completion");
        deviceFailureRecordingReady        = true;
        deviceFailureOwnerHeldBeforeRemove = !deviceFailureOwnerWeak.expired();
        require(deviceFailureOwnerHeldBeforeRemove,
                "BC2 device-removal owner was not held before removing the device");

        Microsoft::WRL::ComPtr<ID3D12Device5> removedDevice;
        require(SUCCEEDED(warp.device->QueryInterface(IID_PPV_ARGS(&removedDevice))),
                "BC2 device-removal ID3D12Device5 query");
        removedDevice->RemoveDevice();
        require(FAILED(warp.device->GetDeviceRemovedReason()),
                "BC2 device-removal device did not enter the removed state");
        require(deviceFailureTicket->Completion().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "BC2 device-removal completion did not fail after RemoveDevice");
        const auto deviceFailureCompleted = deviceFailureTicket->Completion().get();
        deviceFailureError                = deviceFailureCompleted.error;
        require(!deviceFailureCompleted.success && !deviceFailureCompleted.completionFenceValue &&
                    !deviceFailureCompleted.presenterToken && !deviceFailureCompleted.gpuOutputTicket &&
                    deviceFailureCompleted.resolvedOutput.empty() && !deviceFailureCompleted.error.empty() &&
                    publicationCalls.load(std::memory_order_relaxed) == 0U,
                "BC2 device-removal completion published output or lost its failure reason");
        deviceFailureCompletionFailed = true;
        removedRenderer.Shutdown();
        deviceFailureOwnerRetiredAfterStop = deviceFailureOwnerWeak.expired();
        require(deviceFailureOwnerRetiredAfterStop,
                "BC2 device-removal owner did not retire after renderer shutdown");
        pairSources[1].preparedTexture = deviceFailureReplayTexture;
        deviceFailurePublications      = publicationCalls.load(std::memory_order_relaxed);
    }
    require(deviceFailureRecordingReady && deviceFailureCompletionFailed && deviceFailureOwnerHeldBeforeRemove &&
                deviceFailureOwnerRetiredAfterStop && deviceFailurePublications == 0U,
            "BC2 device-removal lifetime proof was incomplete");
    const auto missingParent = outputRoot / "missing-output-parent";
    require(!std::filesystem::exists(missingParent), "missing-output-parent must be absent before rejection");
    const auto missingPath   = missingParent / "candidate.bin";
    const auto missingResult = candidateRenderer.ReplayOffscreen(
        buildRecipe(firstSource.fixture, candidate, missingPath, firstSourceReplayTexture, kMipCount, firstSource.registers, firstSource.vertexGuestBase));
    require(!missingResult.success && missingResult.error.find("open replay output file") != std::string::npos,
            "absent native output path did not fail at the existing output owner");
    require(!std::filesystem::exists(missingPath), "absent native output path unexpectedly produced a file");
    const auto recoveryPath   = outputRoot / "recovery-after-missing-output.bin";
    const auto recoveryResult = candidateRenderer.ReplayOffscreen(
        buildRecipe(firstSource.fixture, candidate, recoveryPath, firstSourceReplayTexture, kMipCount, firstSource.registers, firstSource.vertexGuestBase));
    require(recoveryResult.success && recoveryResult.completionFenceValue == 0,
            "candidate owner did not recover after absent output rejection: " + recoveryResult.error);
    compare(readNativeOutput(recoveryPath), duplicatePlane(firstSource.expectedPlane), "candidate recovery after absent output");

    std::uint32_t sensitivity = 0;
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto& source             = pairSources[level];
        const auto  expectedOutputCopy = duplicatePlane(source.expectedPlane);
        for (const auto& mutation : std::array<std::pair<const char*, void (*)(NativeDrawReplayTexture&, std::uint32_t)>, 2>{
                 { { "color", &MutateBc2ColorMip }, { "alpha", &MutateBc2AlphaMip } } })
        {
            auto mutatedTexture = level == 0U ? mutableTexture(firstSourceReplayTexture) : mutableTexture(source.preparedTexture);
            mutation.second(mutatedTexture, level);
            const auto candidatePath = outputRoot / ("mutated-" + std::string(mutation.first) + "-candidate-" + std::to_string(level) + ".bin");
            const auto controlPath   = outputRoot / ("mutated-" + std::string(mutation.first) + "-control-" + std::to_string(level) + ".bin");
            const auto controlResult = controlRenderer.ReplayOffscreen(
                buildRecipe(source.fixture, control, controlPath, mutatedTexture, kMipCount, source.registers, source.vertexGuestBase));
            require(controlResult.success && controlResult.completionFenceValue == 0,
                    "control BC2 " + std::string(mutation.first) + " mutation replay failed: " + controlResult.error);
            const auto controlOutput = readNativeOutput(controlPath);
            require(controlOutput != expectedOutputCopy,
                    std::string("BC2 ") + mutation.first + " mutation at mip " + std::to_string(level) + " did not change the control pixels");
            const auto candidateResult = candidateRenderer.ReplayOffscreen(buildRecipe(
                source.fixture, candidate, candidatePath, mutatedTexture, kMipCount, source.registers, source.vertexGuestBase));
            require(candidateResult.success && candidateResult.completionFenceValue == 0,
                    "candidate BC2 " + std::string(mutation.first) + " mutation replay failed: " + candidateResult.error);
            const auto candidateOutput = readNativeOutput(candidatePath);
            require(candidateOutput != expectedOutputCopy,
                    std::string("candidate BC2 ") + mutation.first + " mutation at mip " + std::to_string(level) + " did not change the completed pixels");
            compare(candidateOutput, controlOutput, "candidate/control BC2 " + std::string(mutation.first) + " mutation " + std::to_string(level));
            if (std::string_view(mutation.first) == "alpha")
            {
                require(std::ranges::any_of(std::views::iota(std::size_t{ 0 }, controlOutput.size() / 2U), [&](std::size_t index)
                                            {
                                                return index % 4U != 3U &&
                                                       controlOutput[index] != expectedOutputCopy[index];
                                            }),
                        "BC2 alpha mutation did not affect RGB output through source-alpha blending");
            }
            ++sensitivity;
        }
    }

    struct OmissionDiagnostic
    {
        std::uint32_t level = 0;
        std::string   stage;
        std::string   error;
        bool          outputExists = false;
    };

    std::vector<OmissionDiagnostic> omissions;
    omissions.reserve(kMipCount);
    for (std::uint32_t level = 0; level < kMipCount; ++level)
    {
        const auto&            source        = pairSources[level];
        const auto&            sourceTexture = level == 0U ? firstSourceReplayTexture : source.preparedTexture;
        const auto             path          = outputRoot / ("omitted-mip-" + std::to_string(level) + ".bin");
        const auto             payload       = omitBc2Mip(sourceTexture, level);
        bool                   built         = true;
        NativeDrawReplayRecipe recipe;
        try
        {
            recipe = buildRecipe(source.fixture, candidate, path, payload, kMipCount, source.registers, source.vertexGuestBase);
        }
        catch (const std::exception& error)
        {
            built = false;
            omissions.push_back({ level, "BuildNativeGameplayDrawRecipe", error.what(), std::filesystem::exists(path) });
        }
        if (built)
        {
            const auto result = candidateRenderer.ReplayOffscreen(recipe);
            require(!result.success && !result.error.empty(), "omitted selected mip was not explicitly rejected");
            omissions.push_back({ level, "ReplayOffscreen", result.error, std::filesystem::exists(path) });
        }
        require(!omissions.back().outputExists, "omitted mip rejection unexpectedly produced output");
        ++sensitivity;
    }

    const auto&                                  receipt = pairSources.front();
    rex::graphics::xenos::xe_gpu_texture_fetch_t receiptFetch{};
    std::memcpy(&receiptFetch, receipt.fixture.texture.fetch_words.data(), sizeof(receiptFetch));
    std::ofstream report(outputRoot / "result.toml", std::ios::binary | std::ios::trunc);
    require(report.good(), "could not create native prepared BC2 result");
    report << "schema_version = 1\nuv_bias_base_texels = 0.25\ntexel_edge_guard = true\ncompleted_pairs = 7\ncpu_update_variant_checks = " << cpuUpdateVariantChecks
           << "\nsensitivity_checks = " << sensitivity
           << "\noutput_planes = 2\nabsent_output_rejected = true\nabsent_output_error = "
           << tomlString(missingResult.error) << "\nprepared_collector = true\nprepared_format = \"k_DXT2_3\"\nprepared_format_value = "
           << receipt.prepared_format << "\nprepared_endianness = \"k8in16\"\nprepared_endianness_value = "
           << receipt.prepared_endianness << "\nprepared_swizzle = " << receiptFetch.swizzle << "\n"
           << "\nprepared_component_mapping = " << receipt.prepared_component_mapping
           << "\nprepared_swizzled_signs = " << static_cast<unsigned>(receipt.prepared_swizzled_signs)
           << "\nprepared_pitch = " << receipt.prepared_pitch << "\nprepared_mip_min = "
           << receipt.prepared_mip_min << "\nprepared_mip_max = " << receipt.prepared_mip_max
           << "\nprepared_packed_mips = " << (receipt.prepared_packed_mips ? "true" : "false")
           << "\nprepared_tiled = " << (receipt.prepared_tiled ? "true" : "false")
           << "\nprepared_base_address = " << receipt.prepared_base_address
           << "\nprepared_base_extent = " << receipt.prepared_base_extent
           << "\nprepared_mip_address = " << receipt.prepared_mip_address
           << "\nprepared_mip_extent = " << receipt.prepared_mip_extent
           << "\nprepared_draw_event_order = " << receipt.prepared_event_order
           << "\nprepared_draw_id = " << receipt.prepared_draw_id << "\nprepared_draw_frame = "
           << receipt.prepared_frame << "\nprepared_draw_submission = " << receipt.prepared_submission
           << "\nprepared_packed_tail_offset = 16384\n"
           << "runtime_module_path = " << tomlString(runtimeModulePath) << "\ngpu_module_path = "
           << tomlString(gpuModulePath) << "\n"
           << "route_original = \"actual_original_capture\"\nroute_control = \"control_native_replay_from_raw_decode\"\n"
           << "route_candidate = \"candidate_native_replay\"\ncompletion_identity = \"copy_swap_frame_end\"\n"
           << "baseline_copy_id = " << receipt.baseline_copy_id << "\nbaseline_swap_id = " << receipt.baseline_swap_id
           << "\nbaseline_frame = " << receipt.baseline_frame << "\nbaseline_submission = "
           << receipt.baseline_submission << "\ncompletion_copy_id = " << receipt.completion_copy_id
           << "\ncompletion_swap_id = " << receipt.completion_swap_id << "\ncompletion_frame = "
           << receipt.completion_frame << "\ncompletion_submission = " << receipt.completion_submission << "\n"
           << "raw_storage = \"texture-base.bc2.bin + texture-mips.bc2.bin\"\n"
           << "alpha_scope = \"BC2 four-bit alpha changes RGB through source-alpha blending; presenter capture alpha is normalized to 255\"\n"
           << "cpu_update_levels = " << pairIndex << "\nraw_old_new_copies = "
           << (std::ranges::all_of(pairSources, [](const auto& source)
                                   {
                                       return !source.oldBaseBytes.empty() && !source.oldMipBytes.empty() &&
                                              !source.newBaseBytes.empty() && !source.newMipBytes.empty() &&
                                              (source.oldBaseBytes != source.newBaseBytes || source.oldMipBytes != source.newMipBytes);
                                   })
                   ? "true"
                   : "false")
           << "\nfresh_texture_version_owner = " << (freshTextureVersionOwners ? "true" : "false") << "\nchanged_completed_outputs = " << (changedCompletedOutputs ? "true" : "false") << "\n"
           << "lifetime_recording_ready = " << (lifetimeRecordingReady ? "true" : "false") << "\nlifetime_native_completion = " << (lifetimeNativeCompletion ? "true" : "false") << "\nlifetime_sdk_copy_retired = " << (lifetimeSdkCopyRetired ? "true" : "false") << "\nlifetime_owner_released = " << (lifetimeOwnerReleased ? "true" : "false") << "\nlifetime_owner_held_through_completion = " << (lifetimeOwnerHeldThroughCompletion ? "true" : "false") << "\n"
           << "lifetime_owner_retired_after_drain = " << (lifetimeOwnerRetiredAfterDrain ? "true" : "false") << "\n"
           << "device_failure_recording_ready = " << (deviceFailureRecordingReady ? "true" : "false") << "\n"
           << "device_failure_completion_failed = " << (deviceFailureCompletionFailed ? "true" : "false") << "\n"
           << "device_failure_owner_held_before_remove = " << (deviceFailureOwnerHeldBeforeRemove ? "true" : "false") << "\n"
           << "device_failure_owner_retired_after_shutdown = " << (deviceFailureOwnerRetiredAfterStop ? "true" : "false") << "\n"
           << "device_failure_publications = " << deviceFailurePublications << "\n"
           << "device_failure_error = " << tomlString(deviceFailureError) << "\n"
           << "omission_count = " << omissions.size() << "\n"
           << "prepared_negative_probe_count = " << receipt.prepared_negative_probes.size() << "\n";
    for (std::size_t index = 0; index < omissions.size(); ++index)
    {
        report << "omission_" << index << "_level = " << omissions[index].level << "\n"
               << "omission_" << index << "_stage = " << tomlString(omissions[index].stage) << "\n"
               << "omission_" << index << "_error = " << tomlString(omissions[index].error) << "\n"
               << "omission_" << index << "_output_exists = "
               << (omissions[index].outputExists ? "true" : "false") << "\n";
    }
    for (std::size_t index = 0; index < receipt.prepared_negative_probes.size(); ++index)
    {
        const auto& probe = receipt.prepared_negative_probes[index];
        report << "prepared_negative_" << index << "_name = " << tomlString(probe.name) << "\n"
               << "prepared_negative_" << index << "_rejected = "
               << (probe.rejected ? "true" : "false") << "\n"
               << "prepared_negative_" << index << "_no_admitted_event = "
               << (probe.no_admitted_event ? "true" : "false") << "\n"
               << "prepared_negative_" << index << "_error = " << tomlString(probe.error) << "\n";
    }
    report.flush();
    require(report.good(), "could not flush native prepared BC2 result");
    std::cout << "Native Prepared BC2 matched completed original/control outputs for levels 0..6; CPU update variants "
              << cpuUpdateVariantChecks << "; sensitivity checks " << sensitivity << '\n';
    return 0;
}

} // namespace

static void fixtureGuards()
{
    require(kMipCount == 7 && kOutputWidth == 1280U && kOutputHeight == 720U &&
                kPlane == static_cast<std::size_t>(kOutputWidth) * kOutputHeight * 4U,
            "native prepared BC2 fixture guards changed");
    assertFixtureGuards();
    std::cout << "Native prepared BC2 fixture guards passed; use the emit and run helper commands\n";
}

static int preparedMain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--emit-fixture")
        {
            emitFixture(argv[2]);
            return 0;
        }
        if (argc != 9 || std::wstring_view(argv[1]) != L"--candidate-catalog" ||
            std::wstring_view(argv[3]) != L"--control-catalog" || std::wstring_view(argv[5]) != L"--candidate-source" ||
            std::wstring_view(argv[7]) != L"--output")
            fail("usage: rerevved_prepared_tests --helper native-prepared-bc2.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>");
        return run(argv[2], argv[4], argv[6], argv[8]);
    }
    catch (const std::exception& error)
    {
        std::cerr << "native_prepared_bc2_tests: " << error.what() << '\n';
        return 1;
    }
}

// native-prepared-bc2.emit --emit-fixture <fresh-dir>: writes the fixture inputs.
REREVVED_HELPER_COMMAND(nativePreparedBc2Emit, "native-prepared-bc2.emit")
{
    rerevved::tests::NativeArgv argv(args);
    if (argv.argc() != 3 || std::wstring_view(argv.argv()[1]) != L"--emit-fixture")
    {
        std::cerr << "usage: rerevved_prepared_tests --helper native-prepared-bc2.emit --emit-fixture <fresh-dir>" << std::endl;
        return 2;
    }
    return preparedMain(argv.argc(), argv.argv());
}

// native-prepared-bc2.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>:
// the GPU proof.
REREVVED_HELPER_COMMAND(nativePreparedBc2Run, "native-prepared-bc2.run")
{
    rerevved::tests::NativeArgv argv(args);
    if (argv.argc() != 9)
    {
        std::cerr << "usage: rerevved_prepared_tests --helper native-prepared-bc2.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>" << std::endl;
        return 2;
    }
    return preparedMain(argv.argc(), argv.argv());
}

TEST_CASE("native_prepared_bc2_fixture", "[ctest][native-prepared-bc2]")
{
    UNSCOPED_INFO("the fixture guards hold");
    REQUIRE_NOTHROW(fixtureGuards());
}
