#include "callback_checks.hpp"
#include "developer_checks.hpp"
#include "gpu/d3d12/native_completion_source.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/native_frame_assembler.h"
#include "gpu/guest_d3d/resolve_surface_hooks.h"
#include "gpu/present/native_output_lease.h"
#include "logging_scope.hpp"
#include "native_render_frame_test_access.h"
#include "stream_queue_fixture.h"

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

using namespace rerevved::gpu;
using namespace rerevved::gpu::guest_d3d;

namespace
{

constexpr std::size_t kPlaneBytes = 1280U * 720U * 4U;

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    UNSCOPED_INFO("cannot open " + path.string());
    REQUIRE(bool(file));
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

// The pinned catalog entry specialization-0002 is not kept locally; the
// title loads the panel interface from the capture as the app does.
NativeGameplayShaderInterface solidShader(const std::filesystem::path& capture)
{
    NativeGameplayShaderInterface shader;
    std::string                   error;
    const bool                    loaded = LoadSolidPanelShader(capture, shader, error);
    UNSCOPED_INFO("panel shader from the capture: " + error);
    REQUIRE(loaded);
    return shader;
}

// The renderer stream as the completion source drives it, keeping each
// terminal completion so the test can read its output back.
struct RendererProducer final : INativeFrameProducer
{
    NativeRendererD3D12&                                    renderer;
    ID3D12Fence*                                            fence = nullptr;
    std::vector<std::shared_future<NativeDrawReplayResult>> terminals, terminalRecordings, prefixRecordings;
    std::size_t                                             prefixes        = 0;
    bool                                                    inspectSurfaces = false;
    bool                                                    inspectGraph    = false;
    std::vector<NativeRenderFrame>                          prefixFrames;
    std::vector<std::shared_ptr<NativeRenderFrameTicket>>   tickets;
    std::vector<std::vector<std::uint64_t>>                 released;
    std::uint64_t                                           captureOutput = 0;
    std::optional<NativeRenderFrame>                        capturedFrame;

    explicit RendererProducer(NativeRendererD3D12& value)
    : renderer(value)
    , fence(value.AcquireStreamFence())
    {
        UNSCOPED_INFO("the started stream exposes its fence");
        REQUIRE(fence != nullptr);
    }

    ~RendererProducer() override
    {
        fence->Release();
    }

    bool QueueFrame(NativeRenderFrame frame, std::shared_future<NativeDrawReplayResult>& completion, std::string& error) override
    {
        if (captureOutput && frame.outputTextureId == captureOutput)
        {
            capturedFrame = frame;
        }
        if (inspectSurfaces)
        {
            released.push_back(frame.releasedSurfaces);
        }
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        if (!renderer.QueueRenderFrame(std::move(frame), ticket, error))
            return false;
        completion = ticket->Completion();
        terminals.push_back(completion);
        terminalRecordings.push_back(ticket->Recording());
        if (inspectSurfaces)
        {
            tickets.push_back(ticket);
        }
        return true;
    }

    bool QueuePasses(NativeRenderFrame passes, std::shared_future<NativeDrawReplayResult>& recording, std::string& error) override
    {
        ++prefixes;
        if (inspectGraph)
        {
            prefixFrames.push_back(passes);
        }
        if (!renderer.QueueRenderFramePasses(std::move(passes), recording, error))
            return false;
        prefixRecordings.push_back(recording);
        return true;
    }

    std::uint64_t CompletedFence() const override
    {
        return fence->GetCompletedValue();
    }

    // As the app session waits: on the stream fence's completion event.
    std::uint64_t WaitFence(std::uint64_t value, std::chrono::steady_clock::time_point deadline) const override
    {
        return WaitStreamFence(fence, value, deadline);
    }
};

NativeFrameSwap swapRecord(std::uint32_t swizzle)
{
    NativeFrameSwap swap;
    swap.sequence            = 1;
    swap.frontbufferPhysical = 0x1F000000;
    swap.fetch               = { 2, 0x1F000000 | 6, 1279 | (719U << 13), swizzle, 0, 0 };
    swap.textureFormat       = 6;
    swap.width               = 1280;
    swap.height              = 720;
    // Identity 8-to-10-bit ramp packed as the presentation table expects.
    for (std::uint32_t i = 0; i < swap.gammaTable.size(); ++i)
    {
        const auto value   = (i * 1023U + 127U) / 255U;
        swap.gammaTable[i] = (value << 20) | (value << 10) | value;
    }
    return swap;
}

// The title's 1280x720 scanout view (RB_COPY_DEST_PITCH 0x02D00500) at
// swapRecord's frontbuffer.
constexpr std::uint32_t kFrontbuffer = 0x1F000000, kScanoutDestPitch = 0x02D00500;

// An untiled scanout of the presented target, unswapped so the swap fetch
// swizzle stands; a presenting swap needs one of its frontbuffer.
std::uint64_t tryScanOut(NativeFrameAssembler& assembler, std::string& error)
{
    GuestTarget main;
    main.native = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    GuestResolveCopy copy;
    copy.copyControl = 0x00100040;
    copy.destBase    = kFrontbuffer;
    copy.destPitch   = kScanoutDestPitch;
    copy.destInfo    = 0x300;
    copy.rectWidth   = 1280;
    copy.rectHeight  = 720;
    bool partial     = false;
    return assembler.Resolve(main, copy, true, partial, error);
}

void scanOut(NativeFrameAssembler& assembler)
{
    std::string error;
    UNSCOPED_INFO("the presented target is scanned out: " + error);
    REQUIRE(tryScanOut(assembler, error) != 0);
}

// The saved M1a native recipe is the expectation for the assembled draw.
void compareWithM1a(const NativeDrawReplayRecipe& draw, const std::filesystem::path& adapter)
{
    NativeDrawReplayRecipe saved;
    std::string            error;
    // Each message is built after its call so it carries that call's error.
    const bool loaded = LoadNativeDrawReplayRecipe(adapter / "native-recipe.toml", saved, error);
    UNSCOPED_INFO("saved M1a recipe: " + error);
    REQUIRE(loaded);
    std::shared_ptr<const NativeDrawReplayAdmission> savedAdmission, drawAdmission;
    const bool                                       admitted = AdmitNativeDrawReplayRecipe(saved, savedAdmission, error) && AdmitNativeDrawReplayRecipe(draw, drawAdmission, error);
    UNSCOPED_INFO("recipe admission: " + error);
    REQUIRE(admitted);
    const auto same = [](auto a, auto b)
    {
        return std::equal(a.begin(), a.end(), b.begin(), b.end());
    };
    UNSCOPED_INFO("pipeline state equals M1a");
    REQUIRE(savedAdmission->PipelineStateKey() == drawAdmission->PipelineStateKey());
    UNSCOPED_INFO("geometry equals M1a");
    REQUIRE((same(saved.vertexBytes(), draw.vertexBytes()) && saved.indices == draw.indices && saved.indexCount == draw.indexCount));
    UNSCOPED_INFO("shader bytes equal M1a");
    REQUIRE((same(saved.vertexShaderBytes(), draw.vertexShaderBytes()) && same(saved.pixelShaderBytes(), draw.pixelShaderBytes())));
    UNSCOPED_INFO("constants equal M1a");
    REQUIRE((saved.vertexConstants == draw.vertexConstants && saved.pixelConstants == draw.pixelConstants &&
             saved.sharedConstants == draw.sharedConstants));
    UNSCOPED_INFO("viewport and scissor equal M1a");
    REQUIRE((saved.viewport.x == draw.viewport.x && saved.viewport.y == draw.viewport.y && saved.viewport.width == draw.viewport.width &&
             saved.viewport.height == draw.viewport.height && saved.viewport.minDepth == draw.viewport.minDepth &&
             saved.viewport.maxDepth == draw.viewport.maxDepth && saved.scissor.left == draw.scissor.left &&
             saved.scissor.top == draw.scissor.top && saved.scissor.right == draw.scissor.right &&
             saved.scissor.bottom == draw.scissor.bottom));
    UNSCOPED_INFO("target equals M1a");
    REQUIRE((saved.width == draw.width && saved.height == draw.height && saved.sampleCount == draw.sampleCount &&
             saved.sampleMask == draw.sampleMask && saved.clearColor == draw.clearColor &&
             saved.vertexStrideBytes == draw.vertexStrideBytes));
}

// SDK e19035b src/graphics/pipeline/texture/util.cpp:424, the generic 2D
// tiled offset at 32 bpp, kept independent of the encoder's specialization.
std::uint32_t referenceTiledOffset(std::uint32_t x, std::uint32_t y, std::uint32_t pitch)
{
    constexpr unsigned logBpp = 2;
    pitch                     = (pitch + 31) & ~31U;
    const auto macro          = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (logBpp + 7);
    const auto micro          = ((x & 7) + ((y & 14) << 2)) << logBpp;
    const auto offset         = macro + ((micro & ~15U) << 1) + (micro & 15) + ((y & 1) << 4);
    return ((offset & ~511U) << 3) + ((y & 16) << 7) + ((offset & 448) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 63);
}

// An opted-in GPU case skips on a runner without a D3D12 device.
bool hasD3D12Device()
{
    return SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
}

// 1024x1024 raw clear readback exposes channel order, endian, and tiles.
void checkRawResolve()
{
    constexpr std::array<std::uint8_t, 4> background{ 255, 51, 153, 0 }, patch{ 0, 102, 204, 255 };
    constexpr NativeDrawReplayScissor     patchRect{ 40, 8, 72, 24 };
    const auto                            inside = [&](std::uint32_t x, std::uint32_t y)
    {
        return std::int32_t(x) >= patchRect.left && std::int32_t(x) < patchRect.right && std::int32_t(y) >= patchRect.top &&
               std::int32_t(y) < patchRect.bottom;
    };
    NativeRenderFrame frame;
    frame.textures.push_back({ 1, 1024, 1024, NativeDrawReplayTextureFormat::Rgba8, 1 });
    NativeRenderFramePass pass;
    pass.target = { 1024, 1024, 1, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear clear;
    clear.rectangle = { 0, 0, 1024, 1024 };
    clear.color = clear.depth = clear.stencil = true;
    // Each value times 255 lies within float error of an integer, so UNORM
    // conversion has no rounding tie.
    clear.colorValue = { 1.0F, 0.2F, 0.6F, 0.0F };
    pass.clears.push_back(clear);
    NativeRenderFramePass::Clear inner;
    inner.rectangle  = patchRect;
    inner.color      = true;
    inner.colorValue = { 0.0F, 0.4F, 0.8F, 1.0F };
    pass.clears.push_back(inner);
    NativeRenderFrameResolve resolve;
    resolve.textureId = 1;
    resolve.width = resolve.height = 1024;
    pass.resolves.push_back(resolve);
    frame.passes.push_back(std::move(pass));
    frame.outputTextureId = 1;
    frame.retainOnly = frame.returnRawOutput = true;
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO("raw resolve stream: " + error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto raw = renderer.SubmitRenderFrame(std::move(frame));
    renderer.Shutdown();
    UNSCOPED_INFO("raw output is one 1024x1024 RGBA8 image: " + raw.error);
    REQUIRE((raw.success && raw.rawRowPitch == 4096 && raw.rawOutput.size() == 4194304));
    std::size_t hostMismatches = 0;
    for (std::uint32_t y = 0; y < 1024; ++y)
        for (std::uint32_t x = 0; x < 1024; ++x)
        {
            const auto& rgba = inside(x, y) ? patch : background;
            hostMismatches += !std::equal(rgba.begin(), rgba.end(), raw.rawOutput.begin() + y * 4096 + x * 4);
        }
    UNSCOPED_INFO("raw readback differs from the clears at " + std::to_string(hostMismatches) + " texels");
    REQUIRE(hostMismatches == 0);

    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destPitch   = 0x04000400;
    copy.destInfo    = 0x01000300;
    copy.destFetch   = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    copy.rectWidth = copy.rectHeight = 1024;
    GuestResolveImage image;
    UNSCOPED_INFO("native clear encodes to a four MiB guest image: " + error);
    REQUIRE((EncodeGuestResolve(copy, raw.rawOutput, raw.rawRowPitch, image, error) && image.physical == 0x1E3B0000 && image.bytes.size() == 4194304));
    // DEST_INFO 0x01000300: k_8_8_8_8, endian 0 and red/blue swap, so each
    // guest word holds B, G, R, A in byte order (encoder test oracle).
    std::size_t guestMismatches = 0;
    for (std::uint32_t y = 0; y < 1024; ++y)
        for (std::uint32_t x = 0; x < 1024; ++x)
        {
            const auto& rgba   = inside(x, y) ? patch : background;
            const auto  offset = referenceTiledOffset(x, y, 1024);
            guestMismatches += image.bytes[offset] != rgba[2] || image.bytes[offset + 1] != rgba[1] || image.bytes[offset + 2] != rgba[0] ||
                               image.bytes[offset + 3] != rgba[3];
        }
    UNSCOPED_INFO("guest image differs from the swapped, tiled reference at " + std::to_string(guestMismatches) + " texels");
    REQUIRE(guestMismatches == 0);
    std::cout << "Native raw resolve encodes to the swapped, tiled guest image\n";
}

// Alias and bytes paths agree through the shared SRV swizzle.
void checkAliasBytes()
{
    constexpr std::uint32_t                                                       size = 1024, base = 0x1E3B0000;
    const std::array<std::pair<NativeDrawReplayScissor, std::array<float, 4>>, 4> patches{ {
        { { 0, 0, 1024, 1024 }, { 1.0F, 0.2F, 0.6F, 0.0F } },
        { { 40, 8, 72, 24 }, { 0.0F, 0.4F, 0.8F, 1.0F } },
        { { 512, 300, 1000, 333 }, { 0.8F, 1.0F, 0.2F, 0.4F } },
        { { 3, 900, 17, 1024 }, { 0.6F, 0.0F, 1.0F, 0.8F } },
    } };

    struct Form
    {
        NativeGuestWordPacking       words;
        GuestResolvePacking          packing;
        NativeDrawReplayTargetFormat target;
        std::uint32_t                destFormat, fetchFormat;
    };

    const std::array<Form, 3> forms{ {
        { NativeGuestWordPacking::None, GuestResolvePacking::HostRgba8, NativeDrawReplayTargetFormat::Rgba8, 6, 6 },
        { NativeGuestWordPacking::Color8888, GuestResolvePacking::Color8888Words, NativeDrawReplayTargetFormat::Rgba8, 6, 6 },
        { NativeGuestWordPacking::Color2101010, GuestResolvePacking::Color2101010Words, NativeDrawReplayTargetFormat::Rgb10a2, 7, 54 },
    } };
    // A texel's channel through a selector: 0..3 a channel, 4 zero, 5 one,
    // as a fraction of the channel's maximum.
    const auto sample = [](const std::uint8_t* texel, bool wide, const std::array<std::uint8_t, 4>& swizzle, unsigned channel)
    {
        std::uint32_t word = 0;
        std::memcpy(&word, texel, 4);
        const auto selector = swizzle[channel];
        if (selector > 3)
            return selector == 4 ? 0.0 : 1.0;
        if (!wide)
            return texel[selector] / 255.0;
        return selector == 3 ? (word >> 30) / 3.0 : ((word >> (10 * selector)) & 0x3FF) / 1023.0;
    };

    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO("alias bytes stream: " + error);
    REQUIRE(renderer.StartDrawStream(error));
    std::size_t checked = 0;
    for (const auto& form : forms)
    {
        const bool typed = form.words != NativeGuestWordPacking::None;
        if (typed && !renderer.CastableGuestWordTextures())
            continue;
        NativeRenderFrame frame;
        frame.textures.push_back({ 1, size, size, typed ? NativeDrawReplayTextureFormat::R32Uint : NativeDrawReplayTextureFormat::Rgba8, 1 });
        NativeRenderFramePass pass;
        pass.target = { size, size, 1, form.target };
        for (const auto& [rectangle, value] : patches)
        {
            NativeRenderFramePass::Clear clear;
            clear.rectangle  = rectangle;
            clear.color      = true;
            clear.colorValue = value;
            pass.clears.push_back(clear);
        }
        NativeRenderFrameResolve resolve;
        resolve.textureId = 1;
        resolve.width = resolve.height = size;
        resolve.guestWords             = form.words;
        pass.resolves.push_back(resolve);
        frame.passes.push_back(std::move(pass));
        frame.outputTextureId = 1;
        frame.retainOnly = frame.returnRawOutput = true;
        const auto raw                           = renderer.SubmitRenderFrame(std::move(frame));
        UNSCOPED_INFO("the resolve reads back one image: " + raw.error);
        REQUIRE((raw.success && raw.rawRowPitch == size * 4 && raw.rawOutput.size() == size * size * 4));

        for (const bool swapped : { true, false })
            for (const std::uint32_t endian : { 0U, 2U })
                for (const std::uint32_t selectors : { 0xD10U, 0xC14U, 0x1510U })
                {
                    // DEST_INFO: the format at bit 7, the endian, bit 24 the red/blue swap.
                    GuestResolveCopy copy;
                    copy.copyControl = 0x100000;
                    copy.destBase    = base;
                    copy.destPitch   = (size << 16) | size;
                    copy.destInfo    = (form.destFormat << 7) | endian | (swapped ? 1U << 24 : 0U);
                    copy.destFetch   = { 0x88000002, 0xFE3AF000 | (endian << 6) | form.fetchFormat, 0x007FE3FF, 0xD10, 0, 0x200 };
                    copy.rectWidth = copy.rectHeight = size;
                    copy.packing                     = form.packing;
                    GuestResolveImage image;
                    UNSCOPED_INFO("the readback encodes: " + error);
                    REQUIRE((EncodeGuestResolve(copy, raw.rawOutput, raw.rawRowPitch, image, error) && image.physical == base));

                    // The draw's fetch names the destination's pages through a virtual view.
                    const std::array<std::uint32_t, 6> fetch{ 0x88000002, 0xE0000000U | base | (endian << 6) | form.fetchFormat, 0x007FE3FF, selectors, 0, 0x200 };
                    NativeTextureMemoryRanges          ranges;
                    UNSCOPED_INFO("the fetch reads the destination: " + error);
                    REQUIRE((GetNativeTextureMemoryRanges(fetch, ranges, error) && ranges.base.address == base && !ranges.mips.size));
                    std::vector<std::uint8_t> bytes(image.bytes.begin(), image.bytes.begin() + std::min<std::size_t>(image.bytes.size(), ranges.base.size));
                    bytes.resize(ranges.base.size);
                    NativeDrawReplayTexture decoded;
                    UNSCOPED_INFO("the bytes path decodes: " + error);
                    REQUIRE(DecodeNativeTexture(fetch, bytes, {}, decoded, error));

                    NativePendingResolve pending;
                    pending.mark      = 1;
                    pending.textureId = 7;
                    pending.chain     = 1;
                    pending.epoch     = 1;
                    pending.destFetch = copy.destFetch;
                    pending.destInfo  = copy.destInfo;
                    pending.destBase  = copy.destBase;
                    pending.destEnd   = std::uint64_t(base) + image.bytes.size();
                    pending.width = pending.height = size;
                    pending.packing                = form.words;
                    NativeResolveAliasTable table;
                    table.Register(pending);
                    const auto lookup = table.Find(fetch, 0, 1, 1, true);
                    UNSCOPED_INFO("the table aliases the fetch");
                    REQUIRE((lookup && std::holds_alternative<NativeResolvedAlias>(*lookup)));
                    const auto& alias = std::get<NativeResolvedAlias>(*lookup).texture;
                    UNSCOPED_INFO("the alias layout is the decoded texture's");
                    REQUIRE((alias.layout == NativeDrawReplayTextureLayout{ decoded.width, decoded.height, decoded.format, decoded.mipLevels, decoded.dimension } &&
                             decoded.bytes.size() == raw.rawOutput.size()));

                    const bool  wide       = alias.layout.format == NativeDrawReplayTextureFormat::Rgb10a2;
                    std::size_t mismatches = 0;
                    for (std::size_t texel = 0; texel < std::size_t(size) * size; ++texel)
                        for (unsigned channel = 0; channel < 4; ++channel)
                            mismatches += sample(decoded.bytes.data() + texel * 4, wide, decoded.swizzle, channel) !=
                                          sample(raw.rawOutput.data() + texel * 4, wide, alias.swizzle, channel);
                    UNSCOPED_INFO(std::format("packing {} swap {} endian {} selectors {:#x}: {} channels differ", static_cast<unsigned>(form.words), swapped, endian, selectors, mismatches));
                    REQUIRE(mismatches == 0);
                    ++checked;
                }
    }
    renderer.Shutdown();
    std::cout << "An aliased graph resolve samples the bytes path's texels in " << checked << " guest forms\n";
}

// Flushes through `mark` as the ledger polls: a flush that submitted part of
// the work retries once that part completes.
completion::DrawFlush flushThrough(NativeFrameAssembler& assembler, D3D12CompletionSource& source, std::uint64_t mark)
{
    auto flush = assembler.FlushThrough(mark);
    for (int polls = 0; !flush.fence && flush.retry && polls < 8; ++polls)
    {
        UNSCOPED_INFO("the submitted part completes");
        REQUIRE(source.WaitFor(assembler.LastSubmitted(), std::chrono::seconds(10)));
        flush = assembler.FlushThrough(mark);
    }
    return flush;
}

// Frame 428 tiles are 448/416/416; later tiles inherit prior resolve clears.
void checkTiledMrtResolves(bool graph, bool presented = false)
{
    // C10 frame 428 raw_object_descriptor uses A0000000 depth/E0000000 colors
    // and Resolve physical bases; presented clear uses EDRAM 0.
    using Target                                 = NativeRenderFramePass::Target;
    constexpr std::uint32_t                width = 1280, height = 720, storage = 1280 * 736 * 4;
    constexpr std::array<GuestRect, 3>     rects{ { { 0, 0, 448, 720 }, { 448, 0, 864, 720 }, { 864, 0, 1280, 720 } } };
    constexpr std::array<std::uint32_t, 3> slots{ kGuestDepthSlot, 0, 1 };
    const Target                           set{ width, height, 4, NativeDrawReplayTargetFormat::Rgba8, 0, kNoDepthEdramBase, 1, { NativeDrawReplayTargetFormat::Rgb10a2 }, { 0x21C } };
    // Memory byte c of a stored word holds word byte endianBytes[endian][c].
    constexpr std::array<std::array<unsigned, 4>, 4> endianBytes{ { { 0, 1, 2, 3 }, { 1, 0, 3, 2 }, { 3, 2, 1, 0 }, { 2, 3, 0, 1 } } };

    struct Destination
    {
        std::array<std::uint32_t, 6> fetch;
        std::uint32_t                physical, copyFormat, endian;
        std::uint32_t                tile0, later; // the guest word in tile 0 and in tiles 1 and 2
    };

    // In slot order. Tile 0: the clear (1,0,0,1), depth 1.0, stencil 0x17;
    // later tiles: the copies' clear words below.
    const std::array<Destination, 3> destinations{ {
        { { 0x8A000002, 0xBD0F0096, 0x0059E4FF, 0x1690, 0x280, 0xBD488A00 }, 0x1D0F0000, 6, 2, 0xFFFFFF17, 0x80000042 },
        { { 0x8A000002, 0xFD817006, 0x0059E4FF, 0x1414, 0, 0x200 }, 0x1D818000, 6, 0, 0xFF0000FF, 0x80FF0033 },
        { { 0x8A000002, 0xFD47F036, 0x0059E4FF, 0xD10, 0, 0x200 }, 0x1D480000, 7, 0, 0xC00003FF, 0xC00FFC01 },
    } };
    const auto                       tiling = [&](std::uint32_t mask)
    {
        GuestTiling value;
        value.flags        = 0x20;
        value.count        = 3;
        value.screenWidth  = width;
        value.screenHeight = height;
        std::copy(rects.begin(), rects.end(), value.rects.begin());
        value.saved         = { 0x50263120, 0x5044C6B0, 0, 0, 0x5044C390 };
        value.predicateMask = mask;
        value.predicated    = 1;
        return value;
    };
    const auto target = [&](std::uint32_t slot, std::uint32_t mask)
    {
        GuestTarget value;
        value.native     = set;
        value.tiling     = tiling(mask);
        value.slot       = slot;
        value.colorCount = 2;
        return value;
    };
    const auto clear = [&](std::uint32_t index)
    {
        GuestClear value;
        value.target             = target(index, 3);
        value.flags              = index ? 0x0F : 0x3F;
        value.index              = index;
        value.predicate          = 1;
        value.tiling             = tiling(3);
        value.clear.rectangle    = { 0, 0, std::int32_t(width), std::int32_t(height) };
        value.clear.color        = true;
        value.clear.depth        = index == 0;
        value.clear.stencil      = index == 0;
        value.clear.colorTargets = static_cast<std::uint8_t>(1U << index);
        value.clear.colorValue   = { 1, 0, 0, 1 };
        value.clear.depthValue   = 1;
        value.clear.stencilValue = 0x17;
        value.depth              = GuestDepthSurface{ 0x5044C390, 0x5E4, 0x5E4, width, height, 4, true };
        return value;
    };

    NativeRendererD3D12 stream;
    std::string         error;
    UNSCOPED_INFO("tiled MRT stream: " + error);
    REQUIRE(stream.StartDrawStream(error));
    std::array<std::vector<std::uint8_t>, 3> memory, expected;
    for (auto& bytes : memory)
        bytes.assign(storage, 0xA5);
    for (auto& bytes : expected)
        bytes.assign(storage, 0xA5);
    {
        auto                       producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource      source({ producer });
        NativeFrameAssemblerConfig config;
        config.completion   = &source;
        config.resolveGraph = graph;
        NativeFrameAssembler assembler(config);
        if (presented)
        {
            GuestClear main;
            main.target.native    = { width, height, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
            main.flags            = 1;
            main.clear.rectangle  = { 0, 0, std::int32_t(width), std::int32_t(height) };
            main.clear.color      = true;
            main.clear.colorValue = { 0.2F, 0.4F, 0.6F, 1.0F };
            const bool cleared    = assembler.Clear(main, error) != 0;
            UNSCOPED_INFO("the presented clear on base 0: " + error);
            REQUIRE(cleared);
        }
        UNSCOPED_INFO("the paired clears: " + error);
        REQUIRE((assembler.Clear(clear(0), error) && assembler.Clear(clear(1), error)));
        for (std::uint32_t tile = 0; tile < 3; ++tile)
            for (std::size_t d = 0; d < slots.size(); ++d)
            {
                const auto&      destination = destinations[d];
                const auto       slot        = slots[d];
                GuestResolveCopy copy;
                copy.copyControl = 0x00100000 | slot | (slot == kGuestDepthSlot ? 0x10 : 0x40) |
                                   (tile == 2 || slot == kGuestDepthSlot ? 0 : slot == 0 ? 0x300
                                                                                         : 0x100);
                copy.destBase    = destination.physical;
                copy.destPitch   = (height << 16) | width;
                copy.destInfo    = (destination.copyFormat << 7) | destination.endian;
                copy.destFetch   = destination.fetch;
                copy.rectX = copy.destX = rects[tile].x0;
                copy.rectWidth          = rects[tile].x1 - rects[tile].x0;
                copy.rectHeight         = height;
                copy.colorClear         = slot == 1 ? 0xC00FFC01 : 0x80FF0033;
                copy.depthClear         = 0x80000042;
                bool       partial      = false;
                const auto mark         = assembler.Resolve(target(slot, 3U << (2 * tile)), copy, false, partial, error);
                UNSCOPED_INFO(std::format("tile {} slot {} resolve: {}", tile, slot, error));
                REQUIRE(mark != 0);
                const auto flush = flushThrough(assembler, source, mark);
                UNSCOPED_INFO(std::format("tile {} slot {} completes: {}", tile, slot, flush.error));
                REQUIRE((flush.fence && source.WaitFor(flush.fence, std::chrono::seconds(10))));
                const auto take = assembler.TakeGuestImage(mark);
                UNSCOPED_INFO(std::format("tile {} slot {} image: {}", tile, slot, take.error));
                REQUIRE((take.image && take.image->physical == copy.destBase));
                for (const auto& run : take.image->runs)
                {
                    UNSCOPED_INFO("a run lies inside the destination");
                    REQUIRE(run.offset + run.length <= storage);
                    std::copy_n(take.image->bytes.begin() + run.offset, run.length, memory[d].begin() + run.offset);
                }
                const auto word = tile ? destination.later : destination.tile0;
                for (std::uint32_t y = 0; y < height; ++y)
                    for (std::uint32_t x = rects[tile].x0; x < rects[tile].x1; ++x)
                        for (unsigned c = 0; c < 4; ++c)
                            expected[d][referenceTiledOffset(x, y, width) + c] = static_cast<std::uint8_t>(word >> (8 * endianBytes[destination.endian][c]));
            }
        UNSCOPED_INFO("the pass leaves no failure");
        REQUIRE((!assembler.Failure() && !source.Failure()));
        UNSCOPED_INFO(std::format("{} resolves submit {} terminals and {} segments", graph ? "graph" : "legacy", producer->terminals.size(), producer->prefixes));
        REQUIRE((graph ? producer->terminals.empty() && producer->prefixes == 9 : producer->terminals.size() == 9 && producer->prefixes == (presented ? 1U : 0U)));
    }
    stream.Shutdown();
    for (std::size_t d = 0; d < slots.size(); ++d)
    {
        std::size_t mismatches = 0, first = storage;
        for (std::size_t i = 0; i < storage; ++i)
            if (memory[d][i] != expected[d][i])
            {
                first = std::min(first, i);
                ++mismatches;
            }
        UNSCOPED_INFO(std::format("destination {} differs from the oracle at {} bytes, first at {:#x}: {:02X} for {:02X}", d, mismatches, first, first < storage ? memory[d][first] : 0, first < storage ? expected[d][first] : 0));
        REQUIRE(mismatches == 0);
    }
    std::cout << "Tiled MRT resolves encode depth, RGBA8 and 2_10_10_10 tiles into guest memory" << (graph ? " as graph segments" : "")
              << (presented ? " after presented work on their EDRAM base\n" : "\n");
}

// Off-screen writes after presented work refuse scanout; earlier writes do not.
struct PresentedBaseRun
{
    std::vector<std::uint8_t> image, shown;
    NativeResolveGraphStats   stats;
    std::size_t               terminals = 0, prefixes = 0;
};

PresentedBaseRun presentedBaseRun(bool graph, bool presentedFirst)
{
    const auto clear = [](const GuestTarget& target, NativeDrawReplayScissor rectangle, std::array<float, 4> value)
    {
        GuestClear result;
        result.target           = target;
        result.flags            = 1;
        result.clear.rectangle  = rectangle;
        result.clear.color      = true;
        result.clear.colorValue = value;
        return result;
    };
    GuestTarget main, offscreen;
    main.native      = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    offscreen.native = { 1024, 1024, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destPitch   = 0x04000400;
    copy.destInfo    = 0x01000300;
    copy.destFetch   = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    copy.rectWidth = copy.rectHeight = 1024;

    PresentedBaseRun    run;
    NativeRendererD3D12 stream;
    std::string         error;
    UNSCOPED_INFO("presented-base stream: " + error);
    REQUIRE(stream.StartDrawStream(error));
    {
        auto                       producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource      source({ producer });
        NativeFrameAssemblerConfig config;
        config.completion   = &source;
        config.readback     = true;
        config.resolveGraph = graph;
        config.timing       = true;
        NativeFrameAssembler assembler(config);
        // Presented color clears are whole-target, so the swap frame's
        // resumed attachments must keep the first clear's color.
        const auto presented = [&](std::array<float, 4> fill)
        {
            const bool cleared = assembler.Clear(clear(main, { 0, 0, 1280, 720 }, fill), error);
            UNSCOPED_INFO("a presented clear at base 0: " + error);
            REQUIRE(cleared);
        };
        if (presentedFirst)
            presented({ 0.2F, 0.4F, 0.6F, 1.0F });
        const bool cleared = assembler.Clear(clear(offscreen, { 0, 0, 1024, 1024 }, { 1.0F, 0.2F, 0.6F, 0.0F }), error);
        UNSCOPED_INFO("an off-screen clear at base 0: " + error);
        REQUIRE(cleared);
        bool       partial = false;
        const auto mark    = assembler.Resolve(offscreen, copy, false, partial, error);
        UNSCOPED_INFO("the off-screen resolve: " + error);
        REQUIRE(mark != 0);
        const auto flush = flushThrough(assembler, source, mark);
        UNSCOPED_INFO("the off-screen resolve completes: " + flush.error);
        REQUIRE((flush.fence && source.WaitFor(flush.fence, std::chrono::seconds(10))));
        const auto take = assembler.TakeGuestImage(mark);
        UNSCOPED_INFO("the off-screen image: " + take.error);
        REQUIRE((take.image && take.image->physical == copy.destBase));
        run.image = take.image->bytes;
        if (presentedFirst)
        {
            std::string refusal;
            const bool  refused = !tryScanOut(assembler, refusal) && refusal.starts_with("presented_edram_aliased: off-screen clear ");
            UNSCOPED_INFO("the scanout of overwritten presented EDRAM is refused: " + refusal);
            REQUIRE(refused);
            const auto result = assembler.Swap(swapRecord(0xD10));
            UNSCOPED_INFO("the swap fails visibly: " + result.detail);
            REQUIRE((result.status == NativeFrameStatus::Failed && result.detail.find("presented_edram_aliased") != std::string::npos));
            if (result.hostFence)
            {
                UNSCOPED_INFO("the failed swap's work completes");
                REQUIRE(source.WaitFor(result.hostFence, std::chrono::seconds(10)));
            }
            run.stats     = assembler.ResolveGraphStats();
            run.terminals = producer->terminals.size();
            run.prefixes  = producer->prefixes;
            source.Close();
            stream.Shutdown();
            return run;
        }
        presented({ 0.6F, 0.2F, 1.0F, 1.0F });
        scanOut(assembler);
        const auto result = assembler.Swap(swapRecord(0xD10));
        UNSCOPED_INFO("the presented-base swap: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence && source.WaitFor(result.hostFence, std::chrono::seconds(10))));
        UNSCOPED_INFO("a terminal frame presents");
        REQUIRE(!producer->terminals.empty());
        const auto& output = producer->terminals.back().get();
        UNSCOPED_INFO("the presented output: " + output.error);
        REQUIRE((output.success && output.resolvedOutput.size() == kPlaneBytes));
        run.shown     = output.resolvedOutput;
        run.stats     = assembler.ResolveGraphStats();
        run.terminals = producer->terminals.size();
        run.prefixes  = producer->prefixes;
        UNSCOPED_INFO("the presented-base epoch leaves no failure");
        REQUIRE((!assembler.Failure() && !source.Failure()));
        source.Close();
    }
    stream.Shutdown();
    return run;
}

// Presented-base resolves preserve legacy bytes across attachment park/resume.
void checkPresentedBaseResolves()
{
    for (const bool presentedFirst : { true, false })
    {
        const auto legacy = presentedBaseRun(false, presentedFirst);
        const auto graph  = presentedBaseRun(true, presentedFirst);
        const auto name   = presentedFirst ? "presented work before the resolve" : "presented work after the resolve";
        UNSCOPED_INFO(std::format("{}: legacy submits {} terminals and {} segments", name, legacy.terminals, legacy.prefixes));
        REQUIRE((legacy.terminals == (presentedFirst ? 1U : 2U) && legacy.prefixes == (presentedFirst ? 1U : 0U)));
        UNSCOPED_INFO(std::format("{}: the graph takes the resolve ({} replays, {} unready fallbacks, {} terminals)", name, graph.stats.replays, graph.stats.notReady, graph.terminals));
        REQUIRE((graph.stats.replays == 1 && !graph.stats.notReady && graph.terminals == (presentedFirst ? 0U : 1U) && graph.prefixes == 1));
        UNSCOPED_INFO(std::format("{}: the graph resolve image equals the legacy one", name));
        REQUIRE((!graph.image.empty() && graph.image == legacy.image));
        UNSCOPED_INFO(std::format("{}: the graph presented output equals the legacy one", name));
        REQUIRE((graph.shown == legacy.shown && graph.shown.empty() == presentedFirst));
    }
    std::cout << "Graph resolves on the presented EDRAM base equal the legacy path, and overwritten presented work is refused on both\n";
}

// Two epochs of one off-screen graph resolve each. With `hold` the queue
// waits on a gate, so epoch 1's terminal frame is recorded but not complete
// when epoch 2's resolve cuts its segment. Returns both resolve images.
std::array<std::vector<std::uint8_t>, 2> inFlightRun(bool hold)
{
    using Microsoft::WRL::ComPtr;
    const auto clear = [](const GuestTarget& target, std::array<float, 4> value)
    {
        GuestClear result;
        result.target           = target;
        result.flags            = 1;
        result.clear.rectangle  = { 0, 0, std::int32_t(target.native.width), std::int32_t(target.native.height) };
        result.clear.color      = true;
        result.clear.colorValue = value;
        return result;
    };
    GuestTarget offscreen;
    offscreen.native = { 1024, 1024, 1, NativeDrawReplayTargetFormat::Rgba8, 0x300 };
    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destPitch   = 0x04000400;
    copy.destInfo    = 0x01000300;
    copy.destFetch   = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    copy.rectWidth = copy.rectHeight = 1024;

    std::array<std::vector<std::uint8_t>, 2> images;
    NativeRendererD3D12                      stream;
    std::string                              error;
    UNSCOPED_INFO("in-flight stream: " + error);
    REQUIRE(stream.StartDrawStream(error));
    {
        auto*                queue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(stream);
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence>  gate;
        UNSCOPED_INFO("the queue gate");
        REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))));

        struct ReleaseGate
        {
            ID3D12Fence* gate;

            ~ReleaseGate()
            {
                gate->Signal(1);
            }
        } release{ gate.Get() };

        if (hold)
        {
            UNSCOPED_INFO("hold the native queue");
            REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)));
        }
        auto                       producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource      source({ producer });
        NativeFrameAssemblerConfig config;
        config.completion   = &source;
        config.resolveGraph = true;
        config.timing       = true;
        NativeFrameAssembler         assembler(config);
        std::array<std::uint64_t, 2> marks{};
        for (std::size_t epoch = 0; epoch < 2; ++epoch)
        {
            const bool cleared = assembler.Clear(clear(offscreen, epoch ? std::array<float, 4>{ 0.0F, 0.4F, 0.8F, 1.0F } : std::array<float, 4>{ 1.0F, 0.2F, 0.6F, 0.0F }), error) != 0;
            UNSCOPED_INFO("the off-screen clear: " + error);
            REQUIRE(cleared);
            bool partial = false;
            marks[epoch] = assembler.Resolve(offscreen, copy, false, partial, error);
            UNSCOPED_INFO("the off-screen resolve: " + error);
            REQUIRE(marks[epoch] != 0);
            const auto flush = assembler.FlushThrough(marks[epoch]);
            UNSCOPED_INFO(std::format("epoch {} cuts its segment at once: {}", epoch + 1, flush.error));
            REQUIRE((flush.fence && !flush.retry));
            if (hold && epoch)
            {
                const bool inFlight = producer->terminals.size() == 1 && producer->terminals[0].wait_for(std::chrono::seconds(0)) != std::future_status::ready;
                UNSCOPED_INFO("the segment was cut while the previous terminal frame was in flight");
                REQUIRE(inFlight);
                // Recording reuses the shared command allocators at their
                // fences, so release the queue before the renderer needs them.
                UNSCOPED_INFO("release the native queue");
                REQUIRE(SUCCEEDED(gate->Signal(1)));
            }
            auto swap        = swapRecord(0xD10);
            swap.sequence    = epoch + 1;
            const auto ended = assembler.Swap(swap);
            UNSCOPED_INFO(std::format("epoch {} ends its off-screen chain: {}", epoch + 1, ended.detail));
            REQUIRE((ended.status == NativeFrameStatus::Empty && ended.hostFence));
            if (hold && !epoch)
            {
                UNSCOPED_INFO("the held terminal frame is recorded but incomplete");
                REQUIRE((producer->terminals.size() == 1 && producer->terminals[0].wait_for(std::chrono::seconds(0)) != std::future_status::ready));
            }
        }
        UNSCOPED_INFO("both resolves join the graph");
        REQUIRE((!assembler.ResolveGraphStats().notReady && assembler.ResolveGraphStats().replays == 2));
        UNSCOPED_INFO("both epochs complete");
        REQUIRE(source.WaitFor(assembler.LastSubmitted(), std::chrono::seconds(10)));
        for (std::size_t epoch = 0; epoch < 2; ++epoch)
        {
            const auto take = assembler.TakeGuestImage(marks[epoch]);
            UNSCOPED_INFO(std::format("epoch {} image: {}", epoch + 1, take.error));
            REQUIRE((take.image && take.image->physical == copy.destBase));
            images[epoch] = take.image->bytes;
        }
        UNSCOPED_INFO("the in-flight epochs leave no failure");
        REQUIRE((!assembler.Failure() && !source.Failure()));
        source.Close();
    }
    stream.Shutdown();
    return images;
}

// Held deferred terminals occupy both contexts when epoch 3 cuts its segment.
std::array<std::vector<std::uint8_t>, 2> saturatedRun(bool hold, bool eager = false)
{
    using Microsoft::WRL::ComPtr;
    const auto clear = [](const GuestTarget& target, std::array<float, 4> value)
    {
        GuestClear result;
        result.target           = target;
        result.flags            = 1;
        result.clear.rectangle  = { 0, 0, std::int32_t(target.native.width), std::int32_t(target.native.height) };
        result.clear.color      = true;
        result.clear.colorValue = value;
        return result;
    };
    GuestTarget main, offscreen;
    main.native      = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    offscreen.native = { 1024, 1024, 1, NativeDrawReplayTargetFormat::Rgba8, 0x300 };
    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destPitch   = 0x04000400;
    copy.destInfo    = 0x01000300;
    copy.destFetch   = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    copy.rectWidth = copy.rectHeight = 1024;

    std::array<std::vector<std::uint8_t>, 2> images;
    NativeRendererD3D12                      stream;
    std::string                              error;
    // The presented epoch publishes to a title host that cancels each offer.
    ComPtr<ID3D12Device> shared;
    UNSCOPED_INFO("the shared device");
    REQUIRE(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&shared))));
    std::uint64_t leases  = 0;
    const auto    publish = [&leases](ID3D12Resource*, ID3D12Fence*, std::uint64_t)
    {
        auto lease = std::make_shared<NativeOutputLease>(++leases);
        lease->Cancel();
        return lease;
    };
    UNSCOPED_INFO("saturated stream: " + error);
    REQUIRE(stream.StartTitleDrawStream(error, shared.Get(), publish));
    {
        auto*                queue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(stream);
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence>  gate;
        UNSCOPED_INFO("the queue gate");
        REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))));

        struct ReleaseGate
        {
            ID3D12Fence* gate;

            ~ReleaseGate()
            {
                gate->Signal(1);
            }
        } release{ gate.Get() };

        auto                       producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource      source({ producer });
        NativeFrameAssemblerConfig config;
        config.completion         = &source;
        config.resolveGraph       = true;
        config.eagerGraphSegments = eager;
        config.timing             = true;
        NativeFrameAssembler assembler(config);
        const auto           pending = [&]
        {
            return std::ranges::none_of(producer->terminals, [](const auto& terminal)
                                        {
                                            return terminal.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                        });
        };
        const auto resolve = [&](std::size_t epoch, std::array<float, 4> value)
        {
            const bool cleared = assembler.Clear(clear(offscreen, value), error) != 0;
            UNSCOPED_INFO("the off-screen clear: " + error);
            REQUIRE(cleared);
            bool       partial  = false;
            const auto prefixes = producer->prefixes;
            const auto mark     = assembler.Resolve(offscreen, copy, false, partial, error);
            UNSCOPED_INFO("the off-screen resolve: " + error);
            REQUIRE(mark != 0);
            UNSCOPED_INFO(std::format("epoch {} submits its segment {}", epoch, eager ? "at the resolve" : "at the read"));
            REQUIRE(producer->prefixes == prefixes + (eager ? 1 : 0));
            const auto flush = assembler.FlushThrough(mark);
            UNSCOPED_INFO(std::format("epoch {} has one segment", epoch));
            REQUIRE(producer->prefixes == prefixes + 1);
            UNSCOPED_INFO(std::format("epoch {} cuts its segment at once: {}", epoch, flush.error));
            REQUIRE((flush.fence && !flush.retry));
            return mark;
        };
        const auto swap = [&](std::size_t epoch, NativeFrameStatus status)
        {
            auto record     = swapRecord(0xD10);
            record.sequence = epoch;
            const auto done = assembler.Swap(record);
            UNSCOPED_INFO(std::format("epoch {} ends: {}", epoch, done.detail));
            REQUIRE((done.status == status && done.hostFence));
        };

        std::array<std::uint64_t, 2> marks{};
        marks[0] = resolve(1, { 1.0F, 0.2F, 0.6F, 0.0F });
        UNSCOPED_INFO("epoch 1's segment is submitted");
        REQUIRE((producer->prefixRecordings.size() == 1 && producer->prefixRecordings[0].wait_for(std::chrono::seconds(4)) == std::future_status::ready &&
                 producer->prefixRecordings[0].get().success));
        if (hold)
        {
            UNSCOPED_INFO("hold the native queue");
            REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)));
        }
        swap(1, NativeFrameStatus::Empty);
        const bool presented = assembler.Clear(clear(main, { 0.2F, 0.4F, 0.6F, 1.0F }), error) != 0;
        UNSCOPED_INFO("the presented clear: " + error);
        REQUIRE(presented);
        scanOut(assembler);
        swap(2, NativeFrameStatus::Provisional);
        UNSCOPED_INFO("both epochs queue a terminal frame");
        REQUIRE(producer->terminals.size() == 2);
        if (hold)
        {
            const bool recorded = std::ranges::all_of(producer->terminalRecordings, [](const auto& recording)
                                                      {
                                                          return recording.wait_for(std::chrono::seconds(4)) == std::future_status::ready && recording.get().success;
                                                      });
            UNSCOPED_INFO("both held terminal frames are recorded but incomplete");
            REQUIRE((recorded && pending()));
        }
        marks[1] = resolve(3, { 0.0F, 0.4F, 0.8F, 1.0F });
        if (hold)
        {
            UNSCOPED_INFO("the segment was cut while both terminal frames were in flight");
            REQUIRE(pending());
            if (eager)
            {
                // The held terminals keep the stream fence's next value
                // pending until the queue runs.
                const auto value  = producer->CompletedFence() + 1;
                auto       waited = std::async(std::launch::async, [&]
                                               {
                                             return producer->WaitFence(value, std::chrono::steady_clock::now() + std::chrono::seconds(10));
                                               });
                UNSCOPED_INFO("the fence wait holds while the terminal is pending");
                REQUIRE(waited.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
                UNSCOPED_INFO("release the native queue");
                REQUIRE(SUCCEEDED(gate->Signal(1)));
                UNSCOPED_INFO("the fence event ends the wait once the terminal completes");
                REQUIRE((waited.wait_for(std::chrono::seconds(4)) == std::future_status::ready && waited.get() >= value));
            }
            // The segment's frame context and command batch wait for the
            // first terminal, so release the queue before they time out.
            UNSCOPED_INFO("release the native queue");
            REQUIRE(SUCCEEDED(gate->Signal(1)));
        }
        swap(3, NativeFrameStatus::Empty);
        UNSCOPED_INFO("both resolves join the graph");
        REQUIRE((!assembler.ResolveGraphStats().notReady && assembler.ResolveGraphStats().replays == 2));
        UNSCOPED_INFO("every epoch completes");
        REQUIRE(source.WaitFor(assembler.LastSubmitted(), std::chrono::seconds(10)));
        for (std::size_t index = 0; index < marks.size(); ++index)
        {
            const auto take = assembler.TakeGuestImage(marks[index]);
            UNSCOPED_INFO(std::format("resolve {} image: {}", index + 1, take.error));
            REQUIRE((take.image && take.image->physical == copy.destBase));
            images[index] = take.image->bytes;
        }
        UNSCOPED_INFO("the saturated epochs leave no failure");
        REQUIRE((!assembler.Failure() && !source.Failure()));
        if (hold && eager)
        {
            // A wait for a value the stream never signals, pending across
            // shutdown, ends at its deadline with the fence still held.
            const auto unreached = producer->CompletedFence() + 1000;
            const auto started   = std::chrono::steady_clock::now();
            auto       waited    = std::async(std::launch::async, [&]
                                              {
                                         return producer->WaitFence(unreached, started + std::chrono::milliseconds(500));
                                              });
            source.Close();
            stream.Shutdown();
            UNSCOPED_INFO("a fence wait pending at shutdown ends at its deadline");
            REQUIRE((waited.wait_for(std::chrono::seconds(4)) == std::future_status::ready && waited.get() < unreached &&
                     std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(450)));
        }
        source.Close();
    }
    stream.Shutdown();
    return images;
}

// Resolve segments submit while prior terminal frames remain in flight.
void checkSegmentWhileTerminalInFlight()
{
    const auto free = inFlightRun(false);
    const auto held = inFlightRun(true);
    UNSCOPED_INFO("the two epochs resolve different images");
    REQUIRE(free[0] != free[1]);
    UNSCOPED_INFO("segments cut behind an in-flight terminal resolve the same bytes");
    REQUIRE(held == free);
    const auto freeSaturated = saturatedRun(false);
    const auto heldSaturated = saturatedRun(true);
    UNSCOPED_INFO("the saturated run resolves different images");
    REQUIRE(freeSaturated[0] != freeSaturated[1]);
    UNSCOPED_INFO("a segment cut behind two deferred terminals resolves the same bytes");
    REQUIRE(heldSaturated == freeSaturated);
    UNSCOPED_INFO("segments submitted at their resolves resolve the same bytes, also behind two deferred terminals");
    REQUIRE((saturatedRun(false, true) == freeSaturated && saturatedRun(true, true) == freeSaturated));
    std::cout << "A graph resolve cuts its segment while the previous terminal frames are in flight, also with both frame contexts held\n";
}

std::vector<std::uint8_t> compile(const char* source, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const auto                       hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    UNSCOPED_INFO((errors ? static_cast<const char*>(errors->GetBufferPointer()) : "shader compilation failed"));
    REQUIRE(SUCCEEDED(hr));
    const auto* bytes = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    return { bytes, bytes + code->GetBufferSize() };
}

// Guest color target 0 of a device at kAliasGraphics as ReadGuestTarget
// decodes it.
constexpr std::uint32_t kAliasGraphics = 0x1000, kAliasSurface = 0x20000;

GuestWordReader targetWords(std::uint32_t width, std::uint32_t height, bool msaa, std::uint32_t base)
{
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 8> words{ { { kAliasGraphics + 0x3098, kAliasSurface },
                                                                          { kAliasGraphics + 0x2880, 0 },
                                                                          { kAliasGraphics + 0x2ABC, 0 },
                                                                          { kAliasGraphics + 0x31A4, 0xFFFFFFFF },
                                                                          { kAliasGraphics + 0x31AC, 0 },
                                                                          { kAliasSurface + 0x24, ((width - 1) << 18) | ((height - 1) << 3) },
                                                                          { kAliasSurface + 0x18, msaa ? 1U << 16 : 0U },
                                                                          { kAliasSurface + 0x1C, base } } };
    return [words](std::uint32_t address, std::uint32_t& out)
    {
        for (const auto& [at, value] : words)
            if (at == address)
            {
                out = value;
                return true;
            }
        return false;
    };
}

// With REREVVED_TEST_SURFACE_REGISTRY=1 the alias runs bind through the
// resolve surface registry, as native_surface_registry binds in the title.
bool surfaceRegistrySwitch()
{
    char value[8]{};
    return GetEnvironmentVariableA("REREVVED_TEST_SURFACE_REGISTRY", value, sizeof(value)) != 0 && std::string(value) == "1";
}

struct AliasResetRun
{
    std::vector<std::uint8_t> shown;
    NativeResolveGraphStats   stats;
};

// Alias mode binds the graph texture; without it, the draw samples the
// bytes-path resolve image.
AliasResetRun aliasResetRun(bool alias)
{
    const auto clear = [](const GuestTarget& target, NativeDrawReplayScissor rectangle, std::array<float, 4> value)
    {
        GuestClear result;
        result.target           = target;
        result.flags            = 1;
        result.clear.rectangle  = rectangle;
        result.clear.color      = true;
        result.clear.colorValue = value;
        return result;
    };
    GuestTarget main, offscreen, signedTarget;
    main.native         = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    offscreen.native    = { 1024, 1024, 1, NativeDrawReplayTargetFormat::Rgba8, 0x300 };
    signedTarget.native = { 64, 64, 1, NativeDrawReplayTargetFormat::Rg16Snorm, 0x900 };
    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destPitch   = 0x04000400;
    copy.destInfo    = 0x01000300;
    copy.destFetch   = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    copy.rectWidth = copy.rectHeight = 1024;
    GuestResolveCopy signedCopy;
    signedCopy.copyControl = 0x100000;
    signedCopy.destBase    = 0x1D800000;
    signedCopy.destInfo    = 0xEF82;
    signedCopy.rectWidth = signedCopy.rectHeight = 64;
    // The draw's fetch of the destination, red and blue exchanged.
    const std::array<std::uint32_t, 6> fetch{ 0x88000002, 0xFE3B0006, 0x007FE3FF, 0xC14, 0, 0x200 };

    NativeDrawReplayRecipe sampler;
    sampler.schemaVersion                               = 2;
    sampler.width                                       = 1280;
    sampler.height                                      = 720;
    sampler.sampleCount                                 = 4;
    sampler.sampleMask                                  = 9;
    sampler.vertexShaderHash                            = 0xA11A5;
    sampler.pixelShaderHash                             = 0xA11A6;
    sampler.vertexShaderDxil                            = compile("float4 main(float4 p:TEXCOORD0):SV_Position { return p; }", "vs_5_1");
    sampler.pixelShaderDxil                             = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 {"
                                                                  " return t.Load(int3(int2(p.xy) % 1024, 0)); }",
                                                                  "ps_5_1");
    sampler.vertexAttributeCount                        = 1;
    sampler.vertexStrideBytes                           = 16;
    const std::array<std::array<float, 4>, 3> positions = { { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } } };
    sampler.vertexData.resize(sizeof(positions));
    std::memcpy(sampler.vertexData.data(), positions.data(), sizeof(positions));
    sampler.indices    = { 0, 1, 2 };
    sampler.indexCount = 3;
    sampler.vertexConstants.resize(4096);
    sampler.pixelConstants.resize(3584);
    sampler.sharedConstants.resize(336);
    sampler.viewport    = { 0, 0, 1280, 720, 0, 1 };
    sampler.scissor     = { 0, 0, 1280, 720 };
    sampler.textureMask = 1;
    // The producer's content: a full-target draw whose texels name their position.
    auto gradient        = sampler;
    gradient.width       = 1024;
    gradient.height      = 1024;
    gradient.sampleCount = gradient.sampleMask = 1;
    gradient.pixelShaderHash                   = 0xA11A7;
    gradient.pixelShaderDxil                   = compile("float4 main(float4 p:SV_Position):SV_Target0 { uint2 q = uint2(p.xy);"
                                                         " return float4(q.x & 255, q.y & 255, (q.x ^ q.y) & 255, 255) / 255.0; }",
                                                         "ps_5_1");
    gradient.viewport                          = { 0, 0, 1024, 1024, 0, 1 };
    gradient.scissor                           = { 0, 0, 1024, 1024 };
    gradient.textureMask                       = 0;

    AliasResetRun       run;
    NativeRendererD3D12 stream;
    std::string         error;
    UNSCOPED_INFO("alias reset stream: " + error);
    REQUIRE(stream.StartDrawStream(error));
    {
        auto                       producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource      source({ producer });
        NativeFrameAssemblerConfig config;
        config.completion         = &source;
        config.readback           = true;
        config.resolveGraph       = true;
        config.eagerGraphSegments = true;
        config.resolveAlias       = alias;
        config.timing             = true;
        if (alias && surfaceRegistrySwitch())
            config.surfaces = std::make_shared<ResolveSurfaceRegistry>();
        bool onScreen     = false;
        config.readTarget = [&onScreen, presented = targetWords(1280, 720, true, 0), offScreen = targetWords(1024, 1024, false, 0x300)](std::uint32_t address, std::uint32_t& out)
        {
            return (onScreen ? presented : offScreen)(address, out);
        };
        config.watchWrites = [](std::uint32_t, std::uint32_t)
        {
        };
        std::optional<NativeDrawReplayTexture> decoded;
        NativeFrameAssembler*                  current = nullptr;
        config.build                                   = [&](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string&)
        {
            if (!onScreen)
            {
                draw = gradient;
                return true;
            }
            draw               = sampler;
            auto       miss    = NativeResolvedTextureMiss::Form;
            const auto texture = current->ResolvedTexture(fetch, value.operationMark, miss);
            UNSCOPED_INFO(std::format("the fetch binds the resolve only with aliasing, miss {}", static_cast<unsigned>(miss)));
            REQUIRE(texture.has_value() == alias);
            draw.textures[0] = texture ? BindNativeResolvedTexture(*texture) : *decoded;
            return true;
        };
        NativeFrameAssembler assembler(config);
        current = &assembler;

        const auto submit = [&](std::uint64_t sequence)
        {
            NativeDrawSubmission draw;
            draw.hook          = NativeDrawHook::IndexedUp;
            draw.caller        = 0x82303E40;
            draw.stack         = 0x7000;
            draw.arguments     = { kAliasGraphics, 4, 0, 3, 3, 0x3000, 1, std::uint32_t(sequence) };
            draw.stateRead     = true;
            draw.sequence      = sequence;
            draw.operationMark = assembler.NumberOperation();
            assembler.Began(draw);
            const auto verdict = assembler.Admit(draw);
            assembler.Recorded(draw, verdict);
            UNSCOPED_INFO(std::format("draw {} is admitted", sequence));
            REQUIRE(!verdict);
            return draw.operationMark;
        };
        // Each message is built after its call so it carries that call's error.
        const bool cleared = assembler.Clear(clear(offscreen, { 0, 0, 1024, 1024 }, { 1.0F, 0.2F, 0.6F, 0.0F }), error) != 0;
        UNSCOPED_INFO("the off-screen clear: " + error);
        REQUIRE(cleared);
        submit(1);
        bool       partial = false;
        const auto mark    = assembler.Resolve(offscreen, copy, false, partial, error);
        UNSCOPED_INFO("the off-screen resolve: " + error);
        REQUIRE(mark != 0);
        const auto produced = flushThrough(assembler, source, mark);
        UNSCOPED_INFO("the resolve completes: " + produced.error);
        REQUIRE((produced.fence && source.WaitFor(produced.fence, std::chrono::seconds(10))));
        const auto take = assembler.TakeGuestImage(mark);
        UNSCOPED_INFO("the resolve's guest image: " + take.error);
        REQUIRE((take.image && take.image->physical == copy.destBase));
        NativeTextureMemoryRanges ranges;
        const bool                ranged = GetNativeTextureMemoryRanges(fetch, ranges, error) && ranges.base.address == copy.destBase;
        UNSCOPED_INFO("the fetch reads the destination: " + error);
        REQUIRE(ranged);
        auto bytes = take.image->bytes;
        bytes.resize(ranges.base.size);
        decoded.emplace();
        const bool decodes = DecodeNativeTexture(fetch, bytes, {}, *decoded, error);
        UNSCOPED_INFO("the bytes path decodes: " + error);
        REQUIRE(decodes);

        const bool presented = assembler.Clear(clear(main, { 0, 0, 1280, 720 }, { 0.1F, 0.1F, 0.1F, 1.0F }), error) != 0;
        UNSCOPED_INFO("the presented clear: " + error);
        REQUIRE(presented);
        onScreen               = true;
        const auto sampledMark = submit(2);
        const auto joined      = flushThrough(assembler, source, sampledMark);
        UNSCOPED_INFO("the draw joins a segment: " + joined.error);
        REQUIRE(joined.fence);

        const bool signedCleared = assembler.Clear(clear(signedTarget, { 0, 0, 64, 64 }, { 0.5F, -0.5F, 0, 0 }), error) != 0;
        UNSCOPED_INFO("the k_16_16 clear: " + error);
        REQUIRE(signedCleared);
        const auto reset = assembler.Resolve(signedTarget, signedCopy, false, partial, error);
        UNSCOPED_INFO("the k_16_16 resolve: " + error);
        REQUIRE(reset != 0);
        const auto ended = flushThrough(assembler, source, reset);
        UNSCOPED_INFO("the k_16_16 resolve is a legacy frame that ends the chain: " + ended.error);
        REQUIRE((ended.fence && producer->terminals.size() == 1));

        scanOut(assembler);
        auto swap            = swapRecord(0xD10);
        swap.numberedThrough = 2;
        const auto result    = assembler.Swap(swap);
        UNSCOPED_INFO("the swap presents: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence && source.WaitFor(result.hostFence, std::chrono::seconds(10))));
        const auto& output = producer->terminals.back().get();
        UNSCOPED_INFO("the presented output: " + output.error);
        REQUIRE((output.success && output.resolvedOutput.size() == kPlaneBytes));
        run.shown = output.resolvedOutput;
        run.stats = assembler.ResolveGraphStats();
        UNSCOPED_INFO("the epoch leaves no failure");
        REQUIRE((!assembler.Failure() && !source.Failure()));
        source.Close();
    }
    stream.Shutdown();
    return run;
}

// Alias and bytes paths agree across a legacy reset.
void checkAliasAcrossReset()
{
    const auto bytes = aliasResetRun(false);
    const auto alias = aliasResetRun(true);
    // Bound through the registry, the draw imports the stored surface and
    // nothing re-records.
    const bool registry = surfaceRegistrySwitch();
    UNSCOPED_INFO(std::format("the aliased draw binds once and its producer re-records once ({} hits, {} reproduced)", alias.stats.aliasHits, alias.stats.aliasReproduced));
    REQUIRE((!bytes.stats.aliasHits && alias.stats.aliasHits == (registry ? 0 : 1) && alias.stats.aliasReproduced == (registry ? 0 : 1)));
    UNSCOPED_INFO("the sampled image is not uniform");
    REQUIRE((!bytes.shown.empty() && std::ranges::any_of(std::span(bytes.shown).subspan(4), [&](std::uint8_t value)
                                                         {
                                                             return value != bytes.shown[0];
                                                         })));
    UNSCOPED_INFO("the aliased frame presents the bytes path's output byte for byte");
    REQUIRE(alias.shown == bytes.shown);
    std::cout << "A presented draw bound to a graph resolve presents the bytes path's output across a chain reset\n";
}

struct SurfaceRun
{
    std::array<std::vector<std::uint8_t>, 2> shown;
    ResolveSurfaceCounts                     counts;
    bool                                     castable = false;
};

// Two epochs test Rg16 and packed surface views, including endian and half
// exchange.
SurfaceRun surfaceRun(bool registry)
{
    const auto clear = [](const GuestTarget& target, NativeDrawReplayScissor rectangle, std::array<float, 4> value)
    {
        GuestClear result;
        result.target           = target;
        result.flags            = 1;
        result.clear.rectangle  = rectangle;
        result.clear.color      = true;
        result.clear.colorValue = value;
        return result;
    };
    GuestTarget main, offscreen, signedTarget;
    main.native         = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    offscreen.native    = { 1024, 1024, 1, NativeDrawReplayTargetFormat::Rgba8, 0x300 };
    signedTarget.native = { 64, 64, 1, NativeDrawReplayTargetFormat::Rg16Snorm, 0x900 };
    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destPitch   = 0x04000400;
    copy.destInfo    = 0x01000300;
    copy.destFetch   = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    copy.rectWidth = copy.rectHeight = 1024;
    // RB_COPY_DEST_INFO 0xEF82 as the fog copies: format 31, number 7, endian 2,
    // into a signed k_16_16_FLOAT fetch.
    GuestResolveCopy halfCopy;
    halfCopy.copyControl = 0x100000;
    halfCopy.destBase    = 0x1D800000;
    halfCopy.destPitch   = 0x00400040;
    halfCopy.destInfo    = 0xEF82;
    halfCopy.destFetch   = { 0x80800156, 0x1D80009F, 0x0007E03F, 0xC14, 0, 0x200 };
    halfCopy.rectWidth = halfCopy.rectHeight = 64;
    const std::array<std::uint32_t, 6> fetch{ 0x88000002, 0xFE3B0006, 0x007FE3FF, 0xC14, 0, 0x200 };
    const std::array<std::uint32_t, 6> halfFetch{ 0x80800156, 0xFD80009F, 0x0007E03F, 0xC14, 0, 0x200 };
    // The 8in32 words' bytes, reversed, as unsigned k_8_8_8_8.
    const std::array<std::uint32_t, 6> byteFetch{ 0x80800002, 0xFD800006, 0x0007E03F, 0xC14, 0, 0x200 };
    const std::array<std::uint32_t, 6> swapFetch{ 0x80800156, 0xFD80005F, 0x0007E03F, 0xC14, 0, 0x200 };

    NativeDrawReplayRecipe sampler;
    sampler.schemaVersion                               = 2;
    sampler.width                                       = 1280;
    sampler.height                                      = 720;
    sampler.sampleCount                                 = 4;
    sampler.sampleMask                                  = 9;
    sampler.vertexShaderHash                            = 0xA11A5;
    sampler.pixelShaderHash                             = 0xA11A6;
    sampler.vertexShaderDxil                            = compile("float4 main(float4 p:TEXCOORD0):SV_Position { return p; }", "vs_5_1");
    sampler.pixelShaderDxil                             = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 {"
                                                                  " return t.Load(int3(int2(p.xy) % 1024, 0)); }",
                                                                  "ps_5_1");
    sampler.vertexAttributeCount                        = 1;
    sampler.vertexStrideBytes                           = 16;
    const std::array<std::array<float, 4>, 3> positions = { { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } } };
    sampler.vertexData.resize(sizeof(positions));
    std::memcpy(sampler.vertexData.data(), positions.data(), sizeof(positions));
    sampler.indices    = { 0, 1, 2 };
    sampler.indexCount = 3;
    sampler.vertexConstants.resize(4096);
    sampler.pixelConstants.resize(3584);
    sampler.sharedConstants.resize(336);
    sampler.viewport    = { 0, 0, 1280, 720, 0, 1 };
    sampler.scissor     = { 0, 0, 1280, 720 };
    sampler.textureMask = 1;
    // Signed halves land in an RGBA8 target as 0 or their clamped value.
    auto halfSampler            = sampler;
    halfSampler.scissor         = { 0, 0, 64, 64 };
    auto byteSampler            = halfSampler;
    byteSampler.scissor         = { 64, 0, 128, 64 };
    byteSampler.pixelShaderHash = 0xA11A8;
    byteSampler.pixelShaderDxil = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 {"
                                          " return t.Load(int3(int2(p.xy) % 64, 0)); }",
                                          "ps_5_1");
    auto swapSampler            = byteSampler;
    swapSampler.scissor         = { 128, 0, 192, 64 };
    auto gradient               = sampler;
    gradient.width              = 1024;
    gradient.height             = 1024;
    gradient.sampleCount = gradient.sampleMask = 1;
    gradient.pixelShaderHash                   = 0xA11A7;
    gradient.pixelShaderDxil                   = compile("float4 main(float4 p:SV_Position):SV_Target0 { uint2 q = uint2(p.xy);"
                                                         " return float4(q.x & 255, q.y & 255, (q.x ^ q.y) & 255, 255) / 255.0; }",
                                                         "ps_5_1");
    gradient.viewport                          = { 0, 0, 1024, 1024, 0, 1 };
    gradient.scissor                           = { 0, 0, 1024, 1024 };
    gradient.textureMask                       = 0;

    SurfaceRun          run;
    NativeRendererD3D12 stream;
    std::string         error;
    UNSCOPED_INFO("surface stream: " + error);
    REQUIRE(stream.StartDrawStream(error));
    run.castable = stream.CastableGuestWordTextures();
    {
        auto                       producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource      source({ producer });
        NativeFrameAssemblerConfig config;
        config.completion         = &source;
        config.readback           = true;
        config.resolveGraph       = true;
        config.eagerGraphSegments = true;
        config.castableGuestWords = run.castable;
        config.timing             = true;
        if (registry)
            config.surfaces = std::make_shared<ResolveSurfaceRegistry>();
        const auto surfaces = config.surfaces;
        bool       onScreen = false;
        config.readTarget   = [&onScreen, presented = targetWords(1280, 720, true, 0), offScreen = targetWords(1024, 1024, false, 0x300)](std::uint32_t address, std::uint32_t& out)
        {
            return (onScreen ? presented : offScreen)(address, out);
        };
        config.watchWrites = [](std::uint32_t, std::uint32_t)
        {
        };
        std::optional<NativeDrawReplayTexture> decoded, halfDecoded, byteDecoded, swapDecoded;
        NativeFrameAssembler*                  current = nullptr;
        // Presented sequences 2 + 4n sample the gradient, 3 + 4n the halves,
        // 4 + 4n the halves' bytes and 5 + 4n the halves exchanged.
        config.build = [&](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string&)
        {
            if (!onScreen)
            {
                draw = gradient;
                return true;
            }
            const auto                                                   kind = (value.sequence - 2) % 4;
            const std::array<const NativeDrawReplayRecipe*, 4>           recipes{ &sampler, &halfSampler, &byteSampler, &swapSampler };
            const std::array<const std::array<std::uint32_t, 6>*, 4>     fetches{ &fetch, &halfFetch, &byteFetch, &swapFetch };
            const std::array<std::optional<NativeDrawReplayTexture>*, 4> decodes{ &decoded, &halfDecoded, &byteDecoded, &swapDecoded };
            draw               = *recipes[kind];
            auto       miss    = NativeResolvedTextureMiss::Form;
            const auto texture = current->ResolvedTexture(*fetches[kind], value.operationMark, miss);
            const bool expect  = registry && (kind == 0 || run.castable);
            UNSCOPED_INFO(std::format("draw {} binds a surface only with the registry, miss {}", value.sequence, static_cast<unsigned>(miss)));
            REQUIRE(texture.has_value() == expect);
            draw.textures[0] = texture ? BindNativeResolvedTexture(*texture) : **decodes[kind];
            return true;
        };
        NativeFrameAssembler assembler(config);
        current = &assembler;

        std::uint64_t sequence = 0;
        const auto    submit   = [&](std::uint64_t value)
        {
            NativeDrawSubmission draw;
            draw.hook          = NativeDrawHook::IndexedUp;
            draw.caller        = 0x82303E40;
            draw.stack         = 0x7000;
            draw.arguments     = { kAliasGraphics, 4, 0, 3, 3, 0x3000, 1, std::uint32_t(value) };
            draw.stateRead     = true;
            draw.sequence      = value;
            draw.operationMark = assembler.NumberOperation();
            assembler.Began(draw);
            const auto verdict = assembler.Admit(draw);
            assembler.Recorded(draw, verdict);
            UNSCOPED_INFO(std::format("draw {} is admitted", value));
            REQUIRE(!verdict);
            sequence = std::max(sequence, value);
            return draw.operationMark;
        };
        const auto decodeImage = [&](std::uint64_t mark, std::initializer_list<std::pair<std::array<std::uint32_t, 6>, std::optional<NativeDrawReplayTexture>*>> reads)
        {
            const auto take = assembler.TakeGuestImage(mark);
            UNSCOPED_INFO("the resolve's guest image: " + take.error);
            REQUIRE(take.image.has_value());
            for (const auto& [words, out] : reads)
            {
                NativeTextureMemoryRanges ranges;
                const bool                ranged = GetNativeTextureMemoryRanges(words, ranges, error) && ranges.base.address == take.image->physical;
                UNSCOPED_INFO("the fetch reads the destination: " + error);
                REQUIRE(ranged);
                auto bytes = take.image->bytes;
                bytes.resize(ranges.base.size);
                out->emplace();
                const bool decodes = DecodeNativeTexture(words, bytes, {}, **out, error);
                UNSCOPED_INFO("the bytes path decodes: " + error);
                REQUIRE(decodes);
            }
        };

        const bool cleared = assembler.Clear(clear(offscreen, { 0, 0, 1024, 1024 }, { 1.0F, 0.2F, 0.6F, 0.0F }), error) != 0;
        UNSCOPED_INFO("the off-screen clear: " + error);
        REQUIRE(cleared);
        submit(1);
        bool       partial = false;
        const auto mark    = assembler.Resolve(offscreen, copy, false, partial, error);
        UNSCOPED_INFO("the off-screen resolve: " + error);
        REQUIRE(mark != 0);
        const auto produced = flushThrough(assembler, source, mark);
        UNSCOPED_INFO("the resolve completes: " + produced.error);
        REQUIRE((produced.fence && source.WaitFor(produced.fence, std::chrono::seconds(10))));
        decodeImage(mark, { { fetch, &decoded } });

        // Halves near 0x3814 and 0xB4CD: four distinct bytes, so a misplaced lane shows.
        const bool signedCleared = assembler.Clear(clear(signedTarget, { 0, 0, 64, 64 }, { 0.51F, -0.3F, 0, 0 }), error) != 0;
        UNSCOPED_INFO("the k_16_16 clear: " + error);
        REQUIRE(signedCleared);
        const auto halfMark = assembler.Resolve(signedTarget, halfCopy, false, partial, error);
        UNSCOPED_INFO("the k_16_16 resolve: " + error);
        REQUIRE(halfMark != 0);
        const auto ended = flushThrough(assembler, source, halfMark);
        UNSCOPED_INFO("the k_16_16 legacy frame completes: " + ended.error);
        REQUIRE((ended.fence && source.WaitFor(ended.fence, std::chrono::seconds(10))));
        decodeImage(halfMark, { { halfFetch, &halfDecoded }, { byteFetch, &byteDecoded }, { swapFetch, &swapDecoded } });

        for (std::uint32_t epoch = 0; epoch < 2; ++epoch)
        {
            onScreen         = false;
            const bool shown = assembler.Clear(clear(main, { 0, 0, 1280, 720 }, { 0.1F, 0.1F, 0.1F, 1.0F }), error) != 0;
            onScreen         = true;
            UNSCOPED_INFO("the presented clear: " + error);
            REQUIRE(shown);
            for (std::uint32_t kind = 0; kind < 4; ++kind)
                submit(2 + 4 * epoch + kind);
            scanOut(assembler);
            auto swap            = swapRecord(0xD10);
            swap.sequence        = epoch + 1;
            swap.numberedThrough = sequence;
            const auto result    = assembler.Swap(swap);
            UNSCOPED_INFO(std::format("swap {} presents: {}", epoch + 1, result.detail));
            REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence && source.WaitFor(result.hostFence, std::chrono::seconds(10))));
            const auto& output = producer->terminals.back().get();
            UNSCOPED_INFO("the presented output: " + output.error);
            REQUIRE((output.success && output.resolvedOutput.size() == kPlaneBytes));
            run.shown[epoch] = output.resolvedOutput;
        }
        if (surfaces)
            run.counts = surfaces->TakeCounts();
        UNSCOPED_INFO("the epochs leave no failure");
        REQUIRE((!assembler.Failure() && !source.Failure()));
        source.Close();
    }
    stream.Shutdown();
    return run;
}

// Packed-view registry and bytes paths agree across epochs.
void checkSurfaceRegistry()
{
    const auto bytes   = surfaceRun(false);
    const auto bound   = surfaceRun(true);
    const auto expects = bound.castable ? 8U : 2U;
    UNSCOPED_INFO(std::format("the surface draws bind {} times over 2 entries ({} hits, {} misses, {} entries)", expects, bound.counts.hits, bound.counts.misses, bound.counts.entries));
    REQUIRE((bound.counts.hits == expects && bound.counts.entries == 2));
    for (std::size_t epoch = 0; epoch < 2; ++epoch)
    {
        UNSCOPED_INFO("the sampled image is not uniform");
        REQUIRE(std::ranges::any_of(std::span(bytes.shown[epoch]).subspan(4), [&](std::uint8_t value)
                                    {
                                        return value != bytes.shown[epoch][0];
                                    }));
        UNSCOPED_INFO(std::format("epoch {}: the surface frame presents the bytes path's output byte for byte", epoch + 1));
        REQUIRE(bound.shown[epoch] == bytes.shown[epoch]);
    }
    std::cout << "Draws bound to resolve surfaces present the bytes path's output across epochs (" << bound.counts.hits << " hits)\n";
}

class QueuedSurfaceConsumer
{
public:
    QueuedSurfaceConsumer(NativeFrameAssembler& assembler, std::shared_ptr<ResolveSurfaceRegistry> surfaces, rerevved::tests::CallbackChecks& checks)
    : surfaces(std::move(surfaces))
    , previous(PublishedResolveSurfaces())
    , dispatcher(makeConfig(assembler, checks))
    {
        REQUIRE(InstallNativeDrawDispatcher(dispatcher));
        PublishResolveSurfaces(this->surfaces);
    }

    ~QueuedSurfaceConsumer()
    {
        Release();
        surfaces->SetBarrier(nullptr);
        (void)RemoveNativeDrawDispatcher(std::chrono::seconds(10));
        PublishResolveSurfaces(previous);
    }

    void Queue(std::uint32_t kind)
    {
        PPCContext ctx{};
        ctx.r1.u32        = 0x7000;
        ctx.r3.u32        = kAliasGraphics;
        ctx.r10.u32       = kind;
        ctx.lr            = 0x82303E40;
        std::uint8_t base = 0;
        {
            const NativeDrawScope scope(NativeDrawHook::IndexedBuffer, ctx, &base);
            REQUIRE(scope.Root());
        }
    }

    void WaitEntered()
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(10), [&]
                                 {
                                     return entered;
                                 }));
    }

    void Release()
    {
        {
            std::lock_guard lock(mutex);
            released = true;
        }
        changed.notify_all();
    }

    NativeDrawSequencerStats Stats() const
    {
        return dispatcher.Sequencer();
    }

private:
    NativeDrawDispatchConfig makeConfig(NativeFrameAssembler& assembler, rerevved::tests::CallbackChecks& checks)
    {
        NativeDrawDispatchConfig config;
        assembler.Attach(config);
        config.sequenced = true;
        // The authored shaders need no guest payload. The scope still reads
        // state and posts through the installed production dispatcher.
        config.read = [](const std::uint8_t*, std::uint32_t, std::uint32_t& value)
        {
            value = 0;
            return true;
        };
        config.defer = [](const NativeDrawSubmission&) -> std::shared_ptr<const void>
        {
            return std::make_shared<int>(1);
        };
        config.began = [this, began = std::move(config.began), &checks](const NativeDrawSubmission& draw)
        {
            {
                std::unique_lock lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&]
                             {
                                 return released;
                             });
            }
            checks.Require(draw.deferral != nullptr, "the surface consumer runs on the sequencer");
            began(draw);
        };
        config.recorded = [recorded = std::move(config.recorded), &checks](const NativeDrawSubmission& draw, std::optional<NativeDrawReason> verdict)
        {
            recorded(draw, verdict);
            checks.Require(!verdict, "the queued surface consumer is admitted");
        };
        config.failed = [&checks](const NativeDrawFailure&)
        {
            checks.Require(false, "the queued surface consumer must not fail dispatch");
        };
        return config;
    }

    std::shared_ptr<ResolveSurfaceRegistry> surfaces, previous;
    std::mutex                              mutex;
    std::condition_variable                 changed;
    bool                                    entered = false, released = false;
    NativeDrawDispatcher                    dispatcher;
};

struct OrderedSurfaceInputs
{
    static constexpr std::uint32_t                              width = 1280, height = 720;
    static constexpr std::array<std::uint32_t, 6>               fetch{ 0x8A000002, 0x1DBB0086, 0x0059E4FF, 0x00A80C14, 0x00000003, 0x00000218 };
    static constexpr std::array<std::array<std::uint8_t, 4>, 2> colors{ { { 17, 71, 163, 217 }, { 210, 107, 37, 143 } } };
    std::array<std::vector<std::uint8_t>, 2>                    expected;
    std::array<NativeDrawReplayRecipe, 2>                       producers;
    NativeDrawReplayRecipe                                      sampler;

    OrderedSurfaceInputs()
    {
        for (std::size_t version = 0; version < expected.size(); ++version)
        {
            expected[version].resize(kPlaneBytes);
            for (std::uint32_t y = 0; y < height; ++y)
            {
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const std::array<std::uint32_t, 4> offset{ 3 * x + 5 * y, 7 * x + 11 * y, 13 * x + 19 * y, 0 };
                    for (std::size_t c = 0; c < 4; ++c)
                    {
                        expected[version][(std::size_t(y) * width + x) * 4 + c] = static_cast<std::uint8_t>(colors[version][c] + offset[c]);
                    }
                }
            }
        }
        NativeDrawReplayRecipe producerDraw;
        producerDraw.schemaVersion = 2;
        producerDraw.width         = width;
        producerDraw.height        = height;
        producerDraw.sampleCount = producerDraw.sampleMask = 1;
        producerDraw.vertexShaderHash                      = 0xA11C5;
        producerDraw.vertexShaderDxil                      = compile("float4 main(float4 p:TEXCOORD0):SV_Position { return p; }", "vs_5_1");
        producerDraw.vertexAttributeCount                  = 1;
        producerDraw.vertexStrideBytes                     = 16;
        const std::array<std::array<float, 4>, 3> positions{ { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } } };
        producerDraw.vertexData.resize(sizeof(positions));
        std::memcpy(producerDraw.vertexData.data(), positions.data(), sizeof(positions));
        producerDraw.indices    = { 0, 1, 2 };
        producerDraw.indexCount = 3;
        producerDraw.vertexConstants.resize(4096);
        producerDraw.pixelConstants.resize(3584);
        producerDraw.sharedConstants.resize(336);
        producerDraw.viewport = { 0, 0, width, height, 0, 1 };
        producerDraw.scissor  = { 0, 0, width, height };
        producers             = { producerDraw, producerDraw };
        for (std::size_t version = 0; version < producers.size(); ++version)
        {
            producers[version].pixelShaderHash = 0xA11C6 + version;
            producers[version].pixelShaderDxil = compile(std::format(
                                                             "float4 main(float4 p:SV_Position):SV_Target0 {{ uint2 q=uint2(p.xy);"
                                                             " return float4((uint3({},{},{})+uint3(3*q.x+5*q.y,7*q.x+11*q.y,13*q.x+19*q.y))&255,{})/255.0; }}",
                                                             colors[version][0],
                                                             colors[version][1],
                                                             colors[version][2],
                                                             colors[version][3])
                                                             .c_str(),
                                                         "ps_5_1");
        }
        sampler                 = producerDraw;
        sampler.pixelShaderHash = 0xA11C8;
        sampler.pixelShaderDxil = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(int2(p.xy),0)); }", "ps_5_1");
        sampler.textureMask     = 1;
    }

    void CheckPixels(std::span<const std::uint8_t> bytes, std::uint32_t rowPitch, std::size_t version) const
    {
        REQUIRE((rowPitch >= width * 4 && bytes.size() >= std::size_t(rowPitch) * height));
        for (std::uint32_t y = 0; y < height; ++y)
        {
            INFO(std::format("version {} row {}", version + 1, y));
            REQUIRE(std::equal(expected[version].begin() + std::size_t(y) * width * 4,
                               expected[version].begin() + std::size_t(y + 1) * width * 4,
                               bytes.begin() + std::size_t(y) * rowPitch));
        }
    }
};

// Both producer routes retain the actual A binding when B uses the same range.
void checkOrderedSurfaceVersions(bool graph = false, bool staged = false)
{
    using Microsoft::WRL::ComPtr;
    const OrderedSurfaceInputs inputs;
    constexpr auto             width = OrderedSurfaceInputs::width, height = OrderedSurfaceInputs::height;
    const auto&                fetch       = OrderedSurfaceInputs::fetch;
    const auto&                producers   = inputs.producers;
    const auto&                sampler     = inputs.sampler;
    const auto                 checkPixels = [&](std::span<const std::uint8_t> bytes, std::uint32_t pitch, std::size_t version)
    {
        inputs.CheckPixels(bytes, pitch, version);
    };

    std::string               error;
    NativeTextureMemoryRanges ranges;
    const bool                ranged = GetNativeTextureMemoryRanges(fetch, ranges, error);
    UNSCOPED_INFO("the authored destination layout: " + error);
    REQUIRE((ranged && ranges.base.address == 0x1DBB0000 && ranges.base.size == 0x398000 &&
             std::uint64_t(ranges.base.address) + ranges.base.size == 0x1DF48000 && ranges.mips.size == 0));

    std::uint64_t        offers = 0;
    NativeRendererD3D12  stream;
    ComPtr<ID3D12Device> shared;
    REQUIRE(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&shared))));
    const auto publish = [&offers](ID3D12Resource*, ID3D12Fence*, std::uint64_t)
    {
        auto lease = std::make_shared<NativeOutputLease>(++offers);
        lease->Cancel();
        return lease;
    };
    UNSCOPED_INFO("the ordered surface stream: " + error);
    REQUIRE(stream.StartTitleDrawStream(error, shared.Get(), publish));
    if (staged && !stream.CastableGuestWordTextures())
    {
        stream.Shutdown();
        SKIP("The actual D3D12 device does not support castable guest word textures");
    }
    {
        auto producer             = std::make_shared<RendererProducer>(stream);
        producer->inspectSurfaces = true;
        producer->inspectGraph    = graph;
        D3D12CompletionSource      source({ producer });
        auto                       surfaces = std::make_shared<ResolveSurfaceRegistry>();
        NativeFrameAssemblerConfig config;
        config.completion                   = &source;
        config.resolveGraph                 = graph;
        config.eagerGraphSegments           = graph;
        config.resolveAlias                 = graph;
        config.timing                       = graph;
        config.castableGuestWords           = stream.CastableGuestWordTextures();
        config.surfaces                     = surfaces;
        bool                  tiledProducer = false;
        const GuestWordReader tiledWords    = [words = targetWords(width, height, false, 0x300)](std::uint32_t address, std::uint32_t& out)
        {
            const auto offset = address - kAliasGraphics;
            switch (offset)
            {
                case 0x2ABC:
                    out = 0x20000000;
                    return true;
                case 0x31A4:
                    out = 0xF;
                    return true;
                case 0x31AC:
                    out = 1;
                    return true;
                case 0x31CC:
                    out = 2;
                    return true;
                case 0x337C:
                    out = width;
                    return true;
                case 0x3380:
                    out = height;
                    return true;
                default:
                    break;
            }
            if (offset >= 0x31B8 && offset <= 0x31C8 && offset % 4 == 0)
            {
                out = offset == 0x31B8 ? kAliasSurface : 0;
                return true;
            }
            constexpr std::array<std::uint32_t, 8> rects{ 0, 0, width / 2, height, width / 2, 0, width, height };
            if (offset >= 0x31D0 && offset < 0x31F0 && offset % 4 == 0)
            {
                out = rects[(offset - 0x31D0) / 4];
                return true;
            }
            return words(address, out);
        };
        config.readTarget = [&tiledProducer, tiledWords, words = targetWords(width, height, false, 0)](std::uint32_t address, std::uint32_t& out)
        {
            return (tiledProducer ? tiledWords : words)(address, out);
        };
        if (staged)
        {
            config.coverage = [](const NativeDrawSubmission& value, const NativeDrawReplayRecipe& draw) -> std::optional<NativeFrameDrawCover>
            {
                if (value.arguments[7] != 3)
                    return std::nullopt;
                // The authored triangle writes every sample without blending,
                // depth, stencil, or pixel kill.
                return NativeFrameDrawCover{ draw.scissor };
            };
        }
        config.watchWrites = [](std::uint32_t, std::uint32_t)
        {
        };
        NativeFrameAssembler*           current = nullptr;
        std::array<std::uint64_t, 2>    boundIds{};
        rerevved::tests::CallbackChecks callbackChecks;
        config.build = [&](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string&)
        {
            const auto kind = (staged ? value.arguments[7] : value.sequence) - 1;
            if (kind % 2 == 0)
            {
                draw = producers[kind / 2];
            }
            else
            {
                draw               = sampler;
                auto       miss    = NativeResolvedTextureMiss::Form;
                const auto texture = current->ResolvedTexture(fetch, value.operationMark, miss);
                callbackChecks.Require(texture.has_value(), "the ordered consumer binds a committed surface");
                callbackChecks.Require((texture->layout.width == width && texture->layout.height == height &&
                                        texture->layout.format == NativeDrawReplayTextureFormat::Rgba8 &&
                                        texture->swizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 }),
                                       "the ordered consumer uses the authored RGBA8 view");
                boundIds[kind / 2] = texture->frameTextureId;
                draw.textures[0]   = BindNativeResolvedTexture(*texture);
            }
            return true;
        };
        NativeFrameAssembler assembler(config);
        current = &assembler;
        GuestTarget target;
        target.native = { width, height, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
        GuestTarget main;
        main.native      = { width, height, 4, NativeDrawReplayTargetFormat::Rgba8, 0x900 };
        const auto clear = [&](const GuestTarget& destination)
        {
            GuestClear value;
            value.target          = destination;
            value.flags           = 1;
            value.clear.rectangle = { 0, 0, width, height };
            value.clear.color     = true;
            const auto mark       = assembler.Clear(value, error);
            UNSCOPED_INFO("the ordered target clear: " + error);
            REQUIRE(mark != 0);
        };
        const auto scanout = [&]
        {
            GuestResolveCopy copy;
            copy.copyControl   = 0x00100040;
            copy.destBase      = kFrontbuffer;
            copy.destPitch     = kScanoutDestPitch;
            copy.destInfo      = 0x300;
            copy.rectWidth     = width;
            copy.rectHeight    = height;
            bool       partial = false;
            const auto mark    = assembler.Resolve(main, copy, true, partial, error);
            UNSCOPED_INFO("the ordered scanout: " + error);
            REQUIRE(mark != 0);
        };
        const auto submitDraw = [&](std::uint64_t sequence)
        {
            NativeDrawSubmission draw;
            draw.hook          = NativeDrawHook::IndexedUp;
            draw.caller        = 0x82303E40;
            draw.stack         = 0x7000;
            draw.arguments     = { kAliasGraphics, 4, 0, 3, 3, 0x3000, 1, std::uint32_t(sequence) };
            draw.stateRead     = true;
            draw.sequence      = sequence;
            draw.operationMark = assembler.NumberOperation();
            assembler.Began(draw);
            const auto verdict = assembler.Admit(draw);
            assembler.Recorded(draw, verdict);
            callbackChecks.Verify();
            REQUIRE(!verdict);
        };
        GuestResolveCopy copy;
        copy.copyControl   = 0x00100000;
        copy.destBase      = 0x1DBB0000;
        copy.destPitch     = 0x02D00500;
        copy.destInfo      = 0x01000302;
        copy.destFetch     = fetch;
        copy.rectWidth     = width;
        copy.rectHeight    = height;
        const auto resolve = [&](std::uint32_t base)
        {
            auto destination         = copy;
            destination.destBase     = base;
            destination.destFetch[1] = base | 0x86;
            bool       partial       = false;
            const auto mark          = assembler.Resolve(target, destination, false, partial, error);
            UNSCOPED_INFO("the ordered resolve: " + error);
            REQUIRE((mark && !partial));
            return mark;
        };
        const auto complete = [&](std::uint64_t mark)
        {
            const auto flush = flushThrough(assembler, source, mark);
            UNSCOPED_INFO("the ordered resolve completes: " + flush.error);
            REQUIRE((flush.fence && source.WaitFor(flush.fence, std::chrono::seconds(10))));
            return (graph ? producer->prefixRecordings.back() : producer->terminals.back()).get().completionFenceValue;
        };
        const auto binding = [&](std::uint64_t mark, std::uint64_t version)
        {
            const auto found = surfaces->Find(fetch, false);
            REQUIRE((found && std::holds_alternative<ResolveSurfaceBinding>(*found)));
            const auto& value = std::get<ResolveSurfaceBinding>(*found);
            REQUIRE((value.mark == mark && value.version == version &&
                     value.texture.frameTextureId == (std::uint64_t(3) << 60) + mark));
            const auto imported = surfaces->Import(value.texture.frameTextureId);
            REQUIRE((imported && imported->persistentKey == ((std::uint64_t(1) << 63) | mark) &&
                     imported->importedWords == NativeGuestWordPacking::None));
        };
        const auto checkBytes = [&](std::uint64_t mark, std::size_t version)
        {
            const auto take = assembler.TakeGuestImage(mark);
            UNSCOPED_INFO("the ordered guest image: " + take.error);
            REQUIRE((take.image && take.image->physical == copy.destBase));
            std::vector<std::uint8_t> memory(ranges.base.size, 0xE7);
            for (const auto& run : take.image->runs)
            {
                REQUIRE(run.offset + run.length <= memory.size());
                std::copy_n(take.image->bytes.begin() + run.offset, run.length, memory.begin() + run.offset);
            }
            NativeDrawReplayTexture decoded;
            const bool              decodes = DecodeNativeTexture(fetch, memory, {}, decoded, error);
            UNSCOPED_INFO("the ordered guest image decodes: " + error);
            REQUIRE((decodes && decoded.width == width && decoded.height == height && decoded.payloadBytes().size() == kPlaneBytes));
            REQUIRE(std::ranges::all_of(decoded.swizzle, [](std::uint8_t component)
                                        {
                                            return component < 4;
                                        }));
            std::vector<std::uint8_t> logical(kPlaneBytes);
            for (std::size_t pixel = 0; pixel < kPlaneBytes / 4; ++pixel)
            {
                for (std::size_t c = 0; c < 4; ++c)
                {
                    logical[pixel * 4 + c] = decoded.payloadBytes()[pixel * 4 + decoded.swizzle[c]];
                }
            }
            checkPixels(logical, width * 4, version);
            assembler.GuestImageWritten(mark);
        };

        if (graph)
        {
            constexpr auto graphBase   = std::uint64_t(1) << 62;
            const auto     graphPixels = [&](std::size_t index, std::uint64_t mark, std::size_t version)
            {
                const auto& frame  = producer->prefixFrames.at(index);
                const auto& output = producer->prefixRecordings.at(index).get();
                REQUIRE((output.success && output.completionFenceValue && producer->CompletedFence() >= output.completionFenceValue));
                REQUIRE(std::ranges::any_of(frame.surfaces, [&](const NativeRenderFrameSurface& surface)
                                            {
                                                return surface.textureId == graphBase + mark && surface.key == ((std::uint64_t(1) << 63) | mark);
                                            }));
                const auto readback = std::ranges::find_if(output.rawReadbacks, [&](const auto& value)
                                                           {
                                                               return value.first == graphBase + mark;
                                                           });
                REQUIRE((readback != output.rawReadbacks.end() && readback->second));
                std::vector<std::uint8_t> pixels;
                std::uint32_t             rowPitch = 0;
                REQUIRE(readback->second->Read(pixels, rowPitch, error));
                checkPixels(pixels, rowPitch, version);
            };
            const auto swap = [&](std::uint64_t sequence)
            {
                auto value            = swapRecord(0xD10);
                value.sequence        = sequence;
                value.numberedThrough = 4;
                const auto result     = assembler.Swap(value);
                UNSCOPED_INFO("the graph epoch presents: " + result.detail);
                REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence));
                return result.hostFence;
            };

            // Keep the source containing A's consumer across the first swap.
            clear(main);
            scanout();
            clear(target);
            submitDraw(1);
            const auto a = resolve(copy.destBase);
            REQUIRE((producer->prefixFrames.size() == 1 && producer->terminals.empty()));
            const auto aFence = complete(a);
            const auto keyA   = (std::uint64_t(1) << 63) | a;
            binding(a, 1);
            graphPixels(0, a, 0);
            checkBytes(a, 0);

            // The captured consumer pass owns its clear. Replay needs no
            // producer work or attachments from the original graph chain.
            clear(target);
            submitDraw(2);
            const auto consumerA = resolve(0x1C000000);
            REQUIRE(producer->prefixFrames.size() == 2);
            const auto consumerAFence = complete(consumerA);
            graphPixels(1, consumerA, 0);
            const auto& captured = producer->capturedFrame.emplace(producer->prefixFrames[1]);
            REQUIRE((captured.passes.size() == 1 && captured.passes[0].DrawCount() == 1 &&
                     !captured.passes[0].clears.empty() && !captured.passes[0].preservePreviousAttachments && !captured.passes[0].resumeAttachments &&
                     captured.passes[0].Draw(0).textures[0].frameTextureId == ResolveSurfaceTexture(a)));

            submitDraw(3);
            const auto b = resolve(copy.destBase);
            REQUIRE((producer->prefixFrames.size() == 3 && producer->terminals.empty()));
            const auto bFence = complete(b);
            const auto keyB   = (std::uint64_t(1) << 63) | b;
            REQUIRE((b > consumerA && consumerAFence > aFence && bFence > consumerAFence));
            binding(b, 2);
            graphPixels(2, b, 1);
            checkBytes(b, 1);
            REQUIRE(surfaces->Import(ResolveSurfaceTexture(a)));

            submitDraw(4);
            const auto consumerB = resolve(0x1C400000);
            REQUIRE((producer->prefixFrames.size() == 4 && producer->terminals.empty()));
            const auto consumerBFence = complete(consumerB);
            REQUIRE((consumerBFence > bFence &&
                     boundIds[0] == ResolveSurfaceTexture(a) && boundIds[1] == ResolveSurfaceTexture(b)));
            graphPixels(3, consumerB, 1);
            const auto stats = assembler.ResolveGraphStats();
            REQUIRE((stats.replays == 2 && stats.continuations == 2 && !stats.notReady && !stats.noSource && !stats.swapBatch));

            const auto heldFence = swap(1);
            REQUIRE(source.WaitFor(heldFence, std::chrono::seconds(10)));
            REQUIRE((producer->tickets.size() == 1 && surfaces->Import(boundIds[0]) &&
                     std::ranges::find(producer->released.back(), keyA) == producer->released.back().end()));
            const auto terminalFence = producer->terminals.back().get().completionFenceValue;
            REQUIRE(terminalFence > consumerBFence);
            ID3D12Resource* aResource = nullptr;
            ID3D12Resource* bResource = nullptr;
            auto            aOwner    = rerevved::gpu::detail::NativeRenderFrameTestAccess::CompletedTexture(stream, producer->tickets.back()->Export(keyA), aResource);
            auto            bOwner    = rerevved::gpu::detail::NativeRenderFrameTestAccess::CompletedTexture(stream, producer->tickets.back()->Export(keyB), bResource);
            REQUIRE((aOwner && bOwner && aResource && bResource && aOwner != bOwner && aResource != bResource));
            const std::weak_ptr<const void> weakA = aOwner;
            const std::weak_ptr<const void> weakB = bOwner;
            aOwner.reset();
            bOwner.reset();

            std::uint64_t replayFence = 0;
            {
                auto retainedFrame = captured;
                REQUIRE((retainedFrame.passes[0].resolves.size() == 1 && retainedFrame.passes[0].resolves[0].readback));
                // Only prefix-to-terminal output transport changes. The
                // admitted draw and A import remain those actually bound.
                retainedFrame.passes[0].resolves[0].readback = false;
                retainedFrame.outputTextureId                = graphBase + consumerA;
                retainedFrame.retainOnly = retainedFrame.returnRawOutput = true;
                retainedFrame.surfaces.clear();
                auto output = std::ranges::find(retainedFrame.textures, retainedFrame.outputTextureId, &NativeRenderFrameTexture::id);
                REQUIRE((output != retainedFrame.textures.end() && !output->imported && !output->persistentKey));
                output->persistentKey = (std::uint64_t(0x1C000000) << 32) | consumerA;
                const bool valid      = ValidateNativeRenderFrame(retainedFrame, error);
                UNSCOPED_INFO("the standalone graph-bound A frame: " + error);
                REQUIRE(valid);
                REQUIRE(retainedFrame.passes[0].admittedDraws == captured.passes[0].admittedDraws);
                for (const auto& texture : captured.textures)
                {
                    if (texture.imported)
                    {
                        REQUIRE(std::ranges::find(retainedFrame.textures, texture) != retainedFrame.textures.end());
                    }
                }
                std::shared_ptr<NativeRenderFrameTicket> ticket;
                REQUIRE(stream.QueueRenderFrame(std::move(retainedFrame), ticket, error));
                const auto retainedA = ticket->Completion().get();
                UNSCOPED_INFO("the graph-bound A recipe after B: " + retainedA.error);
                REQUIRE((retainedA.success && retainedA.completionFenceValue > terminalFence && retainedA.completionFenceValue > bFence));
                replayFence = retainedA.completionFenceValue;
                checkPixels(retainedA.rawOutput, retainedA.rawRowPitch, 0);
                ID3D12Resource* replayResource = nullptr;
                auto            replayOwner    = rerevved::gpu::detail::NativeRenderFrameTestAccess::CompletedTexture(stream, ticket->Export(keyA), replayResource);
                REQUIRE((replayOwner && replayOwner == weakA.lock() && replayResource == aResource && !weakB.expired()));
            }
            producer->capturedFrame.reset();
            producer->prefixFrames.clear();

            clear(main);
            scanout();
            auto*                queue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(stream);
            ComPtr<ID3D12Device> device;
            ComPtr<ID3D12Fence>  gate;
            REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))));

            struct ReleaseGate
            {
                ID3D12Fence* gate;

                ~ReleaseGate()
                {
                    gate->Signal(1);
                }
            } release{ gate.Get() };

            const auto completedBeforeRelease = producer->CompletedFence();
            REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)));
            const auto releasedFence = swap(2);
            REQUIRE((std::ranges::find(producer->released.back(), keyA) != producer->released.back().end() && !surfaces->Import(boundIds[0])));
            const auto recording = producer->terminalRecordings.back().get();
            UNSCOPED_INFO("the graph A release recording: " + recording.error);
            REQUIRE((recording.success && producer->terminals.back().wait_for(std::chrono::seconds(0)) == std::future_status::timeout &&
                     producer->CompletedFence() == completedBeforeRelease && !weakA.expired()));
            REQUIRE(SUCCEEDED(gate->Signal(1)));
            REQUIRE(source.WaitFor(releasedFence, std::chrono::seconds(10)));
            const auto releaseNativeFence = producer->terminals.back().get().completionFenceValue;
            REQUIRE((releaseNativeFence > replayFence && producer->CompletedFence() >= releaseNativeFence));
            producer->tickets.clear();
            for (std::uint64_t epoch = 3; epoch <= 6 && !weakA.expired(); ++epoch)
            {
                clear(main);
                scanout();
                REQUIRE(source.WaitFor(swap(epoch), std::chrono::seconds(10)));
            }
            REQUIRE(weakA.expired());
            std::cout << std::format("Graph surfaces A={} B={}; native fences A={} consumerA={} B={} consumerB={} terminal={} retainedA={} release={}; exact A/B pixels and owner retirement passed\n",
                                     a,
                                     b,
                                     aFence,
                                     consumerAFence,
                                     bFence,
                                     consumerBFence,
                                     terminalFence,
                                     replayFence,
                                     releaseNativeFence);
        }
        else
        {
            // The scanout precedes off-screen work so Swap can retain that source.
            clear(main);
            scanout();
            clear(target);
            submitDraw(1);
            const auto a      = resolve(copy.destBase);
            const auto keyA   = (std::uint64_t(1) << 63) | a;
            const auto aFence = complete(a);
            binding(a, 1);
            checkBytes(a, 0);
            ID3D12Resource* aResource = nullptr;
            auto            aOwner    = rerevved::gpu::detail::NativeRenderFrameTestAccess::CompletedTexture(stream, producer->tickets[0]->Export((std::uint64_t(copy.destBase) << 32) | a), aResource);
            REQUIRE((aOwner && aResource));
            const std::weak_ptr<const void> weakA = aOwner;
            aOwner.reset();

            std::uint64_t                          consumerA = 0, b = 0, bFence = 0;
            std::size_t                            consumerAIndex = 1;
            std::unique_ptr<QueuedSurfaceConsumer> queued;
            if (staged)
            {
                tiledProducer = true;
                submitDraw(3);
                GuestTarget tiledTarget;
                REQUIRE(ReadGuestTarget(config.readTarget, kAliasGraphics, 0, tiledTarget));
                REQUIRE(tiledTarget.tiling.has_value());
                const auto tile = [&](std::uint32_t index)
                {
                    auto destination  = copy;
                    destination.rectX = destination.destX = index * width / 2;
                    destination.rectWidth                 = width / 2;
                    tiledTarget.tiling->predicateMask     = 3U << (2 * index);
                    bool       partial                    = false;
                    const auto mark                       = assembler.Resolve(tiledTarget, destination, false, partial, error);
                    UNSCOPED_INFO("the staged packed tile resolve: " + error);
                    REQUIRE((mark && !partial));
                    return mark;
                };
                const auto checkTileStorage = [&](std::uint64_t mark)
                {
                    ID3D12Resource* resource = nullptr;
                    const auto      owner    = rerevved::gpu::detail::NativeRenderFrameTestAccess::CompletedTexture(stream,
                                                                                                                    producer->tickets.back()->Export((std::uint64_t(copy.destBase) << 32) | mark),
                                                                                                                    resource);
                    REQUIRE((owner && resource && resource->GetDesc().Format == DXGI_FORMAT_R32_UINT &&
                             resource->GetDesc().Width == width / 2 && resource->GetDesc().Height == height));
                };
                b                    = tile(0);
                const auto leftFence = complete(b);
                checkTileStorage(b);
                const auto right = tile(1);
                REQUIRE(right > b);
                const auto pending = surfaces->View(right);
                REQUIRE((pending && !pending->committed && !pending->admitted && pending->width == width && pending->height == height));
                binding(a, 1);

                tiledProducer = false;
                clear(main);
                scanout();
                clear(target);
                queued = std::make_unique<QueuedSurfaceConsumer>(assembler, surfaces, callbackChecks);
                queued->Queue(2);
                queued->WaitEntered();
                REQUIRE(boundIds[0] == 0);
                unsigned barriers          = 0;
                bool     boundBeforeCommit = false;
                surfaces->SetBarrier([&]
                                     {
                                         ++barriers;
                                         queued->Release();
                                         DrainNativeDraws();
                                         const auto stillStaged = surfaces->View(right);
                                         boundBeforeCommit      = boundIds[0] == ResolveSurfaceTexture(a) && stillStaged && !stillStaged->committed && !stillStaged->admitted;
                                     });
                ObserveResolveSurfaceTiling();
                surfaces->SetBarrier(nullptr);
                callbackChecks.Verify();
                REQUIRE((barriers == 1 && boundBeforeCommit && !surfaces->TilesPending()));
                const auto unavailable = surfaces->Find(fetch, stream.CastableGuestWordTextures());
                REQUIRE((unavailable && std::holds_alternative<NativeResolvedTextureMiss>(*unavailable) &&
                         std::get<NativeResolvedTextureMiss>(*unavailable) == NativeResolvedTextureMiss::Unmodeled &&
                         !surfaces->Import(ResolveSurfaceTexture(b))));
                const auto committed = surfaces->View(right);
                REQUIRE((committed && committed->committed && !committed->admitted && committed->version == 2));
                bFence = complete(right);
                REQUIRE(bFence > leftFence);
                checkTileStorage(right);
                const auto found = surfaces->Find(fetch, stream.CastableGuestWordTextures());
                REQUIRE((found && std::holds_alternative<ResolveSurfaceBinding>(*found)));
                const auto& value = std::get<ResolveSurfaceBinding>(*found);
                REQUIRE((value.mark == b && value.version == 2 && value.texture.frameTextureId == ResolveSurfaceTexture(b)));
                const auto imported = surfaces->Import(value.texture.frameTextureId);
                REQUIRE((imported && imported->persistentKey == ResolveSurfaceKey(b) && imported->width == width && imported->height == height &&
                         imported->format == NativeDrawReplayTextureFormat::R32Uint && imported->importedWords == NativeGuestWordPacking::Color8888));
                const auto records = surfaces->HostCopy(ranges.base.address, std::uint64_t(ranges.base.address) + ranges.base.size);
                REQUIRE((records.size() == 2 && records[0].mark == b && records[1].mark == right));
                for (std::size_t index = 0; index < records.size(); ++index)
                {
                    REQUIRE((records[index].staged && records[index].packing == NativeGuestWordPacking::Color8888 &&
                             records[index].copy.packing == GuestResolvePacking::Color8888Words));
                }
                // The real draw was bound before commit. Its GPU work follows
                // the pending tile frames through the supported stream chain.
                consumerA               = resolve(0x1C000000);
                producer->captureOutput = consumerA;
                consumerAIndex          = producer->terminals.size();
                REQUIRE(complete(consumerA) > bFence);
            }
            else
            {
                submitDraw(2);
                consumerA               = resolve(0x1C000000);
                producer->captureOutput = consumerA;
                submitDraw(3);
                b = resolve(copy.destBase);
                REQUIRE(b > consumerA);
                bFence = complete(b);
                binding(b, 2);
                checkBytes(b, 1);
            }
            REQUIRE(surfaces->Import(ResolveSurfaceTexture(a)));
            REQUIRE(producer->terminals.size() == (staged ? 4 : 3));
            const auto& aOutput = producer->terminals[consumerAIndex].get();
            REQUIRE((aOutput.success && aOutput.completionFenceValue > aFence &&
                     (staged ? aOutput.completionFenceValue > bFence : aOutput.completionFenceValue < bFence)));
            checkPixels(aOutput.rawOutput, aOutput.rawRowPitch, 0);
            // The producer export and the registry-key import name the same owner.
            ID3D12Resource* importedAResource = nullptr;
            auto            importedAOwner    = rerevved::gpu::detail::NativeRenderFrameTestAccess::CompletedTexture(stream, producer->tickets[consumerAIndex]->Export(keyA), importedAResource);
            REQUIRE((importedAOwner && importedAOwner == weakA.lock() && importedAResource == aResource));
            importedAOwner.reset();
            ID3D12Resource*           bResource = nullptr;
            std::weak_ptr<const void> weakB;
            if (!staged)
            {
                auto bOwner = rerevved::gpu::detail::NativeRenderFrameTestAccess::CompletedTexture(stream, producer->tickets[2]->Export((std::uint64_t(copy.destBase) << 32) | b), bResource);
                REQUIRE((bOwner && bResource && aResource != bResource && !weakA.expired()));
                weakB = bOwner;
            }
            REQUIRE(producer->capturedFrame.has_value());
            const auto replayA = [&]
            {
                auto retainedFrame = *producer->capturedFrame;
                REQUIRE((retainedFrame.passes.size() == 1 && !retainedFrame.passes[0].clears.empty() &&
                         !retainedFrame.passes[0].preservePreviousAttachments && !retainedFrame.passes[0].resumeAttachments));
                // The earlier presented prefix was consumed. A's own pass is
                // self-contained. Replay its unchanged bound draws after B completes.
                retainedFrame.continuedPrefixPasses = 0;
                const bool valid                    = ValidateNativeRenderFrame(retainedFrame, error);
                UNSCOPED_INFO("the standalone retained A frame: " + error);
                REQUIRE(valid);
                const auto retainedA = stream.SubmitRenderFrame(retainedFrame);
                UNSCOPED_INFO("the retained A frame after B: " + retainedA.error);
                REQUIRE((retainedA.success && retainedA.completionFenceValue > bFence));
                checkPixels(retainedA.rawOutput, retainedA.rawRowPitch, 0);
                producer->capturedFrame.reset();
            };
            if (!staged)
            {
                replayA();
            }
            if (staged)
            {
                queued->Queue(4);
                DrainNativeDraws();
                callbackChecks.Verify();
                REQUIRE((queued->Stats().posted == 2 && queued->Stats().ran == 2));
            }
            else
            {
                submitDraw(4);
            }
            const auto consumerB      = resolve(0x1C400000);
            const auto consumerBIndex = producer->terminals.size();
            const auto consumerBFence = complete(consumerB);
            REQUIRE((boundIds[0] == (std::uint64_t(3) << 60) + a && boundIds[1] == (std::uint64_t(3) << 60) + b && consumerBFence > bFence));
            const auto& bOutput = producer->terminals[consumerBIndex].get();
            REQUIRE(bOutput.success);
            checkPixels(bOutput.rawOutput, bOutput.rawRowPitch, 1);
            ID3D12Resource* importedBResource = nullptr;
            auto            importedBOwner    = rerevved::gpu::detail::NativeRenderFrameTestAccess::CompletedTexture(stream, producer->tickets[consumerBIndex]->Export((std::uint64_t(1) << 63) | b), importedBResource);
            if (staged)
            {
                REQUIRE((importedBOwner && importedBResource && importedBResource != aResource &&
                         importedBResource->GetDesc().Format == DXGI_FORMAT_R32_UINT &&
                         importedBResource->GetDesc().Width == width && importedBResource->GetDesc().Height == height));
                weakB     = importedBOwner;
                bResource = importedBResource;
            }
            REQUIRE((importedBOwner && importedBOwner == weakB.lock() && importedBResource == bResource));
            importedBOwner.reset();
            queued.reset();

            const auto swap = [&](std::uint64_t sequence)
            {
                auto value            = swapRecord(0xD10);
                value.sequence        = sequence;
                value.numberedThrough = 4;
                const auto result     = assembler.Swap(value);
                UNSCOPED_INFO("the ordered epoch presents: " + result.detail);
                REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence));
                return result.hostFence;
            };
            const auto heldFence = swap(1);
            REQUIRE(source.WaitFor(heldFence, std::chrono::seconds(10)));
            REQUIRE((surfaces->Import(boundIds[0]) && std::ranges::find(producer->released.back(), keyA) == producer->released.back().end() && !weakA.expired()));
            if (staged)
            {
                replayA();
            }

            clear(main);
            scanout();
            auto*                queue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(stream);
            ComPtr<ID3D12Device> device;
            ComPtr<ID3D12Fence>  gate;
            REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))));

            struct ReleaseGate
            {
                ID3D12Fence* gate;

                ~ReleaseGate()
                {
                    gate->Signal(1);
                }
            } release{ gate.Get() };

            const auto completedBeforeRelease = producer->CompletedFence();
            REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)));
            const auto releasedFence = swap(2);
            REQUIRE((std::ranges::find(producer->released.back(), keyA) != producer->released.back().end() && !surfaces->Import(boundIds[0])));
            const auto recording = producer->terminalRecordings.back().get();
            UNSCOPED_INFO("the release recording: " + recording.error);
            REQUIRE((recording.success && producer->terminals.back().wait_for(std::chrono::seconds(0)) == std::future_status::timeout &&
                     producer->CompletedFence() == completedBeforeRelease && !weakA.expired()));
            REQUIRE(SUCCEEDED(gate->Signal(1)));
            REQUIRE(source.WaitFor(releasedFence, std::chrono::seconds(10)));
            const auto releaseNativeFence = producer->terminals.back().get().completionFenceValue;
            REQUIRE((releaseNativeFence > completedBeforeRelease && releaseNativeFence > consumerBFence && producer->CompletedFence() >= releaseNativeFence));
            producer->tickets.clear();
            // Both renderer contexts must discard their last imported draw views.
            for (std::uint64_t epoch = 3; epoch <= 6 && !weakA.expired(); ++epoch)
            {
                clear(main);
                scanout();
                REQUIRE(source.WaitFor(swap(epoch), std::chrono::seconds(10)));
            }
            REQUIRE(weakA.expired());
            if (staged)
            {
                REQUIRE(!weakB.expired());
                std::cout << "Staged legacy producer-frame tiles: deferred A bound before commit and rendered after B admission, B missed until both admissions, and castable R32Uint B pixels and fence-held A retirement passed\n";
            }
        }
        REQUIRE((!assembler.Failure() && !source.Failure()));
        source.Close();
    }
    stream.Shutdown();
}

struct HeldSurfaceRun
{
    std::vector<std::uint8_t> bytes;
    bool                      held = false;
    // Source encodes once the ledger's take returned, and once the image landed.
    std::uint64_t        takenEncodes = 0, landedEncodes = 0;
    ResolveSurfaceCounts taken, landed;
};

// Held image stays unencoded behind the write gate; without hold it encodes.
HeldSurfaceRun heldSurfaceRun(bool hold)
{
    GuestTarget offscreen;
    offscreen.native = { 1024, 1024, 1, NativeDrawReplayTargetFormat::Rgba8, 0x300 };
    GuestResolveCopy copy;
    copy.copyControl = 0x100000;
    copy.destBase    = 0x1E3B0000;
    copy.destPitch   = 0x04000400;
    copy.destInfo    = 0x01000300;
    copy.destFetch   = { 0x88000002, 0xFE3AF006, 0x007FE3FF, 0xC14, 0, 0x200 };
    copy.rectWidth = copy.rectHeight = 1024;
    NativeDrawReplayRecipe gradient;
    gradient.schemaVersion                              = 2;
    gradient.width                                      = 1024;
    gradient.height                                     = 1024;
    gradient.sampleCount                                = 1;
    gradient.sampleMask                                 = 1;
    gradient.vertexShaderHash                           = 0xA11A5;
    gradient.pixelShaderHash                            = 0xA11A7;
    gradient.vertexShaderDxil                           = compile("float4 main(float4 p:TEXCOORD0):SV_Position { return p; }", "vs_5_1");
    gradient.pixelShaderDxil                            = compile("float4 main(float4 p:SV_Position):SV_Target0 { uint2 q = uint2(p.xy);"
                                                                  " return float4(q.x & 255, q.y & 255, (q.x ^ q.y) & 255, 255) / 255.0; }",
                                                                  "ps_5_1");
    gradient.vertexAttributeCount                       = 1;
    gradient.vertexStrideBytes                          = 16;
    const std::array<std::array<float, 4>, 3> positions = { { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } } };
    gradient.vertexData.resize(sizeof(positions));
    std::memcpy(gradient.vertexData.data(), positions.data(), sizeof(positions));
    gradient.indices    = { 0, 1, 2 };
    gradient.indexCount = 3;
    gradient.vertexConstants.resize(4096);
    gradient.pixelConstants.resize(3584);
    gradient.sharedConstants.resize(336);
    gradient.viewport = { 0, 0, 1024, 1024, 0, 1 };
    gradient.scissor  = { 0, 0, 1024, 1024 };

    HeldSurfaceRun      run;
    NativeRendererD3D12 stream;
    std::string         error;
    UNSCOPED_INFO("held surface stream: " + error);
    REQUIRE(stream.StartDrawStream(error));
    {
        auto                        producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSourceConfig sourceConfig{ producer };
        sourceConfig.timeEncodes = true;
        D3D12CompletionSource      source(sourceConfig);
        NativeFrameAssemblerConfig config;
        config.completion         = &source;
        config.readback           = true;
        config.resolveGraph       = true;
        config.eagerGraphSegments = true;
        config.timing             = true;
        config.surfaces           = std::make_shared<ResolveSurfaceRegistry>();
        const auto surfaces       = config.surfaces;
        config.readTarget         = targetWords(1024, 1024, false, 0x300);
        config.watchWrites        = [](std::uint32_t, std::uint32_t)
        {
        };
        config.build = [&](const NativeDrawSubmission&, NativeDrawReplayRecipe& draw, std::string&)
        {
            draw = gradient;
            return true;
        };
        NativeFrameAssembler           assembler(config);
        std::vector<GuestResolveImage> written;
        if (hold)
            assembler.SetGuestImageWrites([&](const std::function<void(const completion::GuestImageWriter&)>& body)
                                          {
                                              body([&](const GuestResolveImage& image)
                                                   {
                                                       written.push_back(image);
                                                       return true;
                                                   });
                                              return true;
                                          });
        GuestClear clear;
        clear.target           = offscreen;
        clear.flags            = 1;
        clear.clear.rectangle  = { 0, 0, 1024, 1024 };
        clear.clear.color      = true;
        clear.clear.colorValue = { 1.0F, 0.2F, 0.6F, 0.0F };
        UNSCOPED_INFO("the off-screen clear: " + error);
        REQUIRE(assembler.Clear(clear, error) != 0);
        NativeDrawSubmission draw;
        draw.hook          = NativeDrawHook::IndexedUp;
        draw.caller        = 0x82303E40;
        draw.stack         = 0x7000;
        draw.arguments     = { kAliasGraphics, 4, 0, 3, 3, 0x3000, 1, 1 };
        draw.stateRead     = true;
        draw.sequence      = 1;
        draw.operationMark = assembler.NumberOperation();
        assembler.Began(draw);
        const auto verdict = assembler.Admit(draw);
        assembler.Recorded(draw, verdict);
        UNSCOPED_INFO("the gradient draw is admitted");
        REQUIRE(!verdict);
        bool       partial = false;
        const auto mark    = assembler.Resolve(offscreen, copy, false, partial, error);
        UNSCOPED_INFO("the off-screen resolve: " + error);
        REQUIRE(mark != 0);
        const auto produced = flushThrough(assembler, source, mark);
        UNSCOPED_INFO("the resolve completes: " + produced.error);
        REQUIRE((produced.fence && source.WaitFor(produced.fence, std::chrono::seconds(10))));
        const auto take  = assembler.TakeGuestImage(mark);
        run.held         = take.held;
        run.takenEncodes = source.GuestImageEncodes().first;
        run.taken        = surfaces->TakeCounts();
        if (hold)
        {
            UNSCOPED_INFO("the take holds the readback: " + take.error);
            REQUIRE((take.held && !take.image && take.error.empty() && written.empty()));
            const ResolveSurfaceLockRanges ranges{ { { copy.destBase, copy.destBase + 4 }, { 0, 0 } } };
            surfaces->Locked(9, ranges, false);
            surfaces->Settle(&ranges);
            UNSCOPED_INFO("the lock wrote the held image");
            REQUIRE(written.size() == 1);
            run.bytes = written[0].bytes;
        }
        else
        {
            UNSCOPED_INFO("the take encodes the image: " + take.error);
            REQUIRE(take.image.has_value());
            run.bytes = take.image->bytes;
        }
        run.landedEncodes = source.GuestImageEncodes().first;
        run.landed        = surfaces->TakeCounts();
        UNSCOPED_INFO("the run leaves no failure");
        REQUIRE((!assembler.Failure() && !source.Failure()));
        source.Close();
    }
    stream.Shutdown();
    return run;
}

// Held graph readback waits for lock, then lands through the ledger gate.
void checkHeldSurfaceImage()
{
    const auto encoded = heldSurfaceRun(false);
    const auto held    = heldSurfaceRun(true);
    UNSCOPED_INFO("without the gate the take encodes once, uncounted");
    REQUIRE((!encoded.held && encoded.takenEncodes == 1 && encoded.taken.flushes == 0));
    UNSCOPED_INFO(std::format("the hold encodes nothing and gauges its readback ({} bytes)", held.taken.heldBytes));
    REQUIRE((held.held && held.takenEncodes == 0 && held.taken.heldBytes >= std::uint64_t(1024) * 1024 * 4 && held.taken.lockReadbacks == 0));
    UNSCOPED_INFO("the lock encodes it once, as a lock readback, and releases it");
    REQUIRE((held.landedEncodes == 1 && held.landed.lockReadbacks == 1 && held.landed.flushes == 0 && held.landed.heldBytes == 0));
    UNSCOPED_INFO("the resolved image is not uniform");
    REQUIRE(std::ranges::any_of(encoded.bytes, [&](std::uint8_t value)
                                {
                                    return value != encoded.bytes[0];
                                }));
    UNSCOPED_INFO("the held image lands with the immediate encode's bytes");
    REQUIRE(held.bytes == encoded.bytes);
    std::cout << "A held resolve readback lands at a lock with the immediate encode's " << held.bytes.size() << " bytes\n";
}

// EndTiling drains a real queued consumer before publishing assembled words.
void checkOrderedGraphTiling()
{
    using Microsoft::WRL::ComPtr;
    const OrderedSurfaceInputs inputs;
    constexpr auto             width = OrderedSurfaceInputs::width, height = OrderedSurfaceInputs::height;
    const auto&                fetch = OrderedSurfaceInputs::fetch;
    std::string                error;
    NativeTextureMemoryRanges  ranges;
    REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
    REQUIRE((ranges.base.address == 0x1DBB0000 && ranges.base.size == 0x398000 &&
             std::uint64_t(ranges.base.address) + ranges.base.size == 0x1DF48000 && !ranges.mips.size));

    auto tiledDraw             = inputs.producers[1];
    tiledDraw.vertexShaderHash = 0xA11D0;
    tiledDraw.pixelShaderHash  = 0xA11D1;
    // Interpolation carries logical coordinates independently of tile-local
    // SV_Position. Exchanging the two halves changes the expected pixels.
    tiledDraw.vertexShaderDxil = compile("struct V { float4 p:SV_Position; float2 q:TEXCOORD0; };"
                                         " V main(float4 p:TEXCOORD0) { V v; v.p=p; v.q=float2((p.x+1)*640,(1-p.y)*360); return v; }",
                                         "vs_5_1");
    tiledDraw.pixelShaderDxil  = compile("float4 main(float4 p:SV_Position,float2 v:TEXCOORD0):SV_Target0 { uint2 q=uint2(v);"
                                         " return float4((uint3(210,107,37)+uint3(3*q.x+5*q.y,7*q.x+11*q.y,13*q.x+19*q.y))&255,143)/255.0; }",
                                         "ps_5_1");

    std::uint64_t        offers = 0;
    NativeRendererD3D12  stream;
    ComPtr<ID3D12Device> shared;
    REQUIRE(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&shared))));
    REQUIRE(stream.StartTitleDrawStream(error, shared.Get(), [&](ID3D12Resource*, ID3D12Fence*, std::uint64_t)
                                        {
                                            auto lease = std::make_shared<NativeOutputLease>(++offers);
                                            lease->Cancel();
                                            return lease;
                                        }));
    const bool castable = stream.CastableGuestWordTextures();
    if (!castable)
    {
        stream.Shutdown();
        SKIP("The staged graph surface case requires castable guest word textures");
    }
    {
        auto producer          = std::make_shared<RendererProducer>(stream);
        producer->inspectGraph = true;
        D3D12CompletionSource                  source({ producer });
        auto                                   surfaces = std::make_shared<ResolveSurfaceRegistry>();
        constexpr std::uint32_t                gA = 0x1000, gB = 0x5000, gConsumer = 0x9000;
        constexpr std::uint32_t                sA = 0x20000, sB = 0x21000, sConsumer = 0x22000;
        std::map<std::uint32_t, std::uint32_t> words;
        const auto                             targetWordsAt = [&](std::uint32_t g, std::uint32_t surface, std::uint32_t base, bool tiled)
        {
            words[g + 0x3098] = surface;
            words[g + 0x309C] = words[g + 0x30A0] = words[g + 0x30A4] = 0;
            words[g + 0x2880]                                         = 0;
            words[g + 0x2ABC]                                         = tiled ? 0x20U << 24 : 0;
            words[g + 0x31A4]                                         = 0xFFFFFFFF;
            words[g + 0x31AC]                                         = 0;
            words[g + 0x3198] = words[g + 0x3194] = words[g + 0x2E2C] = 1;
            words[surface + 0x24]                                     = ((tiled ? 639U : width - 1) << 18) | ((height - 1) << 3);
            words[surface + 0x18]                                     = 0;
            words[surface + 0x1C]                                     = base;
            if (tiled)
            {
                words[g + 0x31CC] = 2;
                words[g + 0x337C] = width;
                words[g + 0x3380] = height;
                for (std::uint32_t i = 0; i < 5; ++i)
                {
                    words[g + 0x31B8 + 4 * i] = i == 0 ? surface : 0;
                }
                for (std::uint32_t i = 0; i < 2; ++i)
                {
                    words[g + 0x31D0 + 16 * i] = i * 640;
                    words[g + 0x31D4 + 16 * i] = 0;
                    words[g + 0x31D8 + 16 * i] = (i + 1) * 640;
                    words[g + 0x31DC + 16 * i] = height;
                }
            }
        };
        targetWordsAt(gA, sA, 0, false);
        targetWordsAt(gB, sB, 0, true);
        targetWordsAt(gConsumer, sConsumer, 0x500, false);
        const GuestWordReader read = [words](std::uint32_t address, std::uint32_t& out)
        {
            const auto found = words.find(address);
            if (found == words.end())
            {
                return false;
            }
            out = found->second;
            return true;
        };
        GuestTarget targetA, targetB, observation;
        REQUIRE((ReadGuestTarget(read, gA, 0, targetA) && ReadGuestTarget(read, gB, 0, targetB) && ReadGuestTarget(read, gConsumer, 0, observation)));
        REQUIRE((targetB.tiling && targetB.native.width == width && targetB.native.height == height && targetB.surfaceWidth == 640 && targetB.native.sampleCount == 1));

        struct DeferredDraw
        {
            std::uint32_t          graphics;
            NativeDrawReplayRecipe recipe;
            GuestWordReader        target;
        };

        rerevved::tests::CallbackChecks                     checks;
        NativeFrameAssembler*                               current = nullptr;
        std::array<std::optional<NativeResolvedTexture>, 2> bound;
        std::size_t                                         consumers = 0;
        NativeFrameAssemblerConfig                          config;
        config.completion   = &source;
        config.resolveGraph = config.resolveAlias = config.eagerGraphSegments = config.timing = true;
        config.castableGuestWords                                                             = castable;
        config.surfaces                                                                       = surfaces;
        GuestWordReader deferredTarget                                                        = read;
        config.readTarget                                                                     = [&](std::uint32_t address, std::uint32_t& value)
        {
            return deferredTarget(address, value);
        };
        config.coverage = [](const NativeDrawSubmission& submission, const NativeDrawReplayRecipe&) -> std::optional<NativeFrameDrawCover>
        {
            return submission.Graphics() == gB ? std::optional<NativeFrameDrawCover>{ NativeDrawReplayScissor{ 0, 0, width, height } } : std::nullopt;
        };
        config.watchWrites = [](std::uint32_t, std::uint32_t)
        {
        };
        config.build = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string&)
        {
            const auto snapshot = std::static_pointer_cast<const DeferredDraw>(submission.deferral);
            checks.Require(snapshot && snapshot->graphics == submission.Graphics() && bool(snapshot->target), "the queued root owns its recipe and immutable target words");
            draw = snapshot->recipe;
            if (submission.Graphics() == gConsumer)
            {
                auto       miss    = NativeResolvedTextureMiss::Form;
                const auto texture = current->ResolvedTexture(fetch, submission.operationMark, miss);
                checks.Require(texture.has_value(), "the actual queued consumer binds a resolve surface");
                checks.Require(consumers < bound.size(), "only the two authored consumers build");
                bound[consumers++] = *texture;
                draw.textures[0]   = BindNativeResolvedTexture(*texture);
            }
            return true;
        };
        NativeFrameAssembler assembler(config);
        current = &assembler;
        std::mutex               mutex;
        std::condition_variable  wake;
        bool                     consumerEntered = false, releaseConsumer = false, holdConsumer = true, barrierRan = false;
        NativeDrawDispatchConfig dispatch;
        dispatch.sequenced = true;
        dispatch.read      = [](const std::uint8_t* base, std::uint32_t address, std::uint32_t& out)
        {
            const auto& snapshot = *reinterpret_cast<const std::map<std::uint32_t, std::uint32_t>*>(base);
            const auto  found    = snapshot.find(address);
            if (found == snapshot.end())
            {
                return false;
            }
            out = found->second;
            return true;
        };
        dispatch.defer = [&](const NativeDrawSubmission& submission) -> std::shared_ptr<const void>
        {
            const auto g = submission.Graphics();
            return std::make_shared<DeferredDraw>(g, g == gA ? inputs.producers[0] : g == gB ? tiledDraw
                                                                                             : inputs.sampler,
                                                  read);
        };
        dispatch.numberOperation = [&]
        {
            return assembler.NumberOperation();
        };
        dispatch.began = [&](const NativeDrawSubmission& submission)
        {
            if (submission.Graphics() == gConsumer)
            {
                std::unique_lock lock(mutex);
                if (holdConsumer)
                {
                    consumerEntered = true;
                    wake.notify_all();
                    wake.wait(lock, [&]
                              {
                                  return releaseConsumer;
                              });
                }
            }
            deferredTarget = std::static_pointer_cast<const DeferredDraw>(submission.deferral)->target;
            assembler.Began(submission);
        };
        dispatch.admit = [&](const NativeDrawSubmission& submission)
        {
            return assembler.Admit(submission);
        };
        dispatch.recorded = [&](const NativeDrawSubmission& submission, std::optional<NativeDrawReason> reason)
        {
            std::string refusal;
            assembler.Recorded(submission, reason, &refusal);
            checks.Require(!reason, "the queued root is admitted and recorded: " + refusal);
        };
        dispatch.failed = [&](const NativeDrawFailure&)
        {
            checks.Require(false, "the real dispatcher does not fail");
        };
        NativeDrawDispatcher dispatcher(std::move(dispatch));
        const auto           previous = PublishedResolveSurfaces();

        struct InstalledDraws
        {
            std::shared_ptr<ResolveSurfaceRegistry> surfaces, previous;
            std::mutex&                             mutex;
            std::condition_variable&                wake;
            bool&                                   release;
            bool                                    installed = false;

            ~InstalledDraws()
            {
                {
                    std::lock_guard lock(mutex);
                    release = true;
                }
                wake.notify_all();
                if (installed)
                {
                    RemoveNativeDrawDispatcher(std::chrono::seconds(10));
                }
                surfaces->SetBarrier({});
                PublishResolveSurfaces(previous);
            }
        } installed{ surfaces, previous, mutex, wake, releaseConsumer };

        REQUIRE(InstallNativeDrawDispatcher(dispatcher));
        installed.installed = true;
        PublishResolveSurfaces(surfaces);
        surfaces->SetBarrier([&]
                             {
                                 {
                                     std::lock_guard lock(mutex);
                                     barrierRan      = true;
                                     releaseConsumer = true;
                                 }
                                 wake.notify_all();
                                 DrainNativeDraws();
                             });
        const auto queueDraw = [&](std::uint32_t g)
        {
            PPCContext context{};
            context.r1.u64 = 0x7000;
            context.lr     = 0x82303E40;
            context.r3.u64 = g;
            {
                NativeDrawScope root(NativeDrawHook::IndexedBuffer, context, reinterpret_cast<const std::uint8_t*>(&words));
                REQUIRE((root.Observed() && root.Root()));
                context.r3.u64 = 0;
            }
        };
        const auto drain = [&]
        {
            DrainNativeDraws();
            checks.Verify();
            REQUIRE((!dispatcher.Failure() && !assembler.Failure()));
        };
        const auto clear = [&](const GuestTarget& target)
        {
            GuestClear value;
            value.target          = target;
            value.flags           = 1;
            value.clear.rectangle = { 0, 0, width, height };
            value.clear.color     = true;
            REQUIRE(assembler.Clear(value, error));
        };
        GuestResolveCopy copy;
        copy.copyControl   = 0x00100000;
        copy.destBase      = 0x1DBB0000;
        copy.destPitch     = 0x02D00500;
        copy.destInfo      = 0x01000302;
        copy.destFetch     = fetch;
        copy.rectWidth     = width;
        copy.rectHeight    = height;
        const auto resolve = [&](const GuestTarget& target, const GuestResolveCopy& destination)
        {
            bool       partial = false;
            const auto mark    = assembler.Resolve(target, destination, false, partial, error);
            UNSCOPED_INFO("the staged graph resolve: " + error);
            REQUIRE((mark && !partial));
            return mark;
        };
        const auto complete = [&](std::uint64_t mark)
        {
            const auto flush = flushThrough(assembler, source, mark);
            UNSCOPED_INFO("the staged graph prefix: " + flush.error);
            REQUIRE((flush.fence && source.WaitFor(flush.fence, std::chrono::seconds(10))));
            const auto& result = producer->prefixRecordings.back().get();
            UNSCOPED_INFO(result.error);
            REQUIRE((result.success && result.completionFenceValue && producer->CompletedFence() >= result.completionFenceValue));
            return result.completionFenceValue;
        };
        const auto pixels = [&](std::size_t prefix, std::uint64_t mark, std::size_t version)
        {
            const auto& output   = producer->prefixRecordings.at(prefix).get();
            const auto  readback = std::ranges::find_if(output.rawReadbacks, [&](const auto& item)
                                                        {
                                                           return item.first == (std::uint64_t(1) << 62) + mark;
                                                        });
            REQUIRE((readback != output.rawReadbacks.end() && readback->second));
            std::vector<std::uint8_t> bytes;
            std::uint32_t             pitch = 0;
            REQUIRE(readback->second->Read(bytes, pitch, error));
            inputs.CheckPixels(bytes, pitch, version);
        };
        GuestTarget main;
        main.native = { width, height, 4, NativeDrawReplayTargetFormat::Rgba8, 0x900 };
        clear(main);
        auto scanout        = copy;
        scanout.copyControl = 0x00100040;
        scanout.destBase    = kFrontbuffer;
        scanout.destInfo    = 0x300;
        scanout.destFetch   = {};
        bool partial        = false;
        REQUIRE(assembler.Resolve(main, scanout, true, partial, error));
        clear(targetA);
        queueDraw(gA);
        drain();
        const auto a      = resolve(targetA, copy);
        const auto aFence = complete(a);
        REQUIRE((producer->prefixFrames.size() == 1 && producer->terminals.empty()));
        pixels(0, a, 0);
        REQUIRE(std::ranges::any_of(producer->prefixFrames[0].surfaces, [&](const auto& surface)
                                    {
                                        return surface.textureId == (std::uint64_t(1) << 62) + a && surface.key == ResolveSurfaceKey(a);
                                    }));
        const auto committedA = surfaces->Find(fetch, false);
        REQUIRE((committedA && std::get<ResolveSurfaceBinding>(*committedA).mark == a && std::get<ResolveSurfaceBinding>(*committedA).version == 1));

        // Replacing A's source lets the full-coverage tiled draw define B.
        clear(observation);
        queueDraw(gB);
        drain();
        std::array<std::uint64_t, 2> b{}, bFences{};
        std::vector<std::uint8_t>    memory(ranges.base.size, 0xE7);
        for (std::uint32_t tile = 0; tile < 2; ++tile)
        {
            auto tiledTarget                  = targetB;
            tiledTarget.tiling->predicated    = 1;
            tiledTarget.tiling->predicateMask = 3U << (2 * tile);
            auto tileCopy                     = copy;
            tileCopy.rectX = tileCopy.destX = 640 * tile;
            tileCopy.rectWidth              = 640;
            b[tile]                         = resolve(tiledTarget, tileCopy);
            bFences[tile]                   = complete(b[tile]);
            const auto& frame               = producer->prefixFrames.at(tile + 1);
            REQUIRE((frame.passes.size() == 1 && frame.passes[0].resolves.size() == 1 &&
                     frame.passes[0].resolves[0].guestWords == NativeGuestWordPacking::Color8888 &&
                     frame.passes[0].resolves[0].sourceX == tile * 640));
            REQUIRE(std::ranges::any_of(frame.textures, [&](const auto& texture)
                                        {
                                            return texture.id == (std::uint64_t(1) << 62) + b[tile] && texture.width == 640 && texture.height == height && texture.format == NativeDrawReplayTextureFormat::R32Uint;
                                        }));
            REQUIRE(std::ranges::any_of(frame.surfaces, [&](const auto& surface)
                                        {
                                            return surface.textureId == (std::uint64_t(1) << 62) + b[tile] && surface.key == ResolveSurfaceKey(b[0]) &&
                                                   surface.width == width && surface.height == height && surface.x == tile * 640;
                                        }));
            const auto take = assembler.TakeGuestImage(b[tile]);
            REQUIRE((take.image && take.image->physical == copy.destBase));
            for (const auto& run : take.image->runs)
            {
                REQUIRE(run.offset + run.length <= memory.size());
                std::copy_n(take.image->bytes.begin() + run.offset, run.length, memory.begin() + run.offset);
            }
            assembler.GuestImageWritten(b[tile]);
        }
        REQUIRE((aFence < bFences[0] && bFences[0] < bFences[1] && surfaces->TilesPending() && producer->prefixFrames.size() == 3 && producer->terminals.empty()));
        REQUIRE(std::get<ResolveSurfaceBinding>(*surfaces->Find(fetch, false)).mark == a);
        NativeDrawReplayTexture decoded;
        REQUIRE(DecodeNativeTexture(fetch, memory, {}, decoded, error));
        REQUIRE((decoded.width == width && decoded.height == height && decoded.payloadBytes().size() == kPlaneBytes));
        std::vector<std::uint8_t> logical(kPlaneBytes);
        REQUIRE(std::ranges::all_of(decoded.swizzle, [](auto component)
                                    {
                                        return component < 4;
                                    }));
        for (std::size_t p = 0; p < kPlaneBytes / 4; ++p)
        {
            for (std::size_t c = 0; c < 4; ++c)
            {
                logical[p * 4 + c] = decoded.payloadBytes()[p * 4 + decoded.swizzle[c]];
            }
        }
        inputs.CheckPixels(logical, width * 4, 1);

        clear(observation);
        queueDraw(gConsumer);
        {
            std::unique_lock lock(mutex);
            REQUIRE(wake.wait_for(lock, std::chrono::seconds(10), [&]
                                  {
                                      return consumerEntered;
                                  }));
        }
        REQUIRE((dispatcher.Sequencer().posted == 3 && dispatcher.Sequencer().ran == 2 && !bound[0]));
        ObserveResolveSurfaceTiling();
        checks.Verify();
        REQUIRE((barrierRan && dispatcher.Sequencer().ran == 3 && !surfaces->TilesPending() && bound[0]));
        REQUIRE((bound[0]->frameTextureId == ResolveSurfaceTexture(a) && bound[0]->swizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 }));
        auto observed             = copy;
        observed.destBase         = 0x1C000000;
        observed.destFetch[1]     = observed.destBase | 0x86;
        const auto consumerA      = resolve(observation, observed);
        const auto consumerAFence = complete(consumerA);
        pixels(3, consumerA, 0);
        REQUIRE(producer->prefixFrames[3].passes[0].Draw(0).textures[0].frameTextureId == ResolveSurfaceTexture(a));

        const auto assembled = surfaces->Find(fetch, true);
        REQUIRE((assembled && std::holds_alternative<ResolveSurfaceBinding>(*assembled)));
        const auto& binding = std::get<ResolveSurfaceBinding>(*assembled);
        INFO(std::format("assembled mark {} expected {} version {} id {} dimensions {}x{} swizzle {},{},{},{}", binding.mark, b[0], binding.version, binding.texture.frameTextureId, binding.texture.layout.width, binding.texture.layout.height, binding.texture.swizzle[0], binding.texture.swizzle[1], binding.texture.swizzle[2], binding.texture.swizzle[3]));
        REQUIRE((binding.mark == b[0] && binding.version == 2 && binding.texture.frameTextureId == ResolveSurfaceTexture(b[0]) &&
                 binding.texture.layout.width == width && binding.texture.layout.height == height &&
                 binding.texture.swizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 }));
        // Fetch selectors B,G,R,A and the copy's red/blue swap compose to
        // identity for this admitted Color8888 view.
        const auto imported = surfaces->Import(binding.texture.frameTextureId);
        REQUIRE((imported && imported->persistentKey == ResolveSurfaceKey(b[0]) && imported->width == width && imported->height == height &&
                 imported->format == NativeDrawReplayTextureFormat::R32Uint && imported->importedWords == NativeGuestWordPacking::Color8888));
        {
            std::lock_guard lock(mutex);
            holdConsumer = false;
        }
        clear(observation);
        queueDraw(gConsumer);
        drain();
        observed.destBase         = 0x1C400000;
        observed.destFetch[1]     = observed.destBase | 0x86;
        const auto consumerB      = resolve(observation, observed);
        const auto consumerBFence = complete(consumerB);
        REQUIRE((bound[1] && bound[1]->frameTextureId == ResolveSurfaceTexture(b[0]) && bound[1]->swizzle == binding.texture.swizzle && consumerBFence > consumerAFence));
        pixels(4, consumerB, 1);
        REQUIRE(producer->prefixFrames[4].passes[0].Draw(0).textures[0].frameTextureId == ResolveSurfaceTexture(b[0]));
        REQUIRE(producer->prefixFrames[4].typedGuestWords);
        REQUIRE(std::ranges::any_of(producer->prefixFrames[4].textures, [&](const auto& texture)
                                    {
                                        return texture.imported && texture.id == ResolveSurfaceTexture(b[0]) && texture.persistentKey == ResolveSurfaceKey(b[0]) &&
                                               texture.width == width && texture.height == height && texture.format == NativeDrawReplayTextureFormat::R32Uint &&
                                               texture.importedWords == NativeGuestWordPacking::Color8888;
                                    }));
        const auto stats = assembler.ResolveGraphStats();
        REQUIRE((producer->prefixFrames.size() == 5 && !stats.notReady && !stats.noSource && !stats.swapBatch && stats.replays == 4 && stats.continuations == 1));
        auto swap            = swapRecord(0xD10);
        swap.numberedThrough = dispatcher.NumberedThrough();
        const auto terminal  = assembler.Swap(swap);
        UNSCOPED_INFO(terminal.detail);
        REQUIRE((terminal.status == NativeFrameStatus::Provisional && terminal.hostFence && source.WaitFor(terminal.hostFence, std::chrono::seconds(10))));
        REQUIRE((producer->terminals.back().get().success && !source.Failure() && !assembler.Failure() && !dispatcher.Failure()));
        checks.Verify();
        REQUIRE(RemoveNativeDrawDispatcher(std::chrono::seconds(10)));
        installed.installed = false;
        source.Close();
        std::cout << "Staged graph tiles publish complete B after the drained consumer samples A; marks " << a << ',' << b[0] << ',' << b[1]
                  << " fences " << aFence << ',' << bFences[0] << ',' << bFences[1] << ',' << consumerAFence << ',' << consumerBFence << " graph replays " << stats.replays << " continuations " << stats.continuations << '\n';
    }
    stream.Shutdown();
}

struct TiledSurfaceRun
{
    std::array<std::vector<std::uint8_t>, 2> shown;
    ResolveSurfaceCounts                     counts;
    bool                                     castable = false;
    // The depth conversions the renderer recorded, and the consumers of a
    // converted depth texture the registry noted.
    std::uint64_t                                        conversions = 0;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> consumers;
};

// census-0009 covers depth, RGBA8 and 2_10_10_10 tiles; segmented draws leave
// the terminal surface-free.
TiledSurfaceRun tiledSurfaceRun(bool registry, std::uint32_t tiles = 3, bool segmented = false)
{
    using Target                                 = NativeRenderFramePass::Target;
    constexpr std::uint32_t                width = 1280, height = 720, storage = 1280 * 736 * 4;
    const std::array<GuestRect, 3>         rects = tiles == 2 ? std::array<GuestRect, 3>{ { { 0, 0, 640, 720 }, { 640, 0, 1280, 720 }, {} } }
                                                              : std::array<GuestRect, 3>{ { { 0, 0, 448, 720 }, { 448, 0, 864, 720 }, { 864, 0, 1280, 720 } } };
    constexpr std::array<std::uint32_t, 3> slots{ kGuestDepthSlot, 0, 1 };
    const Target                           set{ width, height, 4, NativeDrawReplayTargetFormat::Rgba8, 0, kNoDepthEdramBase, 1, { NativeDrawReplayTargetFormat::Rgb10a2 }, { 0x21C } };
    // Beyond depth preserves it; frame 428's 0x1D480000 would end it.
    const std::array<std::array<std::uint32_t, 6>, 3> destFetches{ { { 0x8A000002, 0xBD0F0096, 0x0059E4FF, 0x1690, 0x280, 0xBD488A00 },
                                                                     { 0x8A000002, 0xFD817006, 0x0059E4FF, 0x1414, 0, 0x200 },
                                                                     { 0x8A000002, 0xFDBFF036, 0x0059E4FF, 0xD10, 0, 0x200 } } };
    const std::array<std::uint32_t, 3>                physicals{ 0x1D0F0000, 0x1D818000, 0x1DC00000 };
    const std::array<std::uint32_t, 3>                copyFormats{ 6, 6, 7 }, endians{ 2, 0, 0 };
    // The RGBA8 destination as a draw fetches it.
    const std::array<std::uint32_t, 6> fetch{ 0x8A000002, 0xFD818006, 0x0059E4FF, 0x1414, 0, 0x200 };
    // The depth destination as the world's one k_24_8 consumer fetches it.
    const std::array<std::uint32_t, 6> depthFetch{ 0x8A004802, 0x1D0F0096, 0x0059E4FF, 0x00A81690, 0x00000283, 0x1D488A18 };
    constexpr std::uint32_t            kDepthDraw = 100;
    const auto                         tiling     = [&](std::uint32_t mask)
    {
        GuestTiling value;
        value.flags        = 0x20;
        value.count        = tiles;
        value.screenWidth  = width;
        value.screenHeight = height;
        std::copy_n(rects.begin(), tiles, value.rects.begin());
        value.saved         = { 0x50263120, 0x5044C6B0, 0, 0, 0x5044C390 };
        value.predicateMask = mask;
        value.predicated    = 1;
        return value;
    };
    const auto target = [&](std::uint32_t slot, std::uint32_t mask)
    {
        GuestTarget value;
        value.native     = set;
        value.tiling     = tiling(mask);
        value.slot       = slot;
        value.colorCount = 2;
        return value;
    };
    const auto tiledClear = [&](std::uint32_t index)
    {
        GuestClear value;
        value.target             = target(index, 3);
        value.flags              = index ? 0x0F : 0x3F;
        value.index              = index;
        value.predicate          = 1;
        value.tiling             = tiling(3);
        value.clear.rectangle    = { 0, 0, std::int32_t(width), std::int32_t(height) };
        value.clear.color        = true;
        value.clear.depth        = index == 0;
        value.clear.stencil      = index == 0;
        value.clear.colorTargets = static_cast<std::uint8_t>(1U << index);
        value.clear.colorValue   = { 1, 0, 0, 1 };
        value.clear.depthValue   = 1;
        value.clear.stencilValue = 0x17;
        value.depth              = GuestDepthSurface{ 0x5044C390, 0x5E4, 0x5E4, width, height, 4, true };
        return value;
    };
    GuestTarget main;
    main.native = { width, height, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    GuestClear presentedClear;
    presentedClear.target           = main;
    presentedClear.flags            = 1;
    presentedClear.clear.rectangle  = { 0, 0, std::int32_t(width), std::int32_t(height) };
    presentedClear.clear.color      = true;
    presentedClear.clear.colorValue = { 0.1F, 0.1F, 0.1F, 1.0F };

    NativeDrawReplayRecipe sampler;
    sampler.schemaVersion                               = 2;
    sampler.width                                       = width;
    sampler.height                                      = height;
    sampler.sampleCount                                 = 4;
    sampler.sampleMask                                  = 9;
    sampler.vertexShaderHash                            = 0xA11B5;
    sampler.pixelShaderHash                             = 0xA11B6;
    sampler.vertexShaderDxil                            = compile("float4 main(float4 p:TEXCOORD0):SV_Position { return p; }", "vs_5_1");
    sampler.pixelShaderDxil                             = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 {"
                                                                  " return t.Load(int3(int2(p.xy), 0)); }",
                                                                  "ps_5_1");
    sampler.vertexAttributeCount                        = 1;
    sampler.vertexStrideBytes                           = 16;
    const std::array<std::array<float, 4>, 3> positions = { { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } } };
    sampler.vertexData.resize(sizeof(positions));
    std::memcpy(sampler.vertexData.data(), positions.data(), sizeof(positions));
    sampler.indices    = { 0, 1, 2 };
    sampler.indexCount = 3;
    sampler.vertexConstants.resize(4096);
    sampler.pixelConstants.resize(3584);
    sampler.sharedConstants.resize(336);
    sampler.viewport               = { 0, 0, float(width), float(height), 0, 1 };
    sampler.scissor                = { 0, 0, std::int32_t(width), std::int32_t(height) };
    sampler.textureMask            = 1;
    auto depthSampler              = sampler;
    depthSampler.pixelShaderHash   = 0xA11B7;
    depthSampler.pixelShaderDxil   = compile("Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3); float4 main(float4 p:SV_Position):SV_Target0 {"
                                             " uint2 xy = uint2(p.xy); uint bits = asuint(t.Sample(s, p.xy / float2(1280.0, 720.0))[xy.x & 3]);"
                                             " return float4(bits & 255, (bits >> 8) & 255, (bits >> 16) & 255, bits >> 24) / 255.0; }",
                                             "ps_5_1");
    depthSampler.scissor           = { 0, std::int32_t(height / 2), std::int32_t(width), std::int32_t(height) };
    depthSampler.sampler.minLinear = depthSampler.sampler.magLinear = depthSampler.sampler.mipLinear = true;
    depthSampler.sampler.maxLod                                                                      = 10.0F;

    TiledSurfaceRun     run;
    NativeRendererD3D12 stream;
    std::string         error;
    UNSCOPED_INFO("tiled surface stream: " + error);
    REQUIRE(stream.StartDrawStream(error));
    run.castable = stream.CastableGuestWordTextures();
    {
        auto                       producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource      source({ producer });
        NativeFrameAssemblerConfig config;
        config.completion         = &source;
        config.readback           = true;
        config.resolveGraph       = true;
        config.eagerGraphSegments = true;
        config.castableGuestWords = run.castable;
        config.timing             = true;
        if (registry)
            config.surfaces = std::make_shared<ResolveSurfaceRegistry>();
        const auto surfaces = config.surfaces;
        config.readTarget   = targetWords(width, height, true, 0);
        config.watchWrites  = [](std::uint32_t, std::uint32_t)
        {
        };
        std::optional<NativeDrawReplayTexture> decoded, depthDecoded;
        NativeFrameAssembler*                  current = nullptr;
        config.build                                   = [&](const NativeDrawSubmission& value, NativeDrawReplayRecipe& draw, std::string&)
        {
            if (value.arguments[7] >= kDepthDraw)
            {
                // The converted texture needs no castable storage.
                draw               = depthSampler;
                auto       miss    = NativeResolvedTextureMiss::Form;
                const auto texture = current->ResolvedTexture(depthFetch, value.operationMark, miss);
                UNSCOPED_INFO(std::format("depth draw {} binds the converted texture only with the registry, miss {}", value.sequence, static_cast<unsigned>(miss)));
                REQUIRE(texture.has_value() == registry);
                UNSCOPED_INFO("the k_24_8 fetch selects the converted R32Float base level");
                REQUIRE((!texture || ((texture->frameTextureId & kResolveSurfaceDepthTag) && texture->layout.format == NativeDrawReplayTextureFormat::R32Float && texture->layout.mipLevels == 1)));
                draw.textures[0] = texture ? BindNativeResolvedTexture(*texture) : *depthDecoded;
                return true;
            }
            draw               = sampler;
            auto       miss    = NativeResolvedTextureMiss::Form;
            const auto texture = current->ResolvedTexture(fetch, value.operationMark, miss);
            UNSCOPED_INFO(std::format("draw {} binds the assembled surface only with the registry, miss {}", value.sequence, static_cast<unsigned>(miss)));
            REQUIRE(texture.has_value() == (registry && run.castable));
            draw.textures[0] = texture ? BindNativeResolvedTexture(*texture) : *decoded;
            return true;
        };
        NativeFrameAssembler assembler(config);
        current = &assembler;

        std::vector<std::uint8_t> memory(storage, 0xA5), depthMemory(storage, 0xA5);
        UNSCOPED_INFO("the paired tiled clears: " + error);
        REQUIRE((assembler.Clear(tiledClear(0), error) && assembler.Clear(tiledClear(1), error)));
        for (std::uint32_t tile = 0; tile < tiles; ++tile)
            for (std::size_t d = 0; d < slots.size(); ++d)
            {
                const auto       slot = slots[d];
                GuestResolveCopy copy;
                copy.copyControl = 0x00100000 | slot | (slot == kGuestDepthSlot ? 0x10 : 0x40) |
                                   (tile + 1 == tiles || slot == kGuestDepthSlot ? 0 : slot == 0 ? 0x300
                                                                                                 : 0x100);
                copy.destBase    = physicals[d];
                copy.destPitch   = (height << 16) | width;
                copy.destInfo    = (copyFormats[d] << 7) | endians[d];
                copy.destFetch   = destFetches[d];
                copy.rectX = copy.destX = rects[tile].x0;
                copy.rectWidth          = rects[tile].x1 - rects[tile].x0;
                copy.rectHeight         = height;
                copy.colorClear         = slot == 1 ? 0xC00FFC01 : 0x80FF0033;
                copy.depthClear         = 0x80000042;
                bool       partial      = false;
                const auto mark         = assembler.Resolve(target(slot, 3U << (2 * tile)), copy, false, partial, error);
                UNSCOPED_INFO(std::format("tile {} slot {} resolve: {}", tile, slot, error));
                REQUIRE(mark != 0);
                const auto flush = flushThrough(assembler, source, mark);
                UNSCOPED_INFO(std::format("tile {} slot {} completes: {}", tile, slot, flush.error));
                REQUIRE((flush.fence && source.WaitFor(flush.fence, std::chrono::seconds(10))));
                const auto take = assembler.TakeGuestImage(mark);
                UNSCOPED_INFO(std::format("tile {} slot {} image: {}", tile, slot, take.error));
                REQUIRE((take.image && take.image->physical == copy.destBase));
                if (d == 2)
                    continue;
                auto& composed = d == 1 ? memory : depthMemory;
                for (const auto& piece : take.image->runs)
                {
                    UNSCOPED_INFO("a run lies inside the destination");
                    REQUIRE(piece.offset + piece.length <= storage);
                    std::copy_n(take.image->bytes.begin() + piece.offset, piece.length, composed.begin() + piece.offset);
                }
            }
        // EndTiling (0x8269D320) ends the bracket.
        if (surfaces)
            surfaces->CommitTiles();
        NativeTextureMemoryRanges ranges;
        const bool                ranged = GetNativeTextureMemoryRanges(fetch, ranges, error) && ranges.base.address == physicals[1] && ranges.base.size <= storage;
        UNSCOPED_INFO("the fetch reads the RGBA8 destination: " + error);
        REQUIRE(ranged);
        memory.resize(ranges.base.size);
        decoded.emplace();
        const bool decodes = DecodeNativeTexture(fetch, memory, {}, *decoded, error);
        UNSCOPED_INFO("the bytes path decodes the composed tiles: " + error);
        REQUIRE(decodes);
        // The depth chain's levels 1-10 come from mip memory no resolve writes.
        NativeTextureMemoryRanges depthRanges;
        const bool                depthRanged = GetNativeTextureMemoryRanges(depthFetch, depthRanges, error) && depthRanges.base.address == physicals[0] && depthRanges.base.size <= storage && depthRanges.mips.size;
        UNSCOPED_INFO("the k_24_8 fetch reads the depth destination and a mip chain: " + error);
        REQUIRE(depthRanged);
        depthMemory.resize(depthRanges.base.size);
        std::vector<std::uint8_t> depthMips(depthRanges.mips.size);
        for (std::size_t i = 0; i < depthMips.size(); ++i)
            depthMips[i] = std::uint8_t(0x5A ^ (i * 37));
        depthDecoded.emplace();
        const bool depthDecodes = DecodeNativeTexture(depthFetch, depthMemory, depthMips, *depthDecoded, error) && depthDecoded->mipLevels == 11;
        UNSCOPED_INFO("the bytes path decodes the depth chain: " + error);
        REQUIRE(depthDecodes);

        for (std::uint32_t epoch = 0; epoch < 2; ++epoch)
        {
            UNSCOPED_INFO("the presented clear: " + error);
            REQUIRE(assembler.Clear(presentedClear, error) != 0);
            NativeDrawSubmission draw;
            draw.hook          = NativeDrawHook::IndexedUp;
            draw.caller        = 0x82303E40;
            draw.stack         = 0x7000;
            draw.arguments     = { kAliasGraphics, 4, 0, 3, 3, 0x3000, 1, epoch + 1 };
            draw.stateRead     = true;
            draw.sequence      = 2 * epoch + 1;
            draw.operationMark = assembler.NumberOperation();
            assembler.Began(draw);
            const auto verdict = assembler.Admit(draw);
            assembler.Recorded(draw, verdict);
            UNSCOPED_INFO(std::format("draw {} is admitted", epoch + 1));
            REQUIRE(!verdict);
            NativeDrawSubmission depthDraw = draw;
            depthDraw.arguments[7]         = kDepthDraw + epoch;
            depthDraw.sequence             = 2 * epoch + 2;
            depthDraw.operationMark        = assembler.NumberOperation();
            assembler.Began(depthDraw);
            const auto  depthVerdict = assembler.Admit(depthDraw);
            std::string depthRefusal;
            assembler.Recorded(depthDraw, depthVerdict, &depthRefusal);
            UNSCOPED_INFO(std::format("depth draw {} is admitted: {}", epoch + 1, depthRefusal));
            REQUIRE(!depthVerdict);
            if (segmented)
            {
                // Both draws, the converted depth binding with them, leave in the segment.
                const auto flush = flushThrough(assembler, source, depthDraw.operationMark);
                UNSCOPED_INFO(std::format("draws of epoch {} complete in their segment: {}", epoch + 1, flush.error));
                REQUIRE((flush.fence && source.WaitFor(flush.fence, std::chrono::seconds(10))));
            }
            scanOut(assembler);
            auto swap            = swapRecord(0xD10);
            swap.sequence        = epoch + 1;
            swap.numberedThrough = 2 * epoch + 2;
            const auto result    = assembler.Swap(swap);
            UNSCOPED_INFO(std::format("swap {} presents: {}", epoch + 1, result.detail));
            REQUIRE((result.status == NativeFrameStatus::Provisional && result.hostFence && source.WaitFor(result.hostFence, std::chrono::seconds(10))));
            const auto& output = producer->terminals.back().get();
            UNSCOPED_INFO("the presented output: " + output.error);
            REQUIRE((output.success && output.resolvedOutput.size() == kPlaneBytes));
            run.shown[epoch] = output.resolvedOutput;
        }
        if (surfaces)
        {
            run.counts    = surfaces->TakeCounts();
            run.consumers = surfaces->TakeNewDepthConsumers();
        }
        DEV_DO(run.conversions = stream.ReplayTimingStats().surfaceConversions);
        UNSCOPED_INFO("the epochs leave no failure");
        REQUIRE((!assembler.Failure() && !source.Failure()));
        source.Close();
    }
    stream.Shutdown();
    return run;
}

// Tiled resolves preserve bound bytes across both epochs via surface store.
void checkTiledSurface(std::uint32_t tiles = 3, bool segmented = false)
{
    const auto bytes = tiledSurfaceRun(false, tiles, segmented);
    const auto bound = tiledSurfaceRun(true, tiles, segmented);
    if (!bound.castable)
    {
        std::cout << "No castable guest word textures; the tiled surface draws took the bytes path\n";
        return;
    }
    UNSCOPED_INFO(std::format("the tiled surface draws bind twice each ({} hits, {} depth views, {} misses, {} entries)", bound.counts.hits, bound.counts.depthViews, bound.counts.misses, bound.counts.entries));
    REQUIRE((bound.counts.hits == 4 && bound.counts.depthViews == 2));
    // Each depth tile resolve converts once; the consumer is noted once.
    UNSCOPED_INFO(std::format("each depth tile converts with the registry only ({} and {})", bound.conversions, bytes.conversions));
    REQUIRE(DEV_CHECK(bound.conversions == tiles && bytes.conversions == 0));
    UNSCOPED_INFO("the converted texture's consumer is noted once by shader and caller");
    REQUIRE((bound.consumers == std::vector<std::pair<std::uint64_t, std::uint32_t>>{ { 0xA11B7, 0x82303E40 } }));
    for (std::size_t epoch = 0; epoch < 2; ++epoch)
    {
        // The tiles hold different words, so a misplaced tile is visible.
        UNSCOPED_INFO("the sampled image is not uniform");
        REQUIRE(std::ranges::any_of(std::span(bytes.shown[epoch]).subspan(4), [&](std::uint8_t value)
                                    {
                                        return value != bytes.shown[epoch][0];
                                    }));
        // The depth draw's bottom half reads the first tile's depth and a later tile's.
        const auto word = [&](std::uint32_t x, std::uint32_t y)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, bytes.shown[epoch].data() + (std::size_t(y) * 1280 + x) * 4, 4);
            return value;
        };
        UNSCOPED_INFO("the sampled depth differs between tiles");
        REQUIRE(word(0, 600) != word(1000, 600));
        UNSCOPED_INFO(std::format("epoch {}: the assembled surface presents the bytes path's output byte for byte", epoch + 1));
        REQUIRE(bound.shown[epoch] == bytes.shown[epoch]);
    }
    std::cout << "Draws bound to an assembled surface of " << tiles << (segmented ? " tiles from graph segments" : " tiles") << " present the bytes path's output across epochs (" << bound.counts.hits << " hits, " << bound.counts.depthViews << " of converted depth)\n";
}

void run(const std::filesystem::path& capture)
{
    const auto      adapter = capture / "adapter";
    SolidDrawInputs inputs;
    std::string     error;
    const bool      loaded = LoadSolidDrawInputs(adapter, inputs, error) && inputs.solidCandidate && inputs.inputsComplete;
    UNSCOPED_INFO("M1a inputs: " + error);
    REQUIRE(loaded);
    const auto      shader = solidShader(capture);
    SolidDrawRecipe recipe;
    const bool      built = BuildSolidDrawRecipe(inputs, shader, recipe, error);
    UNSCOPED_INFO("M1a recipe: " + error);
    REQUIRE(built);
    UNSCOPED_INFO("re-derived patched VS and folded state equal the saved M1a recipe");
    REQUIRE((recipe.patchedVertexCode == read(adapter / "vs-patched.be.bin") && recipe.effectiveState == read(adapter / "effective-state.be.bin")));

    // The panel is black with alpha 16, so each frame's fetch swizzle routes
    // alpha into one colour channel: red (A,R,G,A), then blue (G,B,A,A).
    const std::array<std::uint32_t, 2>               swizzleWords{ 0xC86, 0xDA2 };
    const std::array<std::array<std::uint8_t, 4>, 2> swizzles{ { { 3, 0, 1, 3 }, { 1, 2, 3, 3 } } };
    std::array<NativeRenderFrame, 2>                 frames;
    for (std::size_t pass = 0; pass < frames.size(); ++pass)
    {
        NativeFrameAssemblerConfig config;
        config.readback = true;
        config.build    = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            // The capture stands in for the guest reads made before Original.
            return submission.hook == NativeDrawHook::IndexedUp && BuildSolidPanelDraw(inputs, shader, draw, detail);
        };
        NativeFrameAssembler assembler(config);
        const auto&          a = inputs.arguments;
        NativeDrawSubmission submission;
        submission.hook      = NativeDrawHook::IndexedUp;
        submission.caller    = static_cast<std::uint32_t>(a.caller);
        submission.stack     = 0x70000000;
        submission.arguments = { a.graphics, a.primitive, a.minimumVertex, a.vertexCount, a.indexCount, a.indices, a.indexFormat, a.vertices };
        submission.stateRead = true;
        submission.sequence  = 1;
        assembler.Began(submission);
        const auto verdict = assembler.Admit(submission);
        UNSCOPED_INFO("the M1a panel is admitted");
        REQUIRE(!verdict);
        assembler.Recorded(submission, verdict);
        scanOut(assembler);
        auto swap            = swapRecord(swizzleWords[pass]);
        swap.numberedThrough = 1;
        auto result          = assembler.Swap(swap);
        UNSCOPED_INFO("panel epoch: " + result.detail);
        REQUIRE((result.status == NativeFrameStatus::Provisional && result.frame && result.assumptions.clearAssumed &&
                 result.assumptions.impliedResolve));
        frames[pass] = std::move(*result.frame);
        UNSCOPED_INFO("one pass with the panel draw");
        REQUIRE((frames[pass].passes.size() == 1 && frames[pass].passes[0].DrawCount() == 1));
        UNSCOPED_INFO("output swizzle comes from the swap fetch");
        REQUIRE(frames[pass].outputSwizzle == swizzles[pass]);
        compareWithM1a(frames[pass].passes[0].Draw(0), adapter);
    }
    std::cout << "Assembled panel draw equals the saved M1a recipe\n";

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        SKIP("No D3D12 device; the assembled frame was not rendered");
    }
    // M1a's live native replay from a cleared target, two RGBA8 planes for host samples 0 and 3.
    const auto samples = read(capture / "native.samples");
    UNSCOPED_INFO("native.samples holds two 1280x720 planes");
    REQUIRE(samples.size() == kPlaneBytes * 2);
    const auto          gamma = swapRecord(0).gammaTable;
    NativeRendererD3D12 renderer;
    const bool          started = renderer.StartDrawStream(error);
    UNSCOPED_INFO("native stream: " + error);
    REQUIRE(started);
    std::array<NativeDrawReplayResult, 2> rendered;
    for (std::size_t pass = 0; pass < frames.size(); ++pass)
        rendered[pass] = renderer.SubmitRenderFrame(std::move(frames[pass]));
    renderer.Shutdown();
    for (std::size_t pass = 0; pass < frames.size(); ++pass)
    {
        UNSCOPED_INFO("assembled frame render: " + rendered[pass].error);
        REQUIRE((rendered[pass].success && rendered[pass].resolvedOutput.size() == kPlaneBytes));
        std::size_t mismatches = 0, lit = 0;
        for (std::size_t pixel = 0; pixel < kPlaneBytes / 4; ++pixel)
        {
            std::uint32_t expected = 3U << 30;
            for (unsigned channel = 0; channel < 3; ++channel)
            {
                // The frame resolve rounds the two RGBA8 samples half up.
                const auto source = swizzles[pass][channel];
                const auto index  = (samples[pixel * 4 + source] + samples[kPlaneBytes + pixel * 4 + source] + 1U) >> 1;
                lit += index != 0;
                expected |= (gamma[index] & 1023U) << (10 * channel);
            }
            std::uint32_t actual = 0;
            std::memcpy(&actual, rendered[pass].resolvedOutput.data() + pixel * 4, 4);
            mismatches += actual != expected;
        }
        UNSCOPED_INFO("the reference panel covers the frame");
        REQUIRE(lit > 100000);
        UNSCOPED_INFO("assembled frame pixels differ from the M1a native samples at " + std::to_string(mismatches) + " pixels");
        REQUIRE(mismatches == 0);
    }
    std::cout << "Assembled frame output equals the M1a native samples through resolve and gamma, alpha in red and blue\n";

    // Three blended panels accumulate alpha, so a segment that lost its
    // target would change the output. Alpha is routed into red.
    const auto panels = [&](INativeCompletionSource* source)
    {
        NativeFrameAssemblerConfig config;
        config.readback   = true;
        config.completion = source;
        config.build      = [&](const NativeDrawSubmission& submission, NativeDrawReplayRecipe& draw, std::string& detail)
        {
            return submission.hook == NativeDrawHook::IndexedUp && BuildSolidPanelDraw(inputs, shader, draw, detail);
        };
        return config;
    };
    const auto draw = [&](NativeFrameAssembler& assembler, std::uint64_t sequence)
    {
        const auto&          a = inputs.arguments;
        NativeDrawSubmission submission;
        submission.hook      = NativeDrawHook::IndexedUp;
        submission.caller    = static_cast<std::uint32_t>(a.caller);
        submission.stack     = 0x70000000;
        submission.arguments = { a.graphics, a.primitive, a.minimumVertex, a.vertexCount, a.indexCount, a.indices, a.indexFormat, a.vertices };
        submission.stateRead = true;
        submission.sequence  = sequence;
        assembler.Began(submission);
        const auto verdict = assembler.Admit(submission);
        UNSCOPED_INFO("the panel is admitted");
        REQUIRE(!verdict);
        assembler.Recorded(submission, verdict);
    };
    NativeFrameAssembler oneShot(panels(nullptr));
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
        draw(oneShot, sequence);
    scanOut(oneShot);
    auto swap            = swapRecord(swizzleWords[0]);
    swap.numberedThrough = 3;
    auto whole           = oneShot.Swap(swap);
    UNSCOPED_INFO("one-shot frame: " + whole.detail);
    REQUIRE((whole.frame && whole.status == NativeFrameStatus::Provisional));
    NativeRendererD3D12 single;
    UNSCOPED_INFO("one-shot stream: " + error);
    REQUIRE(single.StartDrawStream(error));
    const auto expected = single.SubmitRenderFrame(std::move(*whole.frame));
    single.Shutdown();
    UNSCOPED_INFO("one-shot render: " + expected.error);
    REQUIRE((expected.success && expected.resolvedOutput.size() == kPlaneBytes));
    UNSCOPED_INFO("three panels differ from one, so a lost preserve is visible");
    REQUIRE(expected.resolvedOutput != rendered[0].resolvedOutput);

    NativeRendererD3D12 stream;
    UNSCOPED_INFO("segmented stream: " + error);
    REQUIRE(stream.StartDrawStream(error));
    {
        auto                  producer = std::make_shared<RendererProducer>(stream);
        D3D12CompletionSource source({ producer });
        NativeFrameAssembler  segmented(panels(&source));
        for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
            draw(segmented, sequence);
        const auto first  = segmented.FlushThrough(1);
        const auto second = segmented.FlushThrough(2);
        UNSCOPED_INFO("two segments: " + first.error + second.error);
        REQUIRE((first.fence == 1 && second.fence == 2));
        UNSCOPED_INFO("both prefixes complete at their reported fence values");
        REQUIRE(source.WaitFor(2, std::chrono::seconds(10)));
        scanOut(segmented);
        const auto closing = segmented.Swap(swap);
        UNSCOPED_INFO("final span: " + closing.detail);
        REQUIRE((closing.status == NativeFrameStatus::Provisional && closing.hostFence == 3));
        UNSCOPED_INFO("the final span completes");
        REQUIRE((source.WaitFor(3, std::chrono::seconds(10)) && producer->terminals.size() == 1));
        const auto& output = producer->terminals[0].get();
        UNSCOPED_INFO("segments plus the final span equal the one-shot frame bit-exactly: " + output.error);
        REQUIRE((output.success && output.resolvedOutput == expected.resolvedOutput));

        // A preserve onto a target other than the last segment's never reaches
        // the renderer. The scanout took mark 4.
        draw(segmented, 5);
        UNSCOPED_INFO("the next epoch's segment queues");
        REQUIRE(segmented.FlushThrough(5).fence == 4);
        NativeCompletionSpan wrong;
        wrong.epoch   = 2;
        wrong.segment = true;
        NativeRenderFramePass pass;
        pass.target                      = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
        pass.preservePreviousAttachments = true;
        wrong.frame.emplace();
        wrong.frame->passes.push_back(std::move(pass));
        std::string rejected;
        UNSCOPED_INFO("a wrong-target preserve is rejected: " + rejected);
        REQUIRE((!source.Submit(std::move(wrong), rejected) && rejected == "preserve_target_mismatch"));
        auto next            = swapRecord(swizzleWords[0]);
        next.sequence        = 2;
        next.numberedThrough = 4;
        scanOut(segmented);
        UNSCOPED_INFO("the epoch still completes after the rejection");
        REQUIRE((segmented.Swap(next).hostFence == 5 && source.WaitFor(5, std::chrono::seconds(10)) && producer->terminals[1].get().success));
        UNSCOPED_INFO("nothing failed");
        REQUIRE((!source.Failure() && !segmented.Failure()));
        source.Close();
    }
    stream.Shutdown();
    std::cout << "Segments and the final span equal the one-shot frame; a wrong-target preserve is rejected\n";

    // The boot frame's tiled form: the tile-0 predicated clear of the logical
    // screen and two tile scanouts must render as one full-clear frame.
    GuestTiling tiling;
    tiling.flags         = 0x20;
    tiling.count         = 2;
    tiling.rects[0]      = { 0, 0, 640, 720 };
    tiling.rects[1]      = { 640, 0, 1280, 720 };
    tiling.screenWidth   = 1280;
    tiling.screenHeight  = 720;
    tiling.predicateMask = 3;
    tiling.predicated    = 1;
    GuestClear full;
    full.target.native   = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0 };
    full.flags           = 0x3F;
    full.clear.rectangle = { 0, 0, 1280, 720 };
    full.clear.color = full.clear.depth = full.clear.stencil = true;
    full.clear.colorValue                                    = { 0, 0, 0, 1 };
    full.depth                                               = GuestDepthSurface{ 0x5000, 0x530, 0x530, 1280, 720, 4 };
    auto tiled                                               = full;
    tiled.target.tiling                                      = tiling;
    tiled.tiling                                             = tiling;
    tiled.predicate                                          = 1;
    // Tile 0's scanout clear words start tile 1: opaque black and far depth
    // as the tiled clear, unless `colorClear` names another RB_COLOR_CLEAR.
    const auto frameOf = [&](const GuestClear& clear, bool scanout, std::uint32_t colorClear = 0xFF000000)
    {
        NativeFrameAssembler assembler(panels(nullptr));
        std::string          detail;
        UNSCOPED_INFO("the clear is admitted: " + detail);
        REQUIRE(assembler.Clear(clear, detail) != 0);
        draw(assembler, 1);
        for (std::uint32_t tile = 0; scanout && tile < 2; ++tile)
        {
            GuestResolveCopy copy;
            copy.copyControl = tile ? 0x00100040 : 0x00100340;
            copy.colorClear  = colorClear;
            copy.depthClear  = 0xFFFFFF00;
            // The title's scanout destination, k_8_8_8_8 without the red/blue
            // swap, so the presentation stays that of the one-shot frame and
            // its unswapped untiled scanout.
            copy.destBase  = kFrontbuffer;
            copy.destPitch = kScanoutDestPitch;
            copy.destInfo  = 0x300;
            copy.rectX = copy.destX = tile * 640;
            copy.rectWidth          = 640;
            copy.rectHeight         = 720;
            bool partial            = false;
            // 0x8250AC90 predicates each tile's scanout to that tile.
            auto target                  = clear.target;
            target.tiling->predicateMask = 3U << (2 * tile);
            UNSCOPED_INFO("tile scanout: " + detail);
            REQUIRE(assembler.Resolve(target, copy, true, partial, detail) != 0);
        }
        if (!scanout)
            scanOut(assembler);
        auto record            = swapRecord(swizzleWords[0]);
        record.numberedThrough = 1;
        auto result            = assembler.Swap(record);
        UNSCOPED_INFO("a presented frame: " + result.detail);
        REQUIRE((result.frame && result.status == NativeFrameStatus::Provisional && result.frame->passes.front().presented));
        return std::move(*result.frame);
    };
    NativeRendererD3D12 tiledRenderer;
    UNSCOPED_INFO("tiled stream: " + error);
    REQUIRE(tiledRenderer.StartDrawStream(error));
    // A transparent scanout clear starts tile 1 transparent: the frame is the
    // opaque full-clear frame on tile 0 and the transparent one on tile 1.
    auto transparent             = full;
    transparent.clear.colorValue = { 0, 0, 0, 0 };
    const auto fullOutput        = tiledRenderer.SubmitRenderFrame(frameOf(full, false));
    const auto tiledOutput       = tiledRenderer.SubmitRenderFrame(frameOf(tiled, true));
    const auto transparentOutput = tiledRenderer.SubmitRenderFrame(frameOf(transparent, false));
    const auto splitOutput       = tiledRenderer.SubmitRenderFrame(frameOf(tiled, true, 0));
    tiledRenderer.Shutdown();
    UNSCOPED_INFO("tiled renders: " + fullOutput.error + tiledOutput.error + transparentOutput.error + splitOutput.error);
    REQUIRE((fullOutput.success && tiledOutput.success && transparentOutput.success && splitOutput.success));
    UNSCOPED_INFO("the opaque clear differs from the assumed transparent target");
    REQUIRE(fullOutput.resolvedOutput != rendered[0].resolvedOutput);
    UNSCOPED_INFO("the tiled frame equals the one-shot full-clear frame bit-exactly");
    REQUIRE(tiledOutput.resolvedOutput == fullOutput.resolvedOutput);
    UNSCOPED_INFO("the transparent clear is visible");
    REQUIRE(transparentOutput.resolvedOutput != fullOutput.resolvedOutput);
    std::size_t splitMismatches = 0;
    for (std::size_t pixel = 0; pixel < kPlaneBytes / 4; ++pixel)
    {
        const auto& expected = pixel % 1280 < 640 ? fullOutput.resolvedOutput : transparentOutput.resolvedOutput;
        splitMismatches += std::memcmp(splitOutput.resolvedOutput.data() + pixel * 4, expected.data() + pixel * 4, 4) != 0;
    }
    UNSCOPED_INFO("tile 1 starts from the scanout's transparent clear; mismatches " + std::to_string(splitMismatches));
    REQUIRE((splitOutput.resolvedOutput.size() == kPlaneBytes && splitMismatches == 0));
    std::cout << "The tiled clear and scanouts equal the one-shot full-clear panel frame; tile 1 starts from the scanout clear\n";

    // A presented depth-only clear (flags 0x30) between two panels: the
    // assembler makes it a preserving pass clear on the presented depth surface.
    const auto depthFrame = [&](int count, std::optional<float> depthValue)
    {
        NativeFrameAssembler assembler(panels(nullptr));
        std::string          detail;
        UNSCOPED_INFO("the full clear is admitted: " + detail);
        REQUIRE(assembler.Clear(full, detail) != 0);
        draw(assembler, 1);
        if (depthValue)
        {
            GuestClear depthOnly;
            depthOnly.target.native   = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8, 0x530 };
            depthOnly.flags           = 0x30;
            depthOnly.index           = kGuestDepthOnlyClearIndex;
            depthOnly.clear.rectangle = { 0, 0, 1280, 720 };
            depthOnly.clear.depth = depthOnly.clear.stencil = true;
            depthOnly.clear.depthValue                      = *depthValue;
            depthOnly.depth                                 = full.depth;
            UNSCOPED_INFO("the depth-only clear is admitted: " + detail);
            REQUIRE(assembler.Clear(depthOnly, detail) != 0);
        }
        if (count == 2)
            draw(assembler, 2);
        scanOut(assembler);
        auto record            = swapRecord(swizzleWords[0]);
        record.numberedThrough = static_cast<std::uint64_t>(count);
        auto result            = assembler.Swap(record);
        UNSCOPED_INFO("a depth frame: " + result.detail);
        REQUIRE((result.frame && result.status == NativeFrameStatus::Provisional));
        UNSCOPED_INFO("the depth clear is a preserving pass clear on the presented depth surface");
        REQUIRE((!depthValue || count != 2 ||
                 (result.frame->passes.size() == 2 && result.frame->passes[1].preservePreviousAttachments &&
                  result.frame->passes[1].clears.size() == 1 && !result.frame->passes[1].clears[0].color &&
                  result.frame->passes[1].target.depthEdramBase == 0x530)));
        return std::move(*result.frame);
    };
    // The second panel tests LESS against the cleared depth without writing it.
    const auto lessTested = [](NativeRenderFrame frame)
    {
        // Admitted recipes are immutable: edit a raw copy of the pass.
        auto& pass = frame.passes.back();
        for (const auto& admitted : pass.admittedDraws)
            pass.draws.push_back(admitted->Recipe());
        pass.admittedDraws.clear();
        auto& second              = pass.draws.back();
        second.depth.enabled      = true;
        second.depth.writeEnabled = false;
        second.depth.compare      = 2;
        return frame;
    };
    NativeRendererD3D12 depthRenderer;
    UNSCOPED_INFO("depth stream: " + error);
    REQUIRE(depthRenderer.StartDrawStream(error));
    const auto one        = depthRenderer.SubmitRenderFrame(depthFrame(1, std::nullopt));
    const auto two        = depthRenderer.SubmitRenderFrame(depthFrame(2, std::nullopt));
    const auto cleared    = depthRenderer.SubmitRenderFrame(depthFrame(2, 1.0F));
    const auto lessAtZero = depthRenderer.SubmitRenderFrame(lessTested(depthFrame(2, 0.0F)));
    const auto lessAtOne  = depthRenderer.SubmitRenderFrame(lessTested(depthFrame(2, 1.0F)));
    depthRenderer.Shutdown();
    UNSCOPED_INFO("depth renders: " + one.error + two.error + cleared.error + lessAtZero.error + lessAtOne.error);
    REQUIRE((one.success && two.success && cleared.success && lessAtZero.success && lessAtOne.success));
    UNSCOPED_INFO("the second blended panel is visible");
    REQUIRE(one.resolvedOutput != two.resolvedOutput);
    UNSCOPED_INFO("a depth-only clear between depth-disabled panels leaves color bit-identical");
    REQUIRE(cleared.resolvedOutput == two.resolvedOutput);
    UNSCOPED_INFO("a LESS-tested panel after a depth clear to 0.0 writes nothing");
    REQUIRE(lessAtZero.resolvedOutput == one.resolvedOutput);
    UNSCOPED_INFO("a LESS-tested panel after a depth clear to 1.0 writes its pixels");
    REQUIRE(lessAtOne.resolvedOutput == two.resolvedOutput);
    std::cout << "A presented depth-only clear executes between panels and leaves color unchanged\n";
}

} // namespace

TEST_CASE("native_frame_assembler_resolve_versions", "[ctest][native-frame-assembler-resolve-versions][native_gpu][timeout-120]")
{
    char enabled[8]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) == 0 || std::string(enabled) != "1")
    {
        SKIP("Set REREVVED_TEST_NATIVE_D3D12=1 to run the capture-free ordered surface check");
    }
    if (!hasD3D12Device())
    {
        SKIP("No D3D12 device; the capture-free surface check was not rendered");
    }
    rerevved::tests::ScopedLogging logging;
    checkOrderedSurfaceVersions();
}

TEST_CASE("native_frame_assembler_resolve_graph_versions", "[ctest][native-frame-assembler-resolve-versions][native_gpu][timeout-120]")
{
    char enabled[8]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) == 0 || std::string(enabled) != "1")
    {
        SKIP("Set REREVVED_TEST_NATIVE_D3D12=1 to run the capture-free ordered graph surface check");
    }
    if (!hasD3D12Device())
    {
        SKIP("No D3D12 device; the capture-free surface check was not rendered");
    }
    rerevved::tests::ScopedLogging logging;
    checkOrderedSurfaceVersions(true);
}

TEST_CASE("native_frame_assembler_resolve_graph_tiling", "[ctest][native-frame-assembler-resolve-versions][native_gpu][timeout-120]")
{
    char enabled[8]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) == 0 || std::string(enabled) != "1")
    {
        SKIP("Set REREVVED_TEST_NATIVE_D3D12=1 to run the capture-free staged graph surface check");
    }
    if (!hasD3D12Device())
    {
        SKIP("No D3D12 device; the capture-free surface check was not rendered");
    }
    rerevved::tests::ScopedLogging logging;
    checkOrderedGraphTiling();
}

TEST_CASE("native_frame_assembler_staged_tiling_versions", "[ctest][native-frame-assembler-resolve-versions][native_gpu][timeout-120]")
{
    char enabled[8]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) == 0 || std::string(enabled) != "1")
    {
        SKIP("Set REREVVED_TEST_NATIVE_D3D12=1 to run the capture-free staged tiling surface check");
    }
    if (!hasD3D12Device())
    {
        SKIP("No D3D12 device; the capture-free surface check was not rendered");
    }
    rerevved::tests::ScopedLogging logging;
    checkOrderedSurfaceVersions(false, true);
}

TEST_CASE("native_frame_assembler_d3d12", "[ctest][release-smoke][native-frame-assembler-d3d12][native_gpu][timeout-120]")
{
    char buffer[4096]{}, device[8]{};
    // REREVVED_TEST_NATIVE_D3D12=1 runs the capture-free device check.
    const bool native = GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", device, sizeof(device)) != 0 && std::string(device) == "1";
    // Only an unset capture skips; a set but unusable path fails below.
    const bool capture = GetEnvironmentVariableA("REREVVED_SOLID_ADAPTER_CAPTURE", buffer, sizeof(buffer)) != 0 && buffer[0];
    if (!native && !capture)
    {
        SKIP("Set REREVVED_SOLID_ADAPTER_CAPTURE to an M1a solid adapter capture, or REREVVED_TEST_NATIVE_D3D12=1, to run this check");
    }
    rerevved::tests::ScopedLogging logging;
    if (native)
    {
        checkTiledMrtResolves(false);
        checkTiledMrtResolves(true);
        checkTiledMrtResolves(false, true);
        checkTiledMrtResolves(true, true);
        checkPresentedBaseResolves();
        checkSegmentWhileTerminalInFlight();
        checkAliasBytes();
        checkAliasAcrossReset();
        checkSurfaceRegistry();
        checkHeldSurfaceImage();
        checkTiledSurface();
        // census-0009: the world bracket's two tiles, sampled from a segment.
        checkTiledSurface(2, true);
    }
    if (!capture)
        return;
    checkRawResolve();
    run(buffer);
}
