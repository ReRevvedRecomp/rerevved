// Exact six-face CPU cube proof; expected pixels come only from copied raw bytes.
#include "native_prepared_cube_matrix_fixture.h"

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
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>
#include <wrl/client.h>

using namespace rerevved::gpu;
using namespace rex::graphics::testing;
using rex::graphics::testing::ExpectedNativePreparedCubeMatrixPixels;
using rex::graphics::testing::MakeNativePreparedCubeMatrixFixture;
using rex::graphics::testing::NativePreparedCubeMatrixCopyRecord;
using rex::graphics::testing::NativePreparedCubeMatrixDrawRecord;
using rex::graphics::testing::NativePreparedCubeMatrixFixture;
using rex::graphics::testing::NativePreparedCubeMatrixOriginalParams;
using rex::graphics::testing::NativePreparedCubeMatrixOriginalResult;
using rex::graphics::testing::RunNativePreparedCubeMatrixOriginal;

namespace
{

const std::uint32_t&    kMipCount      = rex::graphics::testing::kNativePreparedCubeMatrixMipCount;
constexpr std::uint32_t kOutputWidth   = rex::graphics::testing::kNativePreparedCubeMatrixOutputWidth;
constexpr std::uint32_t kOutputHeight  = rex::graphics::testing::kNativePreparedCubeMatrixOutputHeight;
constexpr std::size_t   kPlane         = static_cast<std::size_t>(kOutputWidth) * kOutputHeight * 4;
unsigned                currentCubeLod = 0;

std::size_t outputGuestExtent()
{
    // The callback range is the tiled guest allocation, not the linear cube
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
    require(device != nullptr && adapter != nullptr, "cube adapter identity requires a device and adapter");
    DXGI_ADAPTER_DESC1 description{};
    require(SUCCEEDED(adapter->GetDesc1(&description)), "cube adapter identity could not read the DXGI description");
    AdapterIdentity result;
    result.luid = device->GetAdapterLuid();
    require(sameLuid(result.luid, description.AdapterLuid), "cube adapter identity LUID did not match the device");
    require(SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&result.canonical))),
            "cube adapter identity could not query canonical IUnknown");
    return result;
}

AdapterIdentity describeProviderAdapter(const rex::ui::d3d12::D3D12Provider& provider)
{
    const auto luid    = provider.GetDevice()->GetAdapterLuid();
    auto*      factory = provider.GetDXGIFactory();
    require(factory != nullptr, "cube main provider did not retain its DXGI factory");
    for (std::uint32_t index = 0;; ++index)
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const auto                            result = factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf());
        if (result == DXGI_ERROR_NOT_FOUND)
            break;
        require(SUCCEEDED(result), "cube main provider adapter enumeration");
        DXGI_ADAPTER_DESC1 description{};
        require(SUCCEEDED(adapter->GetDesc1(&description)), "cube main provider adapter description");
        if (sameLuid(luid, description.AdapterLuid))
            return describeAdapter(provider.GetDevice(), adapter.Get());
    }
    fail("cube main provider adapter LUID was not present in its factory");
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
            "cube dedicated WARP factory creation");
    Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
    require(SUCCEEDED(result.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),
            "cube dedicated WARP adapter enumeration");
    require(SUCCEEDED(warp.As(&result.adapter)), "cube dedicated WARP adapter query");
    require(SUCCEEDED(D3D12CreateDevice(result.adapter.Get(),
                                        D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&result.device))),
            "cube dedicated WARP device creation");
    result.identity = describeAdapter(result.device.Get(), result.adapter.Get());
    return result;
}

void phase(std::uint32_t pairIndex, const char* name)
{
    std::cerr << "native_prepared_cube_matrix pair=" << pairIndex << " phase=" << name << std::endl;
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

struct CubeSource
{
    rex::graphics::diagnostic::DrawCaptureFrameEvent           event;
    std::vector<std::uint32_t>                                 registers;
    NativeDrawReplayTexture                                    owner;
    std::vector<rex::graphics::diagnostic::DrawCaptureTexture> textures;
    NativeDrawReplayRecipe                                     recipe;
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

    explicit PreparedCollector(NativeGameplayShaderInterface candidate)
    : shader(std::move(candidate))
    {
        inputs.SetEventConsumer([this](const auto& event, const auto& bank, const auto& textures, const auto& cpu, std::string& failure)
                                {
                                    if (event.kind != rex::graphics::diagnostic::DrawCaptureFrameEvent::Kind::kDraw)
                                        return true;
                                    auto exact             = shader;
                                    exact.vertexShaderHash = event.draw.guest_vertex_shader_hash;
                                    exact.pixelShaderHash  = event.draw.guest_pixel_shader_hash;
                                    const std::vector<NativeGameplayShaderInterface> shaders{ exact };
                                    NativeGameplayCommandFeed                        commands(shaders);
                                    if (!commands.Begin(event.frame, event.submission, {}, failure) ||
                                        !commands.ConsumePreparedDraw(event, bank, textures, cpu, failure))
                                        return false;
                                    const auto* draw = commands.Draw(event.id);
                                    if (!draw)
                                    {
                                        failure = "Prepared cube command feed produced no draw";
                                        return false;
                                    }
                                    const auto use = std::ranges::find_if(event.draw.texture_uses, [](const auto& u)
                                                                          {
                                                                              return u.required;
                                                                          });
                                    if (use == event.draw.texture_uses.end() || !cpu.contains(use->texture_index))
                                    {
                                        failure = "Prepared cube required owner disappeared";
                                        return false;
                                    }
                                    sources.push_back({ event, { bank.begin(), bank.end() }, cpu.at(use->texture_index), { textures.begin(), textures.end() }, draw->Recipe() });
                                    return true;
                                });
    }

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
        require(!draw.textures.empty(), "actual Prepared cube draw had no texture view for negative probes");

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
                "actual Prepared cube draw had no required texture for negative probes");
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
        if (kMipCount == 1U)
            truncatedTexture->base_size = 1U;
        else
            truncatedTexture->mip_size = 1U;
        auto truncated     = draw;
        truncated.textures = truncatedTextures;
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
            (kMipCount == 1U ? guardedTexture->base_address : guardedTexture->mip_address + kNativePreparedCubeMatrixTailOffset),
            (kMipCount == 1U ? guardedTexture->base_address + guardedTexture->base_size : guardedTexture->mip_address + kNativePreparedCubeMatrixMipExtent)
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

        const auto rejectCube = [&](const char* name, auto mutate)
        {
            auto textures = std::vector<rex::graphics::diagnostic::DrawCaptureFrameInputTextureView>(draw.textures.begin(), draw.textures.end());
            auto selected = std::ranges::find_if(textures, [](const auto& t)
                                                 {
                                                     return t.required;
                                                 });
            auto words    = std::array<std::uint32_t, 6>{};
            std::copy(selected->fetch_words.begin(), selected->fetch_words.end(), words.begin());
            auto registers = std::vector<std::uint32_t>(draw.registers.begin(), draw.registers.end());
            mutate(*selected, words);
            selected->fetch_words = words;
            std::copy(words.begin(), words.end(), registers.begin() + 0x4800 + 6 * selected->fetch_constant);
            auto bad      = draw;
            bad.registers = registers;
            bad.textures  = textures;
            reject(bad, name);
        };
        rejectCube("wrong_normalized_depth", [](auto& t, auto&)
                   {
                       t.depth = 5;
                   });
        rejectCube("raw_stack_depth", [](auto&, auto& w)
                   {
                       w[2] = (w[2] & 0x03FFFFFFU) | (4U << 26);
                   });
        rejectCube("raw_normalized_dimension", [](auto&, auto& w)
                   {
                       w[5] = (w[5] & ~(3U << 9)) | (1U << 9);
                   });
        rejectCube("non_square", [](auto& t, auto&)
                   {
                       --t.width;
                   });
        rejectCube("wrong_face_count", [](auto& t, auto&)
                   {
                       t.depth = 7;
                   });
        rejectCube("missing_final_face_tail", [](auto& t, auto&)
                   {
                       --t.mip_size;
                   });
        rejectCube("wrong_signed_binding", [](auto& t, auto&)
                   {
                       t.is_signed = !t.is_signed;
                   });
        rejectCube("exponent_adjustment", [](auto&, auto& w)
                   {
                       w[3] |= 1U << 13;
                   });
        rejectCube(cubeMatrixForm->format == 20 ? "other_cube_format" : "bc3_cube", [](auto& t, auto& w)
                   {
                       t.format = cubeMatrixForm->format == 20 ? 6 : 20;
                       w[1]     = (w[1] & ~63U) | t.format;
                   });
        if (cubeMatrixForm->format == 32)
        {
            rejectCube("biased_float_signs", [](auto& t, auto& w)
                       {
                           t.swizzled_signs = 0xAA;
                           w[0]             = (w[0] & ~0x3FCU) | (0xAAU << 2);
                       });
            rejectCube("gamma_float_signs", [](auto& t, auto& w)
                       {
                           t.swizzled_signs = 0xFF;
                           w[0]             = (w[0] & ~0x3FCU) | (0xFFU << 2);
                       });
        }

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

    NativeGameplayShaderInterface                        shader;
    std::vector<CubeSource>                              sources;
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
                shader.textureDimensions[0] == NativeDrawReplayTextureDimension::TextureCube,
            std::string(role) + " catalog must bind one guest slot-0 TextureCube");
    require(!shader.vertexAttributes.empty(), std::string(role) + " catalog has no vertex attributes");
    return shader;
}

NativePreparedCubeMatrixFixture fixtureForLod(std::uint32_t level)
{
    auto fixture          = MakeNativePreparedCubeMatrixFixture();
    fixture.selected_lods = { level, level };
    for (std::size_t vertex = 0; vertex < 72; ++vertex)
        setBigEndianFloat(fixture.vertex_bytes, vertex * fixture.vertex_stride + 28, float(level));
    fixture.expected_selected_pixels = CubeOraclePixels(fixture);
    return fixture;
}

NativeDrawReplayTexture controlTexture(const NativePreparedCubeMatrixFixture& fixture)
{
    const auto                                 raw = CubeOraclePayload(fixture);
    std::vector<std::span<const std::uint8_t>> spans;
    std::size_t                                offset = 0;
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned level = 0; level < kMipCount; ++level)
        {
            const auto bytes = std::size_t(CubeMatrixLevelUnits(level)) * CubeMatrixLevelUnits(level) * cubeMatrixForm->bytes_per_texel;
            spans.emplace_back(raw.data() + offset, bytes);
            offset += bytes;
        }
    NativeDrawReplayTexture             texture;
    std::string                         error;
    const NativeDrawReplayTextureLayout layout{ cubeMatrixForm->width, cubeMatrixForm->width, cubeMatrixForm->format == 20 ? NativeDrawReplayTextureFormat::Bc3 : cubeMatrixForm->format == 6 ? NativeDrawReplayTextureFormat::Rgba8
                                                                                                                                                                                              : NativeDrawReplayTextureFormat::Rgba16Float,
                                                kMipCount,
                                                NativeDrawReplayTextureDimension::TextureCube };
    require(PackNativeDrawReplayTextureVersion(layout, spans, texture, error), error);
    texture.swizzle = cubeMatrixForm->format == 6 ? std::array<std::uint8_t, 4>{ 2, 1, 0, 5 } : std::array<std::uint8_t, 4>{ 0, 1, 2, 3 };
    require(offset == raw.size(), "raw oracle has trailing payload bytes");
    return texture;
}

NativeDrawReplayRecipe recipeFromSource(const CubeSource& source, const NativeGameplayShaderInterface& shader, const NativeDrawReplayTexture* texture)
{
    const auto&                                draw = source.event.draw.draw;
    std::vector<NativeGameplayVertexFetchSpan> fetches;
    for (const auto& fetch : draw.vertex_fetches)
        fetches.push_back({ fetch.fetch_constant, fetch.base, fetch.Payload() });
    NativeDrawReplaySampler sampler;
    sampler.address = { 3, 3, 3 };
    sampler.minLod  = 0;
    sampler.maxLod  = float(kMipCount - 1);
    NativeGameplayTextureBinding binding{ 0, texture, &sampler };
    NativeGameplayDrawView       view;
    view.registers       = source.registers;
    view.vertexMicrocode = draw.vertex_ucode;
    view.pixelMicrocode  = draw.pixel_ucode;
    view.vertexFetches   = fetches;
    view.indexBytes      = draw.index.bytes;
    view.primitive       = draw.primitive_type;
    view.indexCount      = draw.requested_index_count;
    view.indexFormat     = draw.index.format;
    view.indexEndian     = draw.index.endianness;
    view.indexed         = draw.index.present;
    if (texture)
        view.textureBindings = { &binding, 1 };
    NativeGameplayDrawTarget target;
    target.width       = kOutputWidth;
    target.height      = kOutputHeight;
    target.sampleCount = target.sampleMask = 1;
    target.format                          = NativeDrawReplayTargetFormat::Rgba8;
    target.viewport                        = source.recipe.viewport;
    target.scissor                         = source.recipe.scissor;
    NativeDrawReplayRecipe recipe;
    std::string            error;
    const bool             built = BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error);
    if (!texture)
    {
        require(!built && !error.empty() && recipe.outputPath.empty(), "omitted cube owner reached recipe construction");
        return {};
    }
    require(built, error);
    recipe.clearColor = { 0, 0, 0, 255 };
    return recipe;
}

std::vector<NativeDrawReplayRecipe> recipesFor(std::span<const CubeSource> sources, const NativeGameplayShaderInterface& shader, const NativeDrawReplayTexture& texture, bool prepared = false)
{
    require(sources.size() == 24, "cube atlas requires 24 retained Prepared draw events");
    std::vector<NativeDrawReplayRecipe> result;
    for (const auto& source : sources)
    {
        if (prepared)
        {
            // Execute the recipe admitted by the real Prepared command feed.
            // Controls replace only the retained texture or direction inputs.
            auto recipe = source.recipe;
            require(recipe.textures[0].textureVersion == source.owner.textureVersion &&
                        recipe.textures[0].swizzle == source.owner.swizzle,
                    "Prepared cube command feed changed its exact CPU owner or component mapping");
            if (texture.textureVersion != source.owner.textureVersion || texture.swizzle != source.owner.swizzle)
                recipe.textures[0] = texture;
            recipe.clearColor = { 0, 0, 0, 255 };
            result.push_back(std::move(recipe));
        }
        else
            result.push_back(recipeFromSource(source, shader, &texture));
        // Only the first draw clears the frame; later draws keep the native no-clear value.
        if (result.size() > 1)
            result.back().clearColor = { 0, 0, 0, 0 };
    }
    return result;
}

std::vector<std::uint8_t> duplicatePlane(std::span<const std::uint8_t> pixels)
{
    require(pixels.size() == kPlane, "cube terminal plane is incomplete");
    std::vector<std::uint8_t> result(pixels.begin(), pixels.end());
    result.insert(result.end(), pixels.begin(), pixels.end());
    return result;
}

void compare(std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected, const std::string& label)
{
    require(actual.size() == expected.size(), label + " incomplete plane");
    const auto difference = std::ranges::mismatch(actual, expected);
    if (difference.in1 != actual.end())
    {
        const auto byte  = std::size_t(difference.in1 - actual.begin()) % kPlane;
        const auto pixel = byte / 4;
        const auto x = pixel % kOutputWidth, y = pixel / kOutputWidth;
        fail(label + " first mismatch face=" + std::to_string(x / cubeMatrixForm->width) + " LOD=" + std::to_string(currentCubeLod) +
             " texel=" + std::to_string((x % cubeMatrixForm->width) >> currentCubeLod) + "," + std::to_string((y % cubeMatrixForm->width) >> currentCubeLod) +
             " projected-channel=" + std::to_string(y / cubeMatrixForm->width) + " terminal-channel=" + std::to_string(byte % 4));
    }
}

std::vector<std::uint8_t> replay(NativeRendererD3D12& renderer, std::vector<NativeDrawReplayRecipe> recipes, const std::filesystem::path& path)
{
    const auto result = renderer.SubmitDrawFrame(std::move(recipes), path);
    require(result.success && result.completionFenceValue != 0, "cube native completion failed: " + result.error);
    const auto pixels = readBytes(path);
    require(pixels.size() == 2 * kPlane && std::equal(pixels.begin(), pixels.begin() + kPlane, pixels.begin() + kPlane), "cube native RT0 planes differ");
    return pixels;
}

void sensitivePanel(std::span<const std::uint8_t> mutated, std::span<const std::uint8_t> expected, unsigned face, unsigned channel, const std::string& label)
{
    require(mutated.size() == 2 * kPlane && expected.size() == mutated.size(), label + " incomplete output");
    bool changed = false;
    for (unsigned y = channel * cubeMatrixForm->width; y < (channel + 1) * cubeMatrixForm->width; ++y)
        for (unsigned x = face * cubeMatrixForm->width; x < (face + 1) * cubeMatrixForm->width; ++x)
        {
            const auto at = (std::size_t(y) * kOutputWidth + x) * 4;
            changed |= mutated[at] != expected[at];
            require(mutated[at + 3] == 255, label + " changed presenter alpha");
        }
    require(changed, label + " selected face/channel is insensitive");
}

NativeDrawReplayTexture repack(const NativeDrawReplayTexture& source, std::vector<std::uint8_t> bytes)
{
    std::vector<std::span<const std::uint8_t>> spans;
    for (const auto& sub : source.textureVersion->Subresources())
        spans.emplace_back(bytes.data() + sub.sourceOffset, sub.rowBytes * sub.rowCount);
    NativeDrawReplayTexture result;
    std::string             error;
    require(PackNativeDrawReplayTextureVersion(source.textureVersion->Layout(), spans, result, error), error);
    result.swizzle = source.swizzle;
    return result;
}

NativeDrawReplayTexture mutateChannel(const NativeDrawReplayTexture& source, unsigned face, unsigned level, unsigned channel)
{
    require(channel < CubeMatrixDataChannels(), "cube data mutation selects a constant channel");
    const auto                payload = source.payloadBytes();
    std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    const auto&               span = source.textureVersion->Subresources()[face * kMipCount + level];
    if (cubeMatrixForm->format == 20)
    {
        for (unsigned y = 0; y < span.rowCount; ++y)
            for (unsigned x = 0; x < span.rowBytes / 16; ++x)
            {
                const auto at = span.sourceOffset + y * span.rowBytes + x * 16;
                if (channel == 3)
                {
                    bytes[at] ^= 255;
                    bytes[at + 1] ^= 255;
                }
                else
                {
                    const auto code     = bytes[at + 12] & 1;
                    const auto endpoint = at + 8 + code * 2;
                    const auto mask     = channel == 0 ? 0xF800U : channel == 1 ? 0x07E0U
                                                                                : 0x001FU;
                    const auto color    = unsigned(bytes[endpoint]) | unsigned(bytes[endpoint + 1]) << 8;
                    const auto changed  = color ^ mask;
                    bytes[endpoint]     = std::uint8_t(changed);
                    bytes[endpoint + 1] = std::uint8_t(changed >> 8);
                }
            }
        return repack(source, std::move(bytes));
    }
    const auto lane = cubeMatrixForm->format == 6 ? std::array<unsigned, 4>{ 2, 1, 0, 3 }[channel] : channel;
    for (unsigned y = 0; y < span.rowCount; ++y)
        for (unsigned x = 0; x < (cubeMatrixForm->width >> level); ++x)
        {
            const auto at = span.sourceOffset + y * span.rowBytes + x * cubeMatrixForm->bytes_per_texel + lane * (cubeMatrixForm->format == 6 ? 1 : 2);
            if (cubeMatrixForm->format == 6)
                bytes[at] = bytes[at] == 1 ? 2 : 1;
            else
            {
                const auto old  = std::uint16_t(bytes[at] | std::uint16_t(bytes[at + 1]) << 8);
                const auto half = old == kCubeHalfPalette[0] ? kCubeHalfPalette[1] : kCubeHalfPalette[0];
                bytes[at]       = std::uint8_t(half);
                bytes[at + 1]   = std::uint8_t(half >> 8);
            }
        }
    return repack(source, std::move(bytes));
}

std::vector<std::uint8_t> bc3MutationOracle(NativePreparedCubeMatrixFixture fixture, const NativeDrawReplayTexture& texture)
{
    const auto  bytes  = texture.payloadBytes();
    std::size_t offset = 0;
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned level = 0; level < kMipCount; ++level)
            for (unsigned y = 0; y < CubeMatrixLevelUnits(level); ++y)
                for (unsigned x = 0; x < CubeMatrixLevelUnits(level); ++x)
                {
                    auto&      raw = level ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
                    const auto at  = CubeMatrixAddress(*cubeMatrixForm, face, level, x, y);
                    for (unsigned byte = 0; byte < 16; ++byte)
                        raw[at + (byte ^ 1U)] = bytes[offset++];
                }
    require(offset == bytes.size(), "BC3 mutation oracle payload extent changed");
    return CubeOraclePixels(fixture);
}

void perturbFace(std::vector<NativeDrawReplayRecipe>& recipes, unsigned face, std::span<const NativeGameplayVertexAttribute> attributes)
{
    const auto directionAttribute = std::ranges::find_if(attributes, [](const auto& attribute)
                                                         {
                                                             return attribute.fetchConstant == 95 && attribute.offsetBytes == 16;
                                                         });
    require(directionAttribute != attributes.end() &&
                std::ranges::count_if(attributes, [](const auto& attribute)
                                      {
                                          return attribute.fetchConstant == 95 && attribute.offsetBytes == 16;
                                      }) == 1 &&
                directionAttribute->strideBytes == 56 && directionAttribute->format == 38 && !directionAttribute->normalized,
            "cube direction attribute is missing, duplicated or changed");
    // The draw builder packs each catalog attribute into one decoded float4.
    const auto directionOffset = std::size_t(directionAttribute - attributes.begin()) * 4 * sizeof(float);
    // Change only the actual native direction input for this face column.
    for (unsigned channel = 0; channel < 4; ++channel)
    {
        auto&      recipe   = recipes[channel * 6 + face];
        const auto original = recipe.vertexBytes();
        recipe.vertexData.assign(original.begin(), original.end());
        recipe.sharedVertexData.reset();
        require(recipe.vertexAttributeCount == attributes.size() &&
                    recipe.vertexStrideBytes == attributes.size() * 4 * sizeof(float) &&
                    directionOffset + 4 * sizeof(float) <= recipe.vertexStrideBytes && recipe.indices.size() == 3,
                "cube decoded IA interface changed");
        for (unsigned vertex = 0; vertex < 3; ++vertex)
        {
            const auto local     = std::array<std::array<float, 2>, 3>{ { { 0, 0 }, { 0, float(2 * cubeMatrixForm->width) }, { float(2 * cubeMatrixForm->width), 0 } } }[vertex];
            const auto direction = CubeMatrixDirection((face + 1) % 6, (local[0] + .25F) / cubeMatrixForm->width, (local[1] + .25F) / cubeMatrixForm->width);
            const auto offset    = std::size_t(recipe.indices[vertex]) * recipe.vertexStrideBytes + directionOffset;
            require(offset + 12 <= recipe.vertexData.size(), "cube perturbation exceeds decoded vertices");
            std::memcpy(recipe.vertexData.data() + offset, direction.data(), 12);
        }
    }
}

void guardFixture(const NativePreparedCubeMatrixFixture& fixture)
{
    using namespace rex::graphics;
    const auto& f = *cubeMatrixForm;
    require(fixture.vertex_bytes.size() == 72 * 56 && fixture.index_bytes.size() == 144, "cube atlas geometry is incomplete");
    require(fixture.pixel_shader_words.size() == 24, "cube pixel word extent changed");
    const auto alu = [&](unsigned index)
    {
        ucode::AluInstruction instruction;
        std::memcpy(&instruction, fixture.pixel_shader_words.data() + index * 3, 12);
        return instruction;
    };
    const auto cube = alu(2), reciprocal = alu(3), mad = alu(4);
    require(cube.vector_opcode() == ucode::AluVectorOpcode::kCube && cube.vector_dest() == 1 && cube.vector_write_mask() == 15 &&
                cube.src_reg(1) == 0 && cube.src_reg(2) == 0 && cube.src_swizzle(1) == 0xA6 && cube.src_swizzle(2) == 0xCD &&
                cube.src_is_temp(1) && cube.src_is_temp(2) && !cube.is_predicated(),
            "decoded cube ALU fields changed");
    require(reciprocal.scalar_opcode() == ucode::AluScalarOpcode::kRcp && reciprocal.scalar_dest() == 1 && reciprocal.scalar_write_mask() == 4 && reciprocal.src_reg(3) == 0x81 && reciprocal.src_swizzle(3) == 0xC6, "decoded absolute cube reciprocal changed");
    require(mad.vector_opcode() == ucode::AluVectorOpcode::kMad && mad.vector_dest() == 1 && mad.vector_write_mask() == 3 && mad.src_reg(1) == 1 && mad.src_reg(2) == 1 && mad.src_reg(3) == 0 && mad.src_swizzle(2) == 0xC6 && mad.src_swizzle(3) == 0x6C, "decoded canonical cube MAD changed");
    const auto select = alu(6), output = alu(7);
    require(select.vector_opcode() == ucode::AluVectorOpcode::kDp4 && select.src_reg(1) == 2 && select.src_reg(2) == 1 &&
                select.src_is_temp(1) && !select.src_is_temp(2) && select.vector_write_mask() == 15 && select.vector_dest() == 3,
            "decoded cube channel projection changed");
    require(output.is_export() && output.vector_opcode() == ucode::AluVectorOpcode::kMad && output.vector_write_mask() == 7 &&
                output.src_reg(1) == 3 && output.src_reg(2) == 2 && output.src_reg(3) == 2 && output.src_swizzle(2) == 0x6C && output.src_swizzle(3) == 0xB1,
            "decoded cube safe readout changed");
    require(fixture.pixel_shader_words[0] == 0x01016001 && fixture.pixel_shader_words[1] == 0x10071800 && fixture.pixel_shader_words[2] == 0x28000000,
            "authored straight-line cube control flow changed");
    ucode::TextureFetchInstruction setter, fetch;
    std::memcpy(&setter, fixture.pixel_shader_words.data() + 3, 12);
    std::memcpy(&fetch, fixture.pixel_shader_words.data() + 15, 12);
    require(setter.opcode() == ucode::FetchOpcode::kSetTextureLod && setter.src() == 0 && setter.src_swizzle() == 3 && !setter.is_src_relative() && !setter.is_predicated(), "decoded cube LOD setter changed");
    require(fetch.opcode() == ucode::FetchOpcode::kTextureFetch && fetch.dimension() == xenos::FetchOpDimension::kCube && fetch.src() == 1 && fetch.src_swizzle() == 0x31 && fetch.dest() == 2 && fetch.fetch_constant_index() == 0 && fetch.use_register_lod() && !fetch.use_computed_lod() && !fetch.use_register_gradients() && !fetch.unnormalized_coordinates() && !fetch.is_predicated() && !fetch.is_src_relative() && !fetch.is_dest_relative() && !fetch.lod_bias() && !fetch.offset_x() && !fetch.offset_y() && !fetch.offset_z() && fetch.mag_filter() == xenos::TextureFilter::kPoint && fetch.min_filter() == xenos::TextureFilter::kPoint && fetch.mip_filter() == xenos::TextureFilter::kPoint && fetch.aniso_filter() == xenos::AnisoFilter::kDisabled, "decoded point cube fetch fields changed");
    const auto layout = texture_util::GetGuestTextureLayout(xenos::DataDimension::kCube, f.pitch, f.width, f.width, 6, false, static_cast<xenos::TextureFormat>(f.format), true, true, f.levels - 1);
    require(layout.base.level_data_extent_bytes == f.base_extent && layout.mips_total_extent_bytes == f.mip_extent && layout.packed_level == f.packed_level && layout.mip_offsets_bytes[f.packed_level] == f.tail_offset && layout.mips[f.packed_level].level_data_extent_bytes == f.tail_extent, "STOP: compiled SDK layout differs from frozen cube extents: base=" + std::to_string(layout.base.level_data_extent_bytes) + " mips=" + std::to_string(layout.mips_total_extent_bytes) + " packed=" + std::to_string(layout.packed_level) + " tail=" + std::to_string(layout.mips[f.packed_level].level_data_extent_bytes));
    std::array<std::vector<bool>, 2> live{ std::vector<bool>(fixture.texture.base_guest_bytes.size()), std::vector<bool>(fixture.texture.mip_guest_bytes.size()) };
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned level = 0; level < kMipCount; ++level)
        {
            const auto& stored = level ? layout.mips[std::min(level, f.packed_level)] : layout.base;
            unsigned    px = 0, py = 0, pz = 0;
            if (level >= f.packed_level)
                require(texture_util::GetPackedMipOffset(f.width, f.width, 1, static_cast<xenos::TextureFormat>(f.format), level, px, py, pz), "cube packed offset missing");
            require(!pz && stored.row_pitch_bytes == f.rows[level] && stored.array_slice_stride_bytes == f.strides[level], "STOP: cube stored stride differs from frozen table: LOD=" + std::to_string(level) + " row=" + std::to_string(stored.row_pitch_bytes) + " face_stride=" + std::to_string(stored.array_slice_stride_bytes));
            for (unsigned y = 0; y < CubeMatrixLevelUnits(level); ++y)
                for (unsigned x = 0; x < CubeMatrixLevelUnits(level); ++x)
                {
                    const auto at = (level ? layout.mip_offsets_bytes[std::min(level, f.packed_level)] : 0) + face * stored.array_slice_stride_bytes + (py + y) * stored.row_pitch_bytes + (px + x) * f.bytes_per_texel;
                    require(at == CubeMatrixAddress(f, face, level, x, y), "STOP: cube live address differs from frozen table");
                    for (unsigned byte = 0; byte < f.bytes_per_texel; ++byte)
                    {
                        require(at + byte < live[level != 0].size() && !live[level != 0][at + byte], "cube live ranges overlap or exceed the allocation");
                        live[level != 0][at + byte] = true;
                    }
                }
        }
    for (unsigned bank = 0; bank < 2; ++bank)
    {
        const auto& raw = bank ? fixture.texture.mip_guest_bytes : fixture.texture.base_guest_bytes;
        for (std::size_t at = 0; at < raw.size(); ++at)
            if (!live[bank][at])
                require(raw[at] == 0xCD, "cube padding lost poison");
    }
    auto sampling = fixture;
    for (unsigned level = 0; level < kMipCount; ++level)
    {
        sampling.selected_lods = { level, level };
        for (unsigned panel = 0; panel < 24; ++panel)
        {
            std::vector<bool> texels(std::size_t(cubeMatrixForm->width >> level) * (cubeMatrixForm->width >> level));
            for (unsigned y = 0; y < cubeMatrixForm->width; ++y)
                for (unsigned x = 0; x < cubeMatrixForm->width; ++x)
                {
                    const auto original = CubeOracleSample(sampling, panel, x, y, 0, 1.5 / (1024 * cubeMatrixForm->width));
                    const auto integer  = CubeOracleSample(sampling, panel, x, y, 0);
                    const auto half     = CubeOracleSample(sampling, panel, x, y, .5);
                    require(original.face == panel % 6 && integer.face == original.face && half.face == original.face && original.x == integer.x && original.x == half.x && original.y == integer.y && original.y == half.y && half.major_margin >= 1.0 / (2 * cubeMatrixForm->width), "STOP: cube conventions differ or cross a seam");
                    for (const auto p : { original, integer, half })
                    {
                        const auto tx = p.u * (cubeMatrixForm->width >> level), ty = p.v * (cubeMatrixForm->width >> level);
                        require(tx > std::floor(tx) && ty > std::floor(ty) && std::ceil(tx) - tx > 1.0 / 1024 / (2 * cubeMatrixForm->width) && std::ceil(ty) - ty > 1.0 / 1024 / (2 * cubeMatrixForm->width), "STOP: cube sampling reaches a texel boundary");
                    }
                    texels[original.y * (cubeMatrixForm->width >> level) + original.x] = true;
                }
            require(std::ranges::all_of(texels, [](bool sampled)
                                        {
                                            return sampled;
                                        }),
                    "cube atlas does not cover every mip texel");
        }
    }
    auto words = fixture.texture.fetch_words;
    words[1] |= 0x100000;
    words[5] |= 0x180000;
    NativeDrawReplayTexture decoded;
    std::string             error;
    // Guest buffers are page-aligned; the upload consumes only the canonical ranges.
    require(DecodeNativeTextureVersioned(words, std::span(fixture.texture.base_guest_bytes).first(f.base_extent), std::span(fixture.texture.mip_guest_bytes).first(f.mip_extent), decoded, error),
            error);
    compare(decoded.payloadBytes(), CubeOraclePayload(fixture), "full immutable cube payload vs independent raw oracle");
    require(decoded.textureVersion->Subresources().size() == 6 * kMipCount && decoded.payloadBytes().size() == f.payload_bytes, "cube production upload omitted a face/level span");
    auto f6                                       = fixture;
    f6.selected_lods                              = { kMipCount - 1, kMipCount - 1 };
    const auto                             pixels = CubeOraclePixels(f6);
    std::array<std::array<unsigned, 4>, 6> fingerprints{};
    for (unsigned c = 0; c < CubeMatrixDataChannels(); ++c)
    {
        std::array<unsigned, 6> last{};
        for (unsigned face = 0; face < 6; ++face)
        {
            last[face]            = pixels[(std::size_t(c * cubeMatrixForm->width) * kOutputWidth + face * cubeMatrixForm->width) * 4];
            fingerprints[face][c] = last[face];
        }
        for (unsigned a = 0; a < 6; ++a)
            for (unsigned b = 0; b < a; ++b)
                require(f.format == 20 || last[a] != last[b], "STOP: LOD6 face/channel fingerprints collide");
    }
    if (f.format == 20)
        for (unsigned face = 0; face < 6; ++face)
            for (unsigned c = 0; c < 4; ++c)
                require(fingerprints[face][c] != fingerprints[(face + 1) % 6][c], "BC3 last-level adjacent face/channel control is insensitive");
    for (const auto& channels : fingerprints)
        for (unsigned a = 0; a < CubeMatrixDataChannels(); ++a)
            for (unsigned b = 0; b < a; ++b)
                require(f.format == 20 || channels[a] != channels[b], "STOP: LOD6 channels collide within a face");
}

void guardSource(const std::filesystem::path& path)
{
    const auto        bytes = readBytes(path);
    const std::string hlsl(bytes.begin(), bytes.end());
    require(hlsl.find("r1.xyzw = cube(r0.xyzz, cubeMapData);") != std::string::npos && hlsl.find("r1.yxw, cubeMapData, registerLod)") != std::string::npos && hlsl.find("registerLod = r0.w;") != std::string::npos && hlsl.find(".SampleLevel(") != std::string::npos && hlsl.find("register(t0, space2)") != std::string::npos && hlsl.find("= tfetchCube(") == std::string::npos, "STOP: adapter did not restore canonical cube direction/register LOD");
}

void emitFixture(const std::filesystem::path& output)
{
    require(!std::filesystem::exists(output), "cube fixture output must be fresh");
    std::filesystem::create_directories(output);
    const auto fixture = MakeNativePreparedCubeMatrixFixture();
    guardFixture(fixture);
    writeBytes(output / "registers.le.bin", wordsBytes(fixture.register_image));
    writeBytes(output / "vertex-ucode.le.bin", wordsBytes(fixture.vertex_shader_words));
    writeBytes(output / "pixel-ucode.le.bin", wordsBytes(fixture.pixel_shader_words));
    writeBytes(output / "vertices.bin", fixture.vertex_bytes);
    writeBytes(output / "indices.bin", fixture.index_bytes);
    writeBytes(output / "texture-base.cube.bin", fixture.texture.base_guest_bytes);
    writeBytes(output / "texture-mips.cube.bin", fixture.texture.mip_guest_bytes);
    std::ofstream metadata(output / "fixture.json");
    metadata << "{\"schema_version\":1,\"form\":\"cube-matrix\",\"form_count\":3,\"width\":" << cubeMatrixForm->width
             << ",\"height\":" << cubeMatrixForm->width << ",\"faces\":6,\"mip_levels\":" << kMipCount
             << ",\"pitch\":" << cubeMatrixForm->pitch << ",\"panel_width\":" << cubeMatrixForm->width
             << ",\"panel_height\":" << cubeMatrixForm->width
             << ",\"panel_columns\":6,\"panel_rows\":4,\"half_pixel_offset\":true,\"pixel_tolerance\":0,\"interpolated_oracle_claim\":false,\"vertex_word_sha256\":" << tomlString(wordDigest(fixture.vertex_shader_words)) << ",\"pixel_word_sha256\":" << tomlString(wordDigest(fixture.pixel_shader_words)) << "}\n";
    require(metadata.good(), "cube fixture metadata write failed");
}

void retainOriginal(const std::filesystem::path& root, unsigned level, const NativePreparedCubeMatrixOriginalResult& result, const PreparedCollector& collector)
{
    const auto prefix = "original-" + std::to_string(level);
    writeBytes(root / (prefix + "-old-base.bin"), result.old_base_bytes);
    writeBytes(root / (prefix + "-old-mips.bin"), result.old_mip_bytes);
    writeBytes(root / (prefix + "-new-base.bin"), result.new_base_bytes);
    writeBytes(root / (prefix + "-new-mips.bin"), result.new_mip_bytes);
    writeBytes(root / (prefix + "-baseline-terminal.rgba8.bin"), result.baseline_terminal_pixels);
    writeBytes(root / (prefix + "-terminal.rgba8.bin"), result.terminal_pixels);
    writeBytes(root / (prefix + "-baseline-expected.rgba8.bin"), result.baseline_expected_pixels);
    writeBytes(root / (prefix + "-expected.rgba8.bin"), result.expected_pixels);
    std::ofstream receipt(root / (prefix + ".toml"));
    receipt << "schema_version = 2\nhalf_pixel_offset = true\nlevel = " << level << "\nfaces = 6\nchannels = 4\ndata_channels = " << CubeMatrixDataChannels() << "\nupdate_kind = " << static_cast<unsigned>(result.update_kind) << "\nprepared_draws = " << result.prepared_draws.size() << "\ncpu_partial_overlap_writes = " << std::boolalpha << result.cpu_partial_overlap_writes << "\nsource_raw_bytes_frozen = " << result.source_raw_bytes_frozen << "\ncpu_update_overlap_written = " << result.cpu_update_overlap_written << "\ncpu_update_outside_unchanged = " << result.cpu_update_outside_unchanged << "\nbaseline_copy_id = " << result.baseline_copy_id << "\nbaseline_swap_id = " << result.baseline_swap_id << "\ncompletion_copy_id = " << result.completion_copy_id << "\ncompletion_swap_id = " << result.completion_swap_id << "\nsource_texture_fetch_words = [";
    for (unsigned i = 0; i < 6; ++i)
        receipt << (i ? ", " : "") << result.source_texture_fetch_words[i];
    receipt << "]\nfailure = " << tomlString(result.failure) << "\ncollector_error = " << tomlString(collector.error) << "\n";
    for (const auto& probe : collector.negativeProbes)
        receipt << "[[negatives]]\nname = " << tomlString(probe.name) << "\nrejected = " << probe.rejected << "\nno_admitted_event = " << probe.no_admitted_event << "\nerror = " << tomlString(probe.error) << "\n";
    for (const auto& draw : result.prepared_draws)
        for (const auto& texture : draw.textures)
        {
            receipt << "[[prepared_textures]]\ndraw_id = " << draw.id << "\nframe = " << draw.frame << "\nsubmission = " << draw.submission
                    << "\nordinal = " << draw.event_ordinal << "\nfetch_constant = " << texture.fetch_constant << "\nshader_binding_index = " << texture.shader_binding_index
                    << "\nrequired = " << texture.required << "\nis_signed = " << texture.is_signed << "\navailable = " << texture.available
                    << "\nformat = " << texture.format << "\nendianness = " << texture.endianness << "\nswizzled_signs = " << unsigned(texture.swizzled_signs)
                    << "\ncomponent_mapping = " << texture.component_mapping << "\ndimension = " << texture.dimension << "\ndepth = " << texture.depth
                    << "\nbase_address = " << texture.base_address << "\nbase_size = " << texture.base_size << "\nmip_address = " << texture.mip_address << "\nmip_size = " << texture.mip_size
                    << "\nmemory_watch_supplied = " << texture.memory_watch_supplied << "\nmemory_watch_identity = " << texture.memory_watch_identity
                    << "\nmemory_watch_generation = " << texture.memory_watch_generation << "\nmemory_current_before = " << texture.memory_current_before
                    << "\nmemory_current_after = " << texture.memory_current_after << "\nmemory_watch_stale = " << texture.memory_watch_stale
                    << "\nbase_bytes_copied = " << texture.base_bytes_copied << "\nmip_bytes_copied = " << texture.mip_bytes_copied << "\nfetch_words = [";
            for (std::size_t i = 0; i < texture.fetch_words.size(); ++i)
                receipt << (i ? ", " : "") << texture.fetch_words[i];
            receipt << "]\n";
        }
    require(receipt.good(), "cube Original receipt write failed");
}

void guardUpdate(const NativePreparedCubeMatrixOriginalResult& original, unsigned level)
{
    std::array<std::vector<bool>, 2> changed{ std::vector<bool>(original.old_base_bytes.size()), std::vector<bool>(original.old_mip_bytes.size()) };
    for (unsigned face = 0; face < 6; ++face)
        for (unsigned y = 0; y < CubeMatrixLevelUnits(level); ++y)
            for (unsigned x = 0; x < CubeMatrixLevelUnits(level); ++x)
                for (unsigned byte = 0; byte < cubeMatrixForm->bytes_per_texel; ++byte)
                    if (original.update_kind != NativePreparedCubeMatrixUpdateKind::kGuestAlphaOnly || byte == 0)
                        changed[level != 0][CubeMatrixAddress(*cubeMatrixForm, face, level, x, y) + byte] = true;
    for (unsigned bank = 0; bank < 2; ++bank)
    {
        const auto& before = bank ? original.old_mip_bytes : original.old_base_bytes;
        const auto& after  = bank ? original.new_mip_bytes : original.new_base_bytes;
        require(before.size() == after.size(), "cube CPU update changed allocation size");
        for (std::size_t at = 0; at < before.size(); ++at)
            if (!changed[bank][at])
                require(before[at] == after[at], "cube update changed untouched texels or padding");
            else if (original.update_kind == NativePreparedCubeMatrixUpdateKind::kGuestAlphaOnly)
                require(before[at] != after[at], "cube alpha control left a guest alpha byte unchanged");
    }
}

void guardPreparedJoins(const NativePreparedCubeMatrixOriginalResult& original)
{
    using namespace rex::graphics::diagnostic;
    const NativePreparedCubeMatrixDrawRecord::TextureRecord* first   = nullptr;
    const NativePreparedCubeMatrixDrawRecord::TextureRecord* updated = nullptr;
    require(original.prepared_draws.size() == 48, "cube matrix lost a Prepared panel draw");
    for (std::size_t index = 0; index < original.prepared_draws.size(); ++index)
    {
        const auto& prepared = original.prepared_draws[index];
        const auto  issued   = std::ranges::find_if(original.draws, [&](const auto& draw)
                                                    {
                                                     return draw.id == prepared.id && draw.frame == prepared.frame && draw.submission == prepared.submission;
                                                    });
        require(issued != original.draws.end() && prepared.event_ordinal < issued->event_ordinal &&
                    !prepared.original_host_issued && prepared.execution_status == 0 && prepared.cpu_data_complete &&
                    issued->original_host_issued && issued->execution_status == 1 &&
                    prepared.vertex_shader_words == issued->vertex_shader_words && prepared.pixel_shader_words == issued->pixel_shader_words,
                "cube Prepared receipt did not precede its exact Original draw");
        const auto panel = index % 24;
        require(prepared.registers[0x2302] == 4 &&
                    prepared.registers[0x2081] == ((panel % 6) * cubeMatrixForm->width | ((panel / 6) * cubeMatrixForm->width << 16)) &&
                    prepared.registers[0x2082] == (((panel % 6 + 1) * cubeMatrixForm->width) | (((panel / 6 + 1) * cubeMatrixForm->width) << 16)) &&
                    prepared.registers[0x210F] == std::bit_cast<std::uint32_t>(float(cubeMatrixForm->width) / 2) &&
                    prepared.registers[0x2110] == std::bit_cast<std::uint32_t>(float(panel % 6 * cubeMatrixForm->width + cubeMatrixForm->width / 2)) &&
                    prepared.registers[0x2111] == std::bit_cast<std::uint32_t>(-float(cubeMatrixForm->width) / 2) &&
                    prepared.registers[0x2112] == std::bit_cast<std::uint32_t>(float(panel / 6 * cubeMatrixForm->width + cubeMatrixForm->width / 2)),
                "cube Prepared panel viewport or scissor differs from authored geometry");
        for (unsigned channel = 0; channel < 4; ++channel)
            require(prepared.registers[0x4404 + channel] == (channel == panel / 6 ? 0x3F800000U : 0U),
                    "cube Prepared panel channel selector changed");
        unsigned owners = 0;
        for (const auto& texture : prepared.textures)
        {
            if (!texture.required)
            {
                require(!texture.base_bytes_copied && !texture.mip_bytes_copied,
                        "optional cube companion became a second raw owner");
                continue;
            }
            ++owners;
            require(texture.available && !texture.gpu_written && texture.unavailable_reason == 0 && texture.depth == 6 &&
                        texture.width == cubeMatrixForm->width && texture.height == cubeMatrixForm->width && texture.pitch == cubeMatrixForm->pitch && texture.dimension == 3 &&
                        texture.is_signed == (cubeMatrixForm->format == 32) && texture.swizzled_signs == cubeMatrixForm->signs &&
                        texture.component_mapping == (0x1000 | cubeMatrixForm->swizzle) && texture.base_size == cubeMatrixForm->base_extent &&
                        texture.mip_size == cubeMatrixForm->mip_extent && texture.base_bytes_copied && texture.mip_bytes_copied,
                    "STOP: cube signedness, ownership or ranges differ from prediction");
            const auto& base = index < 24 ? original.old_base_bytes : original.new_base_bytes;
            const auto& mips = index < 24 ? original.old_mip_bytes : original.new_mip_bytes;
            require(texture.base_bytes.size() == texture.base_size && texture.mip_bytes.size() == texture.mip_size &&
                        std::equal(texture.base_bytes.begin(), texture.base_bytes.end(), base.begin()) &&
                        std::equal(texture.mip_bytes.begin(), texture.mip_bytes.end(), mips.begin()),
                    "cube Prepared raw copy differs from retained guest allocation");
            if (!first)
                first = &texture;
            if (index == 24)
                updated = &texture;
        }
        require(owners == 1, "STOP: cube Prepared draw has more or fewer than one required owner");
    }
    require(first && updated, "cube old/new raw owner join is missing");
    if (first->memory_current_before && first->memory_current_after && updated->memory_current_before && updated->memory_current_after)
        require(first->memory_watch_identity != updated->memory_watch_identity ||
                    first->memory_watch_generation != updated->memory_watch_generation,
                "STOP: current cube watch did not observe the same-allocation update");
}

// Lifecycle uses an actual Prepared owner and one retained authored panel.
NativeDrawReplayRecipe buildRecipe(const NativePreparedCubeMatrixFixture& fixture, const NativeGameplayShaderInterface& shader, const std::filesystem::path& path, NativeDrawReplayTexture texture, unsigned, std::span<const std::uint32_t> registers, unsigned vertexBase, unsigned = kOutputWidth, unsigned = kOutputHeight)
{
    CubeSource source;
    source.registers         = { registers.begin(), registers.end() };
    source.registers[0x2081] = 0;
    source.registers[0x2082] = cubeMatrixForm->width | (cubeMatrixForm->width << 16);
    source.registers[0x210F] = source.registers[0x2110] = std::bit_cast<std::uint32_t>(float(cubeMatrixForm->width) / 2);
    source.registers[0x2111]                            = std::bit_cast<std::uint32_t>(-float(cubeMatrixForm->width) / 2);
    source.registers[0x2112]                            = std::bit_cast<std::uint32_t>(float(cubeMatrixForm->width) / 2);
    source.registers[0x4404]                            = 0x3F800000;
    source.registers[0x4405] = source.registers[0x4406] = source.registers[0x4407] = 0;
    auto& draw                                                                     = source.event.draw.draw;
    draw.vertex_ucode                                                              = fixture.vertex_shader_words;
    draw.pixel_ucode                                                               = fixture.pixel_shader_words;
    rex::graphics::diagnostic::DrawCaptureVertexFetch fetch;
    fetch.fetch_constant = 95;
    fetch.base           = vertexBase;
    fetch.bytes          = fixture.vertex_bytes;
    draw.vertex_fetches.push_back(std::move(fetch));
    draw.index.bytes.assign(fixture.index_bytes.begin(), fixture.index_bytes.begin() + 6);
    draw.index.present         = true;
    draw.index.format          = 0;
    draw.index.endianness      = 1;
    draw.requested_index_count = 3;
    draw.primitive_type        = 4;
    source.recipe.viewport     = { 0, 0, float(cubeMatrixForm->width), float(cubeMatrixForm->width), 0, 0 };
    source.recipe.scissor      = { 0, 0, std::int32_t(cubeMatrixForm->width), std::int32_t(cubeMatrixForm->width) };
    auto recipe                = recipeFromSource(source, shader, &texture);
    recipe.outputPath          = path;
    return recipe;
}

int runForm(const std::filesystem::path& candidateCatalog, const std::filesystem::path& controlCatalog, const std::filesystem::path& candidateSource, const std::filesystem::path& outputRoot)
{
    require(!std::filesystem::exists(outputRoot), "cube form output must be fresh");
    std::filesystem::create_directories(outputRoot);
    const auto fixture = MakeNativePreparedCubeMatrixFixture();
    guardFixture(fixture);
    const auto candidate = loadShader(candidateCatalog, fixture.vertex_shader_words, fixture.pixel_shader_words, "candidate");
    const auto control   = loadShader(controlCatalog, fixture.vertex_shader_words, fixture.pixel_shader_words, "control");
    require(sameAttributes(candidate.vertexAttributes, control.vertexAttributes), "cube control/translated IA interfaces differ");
    guardSource(candidateSource);
    NativeRendererD3D12 candidateRenderer, controlRenderer;
    std::string         error;
    require(candidateRenderer.StartDrawStream(error) && controlRenderer.StartDrawStream(error), error);

    struct PairSource
    {
        NativePreparedCubeMatrixFixture fixture;
        std::vector<std::uint32_t>      registers;
        unsigned                        vertexGuestBase = 0;
        NativeDrawReplayTexture         preparedTexture;
    };

    std::vector<PairSource> pairSources(1);
    NativeDrawReplayTexture deviceFailureTexture;
    std::size_t             mutations = 0, facePerturbations = 0, omissions = 0, spansOmitted = 0, semanticControls = 0, alphaInvarianceControls = 0;
    for (unsigned level = 0; level < kMipCount; ++level)
    {
        currentCubeLod                                   = level;
        auto                                   requested = fixtureForLod(level);
        auto                                   collector = std::make_shared<PreparedCollector>(candidate);
        NativePreparedCubeMatrixOriginalParams params;
        params.fixture   = requested;
        params.first_lod = params.second_lod = level;
        NativePreparedCubeMatrixOriginalResult original;
        const auto                             status = RunNativePreparedCubeMatrixOriginal(params, collector, original, error);
        retainOriginal(outputRoot, level, original, *collector);
        if (original.baseline_terminal_pixels.size() == kPlane && original.baseline_expected_pixels.size() == kPlane)
            compare(original.baseline_terminal_pixels, original.baseline_expected_pixels, "STOP: Original baseline/oracle");
        if (original.terminal_pixels.size() == kPlane && original.expected_pixels.size() == kPlane)
            compare(original.terminal_pixels, original.expected_pixels, "STOP: Original update/oracle");
        require(status == 0 && original.failure.empty() && collector->error.empty(), "STOP: cube Original/Prepared failure at LOD " + std::to_string(level) + ": " + error + collector->error);
        require(collector->sources.size() == 48 && collector->preparedOwners.size() == 2 && collector->preparedOwners.front().textureVersion != collector->preparedOwners.back().textureVersion, "cube callback did not create one fresh immutable owner per phase");
        guardUpdate(original, level);
        guardPreparedJoins(original);
        require(original.baseline_copy_id && original.completion_copy_id && original.baseline_copy_id != original.completion_copy_id && original.baseline_swap_id && original.completion_swap_id && original.baseline_swap_id != original.completion_swap_id, "cube Original completion lost distinct baseline/update copy/swap identities");
        for (unsigned phaseIndex = 0; phaseIndex < 2; ++phaseIndex)
        {
            const auto  sources     = std::span(collector->sources).subspan(phaseIndex * 24, 24);
            const auto& rawFixture  = phaseIndex ? original.source_fixture : requested;
            const auto  oracle      = CubeOraclePixels(rawFixture);
            auto        independent = controlTexture(rawFixture);
            const auto& owner       = collector->preparedOwners[phaseIndex];
            compare(owner.payloadBytes(), CubeOraclePayload(rawFixture), "cube immutable payload at LOD " + std::to_string(level));
            require(owner.swizzle == independent.swizzle && owner.textureVersion->Subresources().size() == 6 * kMipCount, "cube Prepared interpretation or face count changed");
            compare(phaseIndex ? original.terminal_pixels : original.baseline_terminal_pixels, oracle, "STOP: Original/oracle LOD " + std::to_string(level));
            const auto suffix = (phaseIndex ? "-" : "-baseline-") + std::to_string(level) + ".bin";
            const auto a      = replay(candidateRenderer, recipesFor(sources, candidate, owner, true), outputRoot / ("candidate" + suffix));
            const auto b      = replay(controlRenderer, recipesFor(sources, control, independent), outputRoot / ("control" + suffix));
            compare(a, duplicatePlane(oracle), "cube Prepared/oracle LOD " + std::to_string(level));
            compare(b, a, "cube control/Prepared parity");
            if (!phaseIndex)
                continue;
            const auto checkMutation = [&](const std::string& kind, const NativeDrawReplayTexture& changed, unsigned face, unsigned channel)
            {
                const auto key = kind + "-" + std::to_string(level) + "-" + std::to_string(face) + "-" + std::to_string(channel);
                const auto ma  = replay(candidateRenderer, recipesFor(sources, candidate, changed, true), outputRoot / ("mutated-" + key + "-candidate.bin"));
                const auto mb  = replay(controlRenderer, recipesFor(sources, control, changed), outputRoot / ("mutated-" + key + "-control.bin"));
                compare(ma, mb, "cube mutation routes " + key);
                if (cubeMatrixForm->format == 20)
                {
                    const auto expected = bc3MutationOracle(rawFixture, changed);
                    writeBytes(outputRoot / ("mutated-" + key + "-expected.bin"), expected);
                    compare(ma, duplicatePlane(expected), "BC3 endpoint mutation/oracle " + key);
                }
                sensitivePanel(ma, a, face, channel, "STOP: cube sensitivity " + key);
                return ma;
            };
            for (unsigned face = 0; face < 6; ++face)
            {
                for (unsigned channel = 0; channel < CubeMatrixDataChannels(); ++channel)
                {
                    const auto ma = checkMutation("channel", mutateChannel(owner, face, level, channel), face, channel);
                    for (unsigned other = 0; other < 24; ++other)
                        if (other != channel * 6 + face)
                            for (unsigned y = 0; y < cubeMatrixForm->width; ++y)
                                for (unsigned x = 0; x < cubeMatrixForm->width; ++x)
                                {
                                    const auto at = (std::size_t(other / 6 * cubeMatrixForm->width + y) * kOutputWidth + other % 6 * cubeMatrixForm->width + x) * 4;
                                    require(std::equal(ma.begin() + at, ma.begin() + at + 4, a.begin() + at), "isolated cube channel mutation changed another panel");
                                }
                    ++mutations;
                }
                auto ra = recipesFor(sources, candidate, owner, true), rb = recipesFor(sources, control, independent);
                perturbFace(ra, face, candidate.vertexAttributes);
                perturbFace(rb, face, control.vertexAttributes);
                const auto key = std::to_string(level) + "-" + std::to_string(face);
                const auto ma  = replay(candidateRenderer, std::move(ra), outputRoot / ("face-" + key + "-candidate.bin"));
                const auto mb  = replay(controlRenderer, std::move(rb), outputRoot / ("face-" + key + "-control.bin"));
                compare(ma, mb, "cube face perturbation routes");
                if (cubeMatrixForm->format == 20)
                {
                    auto perturbed = rawFixture;
                    for (unsigned channel = 0; channel < 4; ++channel)
                        for (unsigned vertex = 0; vertex < 3; ++vertex)
                        {
                            const auto local     = std::array<std::array<float, 2>, 3>{ { { 0, 0 }, { 0, float(2 * cubeMatrixForm->width) }, { float(2 * cubeMatrixForm->width), 0 } } }[vertex];
                            const auto direction = CubeMatrixDirection((face + 1) % 6, (local[0] + .25F) / cubeMatrixForm->width, (local[1] + .25F) / cubeMatrixForm->width);
                            for (unsigned lane = 0; lane < 3; ++lane)
                                setBigEndianFloat(perturbed.vertex_bytes, ((channel * 6 + face) * 3 + vertex) * perturbed.vertex_stride + 16 + lane * 4, direction[lane]);
                        }
                    const auto expected = CubeOraclePixels(perturbed);
                    writeBytes(outputRoot / ("face-" + key + "-expected.bin"), expected);
                    compare(ma, duplicatePlane(expected), "BC3 face mutation/oracle");
                }
                for (unsigned channel = 0; channel < CubeMatrixDataChannels(); ++channel)
                    sensitivePanel(ma, a, face, channel, "STOP: cube face perturbation");
                ++facePerturbations;
                std::vector<std::span<const std::uint8_t>> incomplete;
                const auto                                 payload = owner.payloadBytes();
                for (const auto& span : owner.textureVersion->Subresources())
                    incomplete.emplace_back(payload.data() + span.sourceOffset, span.rowBytes * span.rowCount);
                incomplete[face * kMipCount + level] = {};
                NativeDrawReplayTexture rejected;
                require(!PackNativeDrawReplayTextureVersion(owner.textureVersion->Layout(), incomplete, rejected, error) && !error.empty(), "cube face/level omission reached packing");
                ++spansOmitted;
            }
            recipeFromSource(sources[0], candidate, nullptr);
            ++omissions;
            const auto                                 wrongPayload = owner.payloadBytes();
            std::vector<std::span<const std::uint8_t>> firstFace;
            for (unsigned mip = 0; mip < kMipCount; ++mip)
            {
                const auto& sub = owner.textureVersion->Subresources()[mip];
                firstFace.emplace_back(wrongPayload.data() + sub.sourceOffset, sub.rowBytes * sub.rowCount);
            }
            auto twoDimensional      = owner.textureVersion->Layout();
            twoDimensional.dimension = NativeDrawReplayTextureDimension::Texture2D;
            NativeDrawReplayTexture wrongLayout;
            require(PackNativeDrawReplayTextureVersion(twoDimensional, firstFace, wrongLayout, error), error);
            wrongLayout.dimension    = NativeDrawReplayTextureDimension::TextureCube;
            wrongLayout.swizzle      = owner.swizzle;
            auto dynamic             = candidate;
            dynamic.vertexShaderHash = sources[0].event.draw.guest_vertex_shader_hash;
            dynamic.pixelShaderHash  = sources[0].event.draw.guest_pixel_shader_hash;
            const std::vector<NativeGameplayShaderInterface> oneShader{ dynamic };
            NativeGameplayCommandFeed                        badFeed(oneShader);
            auto                                             event = sources[0].event;
            const auto                                       use   = std::ranges::find_if(event.draw.texture_uses, [](const auto& u)
                                                                                          {
                                                      return u.required;
                                                                                          });
            std::map<std::uint32_t, NativeDrawReplayTexture> wrongCpu{ { use->texture_index, wrongLayout } };
            require(badFeed.Begin(event.frame, event.submission, {}, error) &&
                        !badFeed.ConsumePreparedDraw(event, sources[0].registers, sources[0].textures, wrongCpu, error),
                    "wrong immutable cube version layout reached command feed");
            if (level == 0 && cubeMatrixForm->format == 20)
            {
                // Whole-payload selector rewrites test selector decoding and in-block texel
                // order against the oracle; codes stay 0/1, so its endpoint preconditions hold.
                const auto rewrite = [&](auto&& block)
                {
                    auto bytes = std::vector<std::uint8_t>(owner.payloadBytes().begin(), owner.payloadBytes().end());
                    for (std::size_t at = 0; at < bytes.size(); at += 16)
                        block(bytes.data() + at);
                    return repack(owner, std::move(bytes));
                };
                checkMutation("color-selector-complement", rewrite([](std::uint8_t* b)
                                                                   {
                                                                       for (unsigned i = 12; i < 16; ++i)
                                                                           b[i] ^= 0x55;
                                                                   }),
                              0,
                              0);
                ++semanticControls;
                checkMutation("alpha-selector-complement", rewrite([](std::uint8_t* b)
                                                                   {
                                                                       for (unsigned texel = 0; texel < 16; ++texel)
                                                                           b[2 + 3 * texel / 8] ^= std::uint8_t(1U << (3 * texel % 8));
                                                                   }),
                              0,
                              3);
                ++semanticControls;
                checkMutation("selector-transpose", rewrite([](std::uint8_t* b)
                                                            {
                                                                std::uint64_t alpha = 0, alphaOut = 0;
                                                                std::uint32_t color = 0, colorOut = 0;
                                                                for (unsigned i = 0; i < 6; ++i)
                                                                    alpha |= std::uint64_t(b[2 + i]) << (8 * i);
                                                                for (unsigned i = 0; i < 4; ++i)
                                                                    color |= std::uint32_t(b[12 + i]) << (8 * i);
                                                                for (unsigned y = 0; y < 4; ++y)
                                                                    for (unsigned x = 0; x < 4; ++x)
                                                                    {
                                                                        alphaOut |= ((alpha >> (3 * (y * 4 + x))) & 7) << (3 * (x * 4 + y));
                                                                        colorOut |= ((color >> (2 * (y * 4 + x))) & 3) << (2 * (x * 4 + y));
                                                                    }
                                                                for (unsigned i = 0; i < 6; ++i)
                                                                    b[2 + i] = std::uint8_t(alphaOut >> (8 * i));
                                                                for (unsigned i = 0; i < 4; ++i)
                                                                    b[12 + i] = std::uint8_t(colorOut >> (8 * i));
                                                            }),
                              0,
                              0);
                ++semanticControls;
            }
            if (level == 0 && cubeMatrixForm->format != 20)
            {
                // Distinct semantic controls use complete payloads on both routes.
                auto bytes = std::vector<std::uint8_t>(owner.payloadBytes().begin(), owner.payloadBytes().end());
                for (std::size_t at = 0; at < bytes.size(); at += cubeMatrixForm->bytes_per_texel)
                    if (cubeMatrixForm->format == 6)
                    {
                        std::swap(bytes[at], bytes[at + 2]);
                        std::swap(bytes[at + 1], bytes[at + 3]);
                    }
                    else
                    {
                        std::swap(bytes[at], bytes[at + 2]);
                        std::swap(bytes[at + 1], bytes[at + 3]);
                        std::swap(bytes[at + 4], bytes[at + 6]);
                        std::swap(bytes[at + 5], bytes[at + 7]);
                    }
                checkMutation("wrong-endian", repack(owner, std::move(bytes)), 0, 0);
                ++semanticControls;
                if (cubeMatrixForm->format == 6)
                {
                    auto noSwizzle    = owner;
                    noSwizzle.swizzle = { 0, 1, 2, 3 };
                    checkMutation("no-swizzle", noSwizzle, 0, 0);
                    ++semanticControls;
                    auto twiceBytes = std::vector<std::uint8_t>(independent.payloadBytes().begin(), independent.payloadBytes().end());
                    for (std::size_t at = 0; at < twiceBytes.size(); at += 4)
                        std::swap(twiceBytes[at], twiceBytes[at + 2]);
                    auto twice = repack(independent, std::move(twiceBytes));
                    checkMutation("double-swizzle", twice, 0, 0);
                    ++semanticControls;
                }
                else
                    for (unsigned clamp = 0; clamp < 2; ++clamp)
                    {
                        auto bytes = std::vector<std::uint8_t>(owner.payloadBytes().begin(), owner.payloadBytes().end());
                        for (std::size_t at = 0; at < bytes.size(); at += 2)
                        {
                            const auto bits        = std::uint16_t(bytes[at] | std::uint16_t(bytes[at + 1]) << 8);
                            const auto v           = CubeOracleHalf(bits);
                            const auto replacement = v < 0 ? std::uint16_t(0) : clamp && v > 1 ? std::uint16_t(0x3C00)
                                                                                               : bits;
                            bytes[at]              = std::uint8_t(replacement);
                            bytes[at + 1]          = std::uint8_t(replacement >> 8);
                        }
                        checkMutation(clamp ? "clamp-unit" : "clamp-negative", repack(owner, std::move(bytes)), 0, 0);
                        ++semanticControls;
                    }
            }
        }
        if (cubeMatrixForm->format == 6)
        {
            const auto alphaRoot = outputRoot / ("alpha-invariance-" + std::to_string(level));
            std::filesystem::create_directories(alphaRoot);
            auto alphaCollector = std::make_shared<PreparedCollector>(candidate);
            // The baseline run consumed params.fixture.
            params.fixture     = requested;
            params.update_kind = NativePreparedCubeMatrixUpdateKind::kGuestAlphaOnly;
            NativePreparedCubeMatrixOriginalResult alphaOriginal;
            const auto                             alphaStatus = RunNativePreparedCubeMatrixOriginal(params, alphaCollector, alphaOriginal, error);
            retainOriginal(alphaRoot, level, alphaOriginal, *alphaCollector);
            require(alphaStatus == 0 && alphaOriginal.failure.empty() && alphaCollector->error.empty(),
                    "STOP: cube guest alpha Original/Prepared failure: " + error + alphaCollector->error);
            require(alphaCollector->sources.size() == 48 && alphaCollector->preparedOwners.size() == 2 &&
                        alphaCollector->preparedOwners.front().textureVersion != alphaCollector->preparedOwners.back().textureVersion,
                    "cube alpha update did not create a fresh Prepared owner");
            guardUpdate(alphaOriginal, level);
            guardPreparedJoins(alphaOriginal);
            const auto expected = CubeOraclePixels(requested);
            compare(CubeOraclePixels(alphaOriginal.source_fixture), expected, "cube guest alpha oracle invariance");
            compare(alphaOriginal.baseline_terminal_pixels, expected, "cube guest alpha Original baseline");
            compare(alphaOriginal.terminal_pixels, expected, "cube guest alpha Original invariance");
            for (unsigned phase = 0; phase < 2; ++phase)
            {
                const auto  sources    = std::span(alphaCollector->sources).subspan(phase * 24, 24);
                const auto& rawFixture = phase ? alphaOriginal.source_fixture : requested;
                const auto& owner      = alphaCollector->preparedOwners[phase];
                compare(owner.payloadBytes(), CubeOraclePayload(rawFixture), "cube guest alpha retained payload");
                const auto independent = controlTexture(rawFixture);
                const auto suffix      = phase ? "-updated.bin" : "-baseline.bin";
                const auto a           = replay(candidateRenderer, recipesFor(sources, candidate, owner, true), alphaRoot / (std::string("candidate") + suffix));
                const auto b           = replay(controlRenderer, recipesFor(sources, control, independent), alphaRoot / (std::string("control") + suffix));
                compare(a, duplicatePlane(expected), "cube guest alpha Prepared invariance");
                compare(b, a, "cube guest alpha control invariance");
            }
            ++alphaInvarianceControls;
        }
        if (level == 0)
        {
            const auto& source   = collector->sources[24];
            pairSources[0]       = { original.source_fixture, source.registers, source.event.draw.draw.vertex_fetches.front().base, collector->preparedOwners.back() };
            deviceFailureTexture = collector->preparedOwners.front();
        }
    }
    const auto& firstSource = pairSources.front();
    if (cubeMatrixForm->format == 20)
        for (unsigned level = 0; level < kMipCount; ++level)
            for (int mode = 0; mode < 2; ++mode)
            {
                cubeMatrixInterpolationMode = mode;
                currentCubeLod              = level;
                const auto root             = outputRoot / ("interpolated-" + std::to_string(level) + "-" + std::to_string(mode));
                std::filesystem::create_directories(root);
                auto                                   requested = fixtureForLod(level);
                auto                                   collector = std::make_shared<PreparedCollector>(candidate);
                NativePreparedCubeMatrixOriginalParams params;
                params.fixture   = requested;
                params.first_lod = params.second_lod = level;
                NativePreparedCubeMatrixOriginalResult original;
                const auto                             status = RunNativePreparedCubeMatrixOriginal(params, collector, original, error);
                retainOriginal(root, level, original, *collector);
                require(status == 0 && original.failure.empty() && collector->error.empty(), "BC3 interpolated Original/Prepared failed: " + error + collector->error);
                require(original.original_adapter_identity_available && collector->sources.size() == 48 && collector->preparedOwners.size() == 2 &&
                            original.expected_pixels.empty() && original.baseline_expected_pixels.empty(),
                        "BC3 interpolated case lost its device-only boundary");
                const auto nativeLuid = [](NativeRendererD3D12& renderer)
                {
                    Microsoft::WRL::ComPtr<ID3D12Device> device;
                    auto*                                queue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(renderer);
                    require(queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))), "BC3 parity device unavailable");
                    return device->GetAdapterLuid();
                };
                const LUID originalLuid{ original.original_adapter_luid_low, original.original_adapter_luid_high };
                const auto preparedLuid = nativeLuid(candidateRenderer), controlLuid = nativeLuid(controlRenderer);
                require(sameLuid(originalLuid, preparedLuid) && sameLuid(originalLuid, controlLuid), "BC3 interpolated routes used different physical devices");
                unsigned alphaMask = 0, colorMask = 0;
                for (unsigned phase = 0; phase < 2; ++phase)
                {
                    const auto  sources     = std::span(collector->sources).subspan(phase * 24, 24);
                    const auto& raw         = phase ? original.source_fixture : requested;
                    const auto  independent = controlTexture(raw);
                    const auto& owner       = collector->preparedOwners[phase];
                    compare(owner.payloadBytes(), CubeOraclePayload(raw), "BC3 interpolated raw storage");
                    const auto name = phase ? "updated" : "baseline";
                    const auto a    = replay(candidateRenderer, recipesFor(sources, candidate, owner, true), root / (std::string("candidate-") + name + ".bin"));
                    const auto b    = replay(controlRenderer, recipesFor(sources, control, independent), root / (std::string("control-") + name + ".bin"));
                    compare(a, duplicatePlane(phase ? original.terminal_pixels : original.baseline_terminal_pixels), "BC3 interpolated Prepared/Original");
                    compare(b, a, "BC3 interpolated control/Prepared");
                    for (unsigned face = 0; face < 6; ++face)
                        for (unsigned y = 0; y < cubeMatrixForm->width; ++y)
                            for (unsigned x = 0; x < cubeMatrixForm->width; ++x)
                            {
                                const auto    p      = CubeOracleSample(raw, face, x, y, 0, 1.5 / (1024 * cubeMatrixForm->width));
                                const auto&   bank   = level ? raw.texture.mip_guest_bytes : raw.texture.base_guest_bytes;
                                const auto    at     = CubeMatrixAddress(*cubeMatrixForm, face, level, p.x / 4, p.y / 4);
                                const auto    texel  = (p.y % 4) * 4 + p.x % 4;
                                std::uint64_t alpha  = 0;
                                unsigned      colors = 0;
                                for (unsigned i = 0; i < 6; ++i)
                                    alpha |= std::uint64_t(bank[at + ((i + 2) ^ 1U)]) << (8 * i);
                                for (unsigned i = 0; i < 4; ++i)
                                    colors |= unsigned(bank[at + ((i + 12) ^ 1U)]) << (8 * i);
                                const auto a0 = bank[at + 1], a1 = bank[at];
                                require(mode == 0 ? a0 > a1 : a0 < a1, "BC3 interpolated alpha mode changed");
                                alphaMask |= 1U << ((alpha >> (3 * texel)) & 7);
                                colorMask |= 1U << ((colors >> (2 * texel)) & 3);
                            }
                }
                require(alphaMask == 255 && colorMask == 15, "BC3 interpolated panels did not sample every selector code");
                guardUpdate(original, level);
                guardPreparedJoins(original);
                std::ofstream receipt(root / "parity.toml");
                receipt << "schema_version = 1\nparity_kind = \"device_interpolated\"\ninterpolated_oracle_claim = false\nalpha_mode = " << tomlString(mode == 0 ? "a0_gt_a1" : "a0_lt_a1")
                        << "\nsampled_lod = " << level << "\nalpha_codes = [0,1,2,3,4,5,6,7]\ncolor_codes = [0,1,2,3]\npixel_tolerance = 0\nhalf_pixel_offset = true\nuv_bias_base_texels = 0.25\npixel_center_conventions_match = true\nsame_device = true\noriginal_control_prepared_equal = true\n"
                        << "original_luid_low = " << originalLuid.LowPart << "\noriginal_luid_high = " << originalLuid.HighPart
                        << "\ncontrol_luid_low = " << controlLuid.LowPart << "\ncontrol_luid_high = " << controlLuid.HighPart
                        << "\nprepared_luid_low = " << preparedLuid.LowPart << "\nprepared_luid_high = " << preparedLuid.HighPart << "\n";
                require(receipt.good(), "BC3 interpolation receipt failed");
                cubeMatrixInterpolationMode = -1;
            }

    candidateRenderer.Shutdown();
    controlRenderer.Shutdown();
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
                require(fence && SUCCEEDED(fence->Signal(1)), "cube lifetime gate signal");
                released = true;
            }
        };

        auto provider = rex::ui::d3d12::D3D12Provider::Create();
        require(bool(provider), "cube lifetime proof provider");
        mainAdapterIdentity = describeProviderAdapter(*provider);
        auto presenter      = rex::ui::d3d12::D3D12Presenter::Create([](bool, bool)
                                                                     {
                                                                     },
                                                                     *provider);
        require(bool(presenter), "cube lifetime proof presenter");
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
                "cube native completion gate allocation");
        require(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyGate.fence))),
                "cube SDK copy gate allocation");
        auto* lifetimeQueue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(lifetimeRenderer);
        require(lifetimeQueue && SUCCEEDED(lifetimeQueue->Wait(nativeGate.fence.Get(), 1)),
                "cube native queue gate installation");
        std::shared_ptr<NativeRenderFrameTicket> lifetimeTicket;
        require(lifetimeRenderer.QueueRenderFrame(std::move(lifetimeFrame), lifetimeTicket, lifetimeError), lifetimeError);
        lifetimeOwner         = {};
        lifetimeOwnerReleased = true;
        require(lifetimeOwnerWeak.use_count() != 0U, "cube actual prepared owner retired before native completion");
        require(lifetimeTicket->Recording().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "cube Recording did not become ready behind the native gate");
        const auto recorded = lifetimeTicket->Recording().get();
        require(recorded.success && !published &&
                    lifetimeTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "cube Recording did not remain distinct from native completion");
        lifetimeRecordingReady = true;
        nativeGate.Release();
        require(lifetimeTicket->Completion().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "cube native completion did not retire after the real queue gate");
        const auto completed = lifetimeTicket->Completion().get();
        require(completed.success && completed.completionFenceValue && published && completed.gpuOutputTicket == published,
                completed.error);
        lifetimeOwnerHeldThroughCompletion = !lifetimeOwnerWeak.expired();
        require(lifetimeOwnerHeldThroughCompletion,
                "cube actual prepared owner retired before native completion result was consumed");
        lifetimeNativeCompletion = true;
        auto* sdkQueue           = rex::ui::d3d12::detail::GpuGuestOutputTestAccess::CopyQueue(*presenter);
        require(sdkQueue && SUCCEEDED(sdkQueue->Wait(copyGate.fence.Get(), 1)),
                "cube SDK consumer copy gate installation");
        Microsoft::WRL::ComPtr<ID3D12Resource> copied;
        require(presenter->RefreshGuestOutput(kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight, [&](rex::ui::Presenter::GuestOutputRefreshContext& context)
                                              {
                                                  copied = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
                                                  context.SetIs8bpc(true);
                                                  return true;
                                              }),
                "cube SDK consumer copy submission");
        std::uint64_t sdkCopyFence = 0;
        require(NativeRendererD3D12::PollGpuOutput(published, &sdkCopyFence) == NativeRendererD3D12::GpuOutputStatus::Pending &&
                    !sdkCopyFence,
                "cube native completion did not stay distinct from SDK copy retirement");
        copyGate.Release();
        require(NativeRendererD3D12::AwaitGpuOutput(published, lifetimeError, &sdkCopyFence) && sdkCopyFence,
                lifetimeError);
        require(copied != nullptr, "cube SDK copy retirement did not retain its consumer resource");
        lifetimeSdkCopyRetired = true;
        lifetimeRenderer.Shutdown();
    }
    lifetimeOwnerRetiredAfterDrain = lifetimeOwnerWeak.expired();
    require(lifetimeOwnerRetiredAfterDrain, "cube actual prepared owner was not retired after native and SDK drains");

    // Exercise a real pending cube owner on a dedicated removable device. The
    // device fault must fail the native completion before output publication,
    // then normal shutdown must release the actual frame owner.
    {
        auto warp = createDedicatedWarpDevice();
        require(mainAdapterIdentity.canonical && warp.identity.canonical,
                "cube device-removal adapter identities were unavailable");
        require(!sameLuid(mainAdapterIdentity.luid, warp.identity.luid),
                "cube device-removal proof requires a WARP adapter distinct from the main provider");
        require(mainAdapterIdentity.canonical.Get() != warp.identity.canonical.Get(),
                "cube device-removal proof requires distinct canonical adapter identities");
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
                "cube device-removal native queue gate");

        auto                                                deviceFailureOwner     = std::move(deviceFailureTexture);
        std::weak_ptr<const NativeDrawReplayTextureVersion> deviceFailureOwnerWeak = deviceFailureOwner.textureVersion;

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
        deviceFailurePass.draws.push_back(buildRecipe(firstSource.fixture,
                                                      candidate,
                                                      {},
                                                      std::move(deviceFailureOwner),
                                                      kMipCount,
                                                      firstSource.registers,
                                                      firstSource.vertexGuestBase,
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
        require(!deviceFailureOwnerWeak.expired(), "cube device-removal owner retired before native recording");
        require(deviceFailureTicket->Recording().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "cube device-removal Recording did not become ready behind the native gate");
        const auto deviceFailureRecorded = deviceFailureTicket->Recording().get();
        require(deviceFailureRecorded.success &&
                    deviceFailureTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
                "cube device-removal Recording did not remain distinct from native completion");
        deviceFailureRecordingReady        = true;
        deviceFailureOwnerHeldBeforeRemove = !deviceFailureOwnerWeak.expired();
        require(deviceFailureOwnerHeldBeforeRemove,
                "cube device-removal owner was not held before removing the device");

        Microsoft::WRL::ComPtr<ID3D12Device5> removedDevice;
        require(SUCCEEDED(warp.device->QueryInterface(IID_PPV_ARGS(&removedDevice))),
                "cube device-removal ID3D12Device5 query");
        removedDevice->RemoveDevice();
        require(FAILED(warp.device->GetDeviceRemovedReason()),
                "cube device-removal device did not enter the removed state");
        require(deviceFailureTicket->Completion().wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "cube device-removal completion did not fail after RemoveDevice");
        const auto deviceFailureCompleted = deviceFailureTicket->Completion().get();
        deviceFailureError                = deviceFailureCompleted.error;
        require(!deviceFailureCompleted.success && !deviceFailureCompleted.completionFenceValue &&
                    !deviceFailureCompleted.presenterToken && !deviceFailureCompleted.gpuOutputTicket &&
                    deviceFailureCompleted.resolvedOutput.empty() && !deviceFailureCompleted.error.empty() &&
                    publicationCalls.load(std::memory_order_relaxed) == 0U,
                "cube device-removal completion published output or lost its failure reason");
        deviceFailureCompletionFailed = true;
        removedRenderer.Shutdown();
        deviceFailureOwnerRetiredAfterStop = deviceFailureOwnerWeak.expired();
        require(deviceFailureOwnerRetiredAfterStop,
                "cube device-removal owner did not retire after renderer shutdown");
        deviceFailurePublications = publicationCalls.load(std::memory_order_relaxed);
    }
    require(deviceFailureRecordingReady && deviceFailureCompletionFailed && deviceFailureOwnerHeldBeforeRemove &&
                deviceFailureOwnerRetiredAfterStop && deviceFailurePublications == 0U,
            "cube device-removal lifetime proof was incomplete");

    std::ofstream report(outputRoot / "result.toml");
    report << std::boolalpha << "schema_version = 2\nform = " << tomlString(cubeMatrixForm->name) << "\nscene_ids = " << tomlString(cubeMatrixForm->scene_ids) << "\nwidth = " << cubeMatrixForm->width << "\nheight = " << cubeMatrixForm->width << "\nfaces = 6\nchannels = 4\ndata_channels = " << CubeMatrixDataChannels() << "\nmip_levels = " << kMipCount << "\nfetch_pitch = " << cubeMatrixForm->pitch << "\nprepared_format_value = " << cubeMatrixForm->format << "\nprepared_endianness_value = " << cubeMatrixForm->endian << "\nprepared_swizzle = " << cubeMatrixForm->swizzle << "\nprepared_swizzled_signs = " << cubeMatrixForm->signs << "\nprepared_base_extent = " << cubeMatrixForm->base_extent << "\nprepared_mip_extent = " << cubeMatrixForm->mip_extent << "\nprepared_depth = 6\nprepared_mip_min = 0\nprepared_mip_max = " << kMipCount - 1 << "\nprepared_packed_mips = true\nprepared_tiled = false\npixel_tolerance = 0\nhalf_pixel_offset = true\ncompleted_lods = " << kMipCount << "\ncoverage_groups = " << 6 * kMipCount * CubeMatrixDataChannels() << "\nalpha_invariance_count = " << alphaInvarianceControls << "\nnative_mutation_count = " << mutations << "\nface_perturbation_count = " << facePerturbations << "\nomission_count = " << omissions << "\nface_level_omission_count = " << spansOmitted << "\nsemantic_controls = " << semanticControls << "\noutput_planes = 2\nprepared_collector = true\nprepared_command_feed = true\nindependent_raw_oracle = true\nall_levels_decoded = true\nall_channels_compared = true\nraw_old_new_copies = true\nfresh_texture_version_owner = true\nomissions_before_submission = true\nlifetime_recording_ready = " << lifetimeRecordingReady << "\nlifetime_native_completion = " << lifetimeNativeCompletion << "\nlifetime_sdk_copy_retired = " << lifetimeSdkCopyRetired << "\nlifetime_owner_released = " << lifetimeOwnerReleased << "\nlifetime_owner_held_through_completion = " << lifetimeOwnerHeldThroughCompletion << "\nlifetime_owner_retired_after_drain = " << lifetimeOwnerRetiredAfterDrain << "\ndevice_failure_recording_ready = " << deviceFailureRecordingReady << "\ndevice_failure_completion_failed = " << deviceFailureCompletionFailed << "\ndevice_failure_owner_held_before_remove = " << deviceFailureOwnerHeldBeforeRemove << "\ndevice_failure_owner_retired_after_shutdown = " << deviceFailureOwnerRetiredAfterStop << "\ndevice_failure_publications = " << deviceFailurePublications << "\n";
    report << "interpolated_oracle_claim = false\ninterpolated_parity_cases = " << (cubeMatrixForm->format == 20 ? 2 * kMipCount : 0) << "\n";
    if (cubeMatrixForm->format == 20)
        report << "parity_kind = \"endpoint_oracle\"\nendpoint_alpha_codes = [0,1]\nendpoint_color_codes = [0,1]\ncolor_mode = \"c0_gt_c1\"\n";
    require(report.good(), "cube result receipt write failed");
    return 0;
}

} // namespace

static void fixtureGuards()
{
    for (const auto& form : kCubeMatrixForms)
    {
        SelectCubeMatrixForm(form);
        guardFixture(MakeNativePreparedCubeMatrixFixture());
    }
    std::cout << "Three exact cube fixture guards passed\n";
}

static int preparedMain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--emit-fixture")
        {
            SelectCubeMatrixForm(kCubeMatrixForms[0]);
            emitFixture(argv[2]);
            return 0;
        }
        if (argc != 9 || std::wstring_view(argv[1]) != L"--candidate-catalog" || std::wstring_view(argv[3]) != L"--control-catalog" || std::wstring_view(argv[5]) != L"--candidate-source" || std::wstring_view(argv[7]) != L"--output")
            fail("usage: rerevved_prepared_tests --helper native-prepared-cube-matrix.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>");
        const std::filesystem::path output(argv[8]);
        require(!std::filesystem::exists(output), "cube matrix output must be fresh");
        std::filesystem::create_directories(output);
        HarnessLogging logging;
        std::ofstream  matrix(output / "matrix.toml");
        matrix << "schema_version = 2\nform_count = 3\npixel_tolerance = 0\nhalf_pixel_offset = true\n";
        for (const auto& form : kCubeMatrixForms)
        {
            SelectCubeMatrixForm(form);
            require(runForm(argv[2], argv[4], argv[6], output / form.name) == 0, "cube form failed");
            matrix << "[[forms]]\nname = " << tomlString(form.name) << "\nresult = " << tomlString(std::string(form.name) + "/result.toml") << "\n";
            matrix.flush();
            require(matrix.good(), "cube matrix receipt failed");
        }
        matrix << "[completion]\ncompleted_forms = 3\n";
        require(matrix.good(), "cube matrix receipt failed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "native_prepared_cube_matrix_tests: " << error.what() << '\n';
        return 1;
    }
}

// native-prepared-cube-matrix.emit --emit-fixture <fresh-dir>: writes the fixture inputs.
REREVVED_HELPER_COMMAND(nativePreparedCubeMatrixEmit, "native-prepared-cube-matrix.emit")
{
    rerevved::tests::NativeArgv argv(args);
    if (argv.argc() != 3 || std::wstring_view(argv.argv()[1]) != L"--emit-fixture")
    {
        std::cerr << "usage: rerevved_prepared_tests --helper native-prepared-cube-matrix.emit --emit-fixture <fresh-dir>" << std::endl;
        return 2;
    }
    return preparedMain(argv.argc(), argv.argv());
}

// native-prepared-cube-matrix.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>:
// the GPU proof.
REREVVED_HELPER_COMMAND(nativePreparedCubeMatrixRun, "native-prepared-cube-matrix.run")
{
    rerevved::tests::NativeArgv argv(args);
    if (argv.argc() != 9)
    {
        std::cerr << "usage: rerevved_prepared_tests --helper native-prepared-cube-matrix.run --candidate-catalog <dir> --control-catalog <dir> --candidate-source <file> --output <fresh-dir>" << std::endl;
        return 2;
    }
    return preparedMain(argv.argc(), argv.argv());
}

TEST_CASE("native_prepared_cube_matrix_fixture", "[ctest][native-prepared-cube-matrix]")
{
    UNSCOPED_INFO("the fixture guards hold");
    REQUIRE_NOTHROW(fixtureGuards());
}
