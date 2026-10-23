// Multiple-render-target frames and guest-word resolves. The default mode
// checks frame validation; "d3d12" renders through the native renderer and
// compares packed words with an independent CPU model of the SDK resolve.

#include "gpu/d3d12/native_render_frame.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_texture_upload.h"
#include "native_render_frame_test_access.h"
#include "stream_queue_fixture.h"

#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rerevved::gpu;

namespace
{

constexpr std::uint32_t kWidth = 1280, kHeight = 720;

NativeRenderFramePass::Target mrtTarget(std::uint32_t width = kWidth, std::uint32_t height = kHeight)
{
    // The post-ESRB pass: RT0 k_8_8_8_8 at EDRAM 0, RT1 k_2_10_10_10 at 0x21C, depth at 0x5E4.
    NativeRenderFramePass::Target target{ width, height, 4, NativeDrawReplayTargetFormat::Rgba8, 0, 0x5E4 };
    target.extraColorTargets  = 1;
    target.extraFormats[0]    = NativeDrawReplayTargetFormat::Rgb10a2;
    target.extraEdramBases[0] = 0x21C;
    return target;
}

// Validation needs only well-formed byte sizes; the D3D12 mode compiles real shaders.
NativeDrawReplayRecipe mrtDraw(std::uint32_t width = kWidth, std::uint32_t height = kHeight)
{
    NativeDrawReplayRecipe draw;
    draw.schemaVersion    = 2;
    draw.width            = width;
    draw.height           = height;
    draw.sampleCount      = 4;
    draw.sampleMask       = 9;
    draw.vertexShaderHash = 1;
    draw.pixelShaderHash  = 2;
    draw.vertexShaderDxil.assign(4, 0);
    draw.pixelShaderDxil.assign(4, 0);
    draw.vertexAttributeCount                           = 1;
    draw.vertexStrideBytes                              = 16;
    const std::array<std::array<float, 4>, 3> positions = { { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } } };
    draw.vertexData.resize(sizeof(positions));
    std::memcpy(draw.vertexData.data(), positions.data(), sizeof(positions));
    draw.indices    = { 0, 1, 2 };
    draw.indexCount = 3;
    draw.vertexConstants.resize(4096);
    draw.pixelConstants.resize(3584);
    draw.sharedConstants.resize(336);
    draw.viewport               = { 0, 0, float(width), float(height), 0, 1 };
    draw.scissor                = { 0, 0, std::int32_t(width), std::int32_t(height) };
    draw.extraTargetCount       = 1;
    draw.extraTargets[0].format = NativeDrawReplayTargetFormat::Rgb10a2;
    return draw;
}

NativeRenderFramePass::Clear colorClear(NativeDrawReplayScissor rect, std::uint8_t targets, std::array<float, 4> value)
{
    NativeRenderFramePass::Clear clear;
    clear.rectangle    = rect;
    clear.color        = true;
    clear.colorTargets = targets;
    clear.colorValue   = value;
    return clear;
}

NativeRenderFrameResolve guestResolve(std::uint64_t texture, bool depth, std::uint32_t colorTarget, NativeGuestWordPacking packing, std::array<std::uint32_t, 2> samples, std::uint32_t width = kWidth, std::uint32_t height = kHeight)
{
    NativeRenderFrameResolve resolve;
    resolve.textureId   = texture;
    resolve.depth       = depth;
    resolve.colorTarget = colorTarget;
    resolve.width       = width;
    resolve.height      = height;
    resolve.samples     = samples;
    resolve.guestWords  = packing;
    return resolve;
}

// One producer frame: per-slot clears, one MRT draw, guest-word resolves of RT0, RT1 and depth.
NativeRenderFrame acceptedFrame()
{
    NativeRenderFrame frame;
    for (std::uint64_t id = 1; id <= 3; ++id)
        frame.textures.push_back({ id, kWidth, kHeight, NativeDrawReplayTextureFormat::R32Uint, id });
    NativeRenderFramePass pass;
    pass.target = mrtTarget();
    auto first  = colorClear({ 0, 0, 448, 720 }, 1, { 0, 0, 0, 1 });
    first.depth = first.stencil = true;
    pass.clears.push_back(first);
    pass.clears.push_back(colorClear({ 0, 0, 448, 720 }, 2, { 0, 0, 0, 1 }));
    pass.draws.push_back(mrtDraw());
    pass.resolves.push_back(guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, { 0, 3 }));
    pass.resolves.push_back(guestResolve(2, false, 1, NativeGuestWordPacking::Color2101010, { 0, 3 }));
    pass.resolves.push_back(guestResolve(3, true, 0, NativeGuestWordPacking::Depth24Stencil8, { 0, 0 }));
    frame.passes.push_back(std::move(pass));
    frame.outputTextureId = 1;
    frame.retainOnly = frame.returnRawOutput = true;
    return frame;
}

// A prefix packet for corpus frames to continue: one cleared single-sample
// pass resolving texture 9, which no corpus frame declares.
NativeRenderFrame prefixPacket()
{
    NativeRenderFrame packet;
    packet.textures.push_back({ 9, kWidth, kHeight, NativeDrawReplayTextureFormat::Rgba8 });
    NativeRenderFramePass pass;
    pass.target = { kWidth, kHeight, 1, NativeDrawReplayTargetFormat::Rgba8 };
    pass.clears.push_back(colorClear({ 0, 0, std::int32_t(kWidth), std::int32_t(kHeight) }, 1, { 0, 0, 0, 1 }));
    pass.resolves.push_back(guestResolve(9, false, 0, NativeGuestWordPacking::None, { 0, 0 }));
    packet.passes.push_back(std::move(pass));
    return packet;
}

// Validates `frame` as a tail terminal continuing `packet` both ways: in place
// through the prefix state, and joined into one graph validated alone. The
// two verdicts and messages must agree.
bool chainedValidation(const NativeRenderFrame& packet, const NativeRenderFrame& frame, std::string& error)
{
    NativeRenderFramePrefixState chain;
    UNSCOPED_INFO("the prefix packet appends: " + error);
    REQUIRE(chain.Append(packet, error));
    auto tail                  = frame;
    tail.continuedPrefixPasses = packet.passes.size();
    std::string chainedError;
    const bool  chained = chain.FinishTerminal(tail, chainedError);
    auto        passes  = packet.passes;
    UNSCOPED_INFO("the tail joins its prefix: " + error);
    REQUIRE(JoinNativeRenderFrameTail(tail, std::move(passes), packet.textures, packet.typedGuestWords, error));
    const bool joined = ValidateNativeRenderFrame(tail, error);
    UNSCOPED_INFO(std::format("in-place and joined validation agree: '{}' against '{}'", chainedError, error));
    REQUIRE((chained == joined && chainedError == error));
    return joined;
}

void rejects(const char* name, const std::function<void(NativeRenderFrame&)>& mutate, const std::string& message)
{
    auto frame = acceptedFrame();
    mutate(frame);
    std::string error;
    const bool  admitted = ValidateNativeRenderFrame(frame, error);
    UNSCOPED_INFO(std::string(name) + ": expected '" + message + "', got '" + error + "'");
    REQUIRE((!admitted && error == message));
    UNSCOPED_INFO(std::string(name) + " as a tail: expected '" + message + "', got '" + error + "'");
    REQUIRE((!chainedValidation(prefixPacket(), frame, error) && error == message));
}

void runValidation()
{
    std::string error;
    UNSCOPED_INFO("the MRT producer frame is admitted: " + error);
    REQUIRE(ValidateNativeRenderFrame(acceptedFrame(), error));
    UNSCOPED_INFO("the MRT producer frame is admitted as a tail: " + error);
    REQUIRE(chainedValidation(prefixPacket(), acceptedFrame(), error));

    constexpr auto mrtUnsupported = "render frame MRT attachments are unsupported";
    constexpr auto compatible     = "render frame pass requires compatible targets and native attachment initialization";
    constexpr auto clearInvalid   = "render frame clear rectangle or value is invalid";
    constexpr auto resolveInvalid = "render frame resolve rectangle, source or scale is invalid";
    constexpr auto guestInvalid   = "render frame guest-word resolve is invalid";
    constexpr auto mrtPresented   = "render frame MRT pass cannot be presented or aliased";
    constexpr auto guestSampled   = "render frame draw samples a guest-word texture";
    constexpr auto conversion     = "render frame color resolve conversion is unsupported";
    rejects("four extras", [](auto& frame)
            {
                frame.passes[0].target.extraColorTargets = 4;
            },
            mrtUnsupported);
    rejects("an Rg16Snorm extra", [](auto& frame)
            {
                frame.passes[0].target.extraFormats[0] = NativeDrawReplayTargetFormat::Rg16Snorm;
            },
            mrtUnsupported);
    rejects("an unused extra with an identity", [](auto& frame)
            {
                frame.passes[0].target.extraEdramBases[1] = 0x438;
            },
            mrtUnsupported);
    rejects("a draw whose extras differ", [](auto& frame)
            {
                frame.passes[0].draws[0].extraTargets[0].format = NativeDrawReplayTargetFormat::Rgba8;
            },
            compatible);
    rejects("a single-target draw in an MRT pass", [](auto& frame)
            {
                frame.passes[0].draws[0].extraTargetCount = 0;
                frame.passes[0].draws[0].extraTargets[0]  = {};
            },
            compatible);
    rejects("a clear naming RT2 on a two-target pass", [](auto& frame)
            {
                frame.passes[0].clears[1].colorTargets = 4;
            },
            clearInvalid);
    rejects("a resolve of colorTarget 2", [](auto& frame)
            {
                frame.passes[0].resolves[1].colorTarget = 2;
            },
            resolveInvalid);
    rejects("Color2101010 on the Rgba8 slot", [](auto& frame)
            {
                frame.passes[0].resolves[0].guestWords = NativeGuestWordPacking::Color2101010;
            },
            guestInvalid);
    rejects("Float16x2 on the Rgb10a2 slot", [](auto& frame)
            {
                frame.passes[0].resolves[1].guestWords = NativeGuestWordPacking::Float16x2;
            },
            guestInvalid);
    {
        // The k_16_16 fog copy: a lone Rg16Snorm target packs halves.
        NativeRenderFrame fog;
        fog.textures.push_back({ 1, kWidth, kHeight, NativeDrawReplayTextureFormat::R32Uint, 1 });
        NativeRenderFramePass pass;
        pass.target = { kWidth, kHeight, 1, NativeDrawReplayTargetFormat::Rg16Snorm, 0x2D0 };
        pass.clears.push_back(colorClear({ 0, 0, std::int32_t(kWidth), std::int32_t(kHeight) }, 1, { 0, 0, 0, 0 }));
        pass.resolves.push_back(guestResolve(1, false, 0, NativeGuestWordPacking::Float16x2, { 0, 0 }));
        fog.passes.push_back(std::move(pass));
        fog.outputTextureId = 1;
        fog.retainOnly = fog.returnRawOutput = true;
        UNSCOPED_INFO("the k_16_16 producer frame is admitted: " + error);
        REQUIRE(ValidateNativeRenderFrame(fog, error));
        fog.passes[0].resolves[0].guestWords = NativeGuestWordPacking::Color8888;
        UNSCOPED_INFO("Color8888 on the Rg16Snorm slot: " + error);
        REQUIRE((!ValidateNativeRenderFrame(fog, error) && error == guestInvalid));
    }
    rejects("Color8888 on a depth source", [](auto& frame)
            {
                frame.passes[0].resolves[2].guestWords = NativeGuestWordPacking::Color8888;
            },
            guestInvalid);
    rejects("Depth24Stencil8 with samples {0,3}", [](auto& frame)
            {
                frame.passes[0].resolves[2].samples = { 0, 3 };
            },
            guestInvalid);
    rejects("guest words into an Rgba8 texture", [](auto& frame)
            {
                frame.textures[1].format = NativeDrawReplayTextureFormat::Rgba8;
            },
            guestInvalid);
    rejects("scaled guest words", [](auto& frame)
            {
                frame.passes[0].resolves[0].valueScale = 2.0F;
            },
            guestInvalid);
    rejects("swapped guest words", [](auto& frame)
            {
                frame.passes[0].resolves[0].swapRedBlue = true;
            },
            guestInvalid);
    rejects("a presented MRT pass", [](auto& frame)
            {
                frame.passes[0].presented = true;
            },
            mrtPresented);
    rejects("an MRT pass with colorSourcePass", [](auto& frame)
            {
                NativeRenderFramePass source;
                source.target = { kWidth, kHeight, 1, NativeDrawReplayTargetFormat::Rgba8 };
                source.clears.push_back(colorClear({ 0, 0, 16, 16 }, 1, { 0, 0, 0, 1 }));
                frame.passes.insert(frame.passes.begin(), std::move(source));
                frame.passes[1].colorSourcePass = 1;
            },
            mrtPresented);
    const auto sampling = [](NativeDrawReplayTextureFormat binding)
    {
        return [binding](NativeRenderFrame& frame)
        {
            auto& draw             = frame.passes[0].draws[0];
            draw.textureMask       = 1;
            auto& texture          = draw.textures[0];
            texture.width          = kWidth;
            texture.height         = kHeight;
            texture.format         = binding;
            texture.frameTextureId = 1;
        };
    };
    rejects("a draw sampling a guest-word texture", sampling(NativeDrawReplayTextureFormat::R32Uint), guestSampled);
    rejects("a draw binding a guest-word texture as RGBA8", sampling(NativeDrawReplayTextureFormat::Rgba8), guestSampled);
    rejects("a draw binding a CPU payload as R32Uint", [](auto& frame)
            {
                auto& draw       = frame.passes[0].draws[0];
                draw.textureMask = 1;
                auto& texture    = draw.textures[0];
                texture.width = texture.height = 1;
                texture.format                 = NativeDrawReplayTextureFormat::R32Uint;
                texture.bytes.assign(4, 0);
            },
            guestSampled);
    // Later passes view castable storage as RGBA8, R10G10B10A2 or R16G16_FLOAT
    // by producer view rules.
    const auto laterRead = [](std::uint64_t id, NativeDrawReplayTextureFormat view)
    {
        return [id, view](NativeRenderFrame& frame)
        {
            auto draw        = mrtDraw();
            draw.sampleCount = draw.sampleMask = 1;
            draw.extraTargetCount              = 0;
            draw.extraTargets[0]               = {};
            draw.textureMask                   = 1;
            auto& texture                      = draw.textures[0];
            texture.width                      = kWidth;
            texture.height                     = kHeight;
            texture.format                     = view;
            texture.frameTextureId             = id;
            NativeRenderFramePass read;
            read.target = { kWidth, kHeight, 1, NativeDrawReplayTargetFormat::Rgba8 };
            read.draws.push_back(std::move(draw));
            frame.passes.push_back(std::move(read));
            frame.typedGuestWords = true;
        };
    };
    for (const auto& [id, view] : { std::pair{ 1, NativeDrawReplayTextureFormat::Rgba8 }, std::pair{ 2, NativeDrawReplayTextureFormat::Rgb10a2 }, std::pair{ 1, NativeDrawReplayTextureFormat::Rgb10a2 }, std::pair{ 1, NativeDrawReplayTextureFormat::Rg16Float }, std::pair{ 2, NativeDrawReplayTextureFormat::Rgba8 }, std::pair{ 3, NativeDrawReplayTextureFormat::Rgba8 } })
    {
        auto typed = acceptedFrame();
        laterRead(id, view)(typed);
        UNSCOPED_INFO("a typed view of packed words is admitted: " + error);
        REQUIRE(ValidateNativeRenderFrame(typed, error));
    }
    rejects("an undeclared typed view of packed words", [&](auto& frame)
            {
                laterRead(1, NativeDrawReplayTextureFormat::Rgba8)(frame);
                frame.typedGuestWords = false;
            },
            guestSampled);
    rejects("packed words bound as R32Uint in a later pass", laterRead(1, NativeDrawReplayTextureFormat::R32Uint), guestSampled);
    {
        // A terminal frame may sample a resolve an appended segment produced:
        // the source runs it after the segment, and only the chain declares it.
        auto prefix = acceptedFrame();
        for (auto& texture : prefix.textures)
            texture.persistentKey = 0;
        prefix.outputTextureId = 0;
        prefix.retainOnly = prefix.returnRawOutput = false;
        NativeRenderFramePrefixState chain;
        UNSCOPED_INFO("the producer passes append: " + error);
        REQUIRE(chain.Append(prefix, error));
        NativeRenderFrame terminal;
        laterRead(1, NativeDrawReplayTextureFormat::Rgba8)(terminal);
        terminal.textures.push_back({ 9, kWidth, kHeight, NativeDrawReplayTextureFormat::Rgba8 });
        terminal.passes[0].resolves.push_back(guestResolve(9, false, 0, NativeGuestWordPacking::None, { 0, 0 }));
        terminal.outputTextureId = 9;
        terminal.returnGpuOutput = true;
        UNSCOPED_INFO("alone, the terminal samples an undeclared texture");
        REQUIRE(!ValidateNativeRenderFrame(terminal, error));
        UNSCOPED_INFO("after the chain, the terminal is admitted and the chain unchanged: " + error);
        REQUIRE((chain.ValidateTerminal(terminal, error) && chain.ValidateTerminal(terminal, error)));
        terminal.typedGuestWords = false;
        UNSCOPED_INFO("an undeclared typed view stays refused after the chain: " + error);
        REQUIRE((!chain.ValidateTerminal(terminal, error) && error == guestSampled));
        NativeRenderFrame next;
        laterRead(1, NativeDrawReplayTextureFormat::Rgba8)(next);
        UNSCOPED_INFO("a refused terminal leaves the chain appendable: " + error);
        REQUIRE(chain.Append(next, error));
    }
    {
        // A tail terminal continues exactly the appended passes and ends the
        // chain; a frame or packet naming a chain outside one is refused.
        constexpr auto noChain     = "native terminal does not continue this prefix chain";
        constexpr auto ended       = "native prefix chain already ended";
        auto           tail        = acceptedFrame();
        tail.continuedPrefixPasses = 1;
        UNSCOPED_INFO("a tail alone is refused: " + error);
        REQUIRE((!ValidateNativeRenderFrame(tail, error) && error == "render frame continues a prefix chain outside its prefix state"));
        auto continuing                  = prefixPacket();
        continuing.continuedPrefixPasses = 1;
        NativeRenderFramePrefixState packets;
        UNSCOPED_INFO("a packet cannot continue a chain: " + error);
        REQUIRE((!packets.Append(continuing, error) && error == "native prefix packet cannot continue a chain"));
        NativeRenderFramePrefixState empty;
        UNSCOPED_INFO("a tail without appended passes is refused: " + error);
        REQUIRE((!empty.FinishTerminal(tail, error) && error == noChain));
        for (const std::size_t count : { std::size_t(0), std::size_t(2) })
        {
            NativeRenderFramePrefixState chain;
            UNSCOPED_INFO(error);
            REQUIRE(chain.Append(prefixPacket(), error));
            tail.continuedPrefixPasses = count;
            UNSCOPED_INFO(std::format("a tail continuing {} of 1 appended passes is refused: {}", count, error));
            REQUIRE((!chain.FinishTerminal(tail, error) && error == noChain));
            UNSCOPED_INFO("a refused tail ends the chain: " + error);
            REQUIRE((!chain.Append(prefixPacket(), error) && error == ended));
        }
        NativeRenderFramePrefixState chain;
        UNSCOPED_INFO(error);
        REQUIRE(chain.Append(prefixPacket(), error));
        tail.continuedPrefixPasses = 1;
        UNSCOPED_INFO("a tail continuing the appended pass is admitted: " + error);
        REQUIRE(chain.FinishTerminal(tail, error));
        UNSCOPED_INFO("an admitted tail ends the chain: " + error);
        REQUIRE((!chain.ValidateTerminal(tail, error) && error == ended && !chain.FinishTerminal(tail, error) && error == ended));
        auto passes                = prefixPacket().passes;
        tail.continuedPrefixPasses = 2;
        UNSCOPED_INFO("a join of another length changes nothing: " + error);
        REQUIRE((!JoinNativeRenderFrameTail(tail, std::move(passes), {}, false, error) && error == noChain && passes.size() == 1 && tail.passes.size() == 1 &&
                 tail.continuedPrefixPasses == 2));
    }
    {
        // The join puts the prefix first, shifts the tail's color aliases past
        // it, declares each texture once with the prefix's after the tail's,
        // and combines the typed flags.
        auto packet            = prefixPacket();
        packet.typedGuestWords = true;
        packet.textures.push_back({ 11, kWidth, kHeight, NativeDrawReplayTextureFormat::Rgba8 });
        NativeRenderFrame tail;
        tail.textures.push_back(packet.textures[0]);
        tail.textures.push_back({ 10, 640, 360, NativeDrawReplayTextureFormat::Rgba8 });
        NativeRenderFramePass source;
        source.target = { 640, 360, 1, NativeDrawReplayTargetFormat::Rgba8 };
        source.clears.push_back(colorClear({ 0, 0, 640, 360 }, 1, { 0, 0, 0, 1 }));
        auto alias            = source;
        alias.colorSourcePass = 1;
        alias.resolves.push_back(guestResolve(10, false, 0, NativeGuestWordPacking::None, { 0, 0 }, 640, 360));
        tail.passes.push_back(std::move(source));
        tail.passes.push_back(std::move(alias));
        tail.outputTextureId       = 9;
        tail.returnGpuOutput       = true;
        tail.continuedPrefixPasses = 1;
        NativeRenderFramePrefixState chain;
        UNSCOPED_INFO("a tail aliasing its own pass is admitted: " + error);
        REQUIRE((chain.Append(packet, error) && chain.FinishTerminal(tail, error)));
        auto passes = packet.passes;
        UNSCOPED_INFO("the aliasing tail joins: " + error);
        REQUIRE(JoinNativeRenderFrameTail(tail, std::move(passes), packet.textures, packet.typedGuestWords, error));
        UNSCOPED_INFO("the joined passes put the prefix first and shift the alias");
        REQUIRE((tail.passes.size() == 3 && tail.passes[0].target == packet.passes[0].target && tail.passes[2].colorSourcePass == 2 && passes.empty()));
        UNSCOPED_INFO("the joined textures declare each identity once, and the flags combine");
        REQUIRE((tail.textures.size() == 3 && tail.textures[0].id == 9 && tail.textures[1].id == 10 && tail.textures[2].id == 11 && tail.typedGuestWords &&
                 !tail.continuedPrefixPasses));
        UNSCOPED_INFO("the joined graph is admitted: " + error);
        REQUIRE(ValidateNativeRenderFrame(tail, error));
    }
    {
        // A depth conversion reads every resolve into its texture, so color
        // words a prefix resolved refuse it in place as in the joined graph.
        auto packet = prefixPacket();
        packet.textures.push_back({ 7, kWidth, kHeight, NativeDrawReplayTextureFormat::R32Uint });
        packet.passes[0].resolves.push_back(guestResolve(7, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }));
        auto tail = acceptedFrame();
        tail.textures.push_back(packet.textures[1]);
        tail.surfaces.push_back({ 7, 7, 0x4000000000000007ULL });
        UNSCOPED_INFO("a tail converting a prefix's color words is refused: " + error);
        REQUIRE((!chainedValidation(packet, tail, error) && error == "render frame depth conversion needs Depth24Stencil8 words and its own key"));
    }
    rejects("mixed packings bound typed", [&](auto& frame)
            {
                // A Color2101010 strip into the Color8888 destination leaves no typed view.
                auto& resolves     = frame.passes[0].resolves;
                resolves[0].width  = 640;
                auto strip         = guestResolve(1, false, 1, NativeGuestWordPacking::Color2101010, { 0, 3 }, 640);
                strip.destinationX = 640;
                strip.sourceX      = 640;
                resolves.push_back(strip);
                laterRead(1, NativeDrawReplayTextureFormat::Rgba8)(frame);
            },
            guestSampled);
    rejects("an Rgb10a2 slot resolved to an Rgba8 destination", [](auto& frame)
            {
                frame.textures.push_back({ 4, kWidth, kHeight, NativeDrawReplayTextureFormat::Rgba8 });
                frame.passes[0].resolves.push_back(guestResolve(4, false, 1, NativeGuestWordPacking::None, { 0, 0 }));
            },
            conversion);
    rejects("unpacked words into an R32Uint texture", [](auto& frame)
            {
                frame.passes[0].resolves[0].guestWords = NativeGuestWordPacking::None;
            },
            conversion);
    rejects("a guest-word output without raw readback", [](auto& frame)
            {
                frame.returnRawOutput = false;
            },
            "render frame output requires a complete 1280x720 RGBA8 texture");

    // The Rgb10a2 slot resolves to its own format.
    auto converted = acceptedFrame();
    converted.textures.push_back({ 4, kWidth, kHeight, NativeDrawReplayTextureFormat::Rgb10a2 });
    converted.passes[0].resolves.push_back(guestResolve(4, false, 1, NativeGuestWordPacking::None, { 0, 0 }));
    UNSCOPED_INFO("an Rgb10a2 slot resolves to Rgb10a2: " + error);
    REQUIRE(ValidateNativeRenderFrame(converted, error));
    // Without extras the four same-format slots keep the 0xF clear rule.
    auto single               = acceptedFrame();
    single.passes[0].target   = { kWidth, kHeight, 4, NativeDrawReplayTargetFormat::Rgba8, 0, 0x5E4 };
    single.passes[0].draws[0] = []
    {
        auto draw             = mrtDraw();
        draw.extraTargetCount = 0;
        draw.extraTargets     = {};
        return draw;
    }();
    single.passes[0].clears[1].colorTargets = 0xE;
    single.passes[0].resolves[1]            = guestResolve(2, false, 3, NativeGuestWordPacking::Color8888, { 0, 3 });
    UNSCOPED_INFO("a single-target pass keeps four same-format slots: " + error);
    REQUIRE(ValidateNativeRenderFrame(single, error));

    // Recipe-level rules and the pipeline key.
    auto recipe = mrtDraw();
    UNSCOPED_INFO("the MRT recipe is valid: " + error);
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    const auto recipeRejects = [&](const char* name, const std::function<void(NativeDrawReplayRecipe&)>& mutate)
    {
        auto value = mrtDraw();
        mutate(value);
        UNSCOPED_INFO(std::string(name) + ": " + error);
        REQUIRE((!ValidateNativeDrawReplayRecipe(value, error) && error == mrtUnsupported));
    };
    recipeRejects("recipe with four extras", [](auto& value)
                  {
                      value.extraTargetCount = 4;
                  });
    recipeRejects("recipe extra beyond the count", [](auto& value)
                  {
                      value.extraTargets[1].format = NativeDrawReplayTargetFormat::Rgb10a2;
                  });
    recipeRejects("recipe extra with a constant blend factor", [](auto& value)
                  {
                      value.extraTargets[0].blend.sourceColor = NativeDrawReplayBlendFactor::BlendFactor;
                  });
    recipeRejects("recipe extra with initial samples", [](auto& value)
                  {
                      value.initialSample0.assign(std::size_t(kWidth) * kHeight * 4, 0);
                      value.initialSample1 = value.initialSample0;
                  });
    // A depth surface's conversion reads Depth24Stencil8 words of an R32Uint
    // texture into a store key of its own.
    constexpr auto depthConversion = "render frame depth conversion needs Depth24Stencil8 words and its own key";
    {
        auto frame = acceptedFrame();
        frame.surfaces.push_back({ 3, 3, 0x4000000000000003ULL });
        UNSCOPED_INFO("a depth surface's conversion is admitted: " + error);
        REQUIRE(ValidateNativeRenderFrame(frame, error));
    }
    rejects("conversion under the words' key", [](auto& frame)
            {
                frame.surfaces.push_back({ 3, 3, 3 });
            },
            depthConversion);
    rejects("conversion of color words", [](auto& frame)
            {
                frame.surfaces.push_back({ 1, 1, 0x4000000000000001ULL });
            },
            depthConversion);
    rejects("conversion of an RGBA8 texture", [](auto& frame)
            {
                frame.textures[2].format = NativeDrawReplayTextureFormat::Rgba8;
                frame.surfaces.push_back({ 3, 3, 0x4000000000000003ULL });
            },
            depthConversion);
    std::shared_ptr<const NativeDrawReplayAdmission> mrtAdmission, singleAdmission, blendAdmission;
    auto                                             singleRecipe = mrtDraw();
    singleRecipe.extraTargetCount                                 = 0;
    singleRecipe.extraTargets                                     = {};
    auto blended                                                  = mrtDraw();
    blended.extraTargets[0].blend.writeMask                       = 0;
    UNSCOPED_INFO("admission: " + error);
    REQUIRE((AdmitNativeDrawReplayRecipe(recipe, mrtAdmission, error) && AdmitNativeDrawReplayRecipe(singleRecipe, singleAdmission, error) &&
             AdmitNativeDrawReplayRecipe(blended, blendAdmission, error)));
    const auto& singleKey = singleAdmission->PipelineStateKey();
    UNSCOPED_INFO("a single-target key has no extra-target bytes");
    REQUIRE(std::all_of(singleKey.extraTargets.begin(), singleKey.extraTargets.end(), [](auto value)
                        {
                            return value == 0;
                        }));
    UNSCOPED_INFO("the pipeline key separates extra formats and extra blends");
    REQUIRE((mrtAdmission->PipelineStateKey() != singleKey && mrtAdmission->PipelineStateKey() != blendAdmission->PipelineStateKey()));
    std::cout << "MRT frame validation admits the producer frame and refuses each malformed form by message\n";
}

// ---------------------------------------------------------------------------
// D3D12 mode.

std::vector<std::uint8_t> compile(const char* source, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const auto                       hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    UNSCOPED_INFO((errors ? static_cast<const char*>(errors->GetBufferPointer()) : "shader compilation failed"));
    REQUIRE(SUCCEEDED(hr));
    const auto* bytes = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    return { bytes, bytes + code->GetBufferSize() };
}

// SDK e19035b packs averaged UNORM samples with float reciprocal and +0.5
// truncation.
std::uint32_t packWords(const std::array<std::uint32_t, 4>& a, const std::array<std::uint32_t, 4>& b, bool wide)
{
    const float              k8 = std::bit_cast<float>(0x3B808081U), k10 = std::bit_cast<float>(0x3A802008U), k2 = std::bit_cast<float>(0x3EAAAAABU);
    std::uint32_t            word   = 0;
    const std::array<int, 4> shifts = wide ? std::array<int, 4>{ 0, 10, 20, 30 } : std::array<int, 4>{ 0, 8, 16, 24 };
    for (int channel = 0; channel < 4; ++channel)
    {
        const float    unpack  = !wide ? k8 : channel == 3 ? k2
                                                           : k10;
        const float    maximum = !wide ? 255.0F : channel == 3 ? 3.0F
                                                               : 1023.0F;
        volatile float va      = float(a[channel]) * unpack;
        volatile float vb      = float(b[channel]) * unpack;
        volatile float sum     = va + vb;
        float          v       = a == b ? va : sum * 0.5F;
        v                      = std::fmin(std::fmax(v, 0.0F), 1.0F);
        volatile float scaled  = v * maximum;
        volatile float biased  = scaled + 0.5F;
        word |= std::uint32_t(biased) << shifts[channel];
    }
    return word;
}

// IEEE float to half, rounding to nearest even or toward zero. The
// validation GPU's f32tof16 truncated (16.6026 stored as 0x4C26), and the
// SDK resolve shader stores through the same instruction.
std::uint32_t toHalf(float value, bool nearest)
{
    const auto bits     = std::bit_cast<std::uint32_t>(value);
    const auto sign     = (bits >> 16) & 0x8000;
    const auto exponent = int((bits >> 23) & 0xFF) - 127 + 15;
    auto       mantissa = bits & 0x7FFFFF;
    UNSCOPED_INFO("the model packs only normal halves");
    REQUIRE((exponent > 0 && exponent < 31));
    // Drop 13 bits; rounding up may carry into the exponent.
    const auto dropped = mantissa & 0x1FFF;
    auto       half    = (std::uint32_t(exponent) << 10) | (mantissa >> 13);
    if (nearest && (dropped > 0x1000 || (dropped == 0x1000 && (half & 1))))
        ++half;
    return sign | half;
}

// SDK e19035b averages format-4 samples with 0x3A800100 and stores f32tof16
// halves.
float halfInput(const std::array<std::int32_t, 2>& a, const std::array<std::int32_t, 2>& b, int channel)
{
    const float    k   = std::bit_cast<float>(0x3A800100U);
    volatile float va  = float(a[channel]) * k;
    volatile float vb  = float(b[channel]) * k;
    float          ca  = std::fmax(va, -1.0F);
    float          cb  = std::fmax(vb, -1.0F);
    volatile float sum = ca + cb;
    return a == b ? ca : sum * 0.5F;
}

std::uint32_t packHalves(const std::array<std::int32_t, 2>& a, const std::array<std::int32_t, 2>& b, bool nearest)
{
    std::uint32_t word = 0;
    for (int channel = 0; channel < 2; ++channel)
        word |= toHalf(halfInput(a, b, channel), nearest) << (16 * channel);
    return word;
}

struct Rect
{
    std::uint32_t left, top, right, bottom;

    bool contains(std::uint32_t x, std::uint32_t y) const
    {
        return x >= left && x < right && y >= top && y < bottom;
    }
};

constexpr std::uint32_t kW = 64, kH = 16;
constexpr Rect          kRectA{ 4, 2, 40, 12 }, kRectB{ 20, 0, 64, 9 }, kRectC{ 0, 6, 30, 16 };
// Clear values as UNORM codes. RT1's 0.75 replaces the design's 0.5, which is
// a half-unit tie at 10 bits.
constexpr std::array<std::uint32_t, 4> kRt0Clear{ 51, 102, 153, 204 }, kRt1Clear{ 1023, 767, 256, 1 };
constexpr std::uint32_t                kDepthClear = 12582911, kStencilClear = 0x5A;
// Draw outputs as codes: RT0 SV_Target0 and RT1 SV_Target1, sample 0 only.
constexpr std::array<std::uint32_t, 4> kRt0Draw{ 26, 200, 7, 255 }, kRt1Draw{ 100, 513, 1000, 2 };

struct Renderer
{
    NativeRendererD3D12 renderer;
    std::uint64_t       key = 1000;

    Renderer()
    {
        std::string error;
        UNSCOPED_INFO("native stream: " + error);
        REQUIRE(renderer.StartDrawStream(error));
    }

    ~Renderer()
    {
        renderer.Shutdown();
    }

    NativeRenderFrame rawFrame(NativeRenderFramePass pass, const NativeRenderFrameResolve& resolve)
    {
        NativeRenderFrame frame;
        frame.textures.push_back({ resolve.textureId, kW, kH, NativeDrawReplayTextureFormat::R32Uint, ++key });
        pass.resolves = { resolve };
        frame.passes.push_back(std::move(pass));
        frame.outputTextureId = resolve.textureId;
        frame.retainOnly = frame.returnRawOutput = true;
        std::string error;
        const bool  valid = ValidateNativeRenderFrame(frame, error);
        UNSCOPED_INFO("frame validation: " + error);
        REQUIRE(valid);
        return frame;
    }

    std::vector<std::uint32_t> words(NativeRenderFramePass pass, const NativeRenderFrameResolve& resolve)
    {
        const auto result = renderer.SubmitRenderFrame(rawFrame(std::move(pass), resolve));
        UNSCOPED_INFO("guest-word frame: " + result.error);
        REQUIRE((result.success && result.rawRowPitch == kW * 4 && result.rawOutput.size() == kW * kH * 4));
        std::vector<std::uint32_t> values(kW * kH);
        std::memcpy(values.data(), result.rawOutput.data(), result.rawOutput.size());
        return values;
    }

    // As above for several passes, the last of which ends with `resolve`;
    // `textures` are the frame's other resolve destinations.
    std::vector<std::uint32_t> words(std::vector<NativeRenderFramePass> passes, std::vector<NativeRenderFrameTexture> textures, const NativeRenderFrameResolve& resolve, bool typed = false)
    {
        NativeRenderFrame frame;
        frame.typedGuestWords = typed;
        frame.textures        = std::move(textures);
        frame.textures.push_back({ resolve.textureId, kW, kH, NativeDrawReplayTextureFormat::R32Uint, ++key });
        passes.back().resolves.push_back(resolve);
        frame.passes          = std::move(passes);
        frame.outputTextureId = resolve.textureId;
        frame.retainOnly = frame.returnRawOutput = true;
        std::string error;
        UNSCOPED_INFO("frame validation: " + error);
        REQUIRE(ValidateNativeRenderFrame(frame, error));
        const auto result = renderer.SubmitRenderFrame(std::move(frame));
        UNSCOPED_INFO("guest-word frame: " + result.error);
        REQUIRE((result.success && result.rawRowPitch == kW * 4 && result.rawOutput.size() == kW * kH * 4));
        std::vector<std::uint32_t> values(kW * kH);
        std::memcpy(values.data(), result.rawOutput.data(), result.rawOutput.size());
        return values;
    }
};

NativeRenderFramePass clearedPass()
{
    NativeRenderFramePass pass;
    pass.target     = mrtTarget(kW, kH);
    const auto rect = [](Rect value)
    {
        return NativeDrawReplayScissor{ std::int32_t(value.left), std::int32_t(value.top), std::int32_t(value.right), std::int32_t(value.bottom) };
    };
    pass.clears.push_back(colorClear(rect(kRectA), 1, { 0.2F, 0.4F, 0.6F, 0.8F }));
    pass.clears.push_back(colorClear(rect(kRectB), 2, { 1.0F, 0.75F, 0.25F, 0.333F }));
    NativeRenderFramePass::Clear depth;
    depth.rectangle = rect(kRectC);
    depth.depth = depth.stencil = true;
    depth.depthValue            = 0.75F;
    depth.stencilValue          = kStencilClear;
    pass.clears.push_back(depth);
    return pass;
}

void compare(const char* name, const std::vector<std::uint32_t>& actual, const std::function<std::uint32_t(std::uint32_t, std::uint32_t)>& expected)
{
    std::size_t mismatches = 0;
    std::string first;
    for (std::uint32_t y = 0; y < kH; ++y)
        for (std::uint32_t x = 0; x < kW; ++x)
            if (actual[y * kW + x] != expected(x, y) && !mismatches++)
            {
                char text[96];
                std::snprintf(text, sizeof(text), " first at (%u,%u): %08X, expected %08X", x, y, actual[y * kW + x], expected(x, y));
                first = text;
            }
    UNSCOPED_INFO(std::string(name) + ": " + std::to_string(mismatches) + " words differ" + first);
    REQUIRE(mismatches == 0);
}

void runD3D12()
{
    char enabled[8]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) != 1 || enabled[0] != '1')
    {
        SKIP("Set REREVVED_TEST_NATIVE_D3D12=1 to run the D3D12 MRT checks");
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        SKIP("No D3D12 device; the MRT frames were not rendered");
    }
    Renderer                                          renderer;
    const std::array<std::uint32_t, 4>                zero{};
    const std::array<std::array<std::uint32_t, 2>, 3> sampleSets{ { { 0, 0 }, { 3, 3 }, { 0, 3 } } };

    // The MRT draw writes sample 0 only, so {0,3} averages draw and clear.
    auto draw             = mrtDraw(kW, kH);
    draw.vertexShaderDxil = compile("float4 main(float4 p:TEXCOORD0):SV_Position { return p; }", "vs_5_1");
    std::string pixel     = "void main(float4 p:SV_Position, out float4 o0:SV_Target0, out float4 o1:SV_Target1, out uint mask:SV_Coverage) {"
                            " o0 = float4(" +
                            std::to_string(kRt0Draw[0]) + ", " + std::to_string(kRt0Draw[1]) + ", " + std::to_string(kRt0Draw[2]) + ", " + std::to_string(kRt0Draw[3]) +
                            ") / 255.0; o1 = float4(" + std::to_string(kRt1Draw[0]) + " / 1023.0, " + std::to_string(kRt1Draw[1]) + " / 1023.0, " +
                            std::to_string(kRt1Draw[2]) + " / 1023.0, " + std::to_string(kRt1Draw[3]) + " / 3.0); mask = 1; }";
    draw.pixelShaderDxil  = compile(pixel.c_str(), "ps_5_1");

    // 1. Without pass clears the first draw creates the attachments and clears
    // them to its opaque black clear color. It runs first so no attachment
    // cached by a cleared pass stands in for the created slots.
    const std::array<std::uint32_t, 4> rt0Black{ 0, 0, 0, 255 }, rt1Black{ 0, 0, 0, 3 };
#if REREVVED_DEVELOPER
    const auto drawBefore       = renderer.renderer.ReplayCacheStats();
    const auto drawBeforeTiming = renderer.renderer.ReplayTimingStats();
#endif
    for (const auto& samples : sampleSets)
    {
        NativeRenderFramePass pass;
        pass.draws.push_back(draw);
        const auto code = [&](std::uint32_t sample, const std::array<std::uint32_t, 4>& drawCode, const std::array<std::uint32_t, 4>& black)
        {
            return sample == 0 ? drawCode : black;
        };
        const auto rt0 = renderer.words(pass, guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, samples, kW, kH));
        compare("draw-created RT0", rt0, [&](auto, auto)
                {
                    return packWords(code(samples[0], kRt0Draw, rt0Black), code(samples[1], kRt0Draw, rt0Black), false);
                });
        const auto rt1 = renderer.words(pass, guestResolve(2, false, 1, NativeGuestWordPacking::Color2101010, samples, kW, kH));
        compare("draw-created RT1", rt1, [&](auto, auto)
                {
                    return packWords(code(samples[0], kRt1Draw, rt1Black), code(samples[1], kRt1Draw, rt1Black), true);
                });
    }
#if REREVVED_DEVELOPER
    {
        // Those created slots missed the pool at the draw replay color site,
        // and every miss timed one creation call.
        const auto    after       = renderer.renderer.ReplayCacheStats();
        const auto    afterTiming = renderer.renderer.ReplayTimingStats();
        std::uint64_t misses      = 0;
        for (std::size_t site = 0; site < after.attachmentSiteMisses.size(); ++site)
            misses += after.attachmentSiteMisses[site] - drawBefore.attachmentSiteMisses[site];
        const auto colorMisses = after.attachmentSiteMisses[2] - drawBefore.attachmentSiteMisses[2];
        const auto depthMisses = after.attachmentSiteMisses[3] - drawBefore.attachmentSiteMisses[3];
        UNSCOPED_INFO(std::format("draw-created attachments miss at the replay sites and time each creation: {} color, {} depth of {}", colorMisses, depthMisses, misses));
        REQUIRE((colorMisses && afterTiming.attachmentCreations - drawBeforeTiming.attachmentCreations == misses &&
                 afterTiming.attachmentCreateNanoseconds > drawBeforeTiming.attachmentCreateNanoseconds));
    }
#endif
    std::cout << "A draw-initialized MRT pass creates the Rgb10a2 slot in its own format\n";
#if REREVVED_DEVELOPER
    {
        // A clear pass of a shape the renderer never held misses the pool at
        // the clear sites. Each of those misses creates its attachment in the
        // timed creation call, and the miss log counts the shape as new.
        const auto before       = renderer.renderer.ReplayCacheStats();
        const auto beforeTiming = renderer.renderer.ReplayTimingStats();
        auto       wide         = clearedPass();
        wide.target             = mrtTarget(2 * kW, kH);
        renderer.words(wide, guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH));
        const auto    after       = renderer.renderer.ReplayCacheStats();
        const auto    afterTiming = renderer.renderer.ReplayTimingStats();
        const auto    colorMisses = after.attachmentSiteMisses[0] - before.attachmentSiteMisses[0];
        const auto    depthMisses = after.attachmentSiteMisses[1] - before.attachmentSiteMisses[1];
        std::uint64_t sites       = 0;
        for (const auto misses : after.attachmentSiteMisses)
            sites += misses;
        const auto shape = std::find_if(after.attachmentMissShapes.begin(), after.attachmentMissShapes.end(), [](const NativeAttachmentMissShape& missed)
                                        {
                                            return missed.width == 2 * kW && missed.height == kH && missed.samples == 4 && missed.misses;
                                        });
        UNSCOPED_INFO(std::format("a new clear pass shape misses at both clear sites and times each creation: {} color, {} depth", colorMisses, depthMisses));
        REQUIRE((colorMisses && depthMisses && afterTiming.attachmentCreations - beforeTiming.attachmentCreations == colorMisses + depthMisses &&
                 afterTiming.attachmentCreateNanoseconds > beforeTiming.attachmentCreateNanoseconds));
        UNSCOPED_INFO("every attachment miss has one take site, and the new shape's misses count as new");
        REQUIRE((sites == after.attachmentPoolMisses && shape != after.attachmentMissShapes.end() && shape->gaps.back() == shape->misses &&
                 after.attachmentMissFrames > before.attachmentMissFrames));
    }
#endif

    // 2. Clear-only: every sample of a pixel holds its clear, so each sample
    // selection packs the same words.
    for (const auto& samples : sampleSets)
    {
        const auto rt0 = renderer.words(clearedPass(), guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, samples, kW, kH));
        compare("clear RT0", rt0, [&](auto x, auto y)
                {
                    const auto& code = kRectA.contains(x, y) ? kRt0Clear : zero;
                    return packWords(code, code, false);
                });
        const auto rt1 = renderer.words(clearedPass(), guestResolve(2, false, 1, NativeGuestWordPacking::Color2101010, samples, kW, kH));
        compare("clear RT1", rt1, [&](auto x, auto y)
                {
                    const auto& code = kRectB.contains(x, y) ? kRt1Clear : zero;
                    return packWords(code, code, true);
                });
        if (samples[0] != samples[1])
            continue;
        const auto depth = renderer.words(clearedPass(), guestResolve(3, true, 0, NativeGuestWordPacking::Depth24Stencil8, samples, kW, kH));
        compare("clear depth", depth, [&](auto x, auto y)
                {
                    // Outside rect C the pass initializes depth 1.0 and stencil 0.
                    return kRectC.contains(x, y) ? (kDepthClear << 8) | kStencilClear : 0xFFFFFF00U;
                });
    }
    std::cout << "Clear-only MRT words equal the CPU model for RT0, RT1 and depth at samples {0,0}, {3,3} and {0,3}\n";

    // Raw-readback terminals complete at their own fence: while the queue is
    // held, two record back to back and neither completes; released, both
    // complete in order with the bytes an unheld frame returns.
    {
        using Microsoft::WRL::ComPtr;
        const auto           color       = guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH);
        const auto           depth       = guestResolve(3, true, 0, NativeGuestWordPacking::Depth24Stencil8, { 0, 0 }, kW, kH);
        const auto           expectColor = renderer.words(clearedPass(), color);
        const auto           expectDepth = renderer.words(clearedPass(), depth);
        auto*                queue       = detail::NativeRenderFrameTestAccess::Queue(renderer.renderer);
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence>  gate;
        UNSCOPED_INFO("raw readback queue gate");
        REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))));

        struct ReleaseGate
        {
            ID3D12Fence* gate;

            ~ReleaseGate()
            {
                gate->Signal(1);
            }
        } release{ gate.Get() };

        UNSCOPED_INFO("hold the native queue");
        REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)));
        std::shared_ptr<NativeRenderFrameTicket> first, second;
        std::string                              error;
        UNSCOPED_INFO("first raw terminal: " + error);
        REQUIRE(renderer.renderer.QueueRenderFrame(renderer.rawFrame(clearedPass(), color), first, error));
        UNSCOPED_INFO("second raw terminal: " + error);
        REQUIRE(renderer.renderer.QueueRenderFrame(renderer.rawFrame(clearedPass(), depth), second, error));
        for (const auto* ticket : { &first, &second })
        {
            UNSCOPED_INFO("a held raw terminal records");
            REQUIRE((*ticket)->Recording().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
            const auto recorded = (*ticket)->Recording().get();
            UNSCOPED_INFO("recording carries no readback: " + recorded.error);
            REQUIRE((recorded.success && recorded.rawOutput.empty()));
            UNSCOPED_INFO("a held raw terminal does not complete");
            REQUIRE((*ticket)->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        }
        UNSCOPED_INFO("release the native queue");
        REQUIRE(SUCCEEDED(gate->Signal(1)));
        UNSCOPED_INFO("released raw terminals complete without another submission");
        REQUIRE((first->Completion().wait_for(std::chrono::seconds(2)) == std::future_status::ready &&
                 second->Completion().wait_for(std::chrono::seconds(2)) == std::future_status::ready));
        const auto words = [](const NativeDrawReplayResult& result)
        {
            UNSCOPED_INFO("deferred raw readback: " + result.error);
            REQUIRE((result.success && result.rawRowPitch == kW * 4 && result.rawOutput.size() == kW * kH * 4));
            std::vector<std::uint32_t> values(kW * kH);
            std::memcpy(values.data(), result.rawOutput.data(), result.rawOutput.size());
            return values;
        };
        const auto firstDone = first->Completion().get(), secondDone = second->Completion().get();
        UNSCOPED_INFO("raw terminals retire in order");
        REQUIRE(firstDone.completionFenceValue < secondDone.completionFenceValue);
        UNSCOPED_INFO("deferred raw readbacks equal the unheld bytes");
        REQUIRE((words(firstDone) == expectColor && words(secondDone) == expectDepth));
    }
    std::cout << "Raw-readback terminals record while the queue is held and complete at their fences\n";

    // A synchronous terminal (resolved output) after a deferred raw terminal
    // completes only after the raw one: completions never run backwards.
    {
        using Microsoft::WRL::ComPtr;
        NativeRenderFrame shown;
        shown.textures.push_back({ 900, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
        NativeRenderFramePass        pass;
        NativeRenderFramePass::Clear fill;
        pass.target     = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rgba8 };
        fill.rectangle  = { 0, 0, 1280, 720 };
        fill.color      = true;
        fill.colorValue = { 0.25F, 0.5F, 0.75F, 1.0F };
        pass.clears     = { fill };
        NativeRenderFrameResolve resolve;
        resolve.textureId = 900;
        resolve.width     = 1280;
        resolve.height    = 720;
        pass.resolves     = { resolve };
        shown.passes.push_back(std::move(pass));
        shown.outputTextureId      = 900;
        shown.returnResolvedOutput = true;
        for (std::uint32_t index = 0; index < shown.gammaTable.size(); ++index)
            shown.gammaTable[index] = index * 0x010101U;
        std::string error;
        UNSCOPED_INFO("resolved-output frame: " + error);
        REQUIRE(ValidateNativeRenderFrame(shown, error));
        const auto           color = guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH);
        auto*                queue = detail::NativeRenderFrameTestAccess::Queue(renderer.renderer);
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence>  gate;
        UNSCOPED_INFO("mixed terminal queue gate");
        REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))));

        struct ReleaseGate
        {
            ID3D12Fence* gate;

            ~ReleaseGate()
            {
                gate->Signal(1);
            }
        } release{ gate.Get() };

        UNSCOPED_INFO("hold the native queue");
        REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)));
        std::shared_ptr<NativeRenderFrameTicket> raw, synchronous;
        UNSCOPED_INFO("raw terminal: " + error);
        REQUIRE(renderer.renderer.QueueRenderFrame(renderer.rawFrame(clearedPass(), color), raw, error));
        UNSCOPED_INFO("the held raw terminal records");
        REQUIRE(raw->Recording().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        UNSCOPED_INFO("synchronous terminal: " + error);
        REQUIRE(renderer.renderer.QueueRenderFrame(std::move(shown), synchronous, error));
        UNSCOPED_INFO("release the native queue");
        REQUIRE(SUCCEEDED(gate->Signal(1)));
        UNSCOPED_INFO("the synchronous terminal completes");
        REQUIRE(synchronous->Completion().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        UNSCOPED_INFO("the earlier raw terminal completed before the synchronous one");
        REQUIRE(raw->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::ready);
        const auto rawDone = raw->Completion().get(), shownDone = synchronous->Completion().get();
        UNSCOPED_INFO("mixed terminals complete in submission order: " + rawDone.error + shownDone.error);
        REQUIRE((rawDone.success && shownDone.success && rawDone.completionFenceValue < shownDone.completionFenceValue));
    }
    std::cout << "A synchronous terminal after a deferred raw terminal completes after it\n";

    // Presented, off-screen and resumed passes preserve standalone prefix
    // readbacks.
    {
        const auto color = guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH);
        const auto depth = guestResolve(3, true, 0, NativeGuestWordPacking::Depth24Stencil8, { 0, 0 }, kW, kH);
        // Presented passes are single-target; the halves and the depth
        // rectangle make channel and placement errors visible.
        const auto single = [](std::uint32_t base, std::uint32_t depthBase, std::array<float, 4> left, std::array<float, 4> right)
        {
            NativeRenderFramePass pass;
            pass.target = { kW, kH, 4, NativeDrawReplayTargetFormat::Rgba8, base, depthBase };
            pass.clears.push_back(colorClear({ 0, 0, std::int32_t(kW / 2), std::int32_t(kH) }, 1, left));
            pass.clears.push_back(colorClear({ std::int32_t(kW / 2), 0, std::int32_t(kW), std::int32_t(kH) }, 1, right));
            NativeRenderFramePass::Clear depthClear;
            depthClear.rectangle = { 0, 0, std::int32_t(kW), std::int32_t(kH / 2) };
            depthClear.depth = depthClear.stencil = true;
            depthClear.depthValue                 = base ? 0.25F : 0.75F;
            depthClear.stencilValue               = base ? 3 : kStencilClear;
            pass.clears.push_back(depthClear);
            return pass;
        };
        const auto presentedPass = single(0, 0x5E4, { 0.2F, 0.4F, 0.6F, 0.8F }, { 1.0F, 0.75F, 0.25F, 0.333F });
        auto       offscreen     = single(0x300, 0x8E4, { 0.6F, 0.2F, 1.0F, 0.4F }, { 0.0F, 1.0F, 0.0F, 1.0F });
        // After the resume, a depth-tested draw at z 0.5 passes the presented
        // depth (0.75, 1.0) but not the off-screen pass's 0.25 rectangle, and
        // a reinitialized depth would read back as 1.0.
        auto tested                                       = draw;
        tested.extraTargetCount                           = 0;
        tested.extraTargets[0]                            = {};
        tested.sampleMask                                 = 0xF;
        tested.pixelShaderHash                            = 3;
        tested.pixelShaderDxil                            = compile("float4 main(float4 p:SV_Position):SV_Target0 { return float4(0.0, 0.0, 1.0, 1.0); }", "ps_5_1");
        const std::array<std::array<float, 4>, 3> halfway = { { { -1, -1, 0.5F, 1 }, { -1, 3, 0.5F, 1 }, { 3, -1, 0.5F, 1 } } };
        std::memcpy(tested.vertexData.data(), halfway.data(), sizeof(halfway));
        tested.depth.enabled = true;
        tested.depth.compare = 2; // less
        auto drawnPass       = presentedPass;
        drawnPass.draws.push_back(tested);
        const auto expectShown  = renderer.words(drawnPass, color);
        const auto expectDepth  = renderer.words(presentedPass, depth);
        const auto expectSource = renderer.words(offscreen, color);
        UNSCOPED_INFO("the off-screen pass writes other words; the tested draw changes only color");
        REQUIRE((expectShown != expectSource && expectShown != renderer.words(presentedPass, color) && expectDepth == renderer.words(drawnPass, depth)));
        const auto readback = [](NativeRenderFrameResolve resolve, std::uint64_t id)
        {
            resolve.textureId = id;
            resolve.readback  = true;
            return resolve;
        };
        const auto texture = [](std::uint64_t id)
        {
            return NativeRenderFrameTexture{ id, kW, kH, NativeDrawReplayTextureFormat::R32Uint };
        };
        auto shown      = presentedPass;
        shown.presented = true;
        NativeRenderFramePass resumed;
        resumed.target            = shown.target;
        resumed.presented         = true;
        resumed.resumeAttachments = true;
        resumed.draws             = { tested };
        resumed.resolves          = { readback(color, 42), readback(depth, 44) };
        offscreen.resolves        = { readback(color, 41) };
        std::vector<NativeRenderFrame> packets(3);
        packets[0].passes   = { shown };
        packets[1].passes   = { offscreen };
        packets[1].textures = { texture(41) };
        packets[2].passes   = { resumed };
        packets[2].textures = { texture(42), texture(44) };
        std::string                                             error;
        std::vector<std::shared_future<NativeDrawReplayResult>> recordings;
        for (auto& packet : packets)
        {
            packet.identityAttachments = true;
            recordings.emplace_back();
            const bool queued = renderer.renderer.QueueRenderFramePasses(packet, recordings.back(), error);
            UNSCOPED_INFO("identity packet: " + error);
            REQUIRE(queued);
            UNSCOPED_INFO("identity packet records: " + recordings.back().get().error);
            REQUIRE(recordings.back().get().success);
        }
        std::map<std::uint64_t, std::shared_ptr<const NativeRawReadback>> handles;
        std::uint64_t                                                     covered = 0;
        for (auto& recording : recordings)
        {
            const auto& result = recording.get();
            UNSCOPED_INFO("identity packet records: " + result.error);
            REQUIRE((result.success && result.completionFenceValue));
            covered = std::max(covered, result.completionFenceValue);
            for (const auto& [id, handle] : result.rawReadbacks)
                handles[id] = handle;
        }
        UNSCOPED_INFO("each readback resolve returns a handle");
        REQUIRE((handles.size() == 3 && handles.contains(41) && handles.contains(42) && handles.contains(44)));
        auto* fence = renderer.renderer.AcquireStreamFence();
        UNSCOPED_INFO("the stream fence");
        REQUIRE(fence != nullptr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (fence->GetCompletedValue() < covered && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool reached = fence->GetCompletedValue() >= covered;
        fence->Release();
        UNSCOPED_INFO("the identity packets complete");
        REQUIRE(reached);
        const auto read = [&](std::uint64_t id)
        {
            std::vector<std::uint8_t> bytes;
            std::uint32_t             pitch = 0;
            std::string               failure;
            UNSCOPED_INFO("readback maps: " + failure);
            REQUIRE((handles.at(id)->Read(bytes, pitch, failure) && pitch == kW * 4 && bytes.size() == kW * kH * 4 && handles.at(id)->Bytes() >= bytes.size()));
            std::vector<std::uint32_t> values(kW * kH);
            std::memcpy(values.data(), bytes.data(), bytes.size());
            return values;
        };
        UNSCOPED_INFO("the off-screen readback equals its standalone frame");
        REQUIRE(read(41) == expectSource);
        UNSCOPED_INFO("the resumed color and depth equal the presented pass's own");
        REQUIRE((read(42) == expectShown && read(44) == expectDepth));

        // The tail terminal continues the packets' three passes and ends the chain.
        NativeRenderFrame graph;
        graph.identityAttachments   = true;
        graph.continuedPrefixPasses = packets.size();
        NativeRenderFramePass ending;
        ending.target                      = shown.target;
        ending.presented                   = true;
        ending.preservePreviousAttachments = true;
        ending.resolves                    = { guestResolve(43, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH) };
        graph.passes.push_back(ending);
        graph.textures.push_back({ 43, kW, kH, NativeDrawReplayTextureFormat::R32Uint, ++renderer.key });
        graph.outputTextureId = 43;
        graph.retainOnly = graph.returnRawOutput = true;
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        const bool                               queued = renderer.renderer.QueueRenderFrame(graph, ticket, error);
        UNSCOPED_INFO("identity terminal: " + error);
        REQUIRE(queued);
        const auto done = ticket->Completion().get();
        UNSCOPED_INFO("identity terminal completes: " + done.error);
        REQUIRE((done.success && done.rawOutput.size() == kW * kH * 4));
        std::vector<std::uint32_t> ended(kW * kH);
        std::memcpy(ended.data(), done.rawOutput.data(), done.rawOutput.size());
        UNSCOPED_INFO("the terminal continues the resumed attachments");
        REQUIRE(ended == expectShown);
    }
    std::cout << "Identity attachments resume a presented pass after an off-screen pass of its layout, with prefix readbacks\n";

    // 3. The MRT draw over cleared attachments.
    const auto drawn = [&](const NativeDrawReplayRecipe& recipe)
    {
        auto pass = clearedPass();
        pass.draws.push_back(recipe);
        return pass;
    };
    const auto check = [&](const char* name, const NativeDrawReplayRecipe& recipe, bool rt0Written, bool rt1Written)
    {
        for (const auto& samples : sampleSets)
        {
            const auto code = [&](std::uint32_t sample, bool written, const std::array<std::uint32_t, 4>& drawCode, const std::array<std::uint32_t, 4>& clearCode, bool inside)
            {
                return written && sample == 0 ? drawCode : inside ? clearCode
                                                                  : zero;
            };
            const auto rt0 = renderer.words(drawn(recipe), guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, samples, kW, kH));
            compare((std::string(name) + " RT0").c_str(), rt0, [&](auto x, auto y)
                    {
                        const bool inside = kRectA.contains(x, y);
                        return packWords(code(samples[0], rt0Written, kRt0Draw, kRt0Clear, inside), code(samples[1], rt0Written, kRt0Draw, kRt0Clear, inside), false);
                    });
            const auto rt1 = renderer.words(drawn(recipe), guestResolve(2, false, 1, NativeGuestWordPacking::Color2101010, samples, kW, kH));
            compare((std::string(name) + " RT1").c_str(), rt1, [&](auto x, auto y)
                    {
                        const bool inside = kRectB.contains(x, y);
                        return packWords(code(samples[0], rt1Written, kRt1Draw, kRt1Clear, inside), code(samples[1], rt1Written, kRt1Draw, kRt1Clear, inside), true);
                    });
        }
    };
    check("MRT draw", draw, true, true);
    auto rt0Masked            = draw;
    rt0Masked.blend.writeMask = 0;
    check("RT0 write mask 0", rt0Masked, false, true);
    auto rt1Masked                            = draw;
    rt1Masked.extraTargets[0].blend.writeMask = 0;
    check("RT1 write mask 0", rt1Masked, true, false);
    std::cout << "An MRT draw writes SV_Target1 to the Rgb10a2 slot under its own write mask; {0,3} averages per the SDK model\n";

    // 4. A draw that writes depth 0 and stencil 0x33 in sample 0 only: the
    // depth resolve of sample 0 sees the draw, sample 3 still the clears.
    auto depthDraw               = draw;
    depthDraw.depth.enabled      = true;
    depthDraw.depth.writeEnabled = true;
    depthDraw.depth.compare      = 8;
    depthDraw.stencil.enabled    = true;
    depthDraw.stencil.reference  = 0x33;
    depthDraw.stencil.front.pass = 3;
    depthDraw.stencil.back.pass  = 3;
    const auto sample0           = renderer.words(drawn(depthDraw), guestResolve(3, true, 0, NativeGuestWordPacking::Depth24Stencil8, { 0, 0 }, kW, kH));
    compare("depth draw sample 0", sample0, [](auto, auto)
            {
                return 0x33U;
            });
    const auto sample3 = renderer.words(drawn(depthDraw), guestResolve(3, true, 0, NativeGuestWordPacking::Depth24Stencil8, { 3, 3 }, kW, kH));
    compare("depth draw sample 3", sample3, [](auto x, auto y)
            {
                return kRectC.contains(x, y) ? (kDepthClear << 8) | kStencilClear : 0xFFFFFF00U;
            });
    std::cout << "A depth resolve reads the selected sample: {0,0} sees a sample-0 depth and stencil write, {3,3} the clears\n";

    // 5. Distinct enabled blends per slot, RT0 additive and RT1 minimum,
    // alternated with RT1 unblended and with the two swapped, so a pipeline
    // keyed on RT0's blend alone would reuse a wrong state.
    const std::array<std::uint32_t, 4> rt0Max{ 255, 255, 255, 255 }, rt1Max{ 1023, 1023, 1023, 3 };
    const auto                         state = [](std::optional<NativeDrawReplayBlendOp> op)
    {
        NativeDrawReplayBlendState blend;
        if (!op)
            return blend;
        blend.enabled     = true;
        blend.sourceColor = blend.destinationColor = blend.sourceAlpha = blend.destinationAlpha = NativeDrawReplayBlendFactor::One;
        blend.colorOp = blend.alphaOp = *op;
        return blend;
    };
    // The stored code after one sample-0 write of `source` over `destination`.
    const auto blended = [](std::optional<NativeDrawReplayBlendOp> op, const std::array<std::uint32_t, 4>& destination, const std::array<std::uint32_t, 4>& source, const std::array<std::uint32_t, 4>& maximum)
    {
        std::array<std::uint32_t, 4> code{};
        for (int c = 0; c < 4; ++c)
            code[c] = !op                                   ? source[c]
                      : *op == NativeDrawReplayBlendOp::Add ? std::min(destination[c] + source[c], maximum[c])
                                                            : std::min(destination[c], source[c]);
        return code;
    };
    const auto blendCheck = [&](const char* name, std::optional<NativeDrawReplayBlendOp> rt0Op, std::optional<NativeDrawReplayBlendOp> rt1Op)
    {
        auto recipe                  = draw;
        recipe.blend                 = state(rt0Op);
        recipe.extraTargets[0].blend = state(rt1Op);
        for (const auto& samples : sampleSets)
        {
            const auto code = [&](std::uint32_t sample, std::optional<NativeDrawReplayBlendOp> op, const std::array<std::uint32_t, 4>& clearCode, const std::array<std::uint32_t, 4>& drawCode, const std::array<std::uint32_t, 4>& maximum)
            {
                return sample == 0 ? blended(op, clearCode, drawCode, maximum) : clearCode;
            };
            const auto rt0 = renderer.words(drawn(recipe), guestResolve(1, false, 0, NativeGuestWordPacking::Color8888, samples, kW, kH));
            compare((std::string(name) + " RT0").c_str(), rt0, [&](auto x, auto y)
                    {
                        const auto& clearCode = kRectA.contains(x, y) ? kRt0Clear : zero;
                        return packWords(code(samples[0], rt0Op, clearCode, kRt0Draw, rt0Max), code(samples[1], rt0Op, clearCode, kRt0Draw, rt0Max), false);
                    });
            const auto rt1 = renderer.words(drawn(recipe), guestResolve(2, false, 1, NativeGuestWordPacking::Color2101010, samples, kW, kH));
            compare((std::string(name) + " RT1").c_str(), rt1, [&](auto x, auto y)
                    {
                        const auto& clearCode = kRectB.contains(x, y) ? kRt1Clear : zero;
                        return packWords(code(samples[0], rt1Op, clearCode, kRt1Draw, rt1Max), code(samples[1], rt1Op, clearCode, kRt1Draw, rt1Max), true);
                    });
        }
    };
    using Op = NativeDrawReplayBlendOp;
    blendCheck("RT0 add, RT1 minimum", Op::Add, Op::Minimum);
    blendCheck("RT0 add, RT1 unblended", Op::Add, std::nullopt);
    blendCheck("RT0 add, RT1 minimum again", Op::Add, Op::Minimum);
    blendCheck("RT0 minimum, RT1 add", Op::Minimum, Op::Add);
    std::cout << "Each MRT slot blends under its own enabled state, and alternating states never share a pipeline\n";

    // 6. k_16_16 fog words: an Rg16Snorm target cleared and drawn in sample 0
    // with codes that exercise the -1.0 clamp, the float32 rounding of 32767
    // (32 - 2^-25, stored as 32.0) and a positive and negative mantissa.
    constexpr std::array<std::int32_t, 2> kFogClear{ 32767, -20000 }, kFogDraw{ 1234, -517 }, kFogEdge{ -32767, 7 };
    const auto                            snorm = [](std::int32_t code)
    {
        return float(code) / 32767.0F;
    };
    // Device f32tof16 rounding is the shared SDK and host resolve oracle.
    bool deviceNearest = false;
    {
        auto probe        = mrtDraw(kW, kH);
        probe.sampleCount = probe.sampleMask = 1;
        probe.extraTargetCount               = 0;
        probe.extraTargets[0]                = {};
        probe.vertexShaderDxil               = draw.vertexShaderDxil;
        // Runtime-zero bits keep the compiler from folding the conversion.
        const auto source     = std::format("void main(float4 p:SV_Position, out float4 o0:SV_Target0) {{ uint z = uint(p.x) >> 31;"
                                            " uint2 h = f32tof16(float2(asfloat({:#010x} | z), asfloat({:#010x} | z)));"
                                            " o0 = float4(h.x & 255, h.x >> 8, h.y & 255, h.y >> 8) / 255.0; }}",
                                            std::bit_cast<std::uint32_t>(halfInput(kFogDraw, kFogClear, 0)),
                                            std::bit_cast<std::uint32_t>(halfInput(kFogDraw, kFogClear, 1)));
        probe.pixelShaderDxil = compile(source.c_str(), "ps_5_1");
        NativeRenderFramePass pass;
        pass.target = { kW, kH, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
        pass.draws.push_back(probe);
        const auto word      = renderer.words(pass, guestResolve(5, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH))[0];
        const bool truncates = word == packHalves(kFogDraw, kFogClear, false), rounds = word == packHalves(kFogDraw, kFogClear, true);
        UNSCOPED_INFO(std::format("the device f32tof16 probe {:08X} is truncation {:08X} or nearest {:08X}", word, packHalves(kFogDraw, kFogClear, false), packHalves(kFogDraw, kFogClear, true)));
        REQUIRE(truncates != rounds);
        deviceNearest = rounds;
    }
    for (const auto samples : { 1U, 4U })
    {
        auto fogDraw             = mrtDraw(kW, kH);
        fogDraw.sampleCount      = samples;
        fogDraw.sampleMask       = samples == 4 ? 9 : 1;
        fogDraw.targetFormat     = NativeDrawReplayTargetFormat::Rg16Snorm;
        fogDraw.extraTargetCount = 0;
        fogDraw.extraTargets[0]  = {};
        fogDraw.scissor          = { 0, 0, std::int32_t(kW / 2), std::int32_t(kH) };
        fogDraw.vertexShaderDxil = draw.vertexShaderDxil;
        const auto output        = std::format("void main(float4 p:SV_Position, out float4 o0:SV_Target0, out uint mask:SV_Coverage) {{ o0 = float4({} / 32767.0, {} / 32767.0, 0, 0); mask = 1; }}", kFogDraw[0], kFogDraw[1]);
        fogDraw.pixelShaderDxil  = compile(output.c_str(), "ps_5_1");
        NativeRenderFramePass pass;
        pass.target = { kW, kH, samples, NativeDrawReplayTargetFormat::Rg16Snorm, 0x2D0 };
        pass.clears.push_back(colorClear({ 0, 0, std::int32_t(kW), std::int32_t(kH) }, 1, { snorm(kFogClear[0]), snorm(kFogClear[1]), 0, 0 }));
        pass.clears.push_back(colorClear({ 0, 0, 8, std::int32_t(kH) }, 1, { snorm(kFogEdge[0]), snorm(kFogEdge[1]), 0, 0 }));
        pass.draws.push_back(fogDraw);
        const std::vector<std::array<std::uint32_t, 2>> selections = samples == 4 ? std::vector<std::array<std::uint32_t, 2>>{ { 0, 0 }, { 3, 3 }, { 0, 3 } }
                                                                                  : std::vector<std::array<std::uint32_t, 2>>{ { 0, 0 } };
        for (const auto& selected : selections)
        {
            const auto words = renderer.words(pass, guestResolve(4, false, 0, NativeGuestWordPacking::Float16x2, selected, kW, kH));
            const auto model = [&](bool nearest)
            {
                return [&, nearest](std::uint32_t x, std::uint32_t)
                {
                    const auto& cleared = x < 8 ? kFogEdge : kFogClear;
                    const auto  code    = [&](std::uint32_t sample)
                    {
                        return x < kW / 2 && sample == 0 ? kFogDraw : cleared;
                    };
                    return packHalves(code(selected[0]), code(selected[1]), nearest);
                };
            };
            compare(std::format("k_16_16 {}X samples {},{}", samples == 4 ? 2 : 1, selected[0], selected[1]).c_str(), words, model(deviceNearest));
        }
    }
    UNSCOPED_INFO("the model rounds 32767 up to 32.0 and clamps -20000 to -1.0");
    REQUIRE(packHalves(kFogClear, kFogClear, false) == (0x5000U | (0xBC00U << 16)));
    UNSCOPED_INFO("the 2X {0,3} words tell the rounding modes apart");
    REQUIRE(packHalves(kFogDraw, kFogClear, false) != packHalves(kFogDraw, kFogClear, true));

    // Prepared Rg16Float stores SDK halves, clamps -20000/-32767 to -1, then
    // returns guest bytes.
    auto readHalves        = mrtDraw(kW, kH);
    readHalves.sampleCount = readHalves.sampleMask = 1;
    readHalves.extraTargetCount                    = 0;
    readHalves.extraTargets[0]                     = {};
    readHalves.vertexShaderDxil                    = draw.vertexShaderDxil;
    readHalves.textureMask                         = 1;
    readHalves.textures[0]                         = { {}, kW, kH, NativeDrawReplayTextureFormat::Rg16Float };
    readHalves.textures[0].frameTextureId          = 10;
    readHalves.pixelShaderDxil                     = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 {"
                                                             " uint2 h = f32tof16(t.Load(int3(int2(p.xy), 0)).rg);"
                                                             " return float4(h.x & 255, h.x >> 8, h.y & 255, h.y >> 8) / 255.0; }",
                                                             "ps_5_1");
    for (const auto samples : { 1U, 4U })
    {
        auto fogDraw                                               = mrtDraw(kW, kH);
        fogDraw.sampleCount                                        = samples;
        fogDraw.sampleMask                                         = samples == 4 ? 9 : 1;
        fogDraw.targetFormat                                       = NativeDrawReplayTargetFormat::Rg16Snorm;
        fogDraw.extraTargetCount                                   = 0;
        fogDraw.extraTargets[0]                                    = {};
        fogDraw.scissor                                            = { 0, 0, std::int32_t(kW / 2), std::int32_t(kH) };
        fogDraw.vertexShaderDxil                                   = draw.vertexShaderDxil;
        const auto output                                          = std::format("void main(float4 p:SV_Position, out float4 o0:SV_Target0, out uint mask:SV_Coverage) {{ o0 = float4({} / 32767.0, {} / 32767.0, 0, 0); mask = 1; }}", kFogDraw[0], kFogDraw[1]);
        fogDraw.pixelShaderDxil                                    = compile(output.c_str(), "ps_5_1");
        const std::vector<std::array<std::uint32_t, 2>> selections = samples == 4 ? std::vector<std::array<std::uint32_t, 2>>{ { 0, 0 }, { 3, 3 }, { 0, 3 } }
                                                                                  : std::vector<std::array<std::uint32_t, 2>>{ { 0, 0 } };
        for (const auto& selected : selections)
        {
            NativeRenderFramePass fog;
            fog.target = { kW, kH, samples, NativeDrawReplayTargetFormat::Rg16Snorm, 0x2D0 };
            fog.clears.push_back(colorClear({ 0, 0, std::int32_t(kW), std::int32_t(kH) }, 1, { snorm(kFogClear[0]), snorm(kFogClear[1]), 0, 0 }));
            fog.clears.push_back(colorClear({ 0, 0, 8, std::int32_t(kH) }, 1, { snorm(kFogEdge[0]), snorm(kFogEdge[1]), 0, 0 }));
            fog.draws.push_back(fogDraw);
            NativeRenderFrameResolve prepared;
            prepared.textureId  = 10;
            prepared.width      = kW;
            prepared.height     = kH;
            prepared.samples    = selected;
            prepared.valueScale = 32.0F;
            fog.resolves.push_back(prepared);
            NativeRenderFramePass read;
            read.target = { kW, kH, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
            read.draws.push_back(readHalves);
            const auto words = renderer.words({ fog, read }, { { 10, kW, kH, NativeDrawReplayTextureFormat::Rg16Float } }, guestResolve(11, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH));
            compare(std::format("prepared k_16_16 {}X samples {},{}", samples == 4 ? 2 : 1, selected[0], selected[1]).c_str(), words, [&](std::uint32_t x, std::uint32_t)
                    {
                        const auto& cleared = x < 8 ? kFogEdge : kFogClear;
                        const auto  code    = [&](std::uint32_t sample)
                        {
                            return x < kW / 2 && sample == 0 ? kFogDraw : cleared;
                        };
                        return packHalves(code(selected[0]), code(selected[1]), deviceNearest);
                    });
        }
    }
    std::cout << "Prepared k_16_16 resolves store the SDK halves, clamped at -1.0\n";

    // C2 samples k_16_16_FLOAT and format 54 typed views from harvested fetch
    // words.
    auto readTexels            = readHalves;
    readTexels.textures[0]     = {};
    readTexels.pixelShaderDxil = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 {"
                                         " uint2 xy = uint2(p.xy); uint bits = asuint(t.Load(int3(xy, 0))[xy.x & 3]);"
                                         " return float4(bits & 255, (bits >> 8) & 255, (bits >> 16) & 255, bits >> 24) / 255.0; }",
                                         "ps_5_1");
    const auto sampled         = [&](const NativeTextureFetch& fetch, std::uint32_t element)
    {
        NativeTextureMemoryRanges ranges;
        std::string               error;
        UNSCOPED_INFO("fetch ranges: " + error);
        REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
        // Guest memory holds each element big-endian; endian 2 swaps 8 in 32
        // and endian 0 reads the bytes as stored, so both store host order
        // after their swap.
        std::vector<std::uint8_t> base(ranges.base.size), mips(ranges.mips.size);
        const bool                swapped = ((fetch[1] >> 6) & 3) == 2;
        for (auto* bytes : { &base, &mips })
            for (std::size_t i = 0; i + 4 <= bytes->size(); i += 4)
                for (unsigned b = 0; b < 4; ++b)
                    (*bytes)[i + b] = std::uint8_t(element >> (8 * (swapped ? 3 - b : b)));
        NativeDrawReplayTexture texture;
        UNSCOPED_INFO("decode: " + error);
        REQUIRE(DecodeNativeTextureVersioned(fetch, base, mips, texture, error));
        auto recipe        = readTexels;
        recipe.textures[0] = texture;
        NativeRenderFramePass pass;
        pass.target = { kW, kH, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
        pass.draws.push_back(recipe);
        return std::pair{ texture, renderer.words(pass, guestResolve(12, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH)) };
    };
    const auto halfFloat = [](std::uint32_t half)
    {
        const auto exponent = (half >> 10) & 31;
        UNSCOPED_INFO("the oracle converts normal halves");
        REQUIRE((exponent && exponent != 31));
        return std::bit_cast<std::uint32_t>(std::ldexp(float(1024 + (half & 1023)), int(exponent) - 25) * ((half & 0x8000) ? -1.0F : 1.0F));
    };
    const NativeTextureFetch kFog{ 0x8A024956, 0x1E7B009F, 0x0059E4FF, 0x00001691, 0, 0x218 };
    // 32.0 and -1.0 (the clamp), a positive and a negative mantissa.
    for (const auto [red, green] : { std::pair<std::uint32_t, std::uint32_t>{ 0x5000, 0xBC00 }, { 0x3555, 0xC4D3 } })
    {
        const auto [texture, words] = sampled(kFog, red | (green << 16));
        UNSCOPED_INFO("format 31 decodes to Rg16Float with swizzle XY11");
        REQUIRE((texture.format == NativeDrawReplayTextureFormat::Rg16Float && texture.width == 1280 && texture.height == 720 &&
                 texture.swizzle == std::array<std::uint8_t, 4>{ 0, 1, 5, 5 }));
        const std::array<std::uint32_t, 4> expected{ halfFloat(red), halfFloat(green), 0x3F800000U, 0x3F800000U };
        compare(std::format("format 31 {:04X} {:04X}", red, green).c_str(), words, [&](std::uint32_t x, std::uint32_t)
                {
                    return expected[x & 3];
                });
    }
    const NativeTextureFetch kWide{ 0x8A004802, 0x1C628036, 0x0059E4FF, 0x00A80D10, 3, 0x218 };
    // Each 2-bit alpha with 10-bit extremes and interior codes.
    const std::array<std::array<std::uint32_t, 4>, 4> wide{ { { 0, 1023, 512, 0 }, { 1, 511, 1022, 1 }, { 1023, 0, 3, 2 }, { 300, 700, 1000, 3 } } };
    for (const auto& codes : wide)
    {
        const auto [texture, words] = sampled(kWide, codes[0] | (codes[1] << 10) | (codes[2] << 20) | (codes[3] << 30));
        UNSCOPED_INFO("format 54 decodes to Rgb10a2 with the identity swizzle");
        REQUIRE((texture.format == NativeDrawReplayTextureFormat::Rgb10a2 && texture.swizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 }));
        compare(std::format("format 54 {} {} {} alpha {}", codes[0], codes[1], codes[2], codes[3]).c_str(), words, [&](std::uint32_t x, std::uint32_t)
                {
                    // UNORM to float is the correctly rounded quotient.
                    const auto channel = x & 3;
                    return std::bit_cast<std::uint32_t>(float(double(codes[channel]) / (channel == 3 ? 3.0 : 1023.0)));
                });
    }
    std::cout << "Harvested format 31 and 54 fetches sample their decoded values, including each 2-bit alpha\n";

    // Packed Color8888 and Color2101010 words round-trip through castable
    // R32Uint views.
    const auto typed = [&](std::uint32_t slot, NativeGuestWordPacking packing, NativeDrawReplayTextureFormat view)
    {
        auto source = clearedPass();
        source.resolves.push_back(guestResolve(20, false, slot, packing, { 0, 0 }, kW, kH));
        auto recipe                       = readTexels;
        recipe.textures[0]                = { {}, kW, kH, view };
        recipe.textures[0].frameTextureId = 20;
        NativeRenderFramePass read;
        read.target = { kW, kH, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
        read.draws.push_back(recipe);
        return renderer.words({ source, read }, { { 20, kW, kH, NativeDrawReplayTextureFormat::R32Uint } }, guestResolve(21, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, kW, kH), true);
    };
    // Depth resolve conversion matches convertDepthUnorm at LOD 0; higher mip
    // levels are ignored.
    {
        constexpr std::uint32_t      W = 1280, H = 720;
        constexpr NativeTextureFetch kDepthFetch{ 0x8A004802, 0x1D0F0096, 0x0059E4FF, 0x00A81690, 0x00000283, 0x1D488A18 };
        std::string                  failure;
        const auto                   submit = [&](NativeRenderFrame frame, const char* name)
        {
            UNSCOPED_INFO(std::string(name) + " validation: " + failure);
            REQUIRE(ValidateNativeRenderFrame(frame, failure));
            const auto result = renderer.renderer.SubmitRenderFrame(std::move(frame));
            UNSCOPED_INFO(std::string(name) + ": " + result.error);
            REQUIRE((result.success && result.rawRowPitch == W * 4 && result.rawOutput.size() == std::size_t(W) * H * 4));
            std::vector<std::uint32_t> values(std::size_t(W) * H);
            std::memcpy(values.data(), result.rawOutput.data(), result.rawOutput.size());
            return values;
        };
        auto depthWrite               = mrtDraw(W, H);
        depthWrite.vertexShaderDxil   = draw.vertexShaderDxil;
        depthWrite.pixelShaderHash    = 0x24A;
        depthWrite.pixelShaderDxil    = compile("void main(float4 p:SV_Position, out float4 o0:SV_Target0, out float4 o1:SV_Target1, out float d:SV_Depth) {"
                                                " uint2 q = uint2(p.xy); uint h = (q.x * 2654435761u) ^ (q.y * 2246822519u); h ^= h >> 15;"
                                                " o0 = 0; o1 = 0; d = float(h & 0xFFFFFF) / 16777215.0; }",
                                                "ps_5_1");
        depthWrite.depth.enabled      = true;
        depthWrite.depth.writeEnabled = true;
        depthWrite.depth.compare      = 8;
        const NativeDrawReplayScissor whole{ 0, 0, std::int32_t(W), std::int32_t(H) };
        NativeRenderFramePass         source;
        source.target = mrtTarget(W, H);
        source.clears.push_back(colorClear(whole, 1, { 0, 0, 0, 1 }));
        source.clears.push_back(colorClear(whole, 2, { 0, 0, 0, 1 }));
        NativeRenderFramePass::Clear depthClear;
        depthClear.rectangle = whole;
        depthClear.depth = depthClear.stencil = true;
        depthClear.depthValue                 = 1.0F;
        depthClear.stencilValue               = kStencilClear;
        source.clears.push_back(depthClear);
        source.draws.push_back(depthWrite);
        source.resolves.push_back(guestResolve(30, true, 0, NativeGuestWordPacking::Depth24Stencil8, { 0, 0 }, W, H));
        const auto        wordsKey = ++renderer.key, depthKey = ++renderer.key;
        NativeRenderFrame producer;
        producer.textures.push_back({ 30, W, H, NativeDrawReplayTextureFormat::R32Uint, wordsKey });
        producer.passes.push_back(std::move(source));
        producer.surfaces.push_back({ 30, wordsKey, depthKey });
        producer.outputTextureId = 30;
        producer.retainOnly = producer.returnRawOutput = true;
#if REREVVED_DEVELOPER
        const auto conversions = renderer.renderer.ReplayTimingStats().surfaceConversions;
#endif
        const auto words = submit(std::move(producer), "the depth producer");
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("the depth resolve records one conversion");
        REQUIRE(renderer.renderer.ReplayTimingStats().surfaceConversions == conversions + 1);
#endif
        std::vector<std::uint32_t> depths(words.size());
        std::ranges::transform(words, depths.begin(), [](std::uint32_t word)
                               {
                                   return word >> 8;
                               });
        std::ranges::sort(depths);
        const auto distinct = std::ranges::unique(depths).begin() - depths.begin();
        UNSCOPED_INFO("the draw writes varied depth in both halves of the 24-bit range");
        REQUIRE((distinct > 100000 && depths.front() < 0x800000 && depths[distinct - 1] >= 0x800000));

        // The guest bytes: words at their tiled offsets, big-endian as the
        // 8in32 copy stores them; the mip levels hold another pattern.
        NativeTextureMemoryRanges ranges;
        UNSCOPED_INFO("the world fetch's ranges: " + failure);
        REQUIRE((GetNativeTextureMemoryRanges(kDepthFetch, ranges, failure) && ranges.mips.size));
        std::vector<std::uint8_t> base(ranges.base.size), mips(ranges.mips.size);
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x)
            {
                const auto offset = std::size_t(rex::graphics::texture_util::GetTiledOffset2D(std::int32_t(x), std::int32_t(y), W, 2));
                UNSCOPED_INFO("a tiled word lies inside the base");
                REQUIRE(offset + 4 <= base.size());
                for (unsigned b = 0; b < 4; ++b)
                    base[offset + b] = std::uint8_t(words[std::size_t(y) * W + x] >> (8 * (3 - b)));
            }
        for (std::size_t i = 0; i < mips.size(); ++i)
            mips[i] = std::uint8_t(0x5A ^ (i * 37));
        NativeDrawReplayTexture chain;
        UNSCOPED_INFO("the bytes path decodes the depth chain: " + failure);
        REQUIRE(DecodeNativeTextureVersioned(kDepthFetch, base, mips, chain, failure));
        UNSCOPED_INFO("the world fetch decodes to an 11-level R32Float chain with swizzle XY11");
        REQUIRE((chain.format == NativeDrawReplayTextureFormat::R32Float && chain.width == W && chain.height == H && chain.mipLevels == 11 &&
                 chain.swizzle == std::array<std::uint8_t, 4>{ 0, 0, 5, 5 }));
        const auto model = [&](std::uint32_t x, std::uint32_t y)
        {
            return std::bit_cast<std::uint32_t>(rex::graphics::xenos::UNorm24To32(words[std::size_t(y) * W + x] >> 8));
        };
        const auto payload = chain.payloadBytes();
        for (std::uint32_t y = 0; y < H; ++y)
            for (std::uint32_t x = 0; x < W; ++x)
            {
                std::uint32_t decodedBits = 0;
                std::memcpy(&decodedBits, payload.data() + (std::size_t(y) * W + x) * 4, 4);
                UNSCOPED_INFO(std::format("the decoded base at ({}, {}) is convertDepthUnorm's value", x, y));
                REQUIRE(decodedBits == model(x, y));
            }

        // Each channel's float bits through RGBA8 bytes, sampled at texel centers.
        auto reader        = mrtDraw(W, H);
        reader.sampleCount = reader.sampleMask = 1;
        reader.extraTargetCount                = 0;
        reader.extraTargets[0]                 = {};
        reader.vertexShaderDxil                = draw.vertexShaderDxil;
        reader.pixelShaderHash                 = 0x24B;
        reader.pixelShaderDxil                 = compile("Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3); float4 main(float4 p:SV_Position):SV_Target0 {"
                                                         " uint2 xy = uint2(p.xy); uint bits = asuint(t.Sample(s, p.xy / float2(1280.0, 720.0))[xy.x & 3]);"
                                                         " return float4(bits & 255, (bits >> 8) & 255, (bits >> 16) & 255, bits >> 24) / 255.0; }",
                                                         "ps_5_1");
        reader.sampler.minLinear = reader.sampler.magLinear = reader.sampler.mipLinear = true;
        reader.sampler.maxLod                                                          = 10.0F;
        reader.textureMask                                                             = 1;
        const auto read                                                                = [&](const NativeDrawReplayRecipe& recipe, std::vector<NativeRenderFrameTexture> textures, const char* name)
        {
            NativeRenderFramePass pass;
            pass.target = { W, H, 1, NativeDrawReplayTargetFormat::Rgba8, 0 };
            pass.draws.push_back(recipe);
            pass.resolves.push_back(guestResolve(42, false, 0, NativeGuestWordPacking::Color8888, { 0, 0 }, W, H));
            NativeRenderFrame frame;
            frame.textures = std::move(textures);
            frame.textures.push_back({ 42, W, H, NativeDrawReplayTextureFormat::R32Uint, ++renderer.key });
            frame.passes.push_back(std::move(pass));
            frame.outputTextureId = 42;
            frame.retainOnly = frame.returnRawOutput = true;
            return submit(std::move(frame), name);
        };
        const auto compareWide = [&](const char* name, const std::vector<std::uint32_t>& actual, const std::function<std::uint32_t(std::uint32_t, std::uint32_t)>& expected)
        {
            std::size_t mismatches = 0;
            std::string first;
            for (std::uint32_t y = 0; y < H; ++y)
                for (std::uint32_t x = 0; x < W; ++x)
                    if (actual[std::size_t(y) * W + x] != expected(x, y) && !mismatches++)
                        first = std::format(" first at ({},{}): {:08X}, expected {:08X}", x, y, actual[std::size_t(y) * W + x], expected(x, y));
            UNSCOPED_INFO(std::string(name) + ": " + std::to_string(mismatches) + " words differ" + first);
            REQUIRE(mismatches == 0);
        };
        auto converted                       = reader;
        converted.textures[0]                = { {}, W, H, NativeDrawReplayTextureFormat::R32Float };
        converted.textures[0].swizzle        = { 0, 0, 5, 5 };
        converted.textures[0].frameTextureId = 41;
        NativeRenderFrameTexture import{ 41, W, H, NativeDrawReplayTextureFormat::R32Float, depthKey, true };
        const auto               bound = read(converted, { import }, "the converted texture's sample");
        compareWide("converted k_24_8 sample", bound, [&](std::uint32_t x, std::uint32_t y)
                    {
                        return (x & 3) < 2 ? model(x, y) : 0x3F800000U;
                    });
        auto decoded        = reader;
        decoded.textures[0] = chain;
        const auto bytes    = read(decoded, {}, "the decoded chain's sample");
        compareWide("decoded k_24_8 chain sample", bytes, [&](std::uint32_t x, std::uint32_t y)
                    {
                        return bound[std::size_t(y) * W + x];
                    });
        // The gate sees the chain: a sample biased past the base differs.
        decoded.sampler.mipBias = 1.0F;
        UNSCOPED_INFO("a sample of level 1 reads the chain's other values");
        REQUIRE(read(decoded, {}, "the biased chain's sample") != bytes);
        std::cout << "A k_24_8 sample of the converted depth texture equals convertDepthUnorm and the bytes path's 11-level chain at LOD 0\n";
    }

    if (!renderer.renderer.CastableGuestWordTextures())
    {
        std::cout << "The device lacks castable formats; typed views of packed words were not rendered\n";
        return;
    }
    const auto rgba = typed(0, NativeGuestWordPacking::Color8888, NativeDrawReplayTextureFormat::Rgba8);
    compare("Rgba8 view of Color8888 words", rgba, [&](std::uint32_t x, std::uint32_t y)
            {
                const auto& code = kRectA.contains(x, y) ? kRt0Clear : zero;
                return std::bit_cast<std::uint32_t>(float(double(code[x & 3]) / 255.0));
            });
    const auto packed = typed(1, NativeGuestWordPacking::Color2101010, NativeDrawReplayTextureFormat::Rgb10a2);
    compare("Rgb10a2 view of Color2101010 words", packed, [&](std::uint32_t x, std::uint32_t y)
            {
                const auto& code    = kRectB.contains(x, y) ? kRt1Clear : zero;
                const auto  channel = x & 3;
                return std::bit_cast<std::uint32_t>(float(double(code[channel]) / (channel == 3 ? 3.0 : 1023.0)));
            });
    std::cout << "Packed Color8888 and Color2101010 words sample through typed Rgba8 and Rgb10a2 views\n";
    std::cout << "k_16_16 words equal the SDK model at 1X and at 2X samples {0,0}, {3,3} and {0,3}\n";
}

} // namespace

TEST_CASE("native_render_frame_mrt", "[ctest][native-render-frame-mrt][timeout-120]")
{
    runValidation();
}

TEST_CASE("native_render_frame_mrt_d3d12", "[ctest][release-smoke][native-render-frame-mrt][native_gpu][timeout-120]")
{
    runD3D12();
}
