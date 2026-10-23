#include "gpu/d3d12/native_gameplay_draw.h"
#include "gpu/d3d12/native_gameplay_render_frame.h"
#include "gpu/guest_d3d/solid_draw_samples.h"
#if defined(REREVVED_PREPARED_FRAME_BRIDGE_TEST) || defined(REREVVED_PREPARED_MAIN_CLEAR_TEST) || defined(REREVVED_PREPARED_RG16_TEST)
#include "gpu/d3d12/native_gameplay_shaders.h"
#endif
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/d3d12/native_upload_pool.h"
#include "gpu/d3d12/native_vertex_residency.h"
#include "native_gameplay_fixture_inputs.h"
#ifdef REREVVED_PREPARED_FRAME_BRIDGE_TEST
#include "prepared_frame_input_test_support.h"
#endif
#ifdef REREVVED_PREPARED_MAIN_CLEAR_TEST
#include "main_clear_control.h"
#endif
#ifdef REREVVED_PREPARED_RG16_TEST
#include "rg16_control.h"
#endif
#include "callback_checks.hpp"
#include "developer_checks.hpp"
#include "logging_scope.hpp"
#include "manual_cases.hpp"
#include "native_render_frame_test_access.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <d3d12sdklayers.h>
#include <rex/crypto/sha256.h>
#include <rex/cvar.h>
#ifdef REREVVED_PREPARED_FRAME_BRIDGE_TEST
#include <rex/graphics/format/ucode.h>
#include <rex/hash.h>
#endif
#include <catch2/catch_test_macros.hpp>
#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>

using namespace rerevved::gpu;

REXCVAR_DECLARE(bool, native_vertex_residency);
REXCVAR_DECLARE(bool, native_vertex_residency_exact);

namespace
{

// The ceiling for a wait that must eventually succeed. It is minutes so that
// a loaded machine only slows these tests; a hang still fails them.
constexpr auto kReadyCeiling = std::chrono::minutes(2);

// Checks in helpers that async bodies and renderer callbacks call: Catch2
// assertions stay on the test thread, so these record and throw instead.
rerevved::tests::CallbackChecks callbackChecks;

// Each run writes about 0.5 GB of images. They are removed when the case
// passes and kept, with their path reported, when it fails.
class OutputCleanup
{
public:
    explicit OutputCleanup(const std::filesystem::path& path)
    : path_(path)
    {
    }

    ~OutputCleanup()
    {
        if (path_.empty())
        {
            return;
        }
        if (std::uncaught_exceptions() != 0)
        {
            std::cerr << "native_frame_stream_test: output kept in " << path_.string() << std::endl;
            return;
        }
        std::error_code removeError;
        std::filesystem::remove_all(path_, removeError);
        if (removeError)
        {
            std::cerr << "native_frame_stream_test: could not remove " << path_.string() << ": " << removeError.message() << std::endl;
        }
    }

    OutputCleanup(const OutputCleanup&)            = delete;
    OutputCleanup& operator=(const OutputCleanup&) = delete;

private:
    const std::filesystem::path& path_;
};

std::vector<std::uint8_t> readGpuMailbox(const rex::ui::d3d12::D3D12Provider& provider,
                                         ID3D12Resource*                      texture,
                                         D3D12_RESOURCE_STATES                state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

struct PresenterPackedPixelTestAccess : rex::ui::Presenter
{
    using Presenter::Packed10bpcRGBTo8bpcBytes;
};

std::vector<std::uint8_t> compile(const char* source, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const auto                       hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    UNSCOPED_INFO((errors ? static_cast<const char*>(errors->GetBufferPointer()) : "shader compilation failed"));
    REQUIRE(SUCCEEDED(hr));
    const auto* bytes = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    return { bytes, bytes + code->GetBufferSize() };
}

NativeDrawReplayRecipe makeDraw(const std::array<float, 4>& color, bool half)
{
    NativeDrawReplayRecipe draw;
    draw.schemaVersion    = 2;
    draw.width            = 1280;
    draw.height           = 720;
    draw.sampleCount      = 4;
    draw.sampleMask       = 9;
    draw.vertexShaderHash = 1;
    draw.pixelShaderHash  = 2;
    draw.vertexShaderDxil = compile(
        "struct V { float4 p:SV_Position; float4 c:COLOR0; };"
        "V main(float4 p:TEXCOORD0,float4 c:TEXCOORD1) { V v; v.p=p; v.c=c; return v; }",
        "vs_5_1");
    draw.pixelShaderDxil                                = compile("float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target0 { return c; }", "ps_5_1");
    draw.vertexAttributeCount                           = 2;
    draw.vertexStrideBytes                              = 32;
    const std::array<std::array<float, 4>, 3> positions = { { { -1, -1, 0, 1 }, { -1, 3, 0, 1 }, { 3, -1, 0, 1 } } };
    draw.vertexData.resize(3 * draw.vertexStrideBytes);
    for (std::size_t i = 0; i < positions.size(); ++i)
    {
        std::memcpy(draw.vertexData.data() + i * 32, positions[i].data(), 16);
        std::memcpy(draw.vertexData.data() + i * 32 + 16, color.data(), 16);
    }
    draw.indices    = { 0, 1, 2 };
    draw.indexCount = 3;
    draw.vertexConstants.resize(4096);
    draw.pixelConstants.resize(3584);
    draw.sharedConstants.resize(336);
    draw.viewport   = { 0, 0, 1280, 720, 0, 1 };
    draw.scissor    = { 0, 0, half ? 640 : 1280, 720 };
    draw.clearColor = {};
    return draw;
}

void checkCentroidEdgeRamp(const std::filesystem::path& directory)
{
    auto draw            = makeDraw({}, false);
    draw.pixelShaderDxil = compile(
        "void main(float4 p:SV_Position,centroid float4 c:COLOR0,out float4 o:SV_Target0,out uint mask:SV_Coverage) { o=c; mask=9; }",
        "ps_5_1");
    draw.indices    = { 0, 1, 2, 1, 3, 2 };
    draw.indexCount = 6;
    draw.vertexData.resize(4 * 32);
    for (std::size_t i = 0; i < 4; ++i)
    {
        const bool                 bottom = i >= 2;
        const std::array<float, 4> position{ i % 2 ? 1.0F : -1.0F, 1.0F - 2.0F * (bottom ? 9.5F : 8.5F) / 720.0F, 0, 1 };
        const std::array<float, 4> color{ 0, 0, 0, bottom ? 0.25F : 0.0F };
        std::memcpy(draw.vertexData.data() + i * 32, position.data(), 16);
        std::memcpy(draw.vertexData.data() + i * 32 + 16, color.data(), 16);
    }
    NativeRendererD3D12 renderer;
    const auto          read = [&](std::uint32_t mask, const char* name)
    {
        draw.sampleMask   = mask;
        draw.outputPath   = directory / name;
        const auto result = renderer.ReplayOffscreen(draw);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        std::ifstream file(draw.outputPath, std::ios::binary);
        return std::vector<std::uint8_t>{ std::istreambuf_iterator<char>(file), {} };
    };
    const auto guest = read(9, "centroid-guest.samples");
    const auto full  = read(15, "centroid-full.samples");
    UNSCOPED_INFO("guest mask must preserve full raster centroid selection");
    REQUIRE((guest.size() == 2 * 1280 * 720 * 4 && guest == full));
    const auto pixel = (8 * 1280 + 320) * 4 + 3;
    // Which of the equidistant covered samples becomes the centroid is
    // implementation-defined; guest == full is the portable regression check.
    UNSCOPED_INFO("top host sample 0 must remain uncovered");
    REQUIRE(guest[pixel] == 0);
    renderer.Shutdown();
}

void checkSolidDestinationSamples(const std::filesystem::path& directory)
{
    using namespace rerevved::gpu::guest_d3d;
    std::vector<std::uint8_t> before(2 * 640 * 720 * 4);
    for (std::size_t i = 0; i < before.size(); ++i)
        before[i] = static_cast<std::uint8_t>((i * 17 + i / (640 * 720 * 4) * 73 + i / (640 * 4) * 11) % 256);
    NativeRendererD3D12 renderer;
    const auto          replay = [&](NativeDrawReplayRecipe recipe, const std::string& name)
    {
        recipe.outputPath = directory / name;
        const auto result = renderer.ReplayOffscreen(recipe);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        std::ifstream             file(recipe.outputPath, std::ios::binary);
        std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file), {} };
        UNSCOPED_INFO("seeded GPU readback requires both full planes");
        REQUIRE(bytes.size() == 2 * 1280 * 720 * 4);
        return bytes;
    };
    for (const unsigned x : { 0U, 640U })
    {
        auto draw                   = makeDraw({ 1, 0, 0, 0.5F }, false);
        draw.blend.enabled          = true;
        draw.blend.sourceColor      = NativeDrawReplayBlendFactor::SourceAlpha;
        draw.blend.destinationColor = NativeDrawReplayBlendFactor::InverseSourceAlpha;
        draw.scissor                = { static_cast<std::int32_t>(x), 0, static_cast<std::int32_t>(x + 640), 720 };
        SeedSolidDrawReferenceDestination(draw, before, x);
        auto omitted         = draw;
        omitted.scissor      = {};
        const auto roundTrip = replay(omitted, "solid-seed-omission-" + std::to_string(x) + ".samples");
        auto       initial   = draw.initialSample0;
        initial.insert(initial.end(), draw.initialSample1.begin(), draw.initialSample1.end());
        UNSCOPED_INFO("host sample 0/3 initialization and readback must preserve guest sample 0/1 bytes");
        REQUIRE((roundTrip == initial && ExtractSolidDrawReferenceTile(roundTrip, x) == before));
        const auto after = replay(draw, "solid-seed-after-" + std::to_string(x) + ".samples");
        UNSCOPED_INFO("seeded blended draw must change destination bytes");
        REQUIRE(ExtractSolidDrawReferenceTile(after, x) != before);
        const auto other = ExtractSolidDrawReferenceTile(after, 640 - x);
        UNSCOPED_INFO("seeded draw must leave the other tile transparent black");
        REQUIRE(std::all_of(other.begin(), other.end(), [](auto byte)
                            {
                                return byte == 0;
                            }));
    }
    renderer.Shutdown();
}

// The tail terminal of a complete graph whose first passes a chain recorded.
NativeRenderFrame tailOf(NativeRenderFrame frame, std::size_t recorded)
{
    std::string error;
    const bool  split = SplitNativeRenderFrameTail(frame, recorded, error);
    callbackChecks.Require(split, "tail split: " + error);
    return frame;
}

void admitFrameDraws(NativeRenderFrame& frame)
{
    std::string error;
    for (auto& pass : frame.passes)
    {
        UNSCOPED_INFO("admission fixture requires raw draw storage");
        REQUIRE(pass.admittedDraws.empty());
        for (auto& draw : pass.draws)
        {
            std::shared_ptr<const NativeDrawReplayAdmission> admitted;
            UNSCOPED_INFO(error);
            REQUIRE(AdmitNativeDrawReplayRecipe(std::move(draw), admitted, error));
            pass.admittedDraws.push_back(std::move(admitted));
        }
        pass.draws.clear();
    }
}

NativeRenderFrame collectorTerminalFrame(bool prepared, const std::array<std::uint32_t, 256>& gammaTable, const std::array<std::uint8_t, 4>& texel = { 255, 0, 0, 255 })
{
    using namespace rex::graphics::diagnostic;
    const auto digest = [](const std::vector<std::uint8_t>& bytes)
    {
        return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    };
    NativeGameplayShaderInterface shader;
    shader.vertexShaderHash      = 1;
    shader.pixelShaderHash       = 2;
    shader.vertexShaderDxil      = compile("float4 main(float2 p:TEXCOORD0):SV_Position { return float4(p,0,1); }", "vs_5_1");
    shader.pixelShaderDxil       = compile("Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3); float4 main():SV_Target0 { return t.SampleLevel(s,float2(0.5,0.5),0); }", "ps_5_1");
    shader.vertexDxilDigest      = digest(shader.vertexShaderDxil);
    shader.pixelDxilDigest       = digest(shader.pixelShaderDxil);
    shader.vertexGuestWordDigest = shader.pixelGuestWordDigest = digest({ 0, 0, 0, 0 });
    shader.vertexAttributes                                    = { { 0, 12, 0, 37, false, false } };
    shader.textureFetchSlots                                   = { 1 };
    shader.textureDimensions                                   = { NativeDrawReplayTextureDimension::Texture2D };
    shader.rasterizedDepth = shader.independentSamplers = true;
    shader.sampleCount = shader.sampleMask = 1;
    const auto bigFloat                    = [](std::vector<std::uint8_t>& bytes, float value)
    {
        const auto word = std::bit_cast<std::uint32_t>(value);
        for (int shift = 24; shift >= 0; shift -= 8)
            bytes.push_back(static_cast<std::uint8_t>(word >> shift));
    };

    struct Memory
    {
        std::vector<std::uint8_t>   vertices, rectangle;
        std::array<std::uint8_t, 4> texel{ 255, 0, 0, 255 };

        static bool copy(void* context, std::uint32_t address, std::span<std::uint8_t> destination) noexcept
        {
            const auto& self   = *static_cast<Memory*>(context);
            const auto* source = address == 0x2000 ? &self.vertices : address == 0x300000 ? &self.rectangle
                                                                                          : nullptr;
            if (source)
            {
                if (source->size() != destination.size())
                    return false;
                std::copy(source->begin(), source->end(), destination.begin());
                return true;
            }
            if (address != 0x100000)
                return false;
            for (std::size_t index = 0; index < destination.size(); ++index)
                destination[index] = self.texel[index % 4];
            return true;
        }
    } memory;

    memory.texel = texel;

    for (const auto value : std::array<float, 9>{ -1, -1, 0, -1, 3, 0, 3, -1, 0 })
        bigFloat(memory.vertices, value);
    for (const auto value : std::array<float, 6>{ -0.5F, -0.5F, 1279.5F, -0.5F, 1279.5F, 719.5F })
        bigFloat(memory.rectangle, value);
    std::vector<std::uint32_t> registers(0x5003), program{ 0 }, gamma(gammaTable.begin(), gammaTable.end());
    registers[0x2000] = 0x14000500;
    registers[0x2002] = 0x530;
    registers[0x200F] = 0x20002000;
    registers[0x2082] = 1280U | (720U << 16);
    registers[0x2100] = 0xFFFFFF;
    registers[0x2104] = 0xF;
    registers[0x210F] = registers[0x2110] = std::bit_cast<std::uint32_t>(640.0F);
    registers[0x2111]                     = std::bit_cast<std::uint32_t>(-360.0F);
    registers[0x2112]                     = std::bit_cast<std::uint32_t>(360.0F);
    registers[0x2201]                     = 0x00010001;
    registers[0x2202]                     = 0x87000000;
    registers[0x2204]                     = 0x80000;
    registers[0x2205]                     = 0x18000;
    registers[0x2206]                     = 0x43F;
    registers[0x2208]                     = 4;
    registers[0x2302]                     = 4;
    registers[0x2307]                     = 0xFF000;
    registers[0x2308]                     = 0xFF100;
    registers[0x4800]                     = 0x2003;
    registers[0x4801]                     = 36U | 2U;
    NativeTextureFetch fetch{ 2U | (1U << 22), 0x00100006, 0, 0xD10, 0, 0x200 };
    std::copy(fetch.begin(), fetch.end(), registers.begin() + 0x4806);
    NativeTextureMemoryRanges ranges;
    std::string               error;
    UNSCOPED_INFO(error);
    REQUIRE(GetNativeTextureMemoryRanges(fetch, ranges, error));
    DrawCaptureFrameInputTextureView texture;
    texture.fetch_words  = fetch;
    texture.base_address = ranges.base.address;
    texture.base_size    = ranges.base.size;
    texture.width = texture.height = texture.depth = texture.pitch = 1;
    texture.dimension                                              = 1;
    texture.format                                                 = 6;
    texture.fetch_constant                                         = 1;
    texture.stage                                                  = 1;
    texture.component_mapping                                      = 0x1688;
    texture.memory                                                 = { &memory, &Memory::copy };
    DrawCaptureFrameInputVertexFetchView vertex{ 0, 0x2000, 36, { &memory, &Memory::copy } };
    DrawCaptureFrameInputDrawView        draw;
    draw.id    = 2;
    draw.frame = draw.submission = 1;
    draw.registers               = registers;
    draw.vertex_shader           = { true, 1, program };
    draw.pixel_shader            = { true, 2, program };
    draw.primitive_type          = 4;
    draw.requested_index_count   = 3;
    draw.vertex_fetches          = { &vertex, 1 };
    draw.textures                = { &texture, 1 };
    draw.original_host_issued    = !prepared;
    draw.execution_status        = prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued;
    DrawCaptureFrameInputSwapView boundary;
    boundary.id = boundary.frame = boundary.submission = 1;
    boundary.registers                                 = registers;
    boundary.gamma_256                                 = gamma;
    boundary.is_8bpc                                   = true;
    NativeGameplayCommandFeed             commands(std::span(&shader, 1));
    diagnostics::NativeGameplayLiveInputs inputs;
    UNSCOPED_INFO(error);
    REQUIRE(commands.Begin(1, 1, {}, error));
    inputs.SetEventConsumer([&](const auto& event, const auto& bank, const auto& textures, const auto& cpu, std::string& failure)
                            {
                                if (!prepared)
                                    return commands.Consume(event, bank, textures, cpu, failure);
                                if (event.kind == DrawCaptureFrameEvent::Kind::kSwap)
                                    return commands.ConsumePreparedBoundary(event, bank, failure);
                                return event.kind == DrawCaptureFrameEvent::Kind::kCopy ? commands.ConsumePreparedCopy(event, bank, textures, cpu, failure) : commands.ConsumePreparedDraw(event, bank, textures, cpu, failure);
                            });
    UNSCOPED_INFO(error);
    REQUIRE(((prepared ? inputs.BeginPrepared(boundary, error) : inputs.Begin(boundary, error)) && (prepared ? inputs.PreparedDraw(draw, error) : inputs.Draw(draw, error))));
    registers[0x2205] = 0x10000;
    registers[0x2208] = 6;
    registers[0x2318] = 0x100040;
    registers[0x2319] = 0x100000;
    registers[0x231A] = 0x02D00500;
    registers[0x231B] = 0x300;
    registers[0x231C] = 0xFFF;
    DrawCaptureFrameInputCopyView copy;
    copy.id    = 3;
    copy.frame = copy.submission = 1;
    copy.registers               = registers;
    copy.source_geometry         = { 0, 0x300000, 24, { &memory, &Memory::copy } };
    copy.cpu_data_complete = copy.source_rect_present = true;
    copy.source_width                                 = 1280;
    copy.source_height                                = 720;
    copy.planned_address                              = 0x100000;
    copy.planned_length                               = 1280U * ((720U + 31U) & ~31U) * 4U;
    copy.original_succeeded                           = !prepared;
    copy.written_address                              = prepared ? 0 : copy.planned_address;
    copy.written_length                               = prepared ? 0 : copy.planned_length;
    UNSCOPED_INFO(error);
    REQUIRE((prepared ? inputs.PreparedCopy(copy, error) : inputs.Copy(copy, true, error)));
    registers[0x4800]           = 2;
    registers[0x4801]           = 0x00100006;
    registers[0x4802]           = 1279U | (719U << 13);
    registers[0x4803]           = 0xD10;
    boundary.id                 = 4;
    boundary.frontbuffer_ptr    = 0x100000;
    boundary.frontbuffer_width  = 1280;
    boundary.frontbuffer_height = 720;
    UNSCOPED_INFO(error);
    REQUIRE((prepared ? inputs.PreparedSwap(boundary, error) : inputs.Finish(boundary, error)));
    NativeRenderFrame                          frame;
    std::vector<NativeGameplayRetainedTexture> next;
    const auto&                                snapshot = inputs.Snapshot();
    UNSCOPED_INFO(error);
    REQUIRE((prepared ? commands.FinishPrepared(snapshot.events.back(), snapshot.final_swap, snapshot.end_frame, snapshot.end_submission, frame, next, error)
                      : commands.Finish(snapshot.events.back(), snapshot.final_swap, snapshot.end_frame, snapshot.end_submission, frame, next, error)));
    std::fill(registers.begin(), registers.end(), 0);
    std::fill(program.begin(), program.end(), 99);
    std::fill(gamma.begin(), gamma.end(), 0);
    std::fill(memory.vertices.begin(), memory.vertices.end(), 0);
    std::fill(memory.rectangle.begin(), memory.rectangle.end(), 0);
    memory.texel = { 0, 0, 255, 255 };
    UNSCOPED_INFO("whole-frame terminal owns final fetch and gamma with truthful prospective provenance");
    REQUIRE((frame.gammaTable == gammaTable && snapshot.events.back().draw.draw.registers[0x4801] == 0x00100006 &&
             snapshot.events.back().success == !prepared && !snapshot.events.back().host_issued &&
             snapshot.events.back().execution_status == (prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued)));
    return frame;
}

void checkPreparedCollectorPixels()
{
    std::array<std::uint32_t, 256> gamma;
    for (std::size_t index = 0; index < gamma.size(); ++index)
        gamma[index] = (static_cast<std::uint32_t>(index) * 4U) * ((1U << 20) | (1U << 10) | 1U);
    std::array<std::vector<std::uint8_t>, 4> pixels;
    for (std::size_t mode = 0; mode < pixels.size(); ++mode)
    {
        auto table = gamma;
        if (mode == 2)
            table[255] = 0;
        auto frame                 = collectorTerminalFrame(mode != 0, table, mode == 3 ? std::array<std::uint8_t, 4>{ 0, 255, 0, 255 } : std::array<std::uint8_t, 4>{ 255, 0, 0, 255 });
        frame.returnResolvedOutput = true;
        NativeRendererD3D12 renderer;
        std::string         error;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.QueueRenderFrame(std::move(frame), ticket, error));
        const auto recorded  = ticket->Recording().get();
        const auto completed = ticket->Completion().get();
        UNSCOPED_INFO("collector-fed native output requires actual terminal fence completion: " + completed.error);
        REQUIRE((recorded.success && !recorded.completionFenceValue && !recorded.presenterToken && recorded.resolvedOutput.empty() &&
                 completed.success && completed.completionFenceValue && completed.resolvedOutput.size() == 1280U * 720U * 4U));
        pixels[mode] = completed.resolvedOutput;
        renderer.Shutdown();
    }
    UNSCOPED_INFO("prepared terminal matches all legacy whole-frame bytes and responds to owned gamma and texel controls");
    REQUIRE((pixels[0] == pixels[1] && pixels[2] != pixels[1] && pixels[3] != pixels[1]));
    for (std::size_t offset = 0; offset < pixels[1].size(); offset += 4)
    {
        std::uint32_t pixel;
        std::memcpy(&pixel, pixels[1].data() + offset, 4);
        UNSCOPED_INFO("owned prepared texture pixels survive source mutation and complete terminal execution");
        REQUIRE(((pixel & 1023U) >= 1000 && ((pixel >> 10) & 1023U) == 0 && ((pixel >> 20) & 1023U) == 0));
    }
}

#if defined(REREVVED_PREPARED_MAIN_CLEAR_TEST) || defined(REREVVED_PREPARED_RG16_TEST)
#ifdef REREVVED_PREPARED_RG16_TEST
void checkPreparedRg16();
#endif

class MainClearGraphObserver final : public rex::graphics::diagnostic::DrawCaptureFrameInputObserver
{
public:
    using CopyView = rex::graphics::diagnostic::DrawCaptureFrameInputCopyView;
    using DrawView = rex::graphics::diagnostic::DrawCaptureFrameInputDrawView;
    using SwapView = rex::graphics::diagnostic::DrawCaptureFrameInputSwapView;

    explicit MainClearGraphObserver(std::span<const NativeGameplayShaderInterface> shaders)
    : commands(shaders)
    {
        inputs.SetEventConsumer([this](const auto& event, const auto& bank, const auto& textures, const auto& cpu, std::string& failure)
                                {
                                    using Kind = rex::graphics::diagnostic::DrawCaptureFrameEvent::Kind;
                                    if (event.kind == Kind::kSwap)
                                        return commands.ConsumePreparedBoundary(event, bank, failure);
                                    if (event.kind == Kind::kCopy)
                                        return commands.ConsumePreparedCopy(event, bank, textures, cpu, failure);
                                    return commands.ConsumePreparedDraw(event, bank, textures, cpu, failure);
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
        journal << "begin id=" << boundary.id << " frame=" << boundary.frame << '\n';
        initialBoundary = boundary.id;
        return commands.Begin(boundary.frame, boundary.submission, {}, error) && inputs.BeginPrepared(boundary, error);
    }

    bool OnDrawPrepared(const DrawView& view) override
    {
        journal << "prepared_draw id=" << view.id << " complete=" << view.cpu_data_complete << " original=" << view.original_host_issued
                << " reason=" << static_cast<unsigned>(view.unavailable_reason) << '\n';
        for (const auto& texture : view.textures)
            journal << "texture fetch=" << texture.fetch_constant << " required=" << texture.required << " available=" << texture.available
                    << " reason=" << static_cast<unsigned>(texture.unavailable_reason) << " gpu_written=" << texture.gpu_written
                    << " base=" << texture.base_address << ':' << texture.base_size << " mips=" << texture.mip_address << ':' << texture.mip_size << '\n';
        if (!inputs.PreparedDraw(view, error))
        {
#ifdef REREVVED_PREPARED_RG16_TEST
            if (firstError.empty())
                firstError = error;
#endif
            return false;
        }
        preparedDrawIds.push_back(view.id);
        return true;
    }

    bool OnCopyPrepared(const CopyView& view) override
    {
        journal << "prepared_copy id=" << view.id << " complete=" << view.cpu_data_complete
                << " original=" << view.original_succeeded << " planned=" << view.planned_address << ':' << view.planned_length
                << " written=" << view.written_address << ':' << view.written_length
                << " rect=" << view.source_x << ',' << view.source_y << ',' << view.source_width << ',' << view.source_height << '\n';
#ifdef REREVVED_PREPARED_RG16_TEST
        if (++seenPreparedCopies == dropPreparedCopy)
        {
            preparedCopyIds.push_back(view.id);
            journal << "omitted_native_producer id=" << view.id << '\n';
            return true;
        }
#endif
        if (!inputs.PreparedCopy(view, error))
            return false;
        preparedCopyIds.push_back(view.id);
        const auto* copy = commands.Copy(view.id);
        if (!copy || !copy->admittedOperation)
        {
            error = "Prepared copy returned without its immutable admission owner";
            return false;
        }
        return true;
    }

    bool OnSwapPrepared(const SwapView& boundary) override
    {
        journal << "prepared_swap id=" << boundary.id << " original=" << boundary.original_succeeded << '\n';
        if (boundary.id == initialBoundary)
            return true;
        if (!inputs.PreparedSwap(boundary, error))
        {
#ifdef REREVVED_PREPARED_RG16_TEST
            if (firstError.empty())
                firstError = error;
#endif
            return false;
        }
        const auto&                                snapshot = inputs.Snapshot();
        std::vector<NativeGameplayRetainedTexture> unused;
        finished = commands.FinishPrepared(snapshot.events.back(), snapshot.final_swap, snapshot.end_frame, snapshot.end_submission, frame, unused, error);
        return finished;
    }

    bool OnDraw(const DrawView& view) override
    {
        journal << "original_draw id=" << view.id << " issued=" << view.original_host_issued << '\n';
#ifdef REREVVED_PREPARED_RG16_TEST
        if (dropPreparedCopy)
            return true;
#endif
        const bool paired = std::ranges::find(preparedDrawIds, view.id) != preparedDrawIds.end();
        if (!paired)
            error = "Original draw did not follow its exact Prepared ID";
        return paired;
    }

    bool OnCopy(const CopyView& view) override
    {
        journal << "original_copy id=" << view.id << " success=" << view.original_succeeded
                << " written=" << view.written_address << ':' << view.written_length << '\n';
        const bool paired = std::ranges::find(preparedCopyIds, view.id) != preparedCopyIds.end();
        if (!paired)
            error = "Original copy did not follow its exact Prepared ID";
        return paired;
    }

    bool OnSwap(const SwapView& view) override
    {
        journal << "original_swap id=" << view.id << " success=" << view.original_succeeded << '\n';
        return true;
    }

    bool OnFrameEnd(std::uint64_t frameId, std::uint64_t submission, bool success) override
    {
        journal << "end frame=" << frameId << " submission=" << submission << " success=" << success << '\n';
        return true;
    }

    NativeGameplayCommandFeed                            commands;
    rerevved::gpu::diagnostics::NativeGameplayLiveInputs inputs;
    NativeRenderFrame                                    frame;
    std::ostringstream                                   journal;
    std::vector<std::uint64_t>                           preparedDrawIds, preparedCopyIds;
#ifdef REREVVED_PREPARED_RG16_TEST
    std::size_t dropPreparedCopy = 0, seenPreparedCopies = 0;
    std::string firstError;
#endif
    std::uint64_t initialBoundary = 0;
    bool          finished        = false;
    std::string   error;
};

std::vector<std::uint8_t> unpackTerminalPixels(std::span<const std::uint8_t> packed)
{
    UNSCOPED_INFO("complete packed terminal required");
    REQUIRE(packed.size() == 1280U * 720U * 4U);
    std::vector<std::uint8_t> result(packed.size());
    for (std::size_t offset = 0; offset < packed.size(); offset += 4)
    {
        std::uint32_t word;
        std::memcpy(&word, packed.data() + offset, 4);
        const auto rgba = PresenterPackedPixelTestAccess::Packed10bpcRGBTo8bpcBytes(word);
        std::memcpy(result.data() + offset, &rgba, 4);
    }
    return result;
}

void writeClearProofBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::noreplace);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    UNSCOPED_INFO("fresh resolve-clear proof artifact required");
    REQUIRE(bool(file));
}

#endif

#ifdef REREVVED_PREPARED_MAIN_CLEAR_TEST
void checkPreparedMainClear()
{
    using rex::ui::d3d12::D3D12Presenter;
    using rex::ui::d3d12::GpuGuestOutputTicket;
    const auto* rootText   = std::getenv("REREVVED_PREPARED_MAIN_CLEAR_OUTPUT");
    const auto* shaderText = std::getenv("REREVVED_PREPARED_NATIVE_SHADER_ROOT");
    UNSCOPED_INFO("main clear proof requires fresh output and pinned shader catalog");
    REQUIRE((rootText && *rootText && shaderText && *shaderText));
    const auto root = std::filesystem::path(rootText);
    UNSCOPED_INFO("fresh main clear proof directory required");
    REQUIRE(std::filesystem::create_directory(root));
    std::string                                error;
    std::vector<NativeGameplayShaderInterface> shaders;
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(std::filesystem::path(shaderText), shaders, error));
    UNSCOPED_INFO("main clear proof requires the generated 4X/mask9 specialization");
    REQUIRE((shaders.size() == 1 && shaders[0].sampleCount == 4 && shaders[0].sampleMask == 9));
    auto                                           observer = std::make_shared<MainClearGraphObserver>(shaders);
    rex::graphics::testing::MainClearControlResult original;
    const auto                                     originalStatus = rex::graphics::testing::RunMainClearControl(observer, original, error);
    const auto                                     journal        = observer->journal.str();
    writeClearProofBytes(root / "callback-journal.txt", { reinterpret_cast<const std::uint8_t*>(journal.data()), journal.size() });
    UNSCOPED_INFO((observer->error.empty() ? error : observer->error));
    REQUIRE((originalStatus == 0 && observer->error.empty()));
    UNSCOPED_INFO("actual SDK control did not finish its Prepared/native graph and original output");
    REQUIRE((observer->finished && original.progressed && original.completion_observed && original.terminal_capture));
    UNSCOPED_INFO("actual original control terminal descriptor differs");
    REQUIRE((original.terminal_width == 1280 && original.terminal_height == 720 && original.terminal_stride == 1280 * 4));
    UNSCOPED_INFO("one left and one right copy must each have one Prepared/original pair");
    REQUIRE((observer->preparedCopyIds.size() == 2 && original.prepared_copies.size() == 2 && original.original_copies.size() == 2));
    for (std::size_t index = 0; index < 2; ++index)
    {
        const auto& prepared = original.prepared_copies[index];
        const auto& issued   = original.original_copies[index];
        UNSCOPED_INFO("actual original copy success/range did not pair with its prospective Prepared ID");
        REQUIRE((prepared.id == issued.id && prepared.frame == issued.frame && prepared.submission == issued.submission &&
                 prepared.id == observer->preparedCopyIds[index] && prepared.cpu_data_complete &&
                 !prepared.original_succeeded && !prepared.written_address && !prepared.written_length &&
                 issued.original_succeeded && issued.written_address == prepared.planned_address &&
                 issued.written_length == prepared.planned_length && issued.written_length));
    }
    UNSCOPED_INFO(error);
    REQUIRE(observer->inputs.WriteSnapshot(root / "inputs", error));
    writeClearProofBytes(root / "original.rgba", original.terminal_pixels);
    auto frame = observer->frame;
    UNSCOPED_INFO("selected graph must resolve left before its preserved three-plane clear and right consumers");
    REQUIRE((frame.passes.size() == 2 && frame.passes[0].resolves.size() == 1 && frame.passes[1].resolves.size() == 1 &&
             frame.passes[1].preservePreviousAttachments && frame.passes[1].clears.size() == 1 &&
             frame.passes[1].clears[0].color && frame.passes[1].clears[0].depth && frame.passes[1].clears[0].stencil &&
             frame.passes[0].resolves[0].destinationX == 0 && frame.passes[1].resolves[0].destinationX == 640 &&
             frame.passes[0].resolves[0].width == 640 && frame.passes[1].resolves[0].width == 640));
    auto provider = rex::ui::d3d12::D3D12Provider::Create();
    UNSCOPED_INFO("main clear proof D3D12 provider");
    REQUIRE(bool(provider));
    auto presenter = D3D12Presenter::Create([](bool, bool)
                                            {
                                            },
                                            *provider);
    UNSCOPED_INFO("main clear proof SDK presenter");
    REQUIRE(bool(presenter));
    const auto                            stream = presenter->CreateGpuGuestOutputStream();
    std::shared_ptr<GpuGuestOutputTicket> published;
    const auto                            publish = [&](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
    {
        D3D12Presenter::GpuGuestOutputFrame output;
        output.resource    = resource;
        output.ready_fence = fence;
        output.ready_value = value;
        output.width       = 1280;
        output.height      = 720;
        published          = presenter->QueueGpuGuestOutputFrame(stream, std::move(output));
        return published;
    };

    struct Gate
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        bool                                released = false;

        ~Gate()
        {
            if (fence && !released)
                (void)fence->Signal(1);
        }

        void Release()
        {
            UNSCOPED_INFO("clear proof gate signal");
            REQUIRE((fence && SUCCEEDED(fence->Signal(1))));
            released = true;
        }
    };

    NativeRendererD3D12 renderer;
    Gate                nativeGate, copyGate;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publish));
    UNSCOPED_INFO("native gate allocation");
    REQUIRE(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nativeGate.fence))));
    UNSCOPED_INFO("SDK copy gate allocation");
    REQUIRE(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyGate.fence))));
    auto* queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    UNSCOPED_INFO("hold actual native completion");
    REQUIRE((queue && SUCCEEDED(queue->Wait(nativeGate.fence.Get(), 1))));
    frame.returnGpuOutput      = true;
    frame.returnResolvedOutput = false;
    std::shared_ptr<NativeRenderFrameTicket> ticket;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(frame, ticket, error));
    UNSCOPED_INFO("main graph recording readiness");
    REQUIRE(ticket->Recording().wait_for(kReadyCeiling) == std::future_status::ready);
    const auto recorded = ticket->Recording().get();
    UNSCOPED_INFO("Recording must precede native completion and output publication");
    REQUIRE((recorded.success && !published && ticket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
    nativeGate.Release();
    UNSCOPED_INFO("main graph completion after real drain fence");
    REQUIRE(ticket->Completion().wait_for(kReadyCeiling) == std::future_status::ready);
    const auto completed = ticket->Completion().get();
    UNSCOPED_INFO(completed.error);
    REQUIRE((completed.success && completed.completionFenceValue && published && completed.gpuOutputTicket == published));
    auto* sdkQueue = rex::ui::d3d12::detail::GpuGuestOutputTestAccess::CopyQueue(*presenter);
    UNSCOPED_INFO("hold actual SDK copy retirement");
    REQUIRE((sdkQueue && SUCCEEDED(sdkQueue->Wait(copyGate.fence.Get(), 1))));
    Microsoft::WRL::ComPtr<ID3D12Resource> copied;
    UNSCOPED_INFO("queue actual SDK consumer copy");
    REQUIRE(presenter->RefreshGuestOutput(1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& context)
                                          {
                                              copied = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
                                              context.SetIs8bpc(true);
                                              return true;
                                          }));
    std::uint64_t copyFence = 0;
    UNSCOPED_INFO("native completion must not establish SDK-copy retirement");
    REQUIRE((NativeRendererD3D12::PollGpuOutput(published, &copyFence) == NativeRendererD3D12::GpuOutputStatus::Pending && !copyFence));
    copyGate.Release();
    UNSCOPED_INFO(error);
    REQUIRE((NativeRendererD3D12::AwaitGpuOutput(published, error, &copyFence) && copyFence));
    const auto positive = unpackTerminalPixels(readGpuMailbox(*provider, copied.Get()));
    writeClearProofBytes(root / "prepared.rgba", positive);
    UNSCOPED_INFO("complete Prepared terminal differs from same-process original GPU control");
    REQUIRE(positive == original.terminal_pixels);
    renderer.Shutdown();
    for (const auto plane : { 0, 1, 2, 3 })
    {
        auto       counter   = observer->frame;
        const auto postClear = counter.passes[1].clears[0];
        if (plane < 3)
        {
            auto& clear = counter.passes[1].clears[0];
            if (plane == 0)
                clear.color = false;
            if (plane == 1)
                clear.depth = false;
            if (plane == 2)
                clear.stencil = false;
        }
        else
        {
            NativeRenderFramePass early;
            early.target                      = counter.passes[0].target;
            early.preservePreviousAttachments = true;
            early.clears                      = { postClear };
            early.resolves                    = std::move(counter.passes[0].resolves);
            counter.passes[1].clears.clear();
            counter.passes.insert(counter.passes.begin() + 1, std::move(early));
        }
        counter.returnGpuOutput      = false;
        counter.returnResolvedOutput = true;
        NativeRendererD3D12 control;
        UNSCOPED_INFO(error);
        REQUIRE(control.StartDrawStream(error));
        const auto result = control.SubmitRenderFrame(std::move(counter));
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        const auto  pixels        = unpackTerminalPixels(result.resolvedOutput);
        const auto  expectedStart = plane == 3 ? 0U : 640U + std::uint32_t(plane) * 640U / 3U;
        const auto  expectedEnd   = plane == 3 ? 640U : 640U + std::uint32_t(plane + 1) * 640U / 3U;
        std::size_t changed       = 0;
        for (std::size_t y = 0; y < 720; ++y)
            for (std::size_t x = expectedStart; x < expectedEnd; ++x)
                changed += std::memcmp(positive.data() + (y * 1280 + x) * 4, pixels.data() + (y * 1280 + x) * 4, 4) != 0;
        UNSCOPED_INFO("omitted clear plane or early clear did not affect its required consumer pixels");
        REQUIRE(changed);
        writeClearProofBytes(root / ("counter-" + std::to_string(plane) + ".rgba"), pixels);
        observer->journal << "counter plane=" << plane << " changed_pixels=" << changed << " native_fence=" << result.completionFenceValue << '\n';
        control.Shutdown();
    }
    observer->journal << "native_completion_fence=" << completed.completionFenceValue << " sdk_copy_fence=" << copyFence << " full_terminal_match=true\n";
    const auto finalJournal = observer->journal.str();
    writeClearProofBytes(root / "proof-journal.txt", { reinterpret_cast<const std::uint8_t*>(finalJournal.data()), finalJournal.size() });
    presenter->CloseGpuGuestOutputStream(stream);
}
#endif

#ifdef REREVVED_PREPARED_FRAME_BRIDGE_TEST
void checkPreparedSdkBridge()
{
    using namespace rex::graphics::diagnostic;
    using rerevved::gpu::diagnostics::NativeGameplayLiveInputs;
    using rex::graphics::testing::PreparedFrameInputTestSupport;
    using rex::ui::d3d12::D3D12Presenter;
    using rex::ui::d3d12::GpuGuestOutputTicket;

    PreparedFrameInputTestSupport support;
    std::string                   error;
    // The first constructor pass is an identity/CPU fixture only. Native
    // source state is armed at the producer copy boundary below, then the
    // analyzed draw is rebuilt after producer recording proves that boundary.
    UNSCOPED_INFO(error);
    REQUIRE(support.BuildConsumerDraw(error, false));
    const auto checkPreparedDemand = [&]()
    {
        UNSCOPED_INFO("SDK prepared constructor did not exercise the qualified raw shader pair and guards");
        REQUIRE((support.vertex_shader_hash() == 0xF7EB47B270EC78CEULL &&
                 support.pixel_shader_hash() == 0xB52D04B35FF2B558ULL &&
                 support.vertex_words().size() == 45 && support.pixel_words().size() == 12 &&
                 !support.vertex_word_digest().empty() && !support.pixel_word_digest().empty() &&
                 support.sampler_calls() && support.readable_calls() && support.version_calls()));

        std::size_t requiredTextureViews = 0;
        for (const auto& texture : support.draw().textures)
            if (texture.required)
            {
                ++requiredTextureViews;
                UNSCOPED_INFO("SDK prepared effective texture demand is not the supported unsigned native source");
                REQUIRE((!texture.is_signed && texture.fetch_constant == support.texture_fetch_constant() &&
                         !texture.available && texture.unavailable_reason == DrawCaptureReason::kTextureUnresolved &&
                         texture.format == 6 && texture.dimension == 1 && texture.depth == 1 &&
                         texture.mip_min_level == 0 && texture.mip_max_level == 0 &&
                         texture.tiled && !texture.packed_mips));
            }
        UNSCOPED_INFO("SDK prepared shader demand did not select one effective native texture view");
        REQUIRE((requiredTextureViews == 1 && support.texture_fetch_constant() == 0));
    };

    const auto* shaderRoot = std::getenv("REREVVED_PREPARED_NATIVE_SHADER_ROOT");
    UNSCOPED_INFO("REREVVED_PREPARED_NATIVE_SHADER_ROOT must name the pinned native shader catalog");
    REQUIRE((shaderRoot && *shaderRoot));
    std::vector<NativeGameplayShaderInterface> shaderCatalog;
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(std::filesystem::path(shaderRoot), shaderCatalog, error));
    std::cout << "prepared_shader_identity vs=" << std::hex << support.vertex_shader_hash()
              << " ps=" << support.pixel_shader_hash() << std::dec
              << " vs_sha256=" << support.vertex_word_digest()
              << " ps_sha256=" << support.pixel_word_digest() << '\n';
    const auto shaderIt = std::find_if(
        shaderCatalog.begin(), shaderCatalog.end(), [&](const auto& shader)
        {
            return shader.vertexShaderHash == support.vertex_shader_hash() &&
                   shader.pixelShaderHash == support.pixel_shader_hash() &&
                   shader.vertexGuestWordDigest == support.vertex_word_digest() &&
                   shader.pixelGuestWordDigest == support.pixel_word_digest() &&
                   shader.alphaTestEnabled == false && shader.alphaTestCompare == 0 &&
                   shader.rasterizedDepth && shader.independentSamplers &&
                   !shader.generatedPixelParameters && shader.colorTargetFormat == 0 &&
                   shader.colorExpBias == 0 && shader.sampleCount == 1 &&
                   shader.sampleMask == 1 && shader.shaderProgram == 0x10010002 &&
                   shader.shaderContext == 4 && shader.shaderInterpolators == 1 &&
                   shader.vertexAttributes.size() == 2 &&
                   shader.vertexAttributes[0].fetchConstant == 95 &&
                   shader.vertexAttributes[0].strideBytes == 56 &&
                   shader.vertexAttributes[0].offsetBytes == 0 &&
                   shader.vertexAttributes[0].format == 57 &&
                   shader.vertexAttributes[0].isSigned &&
                   !shader.vertexAttributes[0].normalized &&
                   shader.vertexAttributes[1].fetchConstant == 95 &&
                   shader.vertexAttributes[1].strideBytes == 56 &&
                   shader.vertexAttributes[1].offsetBytes == 16 &&
                   shader.vertexAttributes[1].format == 37 &&
                   shader.vertexAttributes[1].isSigned &&
                   !shader.vertexAttributes[1].normalized &&
                   shader.textureFetchSlots.size() == 1 &&
                   shader.textureFetchSlots.front() == 0 &&
                   shader.textureDimensions.size() == 1 &&
                   shader.textureDimensions.front() == NativeDrawReplayTextureDimension::Texture2D &&
                   shader.vertexDxilDigest ==
                       "76bb9f9ab75baadcae946853837bc7fb8ceb3292d691ca053508029fc9408f4e" &&
                   shader.pixelDxilDigest ==
                       "05ec1ed8752f94c1f298ad54c735a57adddafd28e2b132330eae33cb0f97da21";
        });
    UNSCOPED_INFO("pinned native shader catalog has no exact prepared raw shader pair");
    REQUIRE(shaderIt != shaderCatalog.end());
    UNSCOPED_INFO("qualified native shader interface does not match the prepared raw demand");
    REQUIRE((shaderIt->vertexGuestWordDigest == support.vertex_word_digest() &&
             shaderIt->pixelGuestWordDigest == support.pixel_word_digest() &&
             shaderIt->sampleCount == 1 && shaderIt->sampleMask == 1 &&
             shaderIt->colorTargetFormat == 0 && shaderIt->shaderProgram == 0x10010002 &&
             shaderIt->shaderContext == 4 && shaderIt->shaderInterpolators == 1));
    const auto* preparedShader = &*shaderIt;

    const auto* producerAlphaText = std::getenv("REREVVED_PREPARED_PRODUCER_ALPHA");
    float       producerAlpha     = 1.0F;
    if (producerAlphaText && *producerAlphaText)
    {
        char* parsedEnd = nullptr;
        producerAlpha   = std::strtof(producerAlphaText, &parsedEnd);
        UNSCOPED_INFO("REREVVED_PREPARED_PRODUCER_ALPHA must be 0, 0.5, or 1");
        REQUIRE((parsedEnd && *parsedEnd == '\0' && std::isfinite(producerAlpha) &&
                 (producerAlpha == 0.0F || producerAlpha == 0.5F || producerAlpha == 1.0F)));
    }
    support.SetClearAlpha(producerAlpha);
    UNSCOPED_INFO(error);
    REQUIRE(support.BuildClearDraw(error));
    DrawCaptureFrameInputCopyView producerCopy;
    UNSCOPED_INFO(error);
    REQUIRE(support.BuildResolveCopy(4, 1, 1, support.texture_base(), true, producerCopy, error));

    std::vector<std::uint32_t> gamma(256);
    for (std::size_t index = 0; index < gamma.size(); ++index)
        gamma[index] = (static_cast<std::uint32_t>(index) * 0x3FFU / 0xFFU) *
                       ((1U << 20) | (1U << 10) | 1U);

    auto connect = [](NativeGameplayCommandFeed& commands,
                      NativeGameplayLiveInputs&  inputs)
    {
        inputs.SetEventConsumer(
            [&commands](const auto& event, const auto& bank, const auto& textures, const auto& cpu, std::string& failure)
            {
                if (event.kind == DrawCaptureFrameEvent::Kind::kSwap)
                    return commands.ConsumePreparedBoundary(event, bank, failure);
                if (event.kind == DrawCaptureFrameEvent::Kind::kCopy)
                    return commands.ConsumePreparedCopy(event, bank, textures, cpu, failure);
                return commands.ConsumePreparedDraw(event, bank, textures, cpu, failure);
            });
    };

    DrawCaptureFrameInputSwapView producerBoundary;
    producerBoundary.id                 = 1;
    producerBoundary.frame              = 1;
    producerBoundary.submission         = 1;
    producerBoundary.registers          = support.registers();
    producerBoundary.gamma_256          = gamma;
    producerBoundary.is_8bpc            = true;
    producerBoundary.original_succeeded = false;

    std::array<NativeGameplayShaderInterface, 1> shaderSpan{ *preparedShader };
    NativeGameplayCommandFeed                    producerCommands{
        std::span<const NativeGameplayShaderInterface>(shaderSpan)
    };
    NativeGameplayLiveInputs producerInputs;
    connect(producerCommands, producerInputs);
    UNSCOPED_INFO(error);
    REQUIRE(producerCommands.Begin(1, 1, {}, error));
    UNSCOPED_INFO(error);
    REQUIRE(producerInputs.BeginPrepared(producerBoundary, error));
    UNSCOPED_INFO(error);
    REQUIRE(producerInputs.PreparedDraw(support.clear_draw(), error));
    UNSCOPED_INFO(error);
    REQUIRE(producerInputs.PreparedCopy(producerCopy, error));
    // The copy event is the real native-source preissue boundary. Complete it
    // as not originally issued, leaving the texture unresolved for the later
    // prepared draw admission.
    UNSCOPED_INFO(error);
    REQUIRE(support.ArmNativeSource(error));
    UNSCOPED_INFO(error);
    REQUIRE(support.CompleteNativeSource(false, error));

    const auto  producerPage = support.texture_base() / 0x1000U;
    const auto* admittedCopy = producerCommands.Copy(producerCopy.id);
    UNSCOPED_INFO("prepared producer copy did not expose the admitted native Rgba8 layout");
    REQUIRE((admittedCopy && admittedCopy->producerLayout.valid &&
             admittedCopy->producerLayout.width == 256 &&
             admittedCopy->producerLayout.height == 32 &&
             admittedCopy->producerLayout.format == 6 &&
             admittedCopy->producerLayout.tiled));

    NativeRenderFrame producerFrame;
    UNSCOPED_INFO(error);
    REQUIRE(producerCommands.FinishProducer(producerPage, producerCopy.id, producerFrame, error));
    UNSCOPED_INFO("prepared producer did not finish through the retained prefix owner");
    REQUIRE((producerFrame.retainOnly && !producerFrame.returnResolvedOutput &&
             producerFrame.outputTextureId));

    const auto producerTexture = std::find_if(
        producerFrame.textures.begin(), producerFrame.textures.end(), [&](const auto& texture)
        {
            return texture.id == producerFrame.outputTextureId;
        });
    UNSCOPED_INFO("prepared producer retained resource has the wrong native descriptor");
    REQUIRE((producerTexture != producerFrame.textures.end() &&
             producerTexture->width == 256 && producerTexture->height == 32 &&
             producerTexture->format == NativeDrawReplayTextureFormat::Rgba8 &&
             producerTexture->persistentKey == producerCopy.id));
    NativeGameplayRetainedTexture retained{
        producerPage, *producerTexture, admittedCopy->producerLayout
    };
    retained.texture.imported = true;

    if (const auto* omitted = std::getenv("REREVVED_PREPARED_OMIT_PRODUCER");
        omitted && std::string_view(omitted) == "1")
    {
        UNSCOPED_INFO(error);
        REQUIRE(support.BuildConsumerDraw(error, false));
        checkPreparedDemand();
        NativeGameplayCommandFeed omittedCommands{
            std::span<const NativeGameplayShaderInterface>(shaderSpan)
        };
        NativeGameplayLiveInputs omittedInputs;
        connect(omittedCommands, omittedInputs);
        auto omittedBoundary  = producerBoundary;
        omittedBoundary.frame = omittedBoundary.submission = 2;
        UNSCOPED_INFO(error);
        REQUIRE(omittedCommands.Begin(2, 2, {}, error));
        UNSCOPED_INFO(error);
        REQUIRE(omittedInputs.BeginPrepared(omittedBoundary, error));
        const bool accepted = omittedInputs.PreparedDraw(support.draw(), error);
        UNSCOPED_INFO("omitted native producer was not rejected: " + error);
        REQUIRE((!accepted && error.find("no retained producer page") != std::string::npos));
        const std::string report = "prepared_bridge producer_omitted=true consumer_rejected=true native_submissions=0\n";
        if (const auto* path = std::getenv("REREVVED_PREPARED_BRIDGE_OUTPUT"); path && *path)
        {
            std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::noreplace);
            file << report << "sha256=" << rex::crypto::sha256(report) << '\n';
            UNSCOPED_INFO("omitted producer graph artifact");
            REQUIRE(bool(file));
        }
        std::cout << report;
        return;
    }

    auto provider = rex::ui::d3d12::D3D12Provider::Create();
    UNSCOPED_INFO("prepared bridge D3D12 provider");
    REQUIRE(bool(provider));
    auto presenter = D3D12Presenter::Create([](bool, bool)
                                            {
                                            },
                                            *provider);
    UNSCOPED_INFO("prepared bridge D3D12 presenter");
    REQUIRE(bool(presenter));
    const auto                                          stream = presenter->CreateGpuGuestOutputStream();
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> publishedResources;
    std::shared_ptr<GpuGuestOutputTicket>               lastPublished;
    const auto                                          publish = [&](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
    {
        D3D12Presenter::GpuGuestOutputFrame output;
        output.resource    = resource;
        output.ready_fence = fence;
        output.ready_value = value;
        output.width       = 1280;
        output.height      = 720;
        lastPublished      = presenter->QueueGpuGuestOutputFrame(
            stream, std::move(output));
        if (lastPublished)
            publishedResources.emplace_back(resource);
        return lastPublished;
    };

    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publish));

    struct NativeFenceGate
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        bool                                signaled = false;

        ~NativeFenceGate()
        {
            if (fence && !signaled)
                (void)fence->Signal(1);
        }

        bool Signal()
        {
            if (!fence || signaled)
                return signaled;
            signaled = SUCCEEDED(fence->Signal(1));
            return signaled;
        }
    } nativeGate;

    UNSCOPED_INFO("prepared bridge external native fence");
    REQUIRE(SUCCEEDED(provider->GetDevice()->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nativeGate.fence))));
    auto* nativeQueue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    UNSCOPED_INFO("prepared bridge native queue gate");
    REQUIRE((nativeQueue && SUCCEEDED(nativeQueue->Wait(nativeGate.fence.Get(), 1))));
    std::shared_ptr<NativeRenderFrameTicket> producerTicket;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(std::move(producerFrame), producerTicket, error));
    UNSCOPED_INFO("prepared producer recording did not become ready behind the native gate");
    REQUIRE(producerTicket->Recording().wait_for(kReadyCeiling) ==
            std::future_status::ready);
    const auto producerRecorded = producerTicket->Recording().get();
    UNSCOPED_INFO(producerRecorded.error);
    REQUIRE(producerRecorded.success);
    UNSCOPED_INFO("prepared producer completed before the external native gate was signaled");
    REQUIRE(producerTicket->Completion().wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::timeout);

    retained.texture.producerVersion =
        producerTicket->Export(retained.texture.persistentKey);
    UNSCOPED_INFO("prepared producer did not expose a pending native import proof");
    REQUIRE(bool(retained.texture.producerVersion));

    UNSCOPED_INFO(error);
    REQUIRE(support.BuildConsumerDraw(error, false));
    checkPreparedDemand();

    const auto producerCompletedBeforeSignal =
        producerTicket->Completion().wait_for(std::chrono::milliseconds(0));
    UNSCOPED_INFO("prepared producer completed before consumer admission");
    REQUIRE(producerCompletedBeforeSignal == std::future_status::timeout);

    auto missingProof = retained;
    missingProof.texture.producerVersion.reset();
    std::array<NativeGameplayRetainedTexture, 1> missingProofArray{ missingProof };

    const auto consumerDestination =
        (support.texture_base() + 0x00400000U) & ~0xFFFU;
    UNSCOPED_INFO("consumer resolve aliases producer page");
    REQUIRE(consumerDestination != support.texture_base());
    DrawCaptureFrameInputCopyView consumerCopy;
    UNSCOPED_INFO(error);
    REQUIRE(support.BuildResolveCopy(12, 2, 2, consumerDestination, false, consumerCopy, error));

    auto consumerDraw           = support.draw();
    consumerDraw.id             = 11;
    consumerDraw.frame          = 2;
    consumerDraw.submission     = 2;
    auto consumerBoundary       = producerBoundary;
    consumerBoundary.id         = 10;
    consumerBoundary.frame      = 2;
    consumerBoundary.submission = 2;

    NativeGameplayCommandFeed rejectedCommands{
        std::span<const NativeGameplayShaderInterface>(shaderSpan)
    };
    NativeGameplayLiveInputs rejectedInputs;
    connect(rejectedCommands, rejectedInputs);
    std::string rejectedError;
    UNSCOPED_INFO(rejectedError);
    REQUIRE(rejectedCommands.Begin(2, 2, missingProofArray, rejectedError));
    UNSCOPED_INFO(rejectedError);
    REQUIRE(rejectedInputs.BeginPrepared(consumerBoundary, rejectedError));
    const bool missingProofAccepted = rejectedInputs.PreparedDraw(consumerDraw, rejectedError);
    UNSCOPED_INFO("missing producer proof was not rejected at native draw admission: " + rejectedError);
    REQUIRE((!missingProofAccepted &&
             rejectedError.find("completed producer proof") != std::string::npos));

    std::vector<std::uint32_t> terminalRegisters(consumerCopy.registers.begin(),
                                                 consumerCopy.registers.end());
    terminalRegisters[0x4800] = 2;
    terminalRegisters[0x4801] = (consumerDestination & 0xFFFFF000U) | 6U;
    terminalRegisters[0x4802] = 1279U | (719U << 13);
    terminalRegisters[0x4803] = 0xD10U;
    DrawCaptureFrameInputSwapView consumerTerminal;
    consumerTerminal.id                 = 13;
    consumerTerminal.frame              = 2;
    consumerTerminal.submission         = 2;
    consumerTerminal.frontbuffer_ptr    = consumerDestination;
    consumerTerminal.frontbuffer_width  = 1280;
    consumerTerminal.frontbuffer_height = 720;
    consumerTerminal.registers          = terminalRegisters;
    consumerTerminal.gamma_256          = gamma;
    consumerTerminal.is_8bpc            = true;
    consumerTerminal.original_succeeded = false;

    NativeGameplayCommandFeed consumerCommands{
        std::span<const NativeGameplayShaderInterface>(shaderSpan)
    };
    NativeGameplayLiveInputs consumerInputs;
    connect(consumerCommands, consumerInputs);
    UNSCOPED_INFO(error);
    REQUIRE(consumerCommands.Begin(2, 2, std::span<const NativeGameplayRetainedTexture>(&retained, 1), error));
    UNSCOPED_INFO(error);
    REQUIRE(consumerInputs.BeginPrepared(consumerBoundary, error));
    UNSCOPED_INFO(error);
    REQUIRE(consumerInputs.PreparedDraw(consumerDraw, error));
    UNSCOPED_INFO(error);
    REQUIRE(consumerInputs.PreparedCopy(consumerCopy, error));
    UNSCOPED_INFO(error);
    REQUIRE(consumerInputs.PreparedSwap(consumerTerminal, error));

    NativeRenderFrame                          consumerFrame;
    std::vector<NativeGameplayRetainedTexture> unusedRetained;
    const auto&                                consumerSnapshot = consumerInputs.Snapshot();
    UNSCOPED_INFO(error);
    REQUIRE(consumerCommands.FinishPrepared(
        consumerSnapshot.events.back(), consumerSnapshot.final_swap, consumerSnapshot.end_frame, consumerSnapshot.end_submission, consumerFrame, unusedRetained, error));
    consumerFrame.returnGpuOutput = true;
    // Deferred terminal overlaps producer and consumer behind an external gate;
    // SDK and native fences are separate.
    consumerFrame.returnResolvedOutput = false;
    std::shared_ptr<NativeRenderFrameTicket> consumerTicket;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(std::move(consumerFrame), consumerTicket, error));
    UNSCOPED_INFO("prepared consumer recording did not become ready behind the native gate");
    REQUIRE(consumerTicket->Recording().wait_for(kReadyCeiling) ==
            std::future_status::ready);
    const auto consumerRecorded = consumerTicket->Recording().get();
    UNSCOPED_INFO(consumerRecorded.error);
    REQUIRE(consumerRecorded.success);
    UNSCOPED_INFO("prepared consumer completed before the external native gate was signaled");
    REQUIRE(consumerTicket->Completion().wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::timeout);

    UNSCOPED_INFO("prepared bridge external native fence signal");
    REQUIRE(nativeGate.Signal());
    UNSCOPED_INFO("prepared producer native completion did not retire after the gate");
    REQUIRE(producerTicket->Completion().wait_for(kReadyCeiling) ==
            std::future_status::ready);
    const auto producerCompleted = producerTicket->Completion().get();
    UNSCOPED_INFO(producerCompleted.error);
    REQUIRE((producerCompleted.success && producerCompleted.completionFenceValue &&
             producerCompleted.resolvedOutput.empty()));
    UNSCOPED_INFO("prepared consumer native completion did not retire after the gate");
    REQUIRE(consumerTicket->Completion().wait_for(kReadyCeiling) ==
            std::future_status::ready);
    const auto consumerCompleted = consumerTicket->Completion().get();
    UNSCOPED_INFO(consumerCompleted.error);
    REQUIRE((consumerCompleted.success && consumerCompleted.completionFenceValue &&
             consumerCompleted.gpuOutputTicket &&
             consumerCompleted.completionFenceValue >
                 producerCompleted.completionFenceValue));
    Microsoft::WRL::ComPtr<ID3D12Resource> copiedDestination;
    UNSCOPED_INFO("prepared bridge actual SDK consumer refresh");
    REQUIRE(presenter->RefreshGuestOutput(
        1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& context)
        {
            copiedDestination =
                static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
            context.SetIs8bpc(true);
            return true;
        }));
    std::uint64_t consumerCopyFence = 0;
    UNSCOPED_INFO(error);
    REQUIRE((NativeRendererD3D12::AwaitGpuOutput(
                 consumerCompleted.gpuOutputTicket, error, &consumerCopyFence) &&
             consumerCopyFence));
    UNSCOPED_INFO("prepared bridge did not publish the distinct native consumer resolve");
    REQUIRE((lastPublished && publishedResources.size() == 1 &&
             consumerCompleted.gpuOutputTicket->status() ==
                 GpuGuestOutputTicket::Status::kCompleted));
    const auto            packedPixels    = readGpuMailbox(*provider, copiedDestination.Get());
    constexpr std::size_t kTerminalWidth  = 1280;
    constexpr std::size_t kTerminalHeight = 720;
    constexpr std::size_t kTerminalStride = kTerminalWidth * 4U;
    UNSCOPED_INFO("prepared bridge native terminal readback has the wrong packed extent");
    REQUIRE(packedPixels.size() == kTerminalStride * kTerminalHeight);
    std::vector<std::uint8_t> capturedPixels(packedPixels.size());
    for (std::size_t offset = 0; offset < packedPixels.size(); offset += sizeof(std::uint32_t))
    {
        std::uint32_t packedPixel;
        std::memcpy(&packedPixel, packedPixels.data() + offset, sizeof(packedPixel));
        const auto convertedPixel =
            PresenterPackedPixelTestAccess::Packed10bpcRGBTo8bpcBytes(packedPixel);
        std::memcpy(capturedPixels.data() + offset, &convertedPixel, sizeof(convertedPixel));
    }
    const auto  pixelDigest = rex::crypto::sha256(std::string_view(
        reinterpret_cast<const char*>(capturedPixels.data()), capturedPixels.size()));
    const auto* pixelPath   = std::getenv("REREVVED_PREPARED_BRIDGE_PIXELS");
    if (pixelPath && *pixelPath)
    {
        std::ofstream pixelFile(std::filesystem::path(pixelPath),
                                std::ios::binary | std::ios::noreplace);
        pixelFile.write(reinterpret_cast<const char*>(capturedPixels.data()),
                        static_cast<std::streamsize>(capturedPixels.size()));
        UNSCOPED_INFO("prepared bridge pixel artifact");
        REQUIRE(bool(pixelFile));
    }
    std::string graphReport =
        "prepared_bridge producer_native_fence=" +
        std::to_string(producerCompleted.completionFenceValue) +
        " consumer_native_fence=" +
        std::to_string(consumerCompleted.completionFenceValue) +
        " sdk_copy_fence=" + std::to_string(consumerCopyFence) +
        " sdk_ticket=" +
        std::to_string(consumerCompleted.gpuOutputTicket->token()) +
        " producer_alpha=" + std::to_string(producerAlpha) +
        " capture_source=sdk_copied_native_terminal" +
        " capture_format=R10G10B10A2" +
        " capture_conversion=rex::ui::Presenter::Packed10bpcRGBTo8bpcBytes" +
        " pixels_width=" + std::to_string(kTerminalWidth) +
        " pixels_height=" + std::to_string(kTerminalHeight) +
        " pixels_stride=" + std::to_string(kTerminalStride) +
        " pixels_sha256=" + pixelDigest + "\n";
    const auto  graphDigest = rex::crypto::sha256(graphReport);
    const auto* graphPath   = std::getenv("REREVVED_PREPARED_BRIDGE_OUTPUT");
    if (graphPath && *graphPath)
    {
        std::ofstream graphFile(std::filesystem::path(graphPath),
                                std::ios::binary | std::ios::noreplace);
        graphFile << graphReport << "sha256=" << graphDigest << "\n";
        UNSCOPED_INFO("prepared bridge graph artifact");
        REQUIRE(bool(graphFile));
    }
    std::cout << graphReport << "sha256=" << graphDigest << '\n';
    presenter->CloseGpuGuestOutputStream(stream);
    renderer.Shutdown();
}

#endif

void checkImage(const std::filesystem::path&       path,
                const std::array<std::uint8_t, 4>& left,
                const std::array<std::uint8_t, 4>& right)
{
    std::ifstream             file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file), {} };
    UNSCOPED_INFO("complete sample planes required");
    REQUIRE(bytes.size() == 1280U * 720U * 4U * 2U);
    for (std::size_t plane = 0; plane < 2; ++plane)
        for (std::size_t y = 0; y < 720; ++y)
            for (std::size_t x = 0; x < 1280; ++x)
            {
                const auto  offset   = ((plane * 720 + y) * 1280 + x) * 4;
                const auto& expected = x < 640 ? left : right;
                UNSCOPED_INFO("draw preservation or next-frame clear differs from expected samples");
                REQUIRE(std::memcmp(bytes.data() + offset, expected.data(), 4) == 0);
            }
}

void checkGameplayTextureBindings(const std::filesystem::path& directory)
{
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto draw        = makeDraw({ 1, 1, 1, 1 }, false);
    draw.textureMask = (1U << 13) - 1;
    draw.samplers.resize(13);
    for (std::size_t slot = 0; slot < 13; ++slot)
    {
        draw.textures[slot] = { { static_cast<std::uint8_t>(slot + 1), 0, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
        if (slot % 2)
        {
            draw.samplers[slot].address = { 4, 4, 4 };
            draw.samplers[slot].border  = { 0, 0, static_cast<float>(slot + 1) / 255.0F, 1 };
        }
    }
    draw.pixelShaderDxil = compile(
        "Texture2D<float4> t[13]:register(t0); SamplerState s[13]:register(s0,space3);"
        "float4 main():SV_Target0 { bool ok=true; [unroll] for(int i=0;i<13;++i) {"
        "float4 expected=(i%2)?float4(0,0,(i+1)/255.0,1):float4((i+1)/255.0,0,0,1);"
        "ok=ok && all(abs(t[i].SampleLevel(s[i],float2(2,2),0)-expected)<0.0001); }"
        "return ok?float4(0,1,0,1):float4(1,0,0,1); }",
        "ps_5_1");
    auto result = renderer.SubmitDrawFrame({ draw }, directory / "gameplay-texture-bindings.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "gameplay-texture-bindings.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    // The same retained descriptors must refresh independent sampler state.
    draw.samplers[11].address = { 3, 3, 3 };
    result                    = renderer.SubmitDrawFrame({ draw }, directory / "gameplay-sampler-refresh.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "gameplay-sampler-refresh.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    draw.textureMask = UINT32_MAX;
    draw.samplers.resize(kNativeDrawReplayTextureSlots);
    for (std::size_t slot = 13; slot < draw.textures.size(); ++slot)
        draw.textures[slot] = draw.textures[0];
    draw.textures.back().bytes   = { 255, 255, 0, 255 };
    draw.samplers.back().address = { 4, 4, 4 };
    draw.samplers.back().border  = { 0, 0, 1, 1 };
    draw.pixelShaderDxil         = compile(
        "Texture2D<float4> t:register(t31); SamplerState s:register(s31,space3);"
        "float4 main():SV_Target0 { return t.SampleLevel(s,float2(2,2),0); }",
        "ps_5_1");
    result = renderer.SubmitDrawFrame({ draw }, directory / "last-texture-sampler.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "last-texture-sampler.rgba", { 0, 0, 255, 255 }, { 0, 0, 255, 255 });
    draw.samplers.back().address = { 3, 3, 3 };
    result                       = renderer.SubmitDrawFrame({ draw }, directory / "last-texture-payload.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "last-texture-payload.rgba", { 255, 255, 0, 255 }, { 255, 255, 0, 255 });
    renderer.Shutdown();
}

void checkDrawBatching(const std::filesystem::path& directory)
{
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto first             = makeDraw({ 1, 1, 1, 1 }, false);
    first.textureMask      = 1;
    first.textures[0]      = { { 0, 255, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    first.vertexShaderDxil = compile(
        "cbuffer Vertex : register(b0,space4) { float4 vc; };"
        "struct V { float4 p:SV_Position; float4 c:COLOR0; };"
        "V main(float4 p:TEXCOORD0,float4 c:TEXCOORD1) { V v; v.p=p; v.c=c*vc; return v; }",
        "vs_5_1");
    first.pixelShaderDxil = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "cbuffer Pixel : register(b1,space4) { float4 uv; float4 pc; };"
        "cbuffer Shared : register(b2,space4) { float4 sc; };"
        "float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target0 { return t.Sample(s,uv.xy)*c+pc+sc; }",
        "ps_5_1");
    const auto put = [](std::vector<std::uint8_t>& bytes, std::size_t index, const std::array<float, 4>& value)
    {
        std::memcpy(bytes.data() + index * 16, value.data(), 16);
    };
    put(first.vertexConstants, 0, { 1, 1, 1, 1 });
    put(first.pixelConstants, 0, { 0.5F, 0.5F, 0, 0 });
    put(first.pixelConstants, 1, { 1, 0, 0, 0 });
    put(first.sharedConstants, 0, { 0, 0, 1, 0 });
    auto second            = first;
    second.scissor.right   = 640;
    second.sampler.address = { 4, 4, 4 };
    second.sampler.border  = { 0, 0, 1, 1 };
    put(second.vertexConstants, 0, { 1, 0, 1, 1 });
    put(second.pixelConstants, 0, { 2, 2, 0, 0 });
    put(second.sharedConstants, 0, { 0, 0, 0, 0 });
    auto result = renderer.SubmitDrawFrame({ first, second }, directory / "batch-isolation.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue == 1));
    checkImage(directory / "batch-isolation.rgba", { 255, 0, 255, 255 }, { 255, 255, 255, 255 });
    const auto initial = renderer.ReplayCounts();
    UNSCOPED_INFO("two recorded draws must use one submission");
    REQUIRE((initial.recordedDraws == 2 && initial.queueSubmissions == 1));
    auto third         = makeDraw({ 0, 1, 0, 1 }, false);
    third.scissor.left = 640;
    result             = renderer.SubmitDrawFrame({ first, second, third }, directory / "batch-odd-tail.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue == 3));
    checkImage(directory / "batch-odd-tail.rgba", { 255, 0, 255, 255 }, { 0, 255, 0, 255 });
    first.textures[0].bytes = { 255, 0, 0, 255 };
    put(first.pixelConstants, 1, { 0, 0, 0, 0 });
    put(first.sharedConstants, 0, { 0, 0, 0, 0 });
    second.sampler.address = { 3, 3, 3 };
    put(second.vertexConstants, 0, { 1, 1, 1, 1 });
    put(second.pixelConstants, 1, { 0, 0, 0, 0 });
    result = renderer.SubmitDrawFrame({ first, second }, directory / "batch-refresh.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue == 4));
    checkImage(directory / "batch-refresh.rgba", { 0, 255, 0, 255 }, { 255, 0, 0, 255 });
    UNSCOPED_INFO("later batches must refresh two retained draw slots without new allocation");
    REQUIRE((renderer.ReplayCacheStats().drawUploadCreations == 10 &&
             renderer.ReplayCacheStats().textureCreations == 2));
    renderer.Shutdown();

    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto oldest = makeDraw({ 1, 0, 0, 1 }, false);
    UNSCOPED_INFO("first cache entry must complete");
    REQUIRE(renderer.SubmitDrawFrame({ oldest }).success);
    for (unsigned index = 1; index < 32; ++index)
    {
        auto              variant = oldest;
        const std::string shader  = "float4 main():SV_Target0 { return float4(" + std::to_string(index) + ".0/32.0,0,0,1); }";
        variant.pixelShaderDxil   = compile(shader.c_str(), "ps_5_1");
        UNSCOPED_INFO("pipeline cache warmup must complete");
        REQUIRE(renderer.SubmitDrawFrame({ variant }).success);
    }
    auto newest            = makeDraw({ 0, 1, 0, 1 }, true);
    newest.pixelShaderDxil = compile("float4 main():SV_Target0 { return float4(0,1,0,1); }", "ps_5_1");
    result                 = renderer.SubmitDrawFrame({ oldest, newest }, directory / "batch-pipeline-eviction.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "batch-pipeline-eviction.rgba", { 0, 255, 0, 255 }, { 255, 0, 0, 255 });
    const auto beforeFailure = renderer.ReplayCounts();
    auto       invalid       = oldest;
    invalid.vertexShaderDxil.assign(4, 0);
    UNSCOPED_INFO("failure in a later batch draw must fail the frame");
    REQUIRE(!renderer.SubmitDrawFrame({ oldest, invalid }).success);
    UNSCOPED_INFO("an incomplete batch must not be submitted");
    REQUIRE(renderer.ReplayCounts().queueSubmissions == beforeFailure.queueSubmissions);
    UNSCOPED_INFO("failed batch must make the stream terminal");
    REQUIRE(!renderer.SubmitDrawFrame({ oldest }).success);
    renderer.Shutdown();
}

void checkAttachmentReuse(const std::filesystem::path& directory)
{
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto       red    = makeDraw({ 1, 0, 0, 1 }, false);
    auto       blue   = makeDraw({ 0, 0, 1, 1 }, true);
    const auto render = [&](std::vector<NativeDrawReplayRecipe> draws, const char* name, const std::array<std::uint8_t, 4>& left, const std::array<std::uint8_t, 4>& right)
    {
        const auto result = renderer.SubmitDrawFrame(std::move(draws), directory / name);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        checkImage(directory / name, left, right);
    };
    UNSCOPED_INFO("attachment warmup without readback must complete");
    REQUIRE(renderer.SubmitDrawFrame({ red }).success);
    blue.clearColor = { 0, 0, 0, 255 };
    render({ blue }, "attachment-clear.rgba", { 0, 0, 255, 255 }, { 0, 0, 0, 255 });
    blue.clearColor = {};
    render({ blue }, "attachment-clear-change.rgba", { 0, 0, 255, 255 }, { 0, 0, 0, 0 });
    UNSCOPED_INFO("changed clears and output requests must reuse all four color targets");
    REQUIRE((renderer.ReplayCacheStats().colorTargetCreations == 4 &&
             renderer.ReplayCacheStats().colorTargetReuses == 8));

    const auto setDepth = [](NativeDrawReplayRecipe& draw, float value)
    {
        draw.depth.enabled      = true;
        draw.depth.writeEnabled = true;
        draw.depth.compare      = 2;
        for (std::size_t i = 0; i < draw.vertexData.size(); i += draw.vertexStrideBytes)
        {
            std::memcpy(draw.vertexData.data() + i + 8, &value, sizeof(value));
        }
    };
    setDepth(red, 0.5F);
    setDepth(blue, 0.75F);
#if REREVVED_DEVELOPER
    const auto depthBefore       = renderer.ReplayCacheStats();
    const auto depthBeforeTiming = renderer.ReplayTimingStats();
#endif
    render({ red, blue }, "attachment-depth-preserve.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    UNSCOPED_INFO("depth must be allocated and initialized only once within a frame");
    REQUIRE((renderer.ReplayCacheStats().depthTargetCreations == 1 &&
             renderer.ReplayCacheStats().depthTargetReuses == 0));
#if REREVVED_DEVELOPER
    // The one depth target missed the pool at the replay depth site and timed
    // its creation call.
    UNSCOPED_INFO("the depth target's pool miss times one creation call");
    REQUIRE((renderer.ReplayCacheStats().attachmentSiteMisses[3] == depthBefore.attachmentSiteMisses[3] + 1 &&
             renderer.ReplayTimingStats().attachmentCreations == depthBeforeTiming.attachmentCreations + 1));
#endif
    render({ blue }, "attachment-depth-new-frame.rgba", { 0, 0, 255, 255 }, { 0, 0, 0, 0 });
    UNSCOPED_INFO("depth frame without readback must complete");
    REQUIRE(renderer.SubmitDrawFrame({ red }).success);
    render({ makeDraw({ 0, 1, 0, 1 }, false) }, "attachment-depth-dormant.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    render({ makeDraw({ 1, 0, 0, 1 }, false), blue }, "attachment-depth-late-use.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });

    render({ red, makeDraw({ 0, 1, 0, 1 }, true) }, "attachment-depth-disabled-last.rgba", { 0, 255, 0, 255 }, { 255, 0, 0, 255 });
    render({ blue }, "attachment-depth-after-disabled-last.rgba", { 0, 0, 255, 255 }, { 0, 0, 0, 0 });
    const auto reused = renderer.ReplayCacheStats();
    UNSCOPED_INFO("all compatible frames must retain color and dormant depth allocations");
    REQUIRE((reused.colorTargetCreations == 4 && reused.colorTargetReuses == 36 &&
             reused.depthTargetCreations == 1 && reused.depthTargetReuses == 5));
    auto pending = std::async(std::launch::async, [&]()
                              {
                                  return renderer.SubmitDrawFrame({ red }, directory / "attachment-shutdown.rgba");
                              });
    renderer.Shutdown();
    UNSCOPED_INFO("attachment shutdown must resolve the submitter");
    REQUIRE(pending.wait_for(kReadyCeiling) == std::future_status::ready);
    const auto stopped = pending.get();
    UNSCOPED_INFO("successful attachment work must complete before shutdown");
    REQUIRE((!stopped.success || stopped.completionFenceValue));
    if (stopped.success)
    {
        checkImage(directory / "attachment-shutdown.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    }
    const auto beforeRestart = renderer.ReplayCacheStats();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    render({ red }, "attachment-restart.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    UNSCOPED_INFO("restart must allocate fresh attachments on its new device");
    REQUIRE((renderer.ReplayCacheStats().colorTargetCreations == beforeRestart.colorTargetCreations + 4 &&
             renderer.ReplayCacheStats().depthTargetCreations == beforeRestart.depthTargetCreations + 1));
    renderer.Shutdown();
}

void checkTextureReuse(const std::filesystem::path& directory)
{
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto draw            = makeDraw({ 1, 1, 1, 1 }, false);
    draw.textureMask     = 1;
    draw.pixelShaderDxil = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return t.Sample(s,p.xy/float2(1280,720)); }",
        "ps_5_1");
    const auto sampleShader = draw.pixelShaderDxil;
    draw.textures[0]        = { { 255, 0, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    const auto render       = [&](const NativeDrawReplayRecipe& recipe, const char* name, const std::array<std::uint8_t, 4>& left, const std::array<std::uint8_t, 4>& right)
    {
        const auto result = renderer.SubmitDrawFrame({ recipe }, directory / name);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        checkImage(directory / name, left, right);
    };
    render(draw, "texture-first.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    const auto first = renderer.ReplayCacheStats();
    UNSCOPED_INFO("first textured draw must create one texture and upload");
    REQUIRE((first.textureCreations == 1 && first.textureUploadCreations == 1));
    draw.textures[0].bytes = { 0, 255, 0, 255 };
    render(draw, "texture-refresh.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    draw.textures[0].swizzle = { 1, 0, 2, 3 };
    render(draw, "texture-swizzle.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    draw.pixelShaderDxil = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return t.Sample(s,float2(1.5,0.5)); }",
        "ps_5_1");
    draw.sampler.address = { 4, 4, 4 };
    draw.sampler.border  = { 0, 0, 1, 1 };
    render(draw, "texture-sampler.rgba", { 0, 0, 255, 255 }, { 0, 0, 255, 255 });
    const auto refreshed = renderer.ReplayCacheStats();
    UNSCOPED_INFO("fresh pixels, swizzle and sampler must reuse compatible resources");
    REQUIRE((refreshed.textureCreations == 1 && refreshed.textureUploadCreations == 1 &&
             refreshed.textureReuses == 3 && refreshed.textureUploadReuses == 3));
    draw.pixelShaderDxil = sampleShader;
    draw.sampler         = {};
    draw.textures[0]     = { { 255, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0, 255, 0, 0, 255, 255 }, 2, 2, NativeDrawReplayTextureFormat::Rgba8 };
    render(draw, "texture-grow.rgba", { 255, 0, 0, 255 }, { 0, 0, 255, 255 });
    const auto grown = renderer.ReplayCacheStats();
    UNSCOPED_INFO("larger extent must recreate the texture; its 64 KiB upload class already holds the padded footprint");
    REQUIRE((grown.textureCreations == 2 && grown.textureUploadCreations == 1 && grown.textureUploadReuses == refreshed.textureUploadReuses + 1));
    draw.textures[0] = { { 0, 255, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    render(draw, "texture-shrink.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    UNSCOPED_INFO("smaller extent must retain sufficient upload capacity");
    REQUIRE((renderer.ReplayCacheStats().textureCreations == 3 &&
             renderer.ReplayCacheStats().textureUploadCreations == 1));
    draw.textures[0] = { { 0, 255, 0, 255 }, 2, 2, NativeDrawReplayTextureFormat::R8 };
    render(draw, "texture-r8.rgba", { 0, 0, 0, 255 }, { 255, 0, 0, 255 });
    draw.textures[0] = { { 255, 255, 255, 255, 0, 255, 0, 255, 255, 255, 255, 255, 0, 255, 0, 255 }, 2, 2, NativeDrawReplayTextureFormat::Rgba8 };
    render(draw, "texture-format.rgba", { 255, 255, 255, 255 }, { 0, 255, 0, 255 });
    UNSCOPED_INFO("format changes at equal dimensions must recreate textures and reuse upload capacity");
    REQUIRE((renderer.ReplayCacheStats().textureCreations == 5 &&
             renderer.ReplayCacheStats().textureUploadCreations == 1));
    draw.textures[0] = { { 0, 248, 0, 0, 0, 0, 0, 0 }, 4, 4, NativeDrawReplayTextureFormat::Bc1 };
    render(draw, "texture-bc1.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    draw.textures[0].bytes = { 31, 0, 0, 0, 0, 0, 0, 0 };
    render(draw, "texture-bc1-refresh.rgba", { 0, 0, 255, 255 }, { 0, 0, 255, 255 });
    draw.textures[0] = { { 255, 255, 255, 255, 255, 255, 255, 255, 224, 7, 0, 0, 0, 0, 0, 0 }, 4, 4, NativeDrawReplayTextureFormat::Bc2 };
    render(draw, "texture-bc2.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    const auto compressed = renderer.ReplayCacheStats();
    UNSCOPED_INFO("compressed format changes and refreshes must preserve upload capacity");
    REQUIRE((compressed.textureCreations == 7 && compressed.textureUploadCreations == 1));

    auto three            = draw;
    three.textureMask     = 7;
    three.pixelShaderDxil = compile(
        "Texture2D<float4> a:register(t0); Texture2D<float4> b:register(t1); Texture2D<float4> c:register(t2);"
        "SamplerState s:register(s0,space3); float4 main(float4 p:SV_Position):SV_Target0 { "
        "float2 uv=p.xy/float2(1280,720); return float4(a.Sample(s,uv).r,b.Sample(s,uv).r,c.Sample(s,uv).r,1); }",
        "ps_5_1");
    three.textures[0] = { { 255, 255, 255, 255 }, 2, 2, NativeDrawReplayTextureFormat::R8 };
    three.textures[1] = { { 0, 0, 0, 0 }, 2, 2, NativeDrawReplayTextureFormat::R8 };
    three.textures[2] = three.textures[0];
    render(three, "texture-three.rgba", { 255, 0, 255, 255 }, { 255, 0, 255, 255 });
    const auto threeCreated = renderer.ReplayCacheStats();
    UNSCOPED_INFO("three slots must retain separate texture and upload resources");
    REQUIRE((threeCreated.textureCreations == compressed.textureCreations + 3 &&
             threeCreated.textureUploadCreations == compressed.textureUploadCreations + 2));
    auto one            = three;
    one.textureMask     = 1;
    one.textures[1]     = {};
    one.textures[2]     = {};
    one.pixelShaderDxil = sampleShader;
    render(one, "texture-one-again.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    UNSCOPED_INFO("untextured draw between textured draws must succeed");
    REQUIRE(renderer.SubmitDrawFrame({ makeDraw({ 1, 0, 0, 1 }, false) }).success);
    three.textures[0].bytes.assign(4, 0);
    three.textures[1].bytes.assign(4, 255);
    three.textures[2].bytes.assign(4, 0);
    render(three, "texture-reactivated.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    const auto reactivated = renderer.ReplayCacheStats();
    UNSCOPED_INFO("reactivated slots must refresh retained resources after one and zero texture draws");
    REQUIRE((reactivated.textureCreations == threeCreated.textureCreations &&
             reactivated.textureUploadCreations == threeCreated.textureUploadCreations &&
             reactivated.textureReuses == threeCreated.textureReuses + 4 &&
             reactivated.textureUploadReuses == threeCreated.textureUploadReuses + 4));
    auto pending = std::async(std::launch::async, [&]()
                              {
                                  return renderer.SubmitDrawFrame({ three }, directory / "texture-shutdown.rgba");
                              });
    renderer.Shutdown();
    UNSCOPED_INFO("shutdown must resolve a concurrent textured submitter");
    REQUIRE(pending.wait_for(kReadyCeiling) == std::future_status::ready);
    const auto stopped = pending.get();
    if (stopped.success)
    {
        UNSCOPED_INFO("accepted textured work must finish before shutdown");
        REQUIRE(stopped.completionFenceValue != 0);
        checkImage(directory / "texture-shutdown.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    }
    const auto beforeRestart = renderer.ReplayCacheStats();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    render(three, "texture-restart.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    const auto restarted = renderer.ReplayCacheStats();
    UNSCOPED_INFO("restart must recreate all texture and upload slots on the fresh device");
    REQUIRE((restarted.textureCreations == beforeRestart.textureCreations + 3 &&
             restarted.textureUploadCreations == beforeRestart.textureUploadCreations + 3));
    renderer.Shutdown();
}

void checkMipCubeAndFormats(const std::filesystem::path& directory)
{
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));

    auto draw            = makeDraw({ 1, 1, 1, 1 }, false);
    draw.textureMask     = 1;
    draw.pixelShaderDxil = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { return t.SampleLevel(s,float2(0.5,0.5),2); }",
        "ps_5_1");
    draw.textures[0].width     = 4;
    draw.textures[0].height    = 4;
    draw.textures[0].mipLevels = 3;
    draw.sampler.maxLod        = 2.0F;
    draw.textures[0].bytes.resize(64 + 16 + 4);
    std::fill(draw.textures[0].bytes.begin(), draw.textures[0].bytes.begin() + 64, 0);
    std::fill(draw.textures[0].bytes.begin() + 64, draw.textures[0].bytes.begin() + 80, 0);
    std::fill(draw.textures[0].bytes.begin() + 80, draw.textures[0].bytes.end(), 255);
    auto result = renderer.SubmitDrawFrame({ draw }, directory / "texture-mip-level.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "texture-mip-level.rgba", { 255, 255, 255, 255 }, { 255, 255, 255, 255 });

    auto vertexSample             = makeDraw({ 1, 1, 1, 1 }, false);
    vertexSample.textureMask      = 1;
    vertexSample.vertexShaderDxil = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "struct V { float4 p:SV_Position; float4 c:COLOR0; };"
        "V main(float4 p:TEXCOORD0) { V v; v.p=p; v.c=t.SampleLevel(s,float2(0.5,0.5),0); return v; }",
        "vs_5_1");
    vertexSample.pixelShaderDxil = compile(
        "float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target0 { return c; }", "ps_5_1");
    vertexSample.textures[0] = { { 0, 255, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    auto vertexResult        = renderer.SubmitDrawFrame({ vertexSample }, directory / "texture-vertex-stage.rgba");
    UNSCOPED_INFO(vertexResult.error);
    REQUIRE(vertexResult.success);
    checkImage(directory / "texture-vertex-stage.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });

    auto shared            = makeDraw({ 1, 1, 1, 1 }, false);
    shared.textureMask     = 1;
    shared.pixelShaderDxil = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { return t.SampleLevel(s,float2(0.5,0.5),0); }",
        "ps_5_1");
    std::vector<std::uint8_t> sharedBytes(64);
    for (std::size_t pixel = 0; pixel < 16; ++pixel)
        std::memcpy(sharedBytes.data() + pixel * 4, std::array<std::uint8_t, 4>{ 0, 255, 255, 255 }.data(), 4);
    shared.textures[0].width = shared.textures[0].height = 4;
    shared.textures[0].sharedBytes =
        std::make_shared<const std::vector<std::uint8_t>>(std::move(sharedBytes));
    auto sharedResult = renderer.SubmitDrawFrame({ shared }, directory / "texture-shared-payload.rgba");
    UNSCOPED_INFO(sharedResult.error);
    REQUIRE(sharedResult.success);
    checkImage(directory / "texture-shared-payload.rgba", { 0, 255, 255, 255 }, { 0, 255, 255, 255 });

    auto cube            = makeDraw({ 1, 1, 1, 1 }, false);
    cube.textureMask     = 1;
    cube.pixelShaderDxil = compile(
        "TextureCube<float4> t:register(t0,space2); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { float4 first=t.SampleLevel(s,float3(1,0,0),0);"
        "float4 last=t.SampleLevel(s,float3(0,0,-1),0); return float4(first.r,0,last.b,1); }",
        "ps_5_1");
    cube.textures[0].width     = 4;
    cube.textures[0].height    = 4;
    cube.textures[0].mipLevels = 2;
    cube.textures[0].dimension = NativeDrawReplayTextureDimension::TextureCube;
    cube.sampler.maxLod        = 1.0F;
    cube.textures[0].bytes.resize((64 + 16) * 6);
    std::size_t cubeOffset = 0;
    for (std::size_t face = 0; face < 6; ++face)
    {
        for (std::size_t mip = 0; mip < 2; ++mip)
        {
            const auto bytes = mip == 0 ? 64U : 16U;
            const auto color = face == 0   ? std::array<std::uint8_t, 4>{ 255, 0, 0, 255 }
                               : face == 5 ? std::array<std::uint8_t, 4>{ 0, 0, 255, 255 }
                                           : std::array<std::uint8_t, 4>{ 0, 0, 0, 255 };
            for (std::size_t pixel = 0; pixel < bytes / 4; ++pixel)
                std::memcpy(cube.textures[0].bytes.data() + cubeOffset + pixel * 4, color.data(), 4);
            cubeOffset += bytes;
        }
    }
    result = renderer.SubmitDrawFrame({ cube }, directory / "texture-cube-faces.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "texture-cube-faces.rgba", { 255, 0, 255, 255 }, { 255, 0, 255, 255 });

    auto mixed            = makeDraw({ 1, 1, 1, 1 }, false);
    mixed.textureMask     = 3;
    mixed.pixelShaderDxil = compile(
        "Texture2D<float4> a:register(t0); TextureCube<float4> c:register(t1,space2);"
        "SamplerState s[2]:register(s0,space3); float4 main():SV_Target0 {"
        "float4 first=a.SampleLevel(s[0],float2(0.5,0.5),0);"
        "float4 last=c.SampleLevel(s[1],float3(0,0,-1),0); return float4(first.r,0,last.b,1); }",
        "ps_5_1");
    mixed.textures[0] = { { 255, 0, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    mixed.textures[1] = cube.textures[0];
    mixed.samplers.resize(2);
    mixed.samplers[1].maxLod = 1.0F;
    result                   = renderer.SubmitDrawFrame({ mixed }, directory / "texture-mixed-spaces.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "texture-mixed-spaces.rgba", { 255, 0, 255, 255 }, { 255, 0, 255, 255 });

    auto format            = makeDraw({ 1, 1, 1, 1 }, false);
    format.textureMask     = 1;
    format.pixelShaderDxil = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { return t.SampleLevel(s,float2(0.5,0.5),0); }",
        "ps_5_1");
    format.textures[0].width = format.textures[0].height = 4;
    format.textures[0].format                            = NativeDrawReplayTextureFormat::Bc3;
    format.textures[0].bytes                             = { 255, 255, 0, 0, 0, 0, 0, 0, 0, 248, 0, 0, 0, 0, 0, 0 };
    result                                               = renderer.SubmitDrawFrame({ format }, directory / "texture-bc3.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "texture-bc3.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });

    format.textures[0].format = NativeDrawReplayTextureFormat::Rgba16Float;
    format.textures[0].bytes.resize(4 * 4 * 8);
    for (std::size_t pixel = 0; pixel < 16; ++pixel)
    {
        const auto offset                    = pixel * 8;
        format.textures[0].bytes[offset + 0] = 0;
        format.textures[0].bytes[offset + 1] = 0x3C;
        format.textures[0].bytes[offset + 6] = 0;
        format.textures[0].bytes[offset + 7] = 0x3C;
    }
    result = renderer.SubmitDrawFrame({ format }, directory / "texture-rgba16f.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "texture-rgba16f.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });

    format.pixelShaderDxil = compile(
        "Texture2D<float2> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { float2 value=t.SampleLevel(s,float2(0.5,0.5),0);"
        "return all(value==float2(1,0.5))?float4(0,1,0,1):float4(1,0,0,1); }",
        "ps_5_1");
    format.textures[0].format = NativeDrawReplayTextureFormat::Rg16Float;
    format.textures[0].bytes.resize(4 * 4 * 4);
    for (std::size_t pixel = 0; pixel < 16; ++pixel)
    {
        const auto offset                    = pixel * 4;
        format.textures[0].bytes[offset + 0] = 0;
        format.textures[0].bytes[offset + 1] = 0x3C;
        format.textures[0].bytes[offset + 2] = 0;
        format.textures[0].bytes[offset + 3] = 0x38;
    }
    result = renderer.SubmitDrawFrame({ format }, directory / "texture-rg16f.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "texture-rg16f.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });

    format.pixelShaderDxil = compile(
        "Texture2D<float> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { float value=t.SampleLevel(s,float2(0.5,0.5),0);"
        "return value==0.5?float4(0,1,0,1):float4(1,0,0,1); }",
        "ps_5_1");
    format.textures[0].format = NativeDrawReplayTextureFormat::R32Float;
    format.textures[0].bytes.resize(4 * 4 * 4);
    const float half = 0.5F;
    for (std::size_t pixel = 0; pixel < 16; ++pixel)
        std::memcpy(format.textures[0].bytes.data() + pixel * 4, &half, sizeof(half));
    result = renderer.SubmitDrawFrame({ format }, directory / "texture-r32f.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "texture-r32f.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    renderer.Shutdown();
}

std::uint32_t packGammaEntry(std::uint32_t red, std::uint32_t green, std::uint32_t blue)
{
    return (red << 20) | (green << 10) | blue;
}

std::uint32_t packRgb10Output(std::uint32_t red, std::uint32_t green, std::uint32_t blue)
{
    return (3U << 30) | (blue << 20) | (green << 10) | red;
}

std::array<std::uint32_t, 256> makeGammaTable(std::uint32_t zeroRed,
                                              std::uint32_t zeroGreen,
                                              std::uint32_t zeroBlue,
                                              std::uint32_t fullRed,
                                              std::uint32_t fullGreen,
                                              std::uint32_t fullBlue)
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index)
        table[index] = packGammaEntry(index, (index * 3U) & 1023U, (index * 5U) & 1023U);
    table[0]   = packGammaEntry(zeroRed, zeroGreen, zeroBlue);
    table[255] = packGammaEntry(fullRed, fullGreen, fullBlue);
    return table;
}

void checkGammaPixels(std::span<const std::uint8_t> bytes,
                      std::uint32_t                 left,
                      std::uint32_t                 right)
{
    UNSCOPED_INFO("complete RGB10 output required");
    REQUIRE(bytes.size() == 1280U * 720U * 4U);
    for (std::size_t y = 0; y < 720; ++y)
        for (std::size_t x = 0; x < 1280; ++x)
        {
            const auto    offset   = (y * 1280 + x) * 4;
            const auto    expected = x < 640 ? left : right;
            std::uint32_t actual   = 0;
            std::memcpy(&actual, bytes.data() + offset, sizeof(actual));
            UNSCOPED_INFO("gamma output channels or preservation differ from expected values");
            REQUIRE(actual == expected);
        }
}

void checkGammaImage(const std::filesystem::path& path,
                     std::uint32_t                left,
                     std::uint32_t                right)
{
    std::ifstream                   file(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file), {} };
    checkGammaPixels(bytes, left, right);
}

void checkSize(const std::filesystem::path& path, std::uintmax_t expected)
{
    std::error_code error;
    UNSCOPED_INFO("output resource variant has an unexpected byte count");
    REQUIRE((std::filesystem::file_size(path, error) == expected && !error));
}

void checkSingleSample(const std::filesystem::path& directory)
{
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto red        = makeDraw({ 1, 0, 0, 1 }, false);
    auto blue       = makeDraw({ 0, 0, 1, 1 }, true);
    red.sampleCount = red.sampleMask = blue.sampleCount = blue.sampleMask = 1;
    red.depth.enabled = red.depth.writeEnabled = true;
    blue.depth.enabled = blue.depth.writeEnabled = true;
    blue.depth.compare                           = 4;
    auto msaa                                    = makeDraw({ 0, 1, 0, 1 }, false);
    UNSCOPED_INFO("one pass cannot preserve attachments across different sample counts");
    REQUIRE(!renderer.SubmitDrawFrame({ red, msaa }).success);
    NativeDrawStreamOutput output;
    output.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    output.returnResolvedOutput = true;
    auto result                 = renderer.SubmitDrawFrame({ red, blue }, directory / "single-sample.rgba", output);
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue));
    checkImage(directory / "single-sample.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 31, 919), packRgb10Output(311, 31, 47));
    // Switching sample counts between frames must replace attachments and views.
    result = renderer.SubmitDrawFrame({ msaa }, directory / "single-to-msaa.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "single-to-msaa.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    result = renderer.SubmitDrawFrame({ red }, directory / "msaa-to-single.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "msaa-to-single.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    renderer.Shutdown();

    // Exercise the single-sample initial-color/depth and depth-read shaders.
    // A rejected draw must leave both restored values intact.
    constexpr std::size_t planeBytes = 1280U * 720U * 4U;
    red.initialSample0.resize(planeBytes);
    red.depth.initialSamples.resize(planeBytes * 2);
    for (std::size_t offset = 0; offset < planeBytes; offset += 4)
    {
        red.initialSample0[offset + 1] = red.initialSample0[offset + 3] = 255;
        red.depth.initialSamples[offset + 3] = red.depth.initialSamples[planeBytes + offset + 3] = 128;
    }
    red.initialSample1 = red.initialSample0;
    red.depth.compare  = 1;
    red.outputPath     = directory / "single-sample-initialized.rgba";
    NativeRendererD3D12 initialized;
    result = initialized.ReplayOffscreen(red);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    std::ifstream                   file(red.outputPath, std::ios::binary);
    const std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file), {} };
    UNSCOPED_INFO("single-sample replay must return color and depth planes");
    REQUIRE(bytes.size() == planeBytes * 4);
    UNSCOPED_INFO("single-sample initialization, depth rejection or packed depth readback differs");
    REQUIRE((std::memcmp(bytes.data(), red.initialSample0.data(), planeBytes) == 0 &&
             std::memcmp(bytes.data() + planeBytes, red.initialSample1.data(), planeBytes) == 0 &&
             std::memcmp(bytes.data() + 2 * planeBytes, red.depth.initialSamples.data(), 2 * planeBytes) == 0));
    initialized.Shutdown();
}

void checkDepthClip(const std::filesystem::path& directory)
{
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto red  = makeDraw({ 1, 0, 0, 1 }, false);
    auto       blue = makeDraw({ 0, 0, 1, 1 }, true);
    // Z beyond W lies outside the depth clip planes; depth testing stays off.
    const float z = 2.0F;
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
        std::memcpy(blue.vertexData.data() + vertex * 32 + 8, &z, sizeof(z));
    auto result = renderer.SubmitDrawFrame({ red, blue }, directory / "depth-clip.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "depth-clip.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    blue.rasterizer.depthClip = false;
    result                    = renderer.SubmitDrawFrame({ red, blue }, directory / "depth-clamp.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "depth-clamp.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
    renderer.Shutdown();
}

void checkSignedTarget(const std::filesystem::path& directory)
{
    auto draw  = makeDraw({ 0.25F, -0.5F, 0, 1 }, false);
    draw.width = draw.height = 4;
    draw.viewport            = { 0, 0, 4, 4, 0, 1 };
    draw.scissor             = { 0, 0, 4, 4 };
    draw.sampleCount = draw.sampleMask = 1;
    draw.targetFormat                  = NativeDrawReplayTargetFormat::Rg16Snorm;
    draw.outputPath                    = directory / "signed-target.rg16";
    draw.blend.enabled                 = true;
    draw.blend.destinationColor        = NativeDrawReplayBlendFactor::One;
    NativeRendererD3D12 renderer;
    const auto          result = renderer.ReplayOffscreen(draw);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    std::ifstream                   file(draw.outputPath, std::ios::binary);
    const std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file), {} };
    UNSCOPED_INFO("signed target must return two complete sample planes");
    REQUIRE(bytes.size() == 4U * 4U * 4U * 2U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4)
    {
        std::int16_t red = 0, green = 0;
        std::memcpy(&red, bytes.data() + offset, 2);
        std::memcpy(&green, bytes.data() + offset + 2, 2);
        UNSCOPED_INFO("signed target lost negative values or exceeded one fixed-point unit");
        REQUIRE((std::abs(double(red) / 32767.0 - 0.25) <= 1.0 / 32767.0 &&
                 std::abs(double(green) / 32767.0 + 0.5) <= 1.0 / 32767.0));
    }
    renderer.Shutdown();
}

NativeDrawReplayTexture frameTexture(std::uint64_t id, NativeDrawReplayTextureFormat format)
{
    NativeDrawReplayTexture texture;
    texture.width          = 1280;
    texture.height         = 720;
    texture.format         = format;
    texture.frameTextureId = id;
    return texture;
}

// Puts draw `index` in its 8-pixel cell of a region `width` pixels wide from
// `left`, row by row, so more draws than an owner batch holds fit a half target.
void placeInCell(NativeDrawReplayRecipe& draw, std::size_t index, std::int32_t left = 0, std::size_t width = 640)
{
    constexpr std::int32_t cell   = 8;
    const auto             perRow = width / cell;
    const auto             x = left + static_cast<std::int32_t>(index % perRow) * cell, y = static_cast<std::int32_t>(index / perRow) * cell;
    draw.scissor = { x, y, x + cell, y + cell };
}

NativeRenderFrameResolve frameResolve(std::uint64_t id, std::uint32_t width, std::uint32_t destinationX = 0)
{
    NativeRenderFrameResolve resolve;
    resolve.textureId    = id;
    resolve.width        = width;
    resolve.height       = 720;
    resolve.destinationX = destinationX;
    return resolve;
}

void checkPreparedPipelines()
{
    auto base        = makeDraw({ 1, 0, 0, 1 }, false);
    base.sampleCount = base.sampleMask = 1;
    const auto digest                  = [](std::span<const std::uint8_t> bytes)
    {
        return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    };
    // Synthetic guest words exercise the validation producer without retail inputs.
    const std::array<std::uint32_t, 3>                  shaderWords{};
    const std::array<std::uint8_t, sizeof(shaderWords)> guestWordBytes{};
    NativeGameplayDrawView                              view;
    view.vertexMicrocode = view.pixelMicrocode = shaderWords;
    NativeGameplayShaderInterface shader;
    shader.vertexShaderHash      = base.vertexShaderHash;
    shader.pixelShaderHash       = base.pixelShaderHash;
    shader.vertexShaderDxil      = base.vertexShaderDxil;
    shader.pixelShaderDxil       = base.pixelShaderDxil;
    shader.vertexGuestWordDigest = shader.pixelGuestWordDigest = digest(guestWordBytes);
    shader.vertexDxilDigest                                    = digest(shader.vertexShaderDxil);
    shader.pixelDxilDigest                                     = digest(shader.pixelShaderDxil);
    shader.sampleCount                                         = base.sampleCount;
    shader.sampleMask                                          = base.sampleMask;
    NativeGameplayShaderValidationCache firstCache, equalCache, changedCache;
    std::string                         error;
    UNSCOPED_INFO(error);
    REQUIRE(firstCache.Validate(view, shader, error));
    UNSCOPED_INFO(error);
    REQUIRE(equalCache.Validate(view, shader, error));
    UNSCOPED_INFO("separate validation producers must supply distinct genuine shader owners");
    REQUIRE((firstCache.Version() != equalCache.Version() &&
             firstCache.VertexBytecode() != equalCache.VertexBytecode() &&
             firstCache.PixelBytecode() != equalCache.PixelBytecode()));
    const auto admit = [&](NativeDrawReplayRecipe recipe, const NativeGameplayShaderValidationCache& cache)
    {
        recipe.vertexShaderDxil.clear();
        recipe.pixelShaderDxil.clear();
        recipe.sharedVertexShaderDxil = cache.VertexBytecode();
        recipe.sharedPixelShaderDxil  = cache.PixelBytecode();
        recipe.shaderVersion          = cache.Version();
        std::shared_ptr<const NativeDrawReplayAdmission> admitted;
        UNSCOPED_INFO(error);
        REQUIRE(AdmitNativeDrawReplayRecipe(std::move(recipe), admitted, error));
        return admitted;
    };
    const auto original     = admit(base, firstCache);
    const auto equal        = admit(base, equalCache);
    auto       masked       = base;
    masked.blend.writeMask  = 0x0E;
    const auto stateChanged = admit(masked, firstCache);
    shader.pixelShaderDxil  = compile(
        "float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target0 { return c.bgra; }", "ps_5_1");
    UNSCOPED_INFO("accepted shader replacement must contain different executable bytes");
    REQUIRE(shader.pixelShaderDxil != base.pixelShaderDxil);
    shader.pixelDxilDigest = digest(shader.pixelShaderDxil);
    UNSCOPED_INFO(error);
    REQUIRE(changedCache.Validate(view, shader, error));
    const auto shaderChanged = admit(std::move(masked), changedCache);

    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.passes               = { { false, {}, { frameResolve(1, 1280) } } };
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    std::uint64_t fence  = 0;
    const auto    submit = [&](const std::shared_ptr<const NativeDrawReplayAdmission>& admitted, std::uint32_t pixel)
    {
        frame.passes[0].admittedDraws = { admitted };
        const auto result             = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue > fence));
        fence = result.completionFenceValue;
        checkGammaPixels(result.resolvedOutput, pixel, pixel);
        return result.resolvedOutput;
    };
    const auto red       = packRgb10Output(311, 31, 47);
    const auto reference = submit(original, red);
    const auto pipelines = renderer.ReplayCacheStats().pipelineCreations;
    UNSCOPED_INFO("same admitted shader and state owners must reuse the pipeline and exact output");
    REQUIRE((submit(original, red) == reference && renderer.ReplayCacheStats().pipelineCreations == pipelines));
    UNSCOPED_INFO("distinct genuine versions with equal shader bytes must reuse the pipeline and exact output");
    REQUIRE((submit(equal, red) == reference && renderer.ReplayCacheStats().pipelineCreations == pipelines));
    UNSCOPED_INFO("prepared color write-mask changes must create a pipeline and change resolved output");
    REQUIRE((submit(stateChanged, packRgb10Output(17, 31, 47)) != reference &&
             renderer.ReplayCacheStats().pipelineCreations == pipelines + 1));
    UNSCOPED_INFO("accepted shader-byte changes must create a pipeline and change resolved output");
    REQUIRE((submit(shaderChanged, packRgb10Output(17, 31, 919)) != reference &&
             renderer.ReplayCacheStats().pipelineCreations == pipelines + 2));
    renderer.Shutdown();
}

// Waits until the renderer's queue has run everything submitted so far.
void drainNativeQueue(const NativeRendererD3D12& renderer)
{
    using Microsoft::WRL::ComPtr;
    auto*                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence>  fence;
    UNSCOPED_INFO("native queue drain fence");
    REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) && SUCCEEDED(queue->Signal(fence.Get(), 1))));
    const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
    while (fence->GetCompletedValue() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    UNSCOPED_INFO("the native queue drains");
    REQUIRE(fence->GetCompletedValue() == 1);
}

// Reused storage cannot reuse recorded flags; byte changes recheck snapshots.
void checkContentAdoptionOwnership()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    auto left                  = makeDraw({ 1, 1, 1, 1 }, true);
    left.sampleCount = left.sampleMask = 1;
    left.textureMask                   = 1;
    left.textures[0]                   = { {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    left.textures[0].sharedBytes       = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{ 255, 0, 0, 255 });
    left.pixelShaderDxil               = compile(
        "Texture2D<float4> source_texture:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return source_texture.Load(int3(0,0,0)); }",
        "ps_5_1");
    auto right        = left;
    right.scissor     = { 640, 0, 1280, 720 };
    right.textures[0] = { { 255, 0, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    frame.passes      = { { false, { left }, {} }, { true, { right }, {} }, { true, { left }, { frameResolve(1, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto red = packRgb10Output(311, 31, 47);
    // A loaded run once created storage in a settled round; the timing source
    // is not identified. Each recording's GPU work completes before the next
    // request, which rules out queue progress, not other timing.
    std::uint64_t settled = 0;
    for (int round = 0; round < 4; ++round)
    {
        const auto before = renderer.ReplayCacheStats();
        if (round == 2)
            settled = before.textureCreations;
        for (std::size_t index = 0; index < 2; ++index)
        {
            NativeRenderFrame segment;
            segment.passes = { frame.passes[index] };
            std::shared_future<NativeDrawReplayResult> recording;
            UNSCOPED_INFO(error);
            REQUIRE(renderer.QueueRenderFramePasses(std::move(segment), recording, error));
            const auto recorded = recording.get();
            UNSCOPED_INFO(recorded.error);
            REQUIRE(recorded.success);
            drainNativeQueue(renderer);
        }
        const auto segments = renderer.ReplayCacheStats();
        if (!round)
        {
            UNSCOPED_INFO("inline bytes equal to a kept shared owner's take its storage and recheck the bytes");
            REQUIRE((segments.textureCreations == before.textureCreations + 1 && segments.textureContentReuses == before.textureContentReuses + 1 &&
                     segments.texturePayloadReuses == before.texturePayloadReuses + 1));
        }
        const auto result = renderer.SubmitRenderFrame(tailOf(frame, 2));
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        checkGammaPixels(result.resolvedOutput, red, red);
        drainNativeQueue(renderer);
    }
    UNSCOPED_INFO("content adoption keeps its storage across frames");
    REQUIRE(renderer.ReplayCacheStats().textureCreations == settled);
    renderer.Shutdown();
}

#if REREVVED_DEVELOPER
// Preparation starts after first timed draw and stops when disabled.
void checkRecordingPhaseTiming()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    auto draw                  = makeDraw({ 1, 1, 1, 1 }, true);
    draw.sampleCount = draw.sampleMask = 1;
    draw.textureMask                   = 1;
    draw.textures[0]                   = { {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    draw.textures[0].sharedBytes       = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{ 255, 0, 0, 255 });
    draw.pixelShaderDxil               = compile(
        "Texture2D<float4> source_texture:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return source_texture.Load(int3(0,0,0)); }",
        "ps_5_1");
    frame.passes = { { false, { draw }, { frameResolve(1, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto phases = NativeDrawReplayTimingStats::kRecordingPhases;
    const auto upload = static_cast<std::size_t>(std::ranges::find(phases, std::string_view("texture_upload")) - phases.begin());
    const auto submit = [&](bool timing)
    {
        renderer.SetPipelineTiming(timing);
        const auto before = renderer.ReplayTimingStats();
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        return std::pair{ before, renderer.ReplayTimingStats() };
    };
    const auto [before, after] = submit(true);
    std::uint64_t phaseSum     = 0;
    for (std::size_t phase = 0; phase < phases.size(); ++phase)
        phaseSum += after.recordingPhaseNanoseconds[phase] - before.recordingPhaseNanoseconds[phase];
    UNSCOPED_INFO(std::format("timed phases sum to the recording time: {} of {} ns", phaseSum, after.recordingNanoseconds - before.recordingNanoseconds));
    REQUIRE((after.recordedDraws > before.recordedDraws && phaseSum == after.recordingNanoseconds - before.recordingNanoseconds));
    UNSCOPED_INFO("the texture_upload phase is the timed upload span");
    REQUIRE((after.textureUploadNanoseconds > before.textureUploadNanoseconds &&
             after.recordingPhaseNanoseconds[upload] - before.recordingPhaseNanoseconds[upload] == after.textureUploadNanoseconds - before.textureUploadNanoseconds));
    const auto [prepared, timed] = submit(true);
    UNSCOPED_INFO("timed frames time texture preparation");
    REQUIRE(timed.texturePrepareNanoseconds > prepared.texturePrepareNanoseconds);
    // Disabling stops preparation timing at once, before any draw follows it.
    for (int round = 0; round < 2; ++round)
    {
        const auto [untimed, done] = submit(false);
        UNSCOPED_INFO("untimed frames record no phase or preparation time");
        REQUIRE((done.recordingNanoseconds > untimed.recordingNanoseconds && done.recordingPhaseNanoseconds == untimed.recordingPhaseNanoseconds &&
                 done.texturePrepareNanoseconds == untimed.texturePrepareNanoseconds));
    }
    renderer.Shutdown();
}

// Timed draws count resident shared vertex/index bytes; another owner misses.
void checkResidentGeometryBytes()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    auto draw                  = makeDraw({ 1, 0, 0, 1 }, true);
    draw.sharedVertexData      = std::make_shared<const std::vector<std::uint8_t>>(std::move(draw.vertexData));
    draw.vertexData.clear();
    const auto vertexBytes = draw.sharedVertexData->size(), indexBytes = draw.indices.size() * sizeof(std::uint32_t);
    frame.passes = { { false, { draw }, { frameResolve(1, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    renderer.SetPipelineTiming(true);
    const auto resident = [&](const NativeRenderFrame& submitted)
    {
        const auto before = renderer.ReplayTimingStats();
        const auto result = renderer.SubmitRenderFrame(submitted);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        // The worker adds its busy span when it next goes idle.
        const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
        auto       after    = renderer.ReplayTimingStats();
        while (after.streamBusyNanoseconds == before.streamBusyNanoseconds && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            after = renderer.ReplayTimingStats();
        }
        UNSCOPED_INFO("a recorded frame adds stream thread busy time");
        REQUIRE(after.streamBusyNanoseconds > before.streamBusyNanoseconds);
        return std::pair{ after.residentGeometryBytes[0] - before.residentGeometryBytes[0], after.residentGeometryBytes[1] - before.residentGeometryBytes[1] };
    };
    UNSCOPED_INFO("a first vertex owner is not resident");
    REQUIRE((resident(frame) == std::pair<std::uint64_t, std::uint64_t>{ 0, 0 }));
    UNSCOPED_INFO("a repeated vertex owner's geometry is resident");
    REQUIRE((resident(frame) == std::pair<std::uint64_t, std::uint64_t>{ vertexBytes, indexBytes }));
    auto copy                                = frame;
    copy.passes[0].draws[0].sharedVertexData = std::make_shared<const std::vector<std::uint8_t>>(*draw.sharedVertexData);
    UNSCOPED_INFO("equal bytes under another owner are not resident");
    REQUIRE((resident(copy) == std::pair<std::uint64_t, std::uint64_t>{ 0, 0 }));
    renderer.SetPipelineTiming(false);
    UNSCOPED_INFO("untimed frames count no resident geometry");
    REQUIRE((resident(frame) == std::pair<std::uint64_t, std::uint64_t>{ 0, 0 }));
    renderer.Shutdown();
}

// Timed draws count repeats; failures after uploads publish no counts.
void checkRepeatCounters()
{
    NativeRenderFrame base;
    base.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    base.outputTextureId      = 1;
    base.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    base.returnResolvedOutput = true;
    base.passes               = { { false, {}, { frameResolve(1, 1280) } } };
    // 4096 + 3584 + 512 aligned constant bytes, marked per stage. The frame's
    // output draw has zero constants, so it repeats none of these.
    const auto marked = [](std::uint8_t vertex, std::uint8_t pixel, std::uint8_t shared)
    {
        auto draw               = makeDraw({ 1, 0, 0, 1 }, true);
        draw.vertexConstants[0] = vertex;
        draw.pixelConstants[0]  = pixel;
        draw.sharedConstants[0] = shared;
        return draw;
    };
    const auto framed = [&](std::vector<NativeDrawReplayRecipe> draws)
    {
        auto frame            = base;
        frame.passes[0].draws = std::move(draws);
        return frame;
    };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    using Repeats        = std::array<std::uint64_t, 6>;
    std::uint64_t chunks = 0;
    const auto    submit = [&](const NativeRenderFrame& submitted, bool timing, bool succeeds = true)
    {
        renderer.SetPipelineTiming(timing);
        const auto before = renderer.ReplayTimingStats();
        const auto result = renderer.SubmitRenderFrame(submitted);
        UNSCOPED_INFO((succeeds ? result.error : std::string("a frame with a broken draw fails")));
        REQUIRE(result.success == succeeds);
        const auto after = renderer.ReplayTimingStats();
        chunks           = after.chunkSubmissions - before.chunkSubmissions;
        const auto rtv   = after.rtvWrites - before.rtvWrites;
        UNSCOPED_INFO(std::format("each timed draw writes four color target views: {}", rtv));
        REQUIRE((!timing || rtv == 4 * (after.recordedDraws - before.recordedDraws)));
        UNSCOPED_INFO("untimed draws count no view writes");
        REQUIRE((timing || rtv == 0));
        std::uint64_t slots = 0;
        for (std::size_t outcome = 0; outcome < after.textureOutcomes.size(); ++outcome)
            slots += after.textureOutcomes[outcome] - before.textureOutcomes[outcome];
        const auto reuses = after.textureDescriptorReuses - before.textureDescriptorReuses, writes = after.textureSrvWrites - before.textureSrvWrites;
        UNSCOPED_INFO(std::format("each timed texture slot reuses its views or writes two: slots {} reuses {} writes {}", slots, reuses, writes));
        REQUIRE((!timing || (reuses + writes / 2 == slots && writes % 2 == 0)));
        Repeats repeats{};
        for (std::size_t stage = 0; stage < 3; ++stage)
        {
            repeats[stage]     = after.constantRepeatPreviousBytes[stage] - before.constantRepeatPreviousBytes[stage];
            repeats[3 + stage] = after.constantRepeatChunkBytes[stage] - before.constantRepeatChunkBytes[stage];
        }
        return std::pair{ repeats, reuses };
    };
    const auto describe = [](const Repeats& repeats)
    {
        return std::format("previous {} {} {} chunk {} {} {}", repeats[0], repeats[1], repeats[2], repeats[3], repeats[4], repeats[5]);
    };
    // Previous-draw repeats: vertex and shared at the second draw, pixel at
    // the fourth. Chunk repeats add the third draw's pixel block and the
    // fourth draw's vertex and shared blocks.
    const auto    mixed = framed({ marked(1, 1, 1), marked(1, 2, 1), marked(2, 1, 2), marked(1, 1, 1) });
    const Repeats mixedRepeats{ 4096, 3584, 512, 2 * 4096, 2 * 3584, 2 * 512 };
    for (int round = 0; round < 2; ++round)
    {
        const auto repeats = submit(mixed, true).first;
        UNSCOPED_INFO("timed repeats of a frame: " + describe(repeats));
        REQUIRE(repeats == mixedRepeats);
    }
    // Equal draws through one more than a chunk: the draw that opens the
    // second chunk repeats nothing, and the one after it repeats again.
    const auto chunkDraws = static_cast<std::uint64_t>(detail::NativeRenderFrameTestAccess::FrameCommandDraws());
    const auto repeats    = submit(framed(std::vector<NativeDrawReplayRecipe>(chunkDraws + 2, marked(3, 3, 3))), true).first;
    UNSCOPED_INFO(std::format("repeats start over at a chunk within a frame ({} chunk ends): ", chunks) + describe(repeats));
    REQUIRE((chunks >= 1 && repeats == Repeats{ chunkDraws * 4096, chunkDraws * 3584, chunkDraws * 512, chunkDraws * 4096, chunkDraws * 3584, chunkDraws * 512 }));
    UNSCOPED_INFO("untimed frames count no repeats");
    REQUIRE(submit(mixed, false).first == Repeats{});

    auto  textured   = framed({ marked(1, 1, 1) });
    auto& draw       = textured.passes[0].draws[0];
    draw.sampleCount = draw.sampleMask = 1;
    draw.textureMask                   = 1;
    draw.textures[0]                   = { {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    draw.textures[0].sharedBytes       = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{ 255, 0, 0, 255 });
    draw.pixelShaderDxil               = compile(
        "Texture2D<float4> source_texture:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return source_texture.Load(int3(0,0,0)); }",
        "ps_5_1");
    // Owners alternate frames, so a draw slot holds its views two frames on.
    std::uint64_t reuses = 0;
    for (int round = 0; round < 3; ++round)
        reuses += submit(textured, true).second;
    UNSCOPED_INFO(std::format("the textured slot reuses its views two frames on: {}", reuses));
    REQUIRE(reuses == 1);
    // The second draw repeats every block of the first, then fails at
    // pipeline creation, after its uploads. The failure ends the stream.
    auto broken            = marked(4, 4, 4);
    broken.pixelShaderDxil = { 1, 2, 3, 4 };
    const auto failed      = submit(framed({ marked(4, 4, 4), broken }), true, false).first;
    UNSCOPED_INFO("a draw that fails after its uploads publishes no repeats: " + describe(failed));
    REQUIRE(failed == Repeats{});
    renderer.Shutdown();
}

// Entering the next pass without idling still publishes prior-frame busy time.
void checkStreamBusyPublished()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    frame.passes               = { { false, { makeDraw({ 1, 0, 0, 1 }, true) }, { frameResolve(1, 1280) } } };
    constexpr auto      kSpan  = std::chrono::milliseconds(2);
    std::atomic<bool>   blocked{ false };
    std::uint64_t       busyBefore = 0;
    std::promise<void>  entered, release;
    auto                released = release.get_future().share();
    auto                reached  = entered.get_future();
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    renderer.SetPipelineTiming(true);
    const auto recorded = renderer.ReplayCounts().recordedDraws;
    // Before the frame, each pass notes the busy time and works for kSpan;
    // the first pass after it holds the worker until released.
    detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, [&]
                                                       {
                                                           const auto stats = renderer.ReplayTimingStats();
                                                           if (stats.recordedDraws == recorded)
                                                           {
                                                               busyBefore = stats.streamBusyNanoseconds;
                                                               std::this_thread::sleep_for(kSpan);
                                                           }
                                                           else if (!blocked.exchange(true))
                                                           {
                                                               entered.set_value();
                                                               released.wait_for(kReadyCeiling);
                                                           }
                                                       });
    auto       submitted = std::async(std::launch::async, [&]
                                      {
                                    return renderer.SubmitRenderFrame(frame);
                                      });
    const bool held      = reached.wait_for(kReadyCeiling) == std::future_status::ready;
    const auto busy      = renderer.ReplayTimingStats().streamBusyNanoseconds;
    release.set_value();
    const auto result = submitted.get();
    detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, {});
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    UNSCOPED_INFO("the worker reaches its next pass after the frame");
    REQUIRE(held);
    UNSCOPED_INFO(std::format("busy time is published without an idle wait: {} ns", busy - busyBefore));
    REQUIRE(busy - busyBefore >= static_cast<std::uint64_t>(std::chrono::nanoseconds(kSpan).count()));
    renderer.Shutdown();
}

// Reuse stage uploads; draw constants stay current and only changes write.
void checkConstantReuse()
{
    const auto colored = [](const std::array<float, 4>& color, std::int32_t left, std::int32_t right)
    {
        auto draw            = makeDraw({ 1, 1, 1, 1 }, false);
        draw.scissor         = { left, 0, right, 720 };
        draw.pixelShaderDxil = compile(
            "cbuffer Pixel : register(b1,space4) { float4 pc[224]; };"
            "float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target0 { return pc[0]; }",
            "ps_5_1");
        std::memcpy(draw.pixelConstants.data(), color.data(), sizeof(color));
        draw.vertexConstants[0] = draw.sharedConstants[0] = 7;
        return draw;
    };
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    frame.passes               = { { false, { colored({ 0, 0, 1, 1 }, 0, 1280), colored({ 1, 0, 0, 1 }, 0, 640), colored({ 1, 0, 0, 1 }, 640, 1280) }, { frameResolve(1, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    renderer.SetPipelineTiming(true);
    const auto red = packRgb10Output(311, 31, 47);
    for (int round = 0; round < 2; ++round)
    {
        const auto before = renderer.ReplayTimingStats();
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        checkGammaPixels(result.resolvedOutput, red, red);
        const auto written = renderer.ReplayTimingStats().drawUploadBytes[0] - before.drawUploadBytes[0];
        UNSCOPED_INFO(std::format("repeated constant blocks are not written again: {} bytes", written));
        REQUIRE(written == (4096 + 3584 + 512) + 3584 + (4096 + 3584 + 512));
    }

    // Across packets, repeated tail blocks reuse arena state; the chain remains
    // one frame.
    NativeRenderFrame chained  = frame;
    auto              redLeft  = colored({ 1, 0, 0, 1 }, 0, 640);
    redLeft.vertexConstants[0] = redLeft.sharedConstants[0] = 9;
    chained.passes                                          = { { false, { colored({ 0, 0, 1, 1 }, 0, 1280) }, {} }, { true, { colored({ 0, 0, 1, 1 }, 640, 1280), redLeft }, { frameResolve(1, 1280) } } };
    const auto whole                                        = renderer.SubmitRenderFrame(chained);
    UNSCOPED_INFO(whole.error);
    REQUIRE(whole.success);
    checkGammaPixels(whole.resolvedOutput, red, packRgb10Output(17, 31, 919));
    NativeRenderFrame opening;
    opening.textures = chained.textures;
    opening.passes   = { chained.passes[0] };
    std::shared_future<NativeDrawReplayResult> recording;
    UNSCOPED_INFO("a pass opens a chain: " + error);
    REQUIRE(renderer.QueueRenderFramePasses(std::move(opening), recording, error));
    UNSCOPED_INFO("the opening packet records: " + recording.get().error);
    REQUIRE((recording.wait_for(kReadyCeiling) == std::future_status::ready && recording.get().success));
    const auto joined = renderer.SubmitRenderFrame(tailOf(chained, 1));
    UNSCOPED_INFO("a tail binds no constants of its opening packet: " + joined.error);
    REQUIRE((joined.success && joined.resolvedOutput == whole.resolvedOutput));

    // Retained frame draws reuse initial arena blocks; the next frame rewrites
    // the arena from the start.
    NativeRenderFrame retained         = frame;
    retained.textures[0].persistentKey = 9601;
    retained.retainOnly                = true;
    retained.returnResolvedOutput      = false;
    retained.passes                    = { { false, std::vector<NativeDrawReplayRecipe>(4, colored({ 0, 0, 1, 1 }, 0, 1280)), { frameResolve(1, 1280) } } };
    std::shared_ptr<NativeRenderFrameTicket> ticket;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(retained, ticket, error));
    UNSCOPED_INFO("the retained frame completes");
    REQUIRE((ticket->Completion().wait_for(kReadyCeiling) == std::future_status::ready && ticket->Completion().get().success));
    auto next        = frame;
    next.passes      = { { false, { colored({ 0, 0, 1, 1 }, 640, 1280), redLeft }, { frameResolve(1, 1280) } } };
    const auto shown = renderer.SubmitRenderFrame(next);
    UNSCOPED_INFO(shown.error);
    REQUIRE(shown.success);
    checkGammaPixels(shown.resolvedOutput, red, packRgb10Output(17, 31, 919));
    renderer.Shutdown();
}

// Timed uploads count written bytes; constants charge aligned capacity.
void checkDrawUploadBytes()
{
    NativeRenderFrame base;
    base.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    base.outputTextureId      = 1;
    base.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    base.returnResolvedOutput = true;
    // Every block differs from its predecessor's and from the output draw's
    // zero blocks, so each draw writes all of its constants.
    const auto marked = [](const std::array<float, 4>& color, std::uint8_t mark)
    {
        auto draw               = makeDraw(color, true);
        draw.vertexConstants[0] = draw.pixelConstants[0] = draw.sharedConstants[0] = mark;
        return draw;
    };
    const auto first = marked({ 1, 0, 0, 1 }, 1);
    base.passes      = { { false, { first }, { frameResolve(1, 1280) } } };
    // 4096 + 3584 + 512 constant bytes, three vertices and three indices.
    auto second    = marked({ 0, 1, 0, 1 }, 2);
    second.scissor = { 640, 0, 1280, 720 };
    // 4352 + 3840 + 512 constant bytes, four vertices and six indices.
    auto third = marked({ 0, 0, 1, 1 }, 3);
    third.vertexConstants.resize(4112);
    third.pixelConstants.resize(3600);
    third.sharedConstants.resize(400);
    third.vertexData.resize(4 * third.vertexStrideBytes);
    third.indices    = { 0, 1, 2, 2, 1, 0 };
    third.indexCount = 6;
    auto added       = base;
    added.passes     = { { false, { first, second, third }, { frameResolve(1, 1280) } } };

    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    using Bytes        = std::array<std::uint64_t, NativeDrawReplayTimingStats::kDrawUploadKinds.size()>;
    const auto written = [&](const NativeRenderFrame& frame, bool timing)
    {
        renderer.SetPipelineTiming(timing);
        const auto before = renderer.ReplayTimingStats();
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        const auto after = renderer.ReplayTimingStats();
        Bytes      bytes{};
        for (std::size_t kind = 0; kind < bytes.size(); ++kind)
            bytes[kind] = after.drawUploadBytes[kind] - before.drawUploadBytes[kind];
        return bytes;
    };
    const auto baseBytes  = written(base, true);
    const auto addedBytes = written(added, true);
    UNSCOPED_INFO("a timed frame counts its draws' upload bytes");
    REQUIRE((baseBytes[0] > 0 && baseBytes[1] > 0 && baseBytes[2] > 0));
    const Bytes expected{ 4096 + 3584 + 512 + 4352 + 3840 + 512, 3 * 32 + 4 * 32, (3 + 6) * sizeof(std::uint32_t) };
    Bytes       delta{};
    for (std::size_t kind = 0; kind < delta.size(); ++kind)
        delta[kind] = addedBytes[kind] - baseBytes[kind];
    UNSCOPED_INFO(std::format("added draws write their bytes: constants {} vertex {} index {}", delta[0], delta[1], delta[2]));
    REQUIRE(delta == expected);
    UNSCOPED_INFO("untimed frames count no upload bytes");
    REQUIRE(written(added, false) == Bytes{});
    // A draw outside a frame writes standalone buffers; a smaller recipe
    // reuses the wider ones and writes them through, zeroing their tails.
    renderer.SetPipelineTiming(true);
    const auto offscreen = [&](const NativeDrawReplayRecipe& draw)
    {
        const auto before = renderer.ReplayTimingStats();
        const auto result = renderer.SubmitDrawFrame({ draw });
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        return renderer.ReplayTimingStats().drawUploadBytes[0] - before.drawUploadBytes[0];
    };
    const auto wider   = offscreen(third);
    const auto smaller = offscreen(first);
    UNSCOPED_INFO(std::format("reused standalone constants write their capacity: {} then {}", wider, smaller));
    REQUIRE((wider >= 4352 + 3840 + 512 && smaller == wider));
    renderer.Shutdown();
}

// Timed keys count new/changed/uncached/presented uses; untimed slots do not.
void checkTextureKeyOutcomes()
{
    using Outcomes   = std::array<std::uint64_t, NativeDrawReplayTimingStats::kTextureOutcomes.size()>;
    const auto names = NativeDrawReplayTimingStats::kTextureOutcomes;
    const auto index = [&](std::string_view name)
    {
        return static_cast<std::size_t>(std::ranges::find(names, name) - names.begin());
    };
    const auto only = [&](std::string_view name, std::uint64_t frameTextures)
    {
        Outcomes expected{};
        expected[index(name)]    = 1;
        expected[index("frame")] = frameTextures;
        return expected;
    };
    auto              texels = std::make_shared<std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{ 255, 0, 0, 255 });
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    auto draw                  = makeDraw({ 1, 1, 1, 1 }, true);
    draw.sampleCount = draw.sampleMask = 1;
    draw.textureMask                   = 1;
    draw.textures[0]                   = { {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    draw.textures[0].sharedBytes       = texels;
    draw.pixelShaderDxil               = compile(
        "Texture2D<float4> source_texture:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return source_texture.Load(int3(0,0,0)); }",
        "ps_5_1");
    frame.passes = { { false, { draw }, { frameResolve(1, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    renderer.SetPipelineTiming(true);
    Outcomes   bytes{};
    const auto counted = [&](auto&& run)
    {
        const auto before = renderer.ReplayTimingStats();
        run();
        const auto after = renderer.ReplayTimingStats();
        Outcomes   counts{};
        for (std::size_t outcome = 0; outcome < counts.size(); ++outcome)
        {
            counts[outcome] = after.textureOutcomes[outcome] - before.textureOutcomes[outcome];
            bytes[outcome]  = after.textureOutcomeBytes[outcome] - before.textureOutcomeBytes[outcome];
        }
        return counts;
    };
    const auto submitFrame = [&]
    {
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
    };
    const auto describe = [&](const Outcomes& counts)
    {
        std::string text;
        for (std::size_t outcome = 0; outcome < counts.size(); ++outcome)
            text += std::format(" {}={}", names[outcome], counts[outcome]);
        return text;
    };
    // Completed synchronous frames keep one storage for the owner.
    for (int round = 0; round < 3; ++round)
    {
        const auto counts = counted(submitFrame);
        UNSCOPED_INFO(std::format("frame {} outcomes:{}", round, describe(counts)));
        REQUIRE((counts == only(round ? "match" : "new", 1) && (round || bytes[index("new")] == texels->size())));
    }
    (*texels)[1] = 255;
    for (int round = 0; round < 2; ++round)
    {
        const auto counts = counted(submitFrame);
        UNSCOPED_INFO(std::format("changed frame {} outcomes:{}", round, describe(counts)));
        REQUIRE((counts == only(round ? "match" : "changed", 1) && (round || bytes[index("changed")] == texels->size())));
    }
    auto offscreen = draw;
    offscreen.textures[0].sharedBytes.reset();
    offscreen.textures[0].bytes = { 0, 255, 0, 255 };
    const auto uncached         = counted([&]
                                          {
                                      const auto result = renderer.SubmitDrawFrame({ offscreen });
                                      UNSCOPED_INFO(result.error);
                                      REQUIRE(result.success);
                                          });
    UNSCOPED_INFO("a draw outside a frame uploads uncached:" + describe(uncached));
    REQUIRE(uncached == only("uncached", 0));
    renderer.SetPipelineTiming(false);
    UNSCOPED_INFO("untimed frames count no texture outcome");
    REQUIRE(counted(submitFrame) == Outcomes{});
    renderer.Shutdown();
}
#endif

void checkFrameTextureStorageReuse()
{
    std::atomic<std::uint64_t> checkedStorage = 0;
    std::atomic<bool>          invalidStorage = false;
    NativeRenderFrame          frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    auto left                  = makeDraw({ 1, 1, 1, 1 }, true);
    left.sampleCount = left.sampleMask = 1;
    left.textureMask                   = 1;
    left.textures[0]                   = { { 255, 0, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    left.pixelShaderDxil               = compile(
        "Texture2D<float4> source_texture:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return source_texture.Load(int3(0,0,0)); }",
        "ps_5_1");
    auto right              = left;
    right.scissor           = { 640, 0, 1280, 720 };
    right.textures[0].bytes = { 0, 0, 255, 255 };
    frame.passes            = { { false, { left, right }, { frameResolve(1, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, [&]
                                                       {
                                                           try
                                                           {
                                                               checkedStorage.fetch_add(detail::NativeRenderFrameTestAccess::CheckedResourceStorage(renderer), std::memory_order_relaxed);
                                                           }
                                                           catch (...)
                                                           {
                                                               invalidStorage.store(true, std::memory_order_relaxed);
                                                           }
                                                       });
    const auto render = [&](std::uint32_t expectedLeft, std::uint32_t expectedRight)
    {
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        checkGammaPixels(result.resolvedOutput, expectedLeft, expectedRight);
    };
    const auto red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47), blue = packRgb10Output(17, 31, 919);
    render(red, blue);
    const auto first = renderer.ReplayCacheStats();
    UNSCOPED_INFO("distinct CPU textures need independent storage within a frame");
    REQUIRE((first.textureCreations == 2 && first.textureUploadCreations == 2 && first.texturePayloadBytes == 8));
    auto& firstTexture  = frame.passes[0].draws[0].textures[0];
    auto& secondTexture = frame.passes[0].draws[1].textures[0];
    firstTexture.bytes  = { 0, 255, 0, 255 };
    secondTexture.bytes = { 255, 0, 0, 255 };
    render(green, red);
    const auto refreshed = renderer.ReplayCacheStats();
    UNSCOPED_INFO("completed frame storage must refresh pixels without allocating again");
    REQUIRE((refreshed.textureCreations == first.textureCreations && refreshed.textureUploadCreations == first.textureUploadCreations));

    firstTexture.width = firstTexture.height = 2;
    firstTexture.bytes                       = { 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255 };
    render(blue, red);
    UNSCOPED_INFO("changed layout needs new storage without reallocating a compatible texture");
    REQUIRE(renderer.ReplayCacheStats().textureCreations == first.textureCreations + 1);
    firstTexture             = secondTexture;
    firstTexture.sharedBytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(firstTexture.bytes));
    secondTexture            = firstTexture;
    render(red, red);
    UNSCOPED_INFO("shared payloads must reuse one matching allocation and retire unused layouts");
    REQUIRE(renderer.ReplayCacheStats().textureCreations == first.textureCreations + 1);
    const auto shared        = renderer.ReplayCacheStats();
    firstTexture.sharedBytes = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{ 0, 255, 0, 255 });
    secondTexture            = firstTexture;
    render(green, green);
    UNSCOPED_INFO("a new shared payload must replace previous pixels in reused storage");
    REQUIRE((renderer.ReplayCacheStats().textureCreations == shared.textureCreations &&
             renderer.ReplayCacheStats().textureUploadReuses == shared.textureUploadReuses + 1));

    auto mutablePixels       = std::make_shared<std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{ 0, 255, 0, 255 });
    firstTexture.sharedBytes = mutablePixels;
    secondTexture            = firstTexture;
    render(green, green);
    const auto unchanged = renderer.ReplayCacheStats();
    render(green, green);
    const auto reused = renderer.ReplayCacheStats();
    UNSCOPED_INFO("unchanged shared pixels must reuse completed contents without an upload");
    REQUIRE((reused.texturePayloadReuses == unchanged.texturePayloadReuses + 1 &&
             reused.textureUploadReuses == unchanged.textureUploadReuses && reused.texturePayloadBytes == 4));
    *mutablePixels = { 255, 0, 0, 255 };
    render(red, red);
    const auto replaced = renderer.ReplayCacheStats();
    UNSCOPED_INFO("changed bytes behind the same shared pointer must upload the new contents");
    REQUIRE((replaced.texturePayloadReuses == reused.texturePayloadReuses &&
             replaced.textureUploadReuses == reused.textureUploadReuses + 1));
    std::weak_ptr<const std::vector<std::uint8_t>> retired = mutablePixels;
    firstTexture.sharedBytes                               = std::make_shared<const std::vector<std::uint8_t>>(*mutablePixels);
    secondTexture                                          = firstTexture;
    mutablePixels.reset();
    render(red, red);
    UNSCOPED_INFO("content snapshots must not retain the caller's shared owner");
    REQUIRE(retired.expired());
    UNSCOPED_INFO("identical bytes from a new owner must match the private completed snapshot");
    REQUIRE(renderer.ReplayCacheStats().texturePayloadReuses == replaced.texturePayloadReuses + 1);

    // This replacement crosses the allocation-size boundary of the tiny textures.
    firstTexture = { {}, 256, 256, NativeDrawReplayTextureFormat::Rgba8 };
    firstTexture.bytes.resize(256 * 256 * 4);
    for (std::size_t index = 0; index < firstTexture.bytes.size(); index += 4)
    {
        firstTexture.bytes[index]     = 255;
        firstTexture.bytes[index + 3] = 255;
    }
    secondTexture = firstTexture;
    render(red, red);
    render(red, red);

    // Narrow images fit the CPU byte bound but their row padding can exceed
    // the GPU footprint bound. Reject the whole set before any new allocation.
    auto narrow               = left;
    narrow.textures[0].height = 4096;
    narrow.textures[0].bytes.resize(4096 * 4);
    frame.passes[0].draws.assign(513, narrow);
    const auto beforeBound = renderer.ReplayCacheStats();
    const auto bounded     = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("padded CPU texture storage must preserve the frame footprint bound");
    REQUIRE((!bounded.success && bounded.error.find("footprint bound") != std::string::npos));
    UNSCOPED_INFO("footprint rejection must precede texture allocation");
    REQUIRE((renderer.ReplayCacheStats().textureCreations == beforeBound.textureCreations &&
             renderer.ReplayCacheStats().textureUploadCreations == beforeBound.textureUploadCreations));
    renderer.Shutdown();
    UNSCOPED_INFO("reused, replaced and retired texture owners must keep their actual allocation charge");
    REQUIRE((checkedStorage.load() > 0 && !invalidStorage.load()));
}

// Runs probe once on the renderer thread between requests. Setting the hook
// wakes the idle stream, so the probe sees the state after the last request.
template <class Probe>
auto onRendererThread(NativeRendererD3D12& renderer, Probe probe)
{
    using Value = decltype(probe());

    struct State
    {
        std::promise<Value> promise;
        std::atomic<bool>   taken{ false };
    };

    auto state  = std::make_shared<State>();
    auto future = state->promise.get_future();
    detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, [state, probe]
                                                       {
                                                           if (state->taken.exchange(true))
                                                               return;
                                                           try
                                                           {
                                                               state->promise.set_value(probe());
                                                           }
                                                           catch (...)
                                                           {
                                                               state->promise.set_exception(std::current_exception());
                                                           }
                                                       });
    UNSCOPED_INFO("renderer thread probe must run between requests");
    REQUIRE(future.wait_for(kReadyCeiling) == std::future_status::ready);
    detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, {});
    return future.get();
}

// A left and a right draw, each sampling texel (0,0) of its own square RGBA8
// texture through a shared payload, so payload keys persist across frames.
NativeRenderFrame uploadPoolFrame(std::uint32_t size, std::array<std::uint8_t, 4> left, std::array<std::uint8_t, 4> right)
{
    const auto texels = [&](std::array<std::uint8_t, 4> color)
    {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::size_t(size) * size * 4);
        for (std::size_t index = 0; index < bytes->size(); index += 4)
            std::memcpy(bytes->data() + index, color.data(), color.size());
        return std::shared_ptr<const std::vector<std::uint8_t>>(std::move(bytes));
    };
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    auto leftDraw              = makeDraw({ 1, 1, 1, 1 }, true);
    leftDraw.sampleCount = leftDraw.sampleMask = 1;
    leftDraw.textureMask                       = 1;
    leftDraw.textures[0]                       = { {}, size, size, NativeDrawReplayTextureFormat::Rgba8 };
    leftDraw.textures[0].sharedBytes           = texels(left);
    leftDraw.pixelShaderDxil                   = compile(
        "Texture2D<float4> source_texture:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return source_texture.Load(int3(0,0,0)); }",
        "ps_5_1");
    auto rightDraw                    = leftDraw;
    rightDraw.scissor                 = { 640, 0, 1280, 720 };
    rightDraw.textures[0].sharedBytes = texels(right);
    frame.passes                      = { { false, { leftDraw, rightDraw }, { frameResolve(1, 1280) } } };
    return frame;
}

// The frame arena demand prepareFrameUploads computes for a terminal frame
// without depth conversions.
std::uint64_t frameArenaBytes(const NativeRenderFrame& frame)
{
    const auto aligned = [](std::size_t size)
    {
        return (std::uint64_t(size) + 255U) & ~std::uint64_t(255U);
    };
    std::uint64_t bytes = 0;
    for (const auto& pass : frame.passes)
    {
        for (std::size_t index = 0; index < pass.DrawCount(); ++index)
        {
            const auto& draw = pass.Draw(index);
            bytes += aligned(draw.vertexConstants.size()) + aligned(draw.pixelConstants.size()) + aligned(draw.sharedConstants.size()) +
                     aligned(draw.vertexBytes().size()) + aligned(draw.indices.size() * sizeof(std::uint32_t));
        }
        bytes += pass.resolves.size() * 256;
    }
    return bytes + aligned(4096) + aligned(3584) + aligned(336) + aligned(3 * 16) + aligned(3 * sizeof(std::uint32_t));
}

// Upload leases return after frame release; idle buffers stay charged.
void checkUploadPoolLeases()
{
    using Microsoft::WRL::ComPtr;
    constexpr std::uint64_t KiB = 1024;
    const auto              red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47), blue = packRgb10Output(17, 31, 919);
    NativeRendererD3D12     renderer;
    std::string             error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto render = [&](const NativeRenderFrame& frame, std::uint32_t left, std::uint32_t right)
    {
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        checkGammaPixels(result.resolvedOutput, left, right);
    };
    const auto census = [&]
    {
        return onRendererThread(renderer, [&]
                                {
                                    return detail::NativeRenderFrameTestAccess::RetainedUploadStorage(renderer);
                                });
    };

    // A 256x256 RGBA8 footprint is 256 KiB, exactly its class.
    const auto base = uploadPoolFrame(256, { 255, 0, 0, 255 }, { 0, 0, 255, 255 });
    render(base, red, blue);
    const auto first       = renderer.ReplayCacheStats();
    const auto firstCensus = census();
    const auto arenaClass  = UploadPoolClassBytes(frameArenaBytes(base));
    UNSCOPED_INFO("the first frame leases its arena and one upload per texture");
    REQUIRE((first.uploadPoolCreations == 3 && first.uploadPoolHits == 0 && first.uploadPoolIdleBytes == 0 &&
             first.uploadPoolLiveBytes == 2 * 256 * KiB + arenaClass));

    // A new layout drops both completed textures, so their uploads go idle;
    // 256 KiB buffers cannot serve the 1 MiB uploads of 512x512 textures.
    render(uploadPoolFrame(512, { 0, 255, 0, 255 }, { 255, 0, 0, 255 }), green, red);
    const auto dropped       = renderer.ReplayCacheStats();
    const auto droppedCensus = census();
    UNSCOPED_INFO("uploads of dropped textures return to the pool idle");
    REQUIRE((dropped.uploadPoolCreations == first.uploadPoolCreations + 2 && dropped.uploadPoolHits == 0 &&
             dropped.uploadPoolIdleBytes == 2 * 256 * KiB && dropped.uploadPoolLiveBytes == first.uploadPoolLiveBytes + 2 * 1024 * KiB));
    UNSCOPED_INFO("the Upload census charges idle pool buffers with the leased ones");
    REQUIRE(droppedCensus - firstCensus == dropped.uploadPoolLiveBytes - first.uploadPoolLiveBytes);

    // The idle 256 KiB uploads serve the next 256 KiB textures with new bytes.
    const auto reused = uploadPoolFrame(256, { 0, 0, 255, 255 }, { 0, 255, 0, 255 });
    render(reused, blue, green);
    const auto hit = renderer.ReplayCacheStats();
    UNSCOPED_INFO("idle uploads of a class serve later textures of that class");
    REQUIRE((hit.uploadPoolHits == 2 && hit.uploadPoolCreations == dropped.uploadPoolCreations &&
             hit.textureUploadCreations == dropped.textureUploadCreations && hit.textureUploadReuses == dropped.textureUploadReuses + 2 &&
             hit.uploadPoolIdleBytes == 2 * 1024 * KiB));

    // Outgrowing the arena replaces its lease: the completed arena goes idle
    // and a larger class is created, which no idle class spans.
    auto grown = reused;
    for (std::size_t index = 0; index < 40; ++index)
        grown.passes[0].draws.push_back(grown.passes[0].draws[0]);
    const auto grownClass = UploadPoolClassBytes(frameArenaBytes(grown));
    UNSCOPED_INFO("the grown arena class is not served by an idle 1 MiB buffer");
    REQUIRE((grownClass > arenaClass && grownClass < 512 * KiB));
    render(grown, blue, green);
    const auto outgrown = renderer.ReplayCacheStats();
    UNSCOPED_INFO("an outgrown arena returns its completed lease and leases a larger class");
    REQUIRE((outgrown.drawUploadCreations == hit.drawUploadCreations + 1 && outgrown.uploadPoolCreations == hit.uploadPoolCreations + 1 &&
             outgrown.uploadPoolHits == hit.uploadPoolHits && outgrown.uploadPoolIdleBytes == hit.uploadPoolIdleBytes + arenaClass &&
             outgrown.uploadPoolLiveBytes == hit.uploadPoolLiveBytes + grownClass));
    render(reused, blue, green);
    const auto smaller = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a smaller frame keeps its leased arena");
    REQUIRE((smaller.drawUploadReuses == outgrown.drawUploadReuses + 1 && smaller.uploadPoolCreations == outgrown.uploadPoolCreations &&
             smaller.uploadPoolHits == outgrown.uploadPoolHits && smaller.uploadPoolIdleBytes == outgrown.uploadPoolIdleBytes));

    // Hold the queue. The first context rewrites its own completed texture
    // storage in place; the second context may take only idle buffers: the
    // replaced arena, but never an upload of the pending first frame.
    auto*                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence>  gate;
    UNSCOPED_INFO("hold both upload pool contexts");
    REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) &&
             SUCCEEDED(queue->Wait(gate.Get(), 1))));

    struct ReleaseGate
    {
        ID3D12Fence* fence;

        ~ReleaseGate()
        {
            fence->Signal(1);
        }
    } release{ gate.Get() };

    // Retained frames record without waiting for their output. The second
    // context's uploads are all new buffers, so no pending upload is shared.
    std::array<NativeRenderFrame, 2> held{ uploadPoolFrame(256, { 255, 0, 0, 255 }, { 255, 255, 255, 255 }),
                                           uploadPoolFrame(256, { 0, 255, 255, 255 }, { 255, 0, 255, 255 }) };
    for (std::size_t index = 0; index < held.size(); ++index)
    {
        held[index].textures[0].persistentKey = 9501 + index;
        held[index].retainOnly                = true;
        held[index].returnResolvedOutput      = false;
    }
    std::array<std::shared_ptr<NativeRenderFrameTicket>, 2> tickets;
    for (std::size_t index = 0; index < tickets.size(); ++index)
    {
        UNSCOPED_INFO(error);
        REQUIRE(renderer.QueueRenderFrame(held[index], tickets[index], error));
        UNSCOPED_INFO("both upload pool contexts record while the queue is held");
        REQUIRE(tickets[index]->Recording().wait_for(kReadyCeiling) == std::future_status::ready);
        const auto recorded = tickets[index]->Recording().get();
        UNSCOPED_INFO(recorded.error);
        REQUIRE(recorded.success);
    }
    const auto pending = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a pending frame keeps its leases: the second context reuses the idle arena and creates its uploads");
    REQUIRE((gate->GetCompletedValue() == 0 && pending.uploadPoolHits == smaller.uploadPoolHits + 1 &&
             pending.uploadPoolCreations == smaller.uploadPoolCreations + 2 && pending.uploadPoolIdleBytes == smaller.uploadPoolIdleBytes - arenaClass));
    UNSCOPED_INFO("release both upload pool contexts");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    for (const auto& ticket : tickets)
    {
        UNSCOPED_INFO("retire upload pool contexts");
        REQUIRE(ticket->Completion().wait_for(kReadyCeiling) == std::future_status::ready);
        const auto completed = ticket->Completion().get();
        UNSCOPED_INFO(completed.error);
        REQUIRE((completed.success && completed.completionFenceValue));
    }
    UNSCOPED_INFO("completion alone returns no lease: owners release the storage that holds it");
    REQUIRE(renderer.ReplayCacheStats().uploadPoolIdleBytes == pending.uploadPoolIdleBytes);
    // The first context's textures hold exactly what its pending uploads carried.
    auto shown                      = held[0];
    shown.textures[0].persistentKey = 0;
    shown.retainOnly                = false;
    shown.returnResolvedOutput      = true;
    render(shown, red, packRgb10Output(311, 617, 919));
    renderer.Shutdown();
    UNSCOPED_INFO("shutdown releases every pooled upload buffer");
    REQUIRE(renderer.ReplayCacheStats().uploadPoolLiveBytes == 0);
}

// Frame textures return after owner signal; pending work delays matching reuse.
void checkTexturePoolReuse()
{
    using Microsoft::WRL::ComPtr;
    const auto          red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47), blue = packRgb10Output(17, 31, 919);
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto*                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    ComPtr<ID3D12Device> device;
    UNSCOPED_INFO("texture pool device");
    REQUIRE(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))));
    const auto allocation = [&](std::uint32_t size)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = size;
        desc.Height           = size;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format                            = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc                        = { 1, 0 };
        return device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    };
    const auto render = [&](const NativeRenderFrame& frame, std::uint32_t left, std::uint32_t right)
    {
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        checkGammaPixels(result.resolvedOutput, left, right);
    };
    // The Texture census less the returned resolve destinations, which this
    // check leaves to checkResolvePoolReuse.
    const auto census = [&]
    {
        return onRendererThread(renderer, [&]
                                {
                                    const auto resolves = renderer.ReplayCacheStats();
                                    return detail::NativeRenderFrameTestAccess::RetainedTextureStorage(renderer, false) - resolves.resolvePoolIdleBytes -
                                           resolves.resolvePoolPendingBytes;
                                });
    };

    render(uploadPoolFrame(256, { 255, 0, 0, 255 }, { 0, 0, 255, 255 }), red, blue);
    const auto first       = renderer.ReplayCacheStats();
    const auto firstCensus = census();
    UNSCOPED_INFO("the first frame creates one texture per payload");
    REQUIRE((first.textureCreations == 2 && first.texturePoolHits == 0 && first.texturePoolIdleBytes == 0));

    // A new layout leaves both completed 256x256 textures unclaimed: they go
    // idle and stay charged beside the two new 512x512 textures.
    render(uploadPoolFrame(512, { 0, 255, 0, 255 }, { 255, 0, 0, 255 }), green, red);
    const auto dropped       = renderer.ReplayCacheStats();
    const auto droppedCensus = census();
    UNSCOPED_INFO("unclaimed completed textures return to the pool idle");
    REQUIRE((dropped.textureCreations == first.textureCreations + 2 && dropped.texturePoolHits == 0 && dropped.texturePoolIdleBytes == 2 * allocation(256)));
    UNSCOPED_INFO("the Texture census charges idle pooled textures with the retained ones");
    REQUIRE(droppedCensus - firstCensus == 2 * allocation(512));

    // The idle textures take new payloads of their allocation, with new bytes
    // and new descriptors; the 512x512 textures go idle in turn.
    render(uploadPoolFrame(256, { 0, 0, 255, 255 }, { 0, 255, 0, 255 }), blue, green);
    const auto hit = renderer.ReplayCacheStats();
    UNSCOPED_INFO("idle textures serve later textures of their allocation");
    REQUIRE((hit.textureCreations == dropped.textureCreations && hit.texturePoolHits == 2 && hit.texturePoolIdleBytes == 2 * allocation(512) &&
             hit.textureReuses >= dropped.textureReuses + 2));
    UNSCOPED_INFO("a pool hit moves a charge without adding one");
    REQUIRE(census() == droppedCensus);

    // Private payloads need separate storage when one allocation is pending;
    // shared payload stays pinned.
    auto source                      = uploadPoolFrame(64, { 255, 0, 0, 255 }, { 0, 255, 0, 255 });
    source.textures[0].persistentKey = 9611;
    for (auto& draw : source.passes[0].draws)
    {
        draw.textures[0].bytes = *draw.textures[0].sharedBytes;
        draw.textures[0].sharedBytes.reset();
    }
    auto rightResolve    = frameResolve(1, 640, 640);
    rightResolve.sourceX = 640;
    NativeRenderFramePass leftPass{ false, { source.passes[0].draws[0] }, { frameResolve(1, 640) } };
    NativeRenderFramePass rightPass{ true, { source.passes[0].draws[1] }, { rightResolve } };
    auto                  appended = source;
    appended.passes                = { leftPass, rightPass };
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(appended, error));
    ComPtr<ID3D12Fence> gate;
    UNSCOPED_INFO("hold texture pool appends");
    REQUIRE((SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) && SUCCEEDED(queue->Wait(gate.Get(), 1))));

    struct ReleaseGate
    {
        ID3D12Fence* fence;

        ~ReleaseGate()
        {
            fence->Signal(1);
        }
    } release{ gate.Get() };

    NativeRenderFrame firstPacket;
    firstPacket.textures = appended.textures;
    firstPacket.passes   = { leftPass };
    UNSCOPED_INFO(error);
    REQUIRE(renderer.AppendRenderFramePasses(firstPacket, error));
    NativeRenderFrame secondPacket;
    secondPacket.passes = { rightPass };
    std::string appendError;
    auto        append = std::async(std::launch::async, [&]
                                    {
                                 return renderer.AppendRenderFramePasses(secondPacket, appendError);
                                    });
    UNSCOPED_INFO(appendError);
    REQUIRE((append.wait_for(kReadyCeiling) == std::future_status::ready && append.get()));
    const auto pending = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a texture retired by pending work stays pending, so a matching texture creates its own storage");
    REQUIRE((gate->GetCompletedValue() == 0 && pending.textureCreations == hit.textureCreations + 2 && pending.texturePoolHits == hit.texturePoolHits &&
             pending.texturePoolPendingBytes == allocation(64)));
    UNSCOPED_INFO("pooled textures keep their actual allocation charge");
    REQUIRE(onRendererThread(renderer, [&]
                             {
                                 return detail::NativeRenderFrameTestAccess::CheckedResourceStorage(renderer);
                             }) > 0);
    UNSCOPED_INFO("release texture pool appends");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    const auto terminal = renderer.SubmitRenderFrame(tailOf(appended, 2));
    UNSCOPED_INFO(terminal.error);
    REQUIRE((terminal.success && terminal.completionFenceValue));
    checkGammaPixels(terminal.resolvedOutput, red, green);

    // After the signal the retired texture serves a new 64x64 payload.
    render(uploadPoolFrame(64, { 0, 0, 255, 255 }, { 255, 0, 0, 255 }), blue, red);
    const auto reclaimed = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a retired texture returns to the pool after its signal completes");
    REQUIRE((reclaimed.textureCreations == pending.textureCreations && reclaimed.texturePoolHits > pending.texturePoolHits &&
             reclaimed.texturePoolPendingBytes == 0));

    const auto pooled = onRendererThread(renderer, [&]
                                         {
                                             return detail::NativeRenderFrameTestAccess::PooledTextures(renderer);
                                         });
    UNSCOPED_INFO("the pool holds the idle 512x512 textures");
    REQUIRE(pooled.size() >= 2);
    renderer.Shutdown();
    for (auto* resource : pooled)
    {
        UNSCOPED_INFO("shutdown releases every pooled texture");
        REQUIRE(resource->Release() == 0);
    }
    UNSCOPED_INFO("shutdown empties the texture pool");
    REQUIRE((renderer.ReplayCacheStats().texturePoolIdleBytes == 0 && renderer.ReplayCacheStats().texturePoolPendingBytes == 0));
}

// Last-holder return zeroes unwritten pixels; held images defer release.
void checkResolvePoolReuse()
{
    using Microsoft::WRL::ComPtr;
    const auto          red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47), blue = packRgb10Output(17, 31, 919);
    const auto          black = packRgb10Output(17, 31, 47);
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto*                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    ComPtr<ID3D12Device> device;
    UNSCOPED_INFO("resolve pool device");
    REQUIRE(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))));
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = 1280;
    desc.Height           = 720;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format                            = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc                        = { 1, 0 };
    desc.Flags                             = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const auto target                      = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    const auto held                        = [&]
    {
        const auto stats = renderer.ReplayCacheStats();
        return stats.resolvePoolIdleBytes + stats.resolvePoolPendingBytes;
    };
    const auto hits = [&]
    {
        return renderer.ReplayCacheStats().resolvePoolHits;
    };
    const auto render = [&](const NativeRenderFrame& frame, std::uint32_t left, std::uint32_t right)
    {
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        checkGammaPixels(result.resolvedOutput, left, right);
    };
    const auto pooled = [&]
    {
        return onRendererThread(renderer, [&]
                                {
                                    return detail::NativeRenderFrameTestAccess::PooledTextures(renderer);
                                });
    };

    render(uploadPoolFrame(64, { 255, 0, 0, 255 }, { 0, 0, 255, 255 }), red, blue);
    UNSCOPED_INFO("the first frame creates its resolve destination");
    REQUIRE((hits() == 0 && held() == 0));
    // The next frame takes over the context and returns the first frame's
    // destination, pending this frame's own signal, so it creates its own.
    render(uploadPoolFrame(64, { 0, 255, 0, 255 }, { 255, 0, 0, 255 }), green, red);
    auto stats = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a returned destination waits for the signal after its return");
    REQUIRE((stats.resolvePoolHits == 0 && stats.resolvePoolPendingBytes == target && stats.resolvePoolIdleBytes == 0));
    // The first frame's destination serves the third, which resolves it whole.
    render(uploadPoolFrame(64, { 0, 0, 255, 255 }, { 0, 255, 0, 255 }), blue, green);
    UNSCOPED_INFO("a returned destination serves a later one of its allocation");
    REQUIRE((hits() == 1 && held() == target));

    // The second frame's destination, red on the right, is the one pooled.
    const auto returned = pooled();
    UNSCOPED_INFO("one returned destination waits in the pool");
    REQUIRE(returned.size() == 1);
    // Only its identity is kept: an extra reference would keep it from
    // returning to the pool.
    ID3D12Resource* dirty = returned.front();
    dirty->Release();

    // It serves a left-half resolve first and enters the surface store, which
    // a later frame imports whole: its right half must read zero, as a
    // created destination's would.
    auto partial = uploadPoolFrame(64, { 255, 0, 0, 255 }, { 0, 0, 255, 255 });
    partial.textures.push_back({ 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
    partial.passes[0].resolves = { frameResolve(2, 640), frameResolve(1, 1280) };
    partial.surfaces.push_back({ 2, 9731 });
    render(partial, red, blue);
    UNSCOPED_INFO("the partial resolve reuses the returned destination");
    REQUIRE(hits() == 2);
    NativeRenderFrame consumer;
    consumer.textures             = { { 10, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9731, true }, { 30, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    consumer.outputTextureId      = 30;
    consumer.returnResolvedOutput = true;
    consumer.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    auto sample                   = makeDraw({ 1, 1, 1, 1 }, false);
    sample.sampleCount = sample.sampleMask = 1;
    sample.textureMask                     = 1;
    sample.textures[0]                     = frameTexture(10, NativeDrawReplayTextureFormat::Rgba8);
    sample.pixelShaderDxil                 = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(p.xy,0)); }", "ps_5_1");
    consumer.passes                        = { { false, { sample }, { frameResolve(30, 1280) } } };
    render(consumer, red, black);

    // The stored destination stays out of the pool until the store releases
    // it and that release retires; when that happens depends on GPU progress.
    const auto contains = [](const std::vector<ID3D12Resource*>& resources, ID3D12Resource* resource)
    {
        return std::find(resources.begin(), resources.end(), resource) != resources.end();
    };
    const auto pooledHas = [&](ID3D12Resource* resource)
    {
        const auto resources = pooled();
        const bool found     = contains(resources, resource);
        for (auto* held : resources)
            held->Release();
        return found;
    };
    for (int frame = 0; frame < 3; ++frame)
    {
        render(uploadPoolFrame(64, { 0, 255, 0, 255 }, { 255, 0, 0, 255 }), green, red);
        UNSCOPED_INFO("a stored destination is not returned while the store holds it");
        REQUIRE(!pooledHas(dirty));
    }
    auto released = uploadPoolFrame(64, { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
    released.releasedSurfaces.push_back(9731);
    render(released, blue, red);
    bool returnedToPool = pooledHas(dirty);
    for (int frame = 0; frame < 8 && !returnedToPool; ++frame)
    {
        render(uploadPoolFrame(64, { 255, 0, 0, 255 }, { 0, 255, 0, 255 }), red, green);
        returnedToPool = pooledHas(dirty);
    }
    UNSCOPED_INFO("a released stored destination returns once its release retired");
    REQUIRE(returnedToPool);

    // A destination another holder still references when its owner goes is
    // released, not pooled; the outside reference is then the last one.
    auto exported                      = uploadPoolFrame(64, { 255, 0, 0, 255 }, { 0, 0, 255, 255 });
    exported.textures[0].persistentKey = 9721;
    std::shared_ptr<NativeRenderFrameTicket> ticket;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(exported, ticket, error));
    UNSCOPED_INFO("exported destination frame");
    REQUIRE((ticket->Completion().wait_for(kReadyCeiling) == std::future_status::ready && ticket->Completion().get().success));
    ID3D12Resource* borrowed = nullptr;
    {
        const auto owner = detail::NativeRenderFrameTestAccess::CompletedTexture(renderer, ticket->Export(9721), borrowed);
        UNSCOPED_INFO("the completed exported destination is readable");
        REQUIRE((owner && borrowed));
        borrowed->AddRef();
    }
    // These frames retain no texture, so the exported destination's owner
    // goes, and a later recording drains its return.
    render(uploadPoolFrame(64, { 0, 255, 0, 255 }, { 255, 0, 0, 255 }), green, red);
    render(uploadPoolFrame(64, { 0, 0, 255, 255 }, { 0, 255, 0, 255 }), blue, green);
    render(uploadPoolFrame(64, { 255, 0, 0, 255 }, { 0, 0, 255, 255 }), red, blue);
    const auto afterExport = pooled();
    const bool inPool      = std::find(afterExport.begin(), afterExport.end(), borrowed) != afterExport.end();
    for (auto* resource : afterExport)
        resource->Release();
    UNSCOPED_INFO("a destination another holder references is released, not pooled");
    REQUIRE(!inPool);
    UNSCOPED_INFO("the outside reference was the destination's last");
    REQUIRE(borrowed->Release() == 0);
    UNSCOPED_INFO("pooled resolve destinations keep their actual allocation charge");
    REQUIRE(onRendererThread(renderer, [&]
                             {
                                 return detail::NativeRenderFrameTestAccess::CheckedResourceStorage(renderer);
                             }) > 0);

    const auto remaining = pooled();
    UNSCOPED_INFO("the pool holds returned destinations");
    REQUIRE((!remaining.empty() && held() > 0));
    renderer.Shutdown();
    for (auto* resource : remaining)
    {
        UNSCOPED_INFO("shutdown releases every pooled destination");
        REQUIRE(resource->Release() == 0);
    }
    stats = renderer.ReplayCacheStats();
    UNSCOPED_INFO("shutdown empties the resolve pool");
    REQUIRE((stats.resolvePoolIdleBytes == 0 && stats.resolvePoolPendingBytes == 0 && stats.resolvePoolEvictions == 0));
}

void checkImportedSurfaceViews()
{
    const auto          red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47), blue = packRgb10Output(17, 31, 919);
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto render = [&](const NativeRenderFrame& frame, std::uint32_t left, std::uint32_t right)
    {
        const auto before = renderer.ReplayCacheStats().descriptorReuses;
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        checkGammaPixels(result.resolvedOutput, left, right);
        return renderer.ReplayCacheStats().descriptorReuses - before;
    };
    const auto store = [&](std::uint64_t key, std::array<std::uint8_t, 4> left, std::array<std::uint8_t, 4> right, std::uint32_t leftOutput, std::uint32_t rightOutput)
    {
        auto frame = uploadPoolFrame(64, left, right);
        frame.textures.push_back({ 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 });
        frame.passes[0].resolves = { frameResolve(2, 1280), frameResolve(1, 1280) };
        frame.surfaces.push_back({ 2, key });
        render(frame, leftOutput, rightOutput);
    };
    const auto consumerOf = [&](std::uint64_t key)
    {
        NativeRenderFrame frame;
        frame.textures             = { { 10, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, key, true }, { 30, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
        frame.outputTextureId      = 30;
        frame.returnResolvedOutput = true;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        auto sample                = makeDraw({ 1, 1, 1, 1 }, false);
        sample.sampleCount = sample.sampleMask = 1;
        sample.textureMask                     = 1;
        sample.textures[0]                     = frameTexture(10, NativeDrawReplayTextureFormat::Rgba8);
        sample.pixelShaderDxil                 = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(p.xy,0)); }", "ps_5_1");
        frame.passes                           = { { false, { sample }, { frameResolve(30, 1280) } } };
        return frame;
    };
    store(9741, { 255, 0, 0, 255 }, { 0, 0, 255, 255 }, red, blue);
    store(9742, { 0, 255, 0, 255 }, { 255, 0, 0, 255 }, green, red);
    const auto first = consumerOf(9741), second = consumerOf(9742);
    // Each draw slot retains its owner views until another surface rewrites
    // them.
    render(first, red, blue);
    render(first, red, blue);
    const auto sameSurface    = render(first, red, blue);
    const auto changedSurface = render(second, green, red);
    UNSCOPED_INFO("an imported surface keeps its draw slot views across frames");
    REQUIRE(sameSurface == changedSurface + 1);
}

void checkAdmittedTextureVersions()
{
    using Payload = std::vector<std::uint8_t>;
    std::string             error;
    auto                    producerPixels = std::make_shared<Payload>(std::initializer_list<std::uint8_t>{ 255, 0, 0, 255 });
    NativeDrawReplayTexture original{ {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    original.sharedBytes = producerPixels;
    UNSCOPED_INFO(error);
    REQUIRE(CopyNativeDrawReplayTextureVersion(original, error));
    UNSCOPED_INFO("an immutable version must detach caller-owned pixels");
    REQUIRE((original.textureVersion && original.sharedBytes != producerPixels));
    *producerPixels = { 0, 0, 255, 255 };

    auto base        = makeDraw({ 1, 1, 1, 1 }, true);
    base.sampleCount = base.sampleMask = 1;
    base.textureMask                   = 1;
    base.pixelShaderDxil               = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { return t.SampleLevel(s,float2(2,2),0); }",
        "ps_5_1");
    const auto makeFrame = [&](const NativeDrawReplayTexture& texture, bool swizzle = false, bool border = false)
    {
        auto left        = base;
        left.textures[0] = texture;
        auto right       = left;
        right.scissor    = { 640, 0, 1280, 720 };
        if (swizzle)
        {
            right.textures[0].swizzle = { 2, 1, 0, 3 };
        }
        if (border)
        {
            right.samplers.resize(1);
            right.samplers[0].address = { 4, 4, 4 };
            right.samplers[0].border  = { 0, 1, 0, 1 };
        }
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
        frame.outputTextureId      = 1;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        frame.returnResolvedOutput = true;
        frame.passes               = { { false, { std::move(left), std::move(right) }, { frameResolve(1, 1280) } } };
        admitFrameDraws(frame);
        UNSCOPED_INFO("draw binding changes must retain one immutable storage version");
        REQUIRE(frame.passes[0].Draw(0).textures[0].textureVersion == frame.passes[0].Draw(1).textures[0].textureVersion);
        return frame;
    };
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    std::uint64_t fence  = 0;
    const auto    render = [&](const NativeRenderFrame& frame, std::uint32_t left, std::uint32_t right)
    {
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue > fence));
        fence = result.completionFenceValue;
        checkGammaPixels(result.resolvedOutput, left, right);
        return result.resolvedOutput;
    };
    const auto red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47), blue = packRgb10Output(17, 31, 919);
    const auto originalFrame = makeFrame(original);
    const auto reference     = render(originalFrame, red, red);
    const auto first         = renderer.ReplayCacheStats();
    UNSCOPED_INFO("two admitted bindings must share one resource, upload and device preparation");
    REQUIRE((first.textureCreations == 1 && first.textureUploadCreations == 1 && first.texturePreparationCreations == 1));
    UNSCOPED_INFO("a producer source rewrite must preserve its earlier admitted pixels");
    REQUIRE(render(originalFrame, red, red) == reference);
    const auto warm = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a warm admitted version must avoid payload comparison, upload and footprint preparation");
    REQUIRE((warm.textureVersionReuses == first.textureVersionReuses + 1 &&
             warm.texturePayloadReuses == first.texturePayloadReuses && warm.textureUploadReuses == first.textureUploadReuses &&
             warm.texturePreparationCreations == first.texturePreparationCreations));

    NativeDrawReplayTexture equal{ { 255, 0, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    UNSCOPED_INFO(error);
    REQUIRE(CopyNativeDrawReplayTextureVersion(equal, error));
    UNSCOPED_INFO("equal pixels must come from a distinct immutable producer version");
    REQUIRE((equal.textureVersion != original.textureVersion && equal.sharedBytes != original.sharedBytes));
    const auto equalFrame = makeFrame(equal);
    UNSCOPED_INFO("equal bytes in a new version must preserve exact output");
    REQUIRE(render(equalFrame, red, red) == reference);
    const auto matched = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a new equal-byte version uploads once into its own stored texture");
    REQUIRE((matched.versionTextureUploads == warm.versionTextureUploads + 1 && matched.textureVersionReuses == warm.textureVersionReuses &&
             matched.texturePayloadReuses == warm.texturePayloadReuses));
    render(equalFrame, red, red);
    const auto equalWarm = renderer.ReplayCacheStats();
    UNSCOPED_INFO("the accepted equal-byte version must become the next warm uploaded identity");
    REQUIRE((equalWarm.textureVersionReuses == matched.textureVersionReuses + 1 && equalWarm.texturePayloadReuses == matched.texturePayloadReuses));

    NativeDrawReplayTexture rewritten{ { 0, 255, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    UNSCOPED_INFO(error);
    REQUIRE(CopyNativeDrawReplayTextureVersion(rewritten, error));
    render(makeFrame(rewritten), green, green);
    const auto changed = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a same-layout CPU rewrite uploads its new version");
    REQUIRE((changed.versionTextureUploads == equalWarm.versionTextureUploads + 1 && changed.textureVersionReuses == equalWarm.textureVersionReuses));

    render(makeFrame(original, true), red, blue);
    const auto swizzled = renderer.ReplayCacheStats();
    UNSCOPED_INFO("independent admitted swizzles must share one stored texture");
    REQUIRE((swizzled.textureCreations == changed.textureCreations && swizzled.versionTextureUploads == changed.versionTextureUploads &&
             swizzled.texturePreparationCreations == changed.texturePreparationCreations));
    const auto samplerFrame = makeFrame(original, false, true);
    render(samplerFrame, red, green);
    const auto sampled = renderer.ReplayCacheStats();
    UNSCOPED_INFO("independent admitted samplers must refresh binding state without reinterpreting shared storage");
    REQUIRE((sampled.textureVersionReuses == swizzled.textureVersionReuses + 1 && sampled.textureUploadReuses == swizzled.textureUploadReuses &&
             sampled.texturePreparationCreations == swizzled.texturePreparationCreations));
    renderer.Shutdown();
}

// Pressure: 16 stored 16 MiB versions plus 17 more exceed the 512 MiB domain.
void checkVersionTexturePressure()
{
    std::string error;
    const auto  versions = [&](std::size_t count)
    {
        std::vector<NativeDrawReplayTexture> textures;
        for (std::size_t index = 0; index < count; ++index)
        {
            NativeDrawReplayTexture texture{ {}, 2048, 2048, NativeDrawReplayTextureFormat::Rgba8 };
            texture.bytes.resize(2048ULL * 2048ULL * 4ULL);
            for (std::size_t texel = 0; texel < texture.bytes.size(); texel += 4)
                texture.bytes[texel + 1] = texture.bytes[texel + 3] = 255;
            UNSCOPED_INFO(error);
            REQUIRE(CopyNativeDrawReplayTextureVersion(texture, error));
            textures.push_back(std::move(texture));
        }
        return textures;
    };
    const auto frameOf = [&](const std::vector<NativeDrawReplayTexture>& textures)
    {
        auto draw        = makeDraw({ 1, 1, 1, 1 }, false);
        draw.sampleCount = draw.sampleMask = 1;
        draw.textureMask                   = (1U << textures.size()) - 1;
        std::copy(textures.begin(), textures.end(), draw.textures.begin());
        const auto source    = "Texture2D<float4> t[" + std::to_string(textures.size()) +
                               "]:register(t0); float4 main():SV_Target0 { return t[" + std::to_string(textures.size() - 1) + "].Load(int3(2047,2047,0)); }";
        draw.pixelShaderDxil = compile(source.c_str(), "ps_5_1");
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
        frame.outputTextureId      = 1;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        frame.returnResolvedOutput = true;
        frame.passes               = { { false, { std::move(draw) }, { frameResolve(1, 1280) } } };
        admitFrameDraws(frame);
        return frame;
    };
    const auto          green = packRgb10Output(17, 617, 47);
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    onRendererThread(renderer, [&]
                     {
                         detail::NativeRenderFrameTestAccess::SetVersionTextureCap(renderer, UINT64_MAX);
                         return 0;
                     });
    const auto stored = [&]
    {
        const auto resources = onRendererThread(renderer, [&]
                                                {
                                                    return detail::NativeRenderFrameTestAccess::VersionTextures(renderer);
                                                });
        for (auto* resource : resources)
            resource->Release();
        return resources.size();
    };
    const auto first  = versions(16);
    auto       result = renderer.SubmitRenderFrame(frameOf(first));
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, green, green);
    const auto held = renderer.ReplayCacheStats();
    UNSCOPED_INFO("the uncapped store holds the first frame's versions");
    REQUIRE((stored() == 16 && held.versionTextureBytes >= 16ULL * 16ULL * 1024ULL * 1024ULL));
    const auto second = versions(17);
    result            = renderer.SubmitRenderFrame(frameOf(second));
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, green, green);
    const auto pressed = renderer.ReplayCacheStats();
    UNSCOPED_INFO("texture storage pressure evicts the stored versions the open frame does not use");
    REQUIRE((pressed.versionTextureEvictions >= held.versionTextureEvictions + 16 && stored() == 17));
    renderer.Shutdown();
}

// Versions upload once across contexts; expiry/eviction returns storage.
void checkVersionTextureStore()
{
    std::string error;
    const auto  version = [&](std::array<std::uint8_t, 4> texel)
    {
        NativeDrawReplayTexture texture{ { texel[0], texel[1], texel[2], texel[3] }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
        UNSCOPED_INFO(error);
        REQUIRE(CopyNativeDrawReplayTextureVersion(texture, error));
        return texture;
    };
    auto base        = makeDraw({ 1, 1, 1, 1 }, true);
    base.sampleCount = base.sampleMask = 1;
    base.textureMask                   = 1;
    base.pixelShaderDxil               = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { return t.SampleLevel(s,float2(2,2),0); }",
        "ps_5_1");
    const auto frameOf = [&](const NativeDrawReplayTexture& left, const NativeDrawReplayTexture& right, bool broken = false)
    {
        auto first         = base;
        first.textures[0]  = left;
        first.scissor      = { 0, 0, 640, 720 };
        auto second        = base;
        second.textures[0] = right;
        second.scissor     = { 640, 0, 1280, 720 };
        // Fails at pipeline creation, after its texture slot was recorded.
        if (broken)
            second.pixelShaderDxil = { 1, 2, 3, 4 };
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
        frame.outputTextureId      = 1;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        frame.returnResolvedOutput = true;
        frame.passes               = { { false, { std::move(first), std::move(second) }, { frameResolve(1, 1280) } } };
        admitFrameDraws(frame);
        return frame;
    };
    const auto          red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47), blue = packRgb10Output(17, 31, 919);
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto render = [&](const NativeRenderFrame& frame, std::uint32_t left, std::uint32_t right)
    {
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue));
        checkGammaPixels(result.resolvedOutput, left, right);
    };
    const auto stats = [&]
    {
        return renderer.ReplayCacheStats();
    };
    // The stored texture of a version, without a reference, or null.
    const auto storedOf = [&](const NativeDrawReplayTexture& texture)
    {
        auto* resource = onRendererThread(renderer, [&]
                                          {
                                              return detail::NativeRenderFrameTestAccess::VersionTexture(renderer, texture.textureVersion.get());
                                          });
        if (resource)
            resource->Release();
        return resource;
    };
    const auto storedCount = [&]
    {
        const auto resources = onRendererThread(renderer, [&]
                                                {
                                                    return detail::NativeRenderFrameTestAccess::VersionTextures(renderer);
                                                });
        for (auto* resource : resources)
            resource->Release();
        return resources.size();
    };
    const auto pooled = [&](ID3D12Resource* resource)
    {
        const auto resources = onRendererThread(renderer, [&]
                                                {
                                                    return detail::NativeRenderFrameTestAccess::PooledTextures(renderer);
                                                });
        const bool found     = std::find(resources.begin(), resources.end(), resource) != resources.end();
        for (auto* held : resources)
            held->Release();
        return found;
    };

    // Two frames sampling different versions alternate, as a world swap's
    // frames do: each version uploads once.
    const auto redTexture = version({ 255, 0, 0, 255 }), greenTexture = version({ 0, 255, 0, 255 });
    const auto redFrame = frameOf(redTexture, redTexture), greenFrame = frameOf(greenTexture, greenTexture);
    const auto before = stats();
    render(redFrame, red, red);
    render(greenFrame, green, green);
    const auto warm = stats();
    UNSCOPED_INFO("each version uploads once, and its second binding in the frame reuses it");
    REQUIRE((warm.versionTextureUploads == before.versionTextureUploads + 2 && warm.versionTextureHits == before.versionTextureHits + 2));
    for (int round = 0; round < 3; ++round)
    {
        render(redFrame, red, red);
        render(greenFrame, green, green);
    }
    const auto alternated = stats();
    UNSCOPED_INFO("alternating frames sample their stored versions without uploading again");
    REQUIRE((alternated.versionTextureUploads == warm.versionTextureUploads && alternated.versionTextureHits == warm.versionTextureHits + 12 &&
             alternated.textureUploadCreations == warm.textureUploadCreations && alternated.textureUploadReuses == warm.textureUploadReuses));
    {
        // A draw slot rebinding the same stored version keeps its two views;
        // another version rewrites them, as the pixel checks show.
        const auto reusesOf = [&](const NativeRenderFrame& frame, std::uint32_t color)
        {
            const auto before = stats().descriptorReuses;
            render(frame, color, color);
            return stats().descriptorReuses - before;
        };
        // Frames may alternate between draw slot sets, so two red frames first
        // leave red in every slot the next frames record into.
        reusesOf(redFrame, red);
        reusesOf(redFrame, red);
        const auto sameVersion    = reusesOf(redFrame, red);
        const auto changedVersion = reusesOf(greenFrame, green);
        UNSCOPED_INFO("stored versions keep their draw slot views until the slot binds another version");
        REQUIRE(sameVersion == changedVersion + 2);
    }
    auto* const redStored = storedOf(redTexture);
    UNSCOPED_INFO("the store holds each version in its own texture");
    REQUIRE((redStored && storedOf(greenTexture) && redStored != storedOf(greenTexture) && storedCount() == 2 && alternated.versionTextureBytes > 0));
    render(frameOf(redTexture, greenTexture), red, green);
    UNSCOPED_INFO("a frame mixing stored versions uploads nothing");
    REQUIRE(stats().versionTextureUploads == warm.versionTextureUploads);

    auto blueTexture = version({ 0, 0, 255, 255 });
    render(frameOf(blueTexture, blueTexture), blue, blue);

    // An expired version's storage returns to the texture pool once the
    // renderer drops its last use of it.
    auto* blueStored = storedOf(blueTexture);
    UNSCOPED_INFO("the store holds red, green and blue");
    REQUIRE((blueStored && storedCount() == 3));
    const auto expiring = stats();
    blueTexture         = version({ 0, 0, 255, 255 });
    // Frames that upload nothing, so no upload takes the returned storage.
    for (int frame = 0; frame < 4 && stats().versionTextureEvictions == expiring.versionTextureEvictions; ++frame)
        render(greenFrame, green, green);
    UNSCOPED_INFO("an expired version leaves the store for the texture pool");
    REQUIRE((stats().versionTextureEvictions == expiring.versionTextureEvictions + 1 && storedCount() == 2 && pooled(blueStored)));
    render(frameOf(blueTexture, blueTexture), blue, blue);
    UNSCOPED_INFO("the new blue version is stored");
    REQUIRE((storedCount() == 3 && storedOf(blueTexture)));

    // Over the cap the least recently used versions leave for the pool, and
    // a version's next use uploads it again. Green is the newest here.
    render(greenFrame, green, green);
    auto* const blueAgain = storedOf(blueTexture);
    const auto  entry     = stats().versionTextureBytes / 3;
    onRendererThread(renderer, [&]
                     {
                         detail::NativeRenderFrameTestAccess::SetVersionTextureCap(renderer, 2 * entry);
                         return 0;
                     });
    const auto uncapped = stats();
    render(redFrame, red, red);
    const auto capped = stats();
    UNSCOPED_INFO("the cap evicts the oldest versions and red uploads again");
    REQUIRE((capped.versionTextureUploads == uncapped.versionTextureUploads + 1 && capped.versionTextureEvictions == uncapped.versionTextureEvictions + 2 &&
             capped.versionTextureBytes == 2 * entry));
    UNSCOPED_INFO("the cap keeps the two most recent versions");
    REQUIRE((storedCount() == 2 && storedOf(greenTexture) && storedOf(redTexture) && !storedOf(blueTexture) && pooled(blueAgain)));

    UNSCOPED_INFO("stored versions keep their actual allocation charge");
    REQUIRE(onRendererThread(renderer, [&]
                             {
                                 return detail::NativeRenderFrameTestAccess::CheckedResourceStorage(renderer);
                             }) > 0);
    const auto remaining = onRendererThread(renderer, [&]
                                            {
                                                return detail::NativeRenderFrameTestAccess::VersionTextures(renderer);
                                            });
    UNSCOPED_INFO("the store holds versions at shutdown");
    REQUIRE(remaining.size() == 2);
    renderer.Shutdown();
    for (auto* resource : remaining)
    {
        UNSCOPED_INFO("shutdown releases every stored version");
        REQUIRE(resource->Release() == 0);
    }
    UNSCOPED_INFO("shutdown empties the store");
    REQUIRE(stats().versionTextureBytes == 0);

    // A recording that fails after staging an upload stops the stream for
    // good; its shutdown releases the staged texture with the rest.
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto staging = stats();
    UNSCOPED_INFO("a recording failing at its pipeline fails its frame");
    REQUIRE(!renderer.SubmitRenderFrame(frameOf(blueTexture, blueTexture, true)).success);
    UNSCOPED_INFO("the failed recording staged its upload");
    REQUIRE((stats().versionTextureUploads == staging.versionTextureUploads + 1 && stats().versionTextureBytes > 0));
    renderer.Shutdown();
    UNSCOPED_INFO("shutdown after a failed recording empties the store");
    REQUIRE(stats().versionTextureBytes == 0);
}

void checkAdmittedTextureLayouts()
{
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto checkLayout = [&](NativeDrawReplayTexture texture, const char* shader, std::uint32_t expected)
    {
        UNSCOPED_INFO(error);
        REQUIRE(CopyNativeDrawReplayTextureVersion(texture, error));
        auto draw        = makeDraw({ 1, 1, 1, 1 }, false);
        draw.sampleCount = draw.sampleMask = 1;
        draw.textureMask                   = 1;
        draw.textures[0]                   = std::move(texture);
        draw.sampler.maxLod                = static_cast<float>(draw.textures[0].mipLevels - 1);
        draw.pixelShaderDxil               = compile(shader, "ps_5_1");
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
        frame.outputTextureId      = 1;
        frame.returnResolvedOutput = true;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        frame.passes               = { { false, { std::move(draw) }, { frameResolve(1, 1280) } } };
        admitFrameDraws(frame);
        const auto before = renderer.ReplayCacheStats();
        const auto first  = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(first.error);
        REQUIRE((first.success && first.completionFenceValue));
        checkGammaPixels(first.resolvedOutput, expected, expected);
        const auto prepared = renderer.ReplayCacheStats();
        UNSCOPED_INFO("a new admitted mip/array/format layout must create one device preparation");
        REQUIRE(prepared.texturePreparationCreations == before.texturePreparationCreations + 1);
        const auto warm = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(warm.error);
        REQUIRE((warm.success && warm.completionFenceValue > first.completionFenceValue && warm.resolvedOutput == first.resolvedOutput));
        const auto reused = renderer.ReplayCacheStats();
        UNSCOPED_INFO("warm admitted mip/array/format layouts must retain exact uploaded contents and footprints");
        REQUIRE((reused.textureVersionReuses == prepared.textureVersionReuses + 1 &&
                 reused.texturePreparationCreations == prepared.texturePreparationCreations && reused.textureUploadReuses == prepared.textureUploadReuses));
    };
    NativeDrawReplayTexture mip{ {}, 4, 4, NativeDrawReplayTextureFormat::Rgba8 };
    mip.mipLevels = 3;
    mip.bytes.resize(64 + 16 + 4);
    std::fill(mip.bytes.begin() + 80, mip.bytes.end(), 255);
    checkLayout(std::move(mip),
                "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
                "float4 main():SV_Target0 { return t.SampleLevel(s,float2(0.5,0.5),2); }",
                packRgb10Output(311, 617, 919));

    NativeDrawReplayTexture cube{ {}, 4, 4, NativeDrawReplayTextureFormat::Rgba8 };
    cube.mipLevels = 2;
    cube.dimension = NativeDrawReplayTextureDimension::TextureCube;
    cube.bytes.resize((64 + 16) * 6);
    std::size_t offset = 0;
    for (std::size_t face = 0; face < 6; ++face)
    {
        for (std::size_t level = 0; level < 2; ++level)
        {
            const auto                        bytes = level == 0 ? 64U : 16U;
            const std::array<std::uint8_t, 4> color{ static_cast<std::uint8_t>(face == 0 ? 255 : 0), 0, static_cast<std::uint8_t>(face == 5 ? 255 : 0), 255 };
            for (std::size_t pixel = 0; pixel < bytes / 4; ++pixel)
            {
                std::memcpy(cube.bytes.data() + offset + pixel * 4, color.data(), 4);
            }
            offset += bytes;
        }
    }
    checkLayout(std::move(cube),
                "TextureCube<float4> t:register(t0,space2); SamplerState s:register(s0,space3);"
                "float4 main():SV_Target0 { return float4(t.SampleLevel(s,float3(1,0,0),1).r,0,t.SampleLevel(s,float3(0,0,-1),1).b,1); }",
                packRgb10Output(311, 31, 919));

    NativeDrawReplayTexture compressed{ {}, 4, 4, NativeDrawReplayTextureFormat::Bc3 };
    compressed.mipLevels = 2;
    compressed.bytes     = { 255, 255, 0, 0, 0, 0, 0, 0, 0, 248, 0, 0, 0, 0, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 31, 0, 0, 0, 0, 0, 0, 0 };
    checkLayout(std::move(compressed),
                "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
                "float4 main():SV_Target0 { return t.SampleLevel(s,float2(0.5,0.5),1); }",
                packRgb10Output(17, 31, 919));
    renderer.Shutdown();
}

void checkRetainedRenderFrame()
{
    NativeRenderFrame producer;
    producer.textures        = { { 1, 512, 512, NativeDrawReplayTextureFormat::Rgba8, 101, false } };
    producer.outputTextureId = 1;
    producer.retainOnly      = true;
    NativeRenderFramePass pass;
    pass.target = { 512, 512, 1, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear clear;
    clear.rectangle  = { 0, 0, 512, 512 };
    clear.color      = true;
    clear.colorValue = { 1, 0, 0, 1 };
    pass.clears.push_back(clear);
    auto resolve   = frameResolve(1, 512);
    resolve.height = 512;
    pass.resolves.push_back(resolve);
    producer.passes.push_back(pass);
    NativeRenderFrame consumer;
    consumer.textures             = { { 1, 512, 512, NativeDrawReplayTextureFormat::Rgba8, 101, true },
                                      { 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    consumer.outputTextureId      = 2;
    consumer.returnResolvedOutput = true;
    consumer.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    auto draw                     = makeDraw({ 1, 1, 1, 1 }, false);
    draw.sampleCount = draw.sampleMask = 1;
    draw.textureMask                   = 1;
    draw.textures[0]                   = frameTexture(1, NativeDrawReplayTextureFormat::Rgba8);
    draw.textures[0].width = draw.textures[0].height = 512;
    draw.pixelShaderDxil                             = compile(
        "Texture2D<float4> source_texture:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return source_texture.Load(int3(256,256,0)); }",
        "ps_5_1");
    consumer.passes = { { false, { draw }, { frameResolve(2, 1280) } } };
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(producer, error));
    auto invalid                        = producer;
    invalid.passes[0].resolves[0].width = 256;
    UNSCOPED_INFO("partial producers cannot publish a retained texture");
    REQUIRE(!ValidateNativeRenderFrame(invalid, error));
    invalid                                 = consumer;
    invalid.passes[0].resolves[0].textureId = 1;
    UNSCOPED_INFO("imports must not be mutated by resolves");
    REQUIRE(!ValidateNativeRenderFrame(invalid, error));
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto result = renderer.SubmitRenderFrame(producer);
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.resolvedOutput.empty() && result.completionFenceValue));
    auto fence = result.completionFenceValue;
    for (unsigned frame = 0; frame < 2; ++frame)
    {
        result = renderer.SubmitRenderFrame(consumer);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue > fence));
        fence = result.completionFenceValue;
        checkGammaPixels(result.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(311, 31, 47));
    }
    producer.textures[0].persistentKey      = 102;
    producer.passes[0].clears[0].colorValue = { 0, 1, 0, 1 };
    result                                  = renderer.SubmitRenderFrame(producer);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    consumer.textures[0].persistentKey = 102;
    result                             = renderer.SubmitRenderFrame(consumer);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 617, 47), packRgb10Output(17, 617, 47));
    consumer.textures[0].persistentKey = 101;
    result                             = renderer.SubmitRenderFrame(consumer);
    UNSCOPED_INFO("retired versions must not be imported");
    REQUIRE((!result.success && result.error.find("producer version") != std::string::npos));
    renderer.Shutdown();
    NativeRendererD3D12 fresh;
    UNSCOPED_INFO(error);
    REQUIRE(fresh.StartDrawStream(error));
    consumer.textures[0].persistentKey = 102;
    result                             = fresh.SubmitRenderFrame(consumer);
    UNSCOPED_INFO("retained textures must be local to their renderer lifetime");
    REQUIRE((!result.success && result.error.find("producer version") != std::string::npos));
    fresh.Shutdown();
}

auto cancelingGpuPublisher(rex::ui::d3d12::D3D12Presenter& presenter)
{
    return [&presenter](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
    {
        const auto                                          stream = presenter.CreateGpuGuestOutputStream();
        rex::ui::d3d12::D3D12Presenter::GpuGuestOutputFrame output;
        output.resource    = resource;
        output.ready_fence = fence;
        output.ready_value = value;
        output.width       = 1280;
        output.height      = 720;
        auto ticket        = presenter.QueueGpuGuestOutputFrame(stream, std::move(output));
        presenter.CloseGpuGuestOutputStream(stream);
        return ticket;
    };
}

void checkQueuedRenderFrames()
{
    using Microsoft::WRL::ComPtr;
    NativeRenderFrame first;
    first.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9101 },
                              { 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9199, true } };
    first.outputTextureId = 1;
    first.returnGpuOutput = true;
    first.gammaTable      = makeGammaTable(17, 31, 47, 311, 617, 919);
    auto red              = makeDraw({ 1, 0, 0, 1 }, false);
    red.sampleCount = red.sampleMask         = 1;
    first.passes                             = { { false, { red }, { frameResolve(1, 1280) } } };
    auto continuation                        = first.passes[0];
    continuation.preservePreviousAttachments = true;
    first.passes.push_back(std::move(continuation));
    NativeRenderFrame second;
    second.textures        = { { 10, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9101, true },
                               { 11, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9102 } };
    second.outputTextureId = 11;
    second.returnGpuOutput = true;
    second.gammaTable      = first.gammaTable;
    auto sample            = red;
    sample.textureMask     = 1;
    sample.textures[0]     = frameTexture(10, NativeDrawReplayTextureFormat::Rgba8);
    sample.pixelShaderDxil = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(p.xy,0)); }", "ps_5_1");
    second.passes          = { { false, { sample }, { frameResolve(11, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    auto                provider = rex::ui::d3d12::D3D12Provider::Create();
    UNSCOPED_INFO("pending proof provider");
    REQUIRE(bool(provider));
    auto presenter = rex::ui::d3d12::D3D12Presenter::Create([](bool, bool)
                                                            {
                                                            },
                                                            *provider);
    UNSCOPED_INFO("pending proof presenter");
    REQUIRE(bool(presenter));
    const auto publisher = cancelingGpuPublisher(*presenter);
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publisher));
    auto prefix            = first;
    prefix.outputTextureId = 0;
    prefix.returnGpuOutput = false;
    prefix.passes.resize(1);
    UNSCOPED_INFO(error);
    REQUIRE(renderer.RecordRenderFramePrefix(std::move(prefix), error));
    auto*                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence>  prefixReady, gate;
    UNSCOPED_INFO("queued complete-frame actual native gates");
    REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&prefixReady))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) && SUCCEEDED(queue->Signal(prefixReady.Get(), 1))));
    const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
    while (!prefixReady->GetCompletedValue() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    UNSCOPED_INFO("first prefix must complete before the terminal gate");
    REQUIRE(prefixReady->GetCompletedValue() == 1);

    struct ReleaseGate
    {
        ComPtr<ID3D12Fence> gate;
        ComPtr<ID3D12Fence> second;

        ~ReleaseGate()
        {
            gate->Signal(1);
            if (second)
                second->Signal(1);
        }
    } release{ gate.Get() };

    UNSCOPED_INFO("hold first terminal native execution");
    REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)));
    std::shared_ptr<NativeRenderFrameTicket> firstTicket, secondTicket, thirdTicket;
    UNSCOPED_INFO("first terminal must record independently of native completion");
    REQUIRE((renderer.QueueRenderFrame(tailOf(first, 1), firstTicket, error) && firstTicket->Recording().wait_for(kReadyCeiling) == std::future_status::ready));
    const auto recorded = firstTicket->Recording().get();
    UNSCOPED_INFO("recording must not publish native completion or output");
    REQUIRE((recorded.success && !recorded.completionFenceValue && !recorded.presenterToken && !recorded.gpuOutputTicket &&
             firstTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
    auto proof = firstTicket->Export(9101);
    UNSCOPED_INFO("all-known prefix descriptors must ignore an unused missing import while exporting canonical generated versions");
    REQUIRE((proof && proof == firstTicket->Export(9101) && !firstTicket->Export(9102) && !firstTicket->Export(9199)));
    second.textures[0].producerVersion = proof;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(second, secondTicket, error));
    UNSCOPED_INFO("second complete graph must sample an exact pending version before the first terminal completes");
    REQUIRE((secondTicket->Recording().wait_for(kReadyCeiling) == std::future_status::ready && secondTicket->Recording().get().success &&
             firstTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
             secondTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
    UNSCOPED_INFO("carried and generated exports must survive graph-local ID changes");
    REQUIRE((secondTicket->Export(9101) == proof && secondTicket->Export(9102)));
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(second, thirdTicket, error));
    UNSCOPED_INFO("a third complete graph must backpressure the two frame contexts");
    REQUIRE(thirdTicket->Recording().wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    UNSCOPED_INFO("release complete-frame terminals");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    UNSCOPED_INFO("quiet worker must retire all actual terminal fences without another CPU submission");
    REQUIRE((firstTicket->Completion().wait_for(kReadyCeiling) == std::future_status::ready &&
             secondTicket->Completion().wait_for(kReadyCeiling) == std::future_status::ready &&
             thirdTicket->Completion().wait_for(kReadyCeiling) == std::future_status::ready));
    const auto firstDone  = firstTicket->Completion().get();
    const auto secondDone = secondTicket->Completion().get();
    const auto thirdDone  = thirdTicket->Completion().get();
    UNSCOPED_INFO("terminal completions must retire FIFO");
    REQUIRE((firstDone.success && secondDone.success && thirdDone.success && firstDone.completionFenceValue < secondDone.completionFenceValue &&
             secondDone.completionFenceValue < thirdDone.completionFenceValue));
    auto output                 = second;
    output.retainOnly           = false;
    output.returnResolvedOutput = true;
    const auto pixels           = renderer.SubmitRenderFrame(std::move(output));
    UNSCOPED_INFO(pixels.error);
    REQUIRE(pixels.success);
    checkGammaPixels(pixels.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(311, 31, 47));
    for (unsigned mode = 0; mode < 5; ++mode)
    {
        NativeRendererD3D12 foreign;
        UNSCOPED_INFO(error);
        REQUIRE(foreign.StartDrawStream(error, provider->GetDevice(), publisher));
        auto                                     invalid = second;
        std::shared_ptr<NativeRenderFrameTicket> localTicket;
        if (mode)
        {
            UNSCOPED_INFO("local exact-proof rejection fixture");
            REQUIRE((foreign.QueueRenderFrame(first, localTicket, error) && localTicket->Completion().get().success));
            invalid.textures[0].producerVersion = localTicket->Export(9101);
        }
        if (mode == 1)
            invalid.textures[0].producerVersion = std::shared_ptr<const NativeRenderFrameTextureVersion>(invalid.textures[0].producerVersion.get(), [](const auto*)
                                                                                                         {
                                                                                                         });
        if (mode == 2)
        {
            invalid.textures[0].width                    = 640;
            invalid.passes[0].draws[0].textures[0].width = 640;
        }
        if (mode == 3)
        {
            auto replacement                      = first;
            replacement.textures[0].persistentKey = 9201;
            UNSCOPED_INFO("retire the exact exported resource while the caller keeps its proof");
            REQUIRE((foreign.QueueRenderFrame(std::move(replacement), localTicket, error) && localTicket->Completion().get().success));
        }
        if (mode == 4)
        {
            auto carry = invalid;
            carry.textures.resize(1);
            carry.textures[0].producerVersion.reset();
            carry.outputTextureId = 10;
            carry.returnGpuOutput = false;
            carry.retainOnly      = true;
            carry.passes[0].resolves.clear();
            UNSCOPED_INFO("a completed bare import may export another canonical proof of the same resource");
            REQUIRE((foreign.QueueRenderFrame(std::move(carry), localTicket, error) && localTicket->Completion().get().success));
            const auto alternateProof = localTicket->Export(9101);
            UNSCOPED_INFO("prefix identity fixture must use distinct valid canonical proofs");
            REQUIRE((alternateProof && alternateProof != invalid.textures[0].producerVersion));
            auto importedPrefix            = invalid;
            importedPrefix.outputTextureId = 0;
            importedPrefix.returnGpuOutput = false;
            UNSCOPED_INFO(error);
            REQUIRE(foreign.RecordRenderFramePrefix(std::move(importedPrefix), error));
            invalid.textures[0].producerVersion = alternateProof;
            invalid                             = tailOf(invalid, invalid.passes.size());
        }
        const auto rejected = foreign.SubmitRenderFrame(std::move(invalid));
        UNSCOPED_INFO("foreign, forged-owner, wrong-descriptor, expired-resource and changed-prefix proofs must reject");
        REQUIRE((!rejected.success && (rejected.error.find("producer proof") != std::string::npos ||
                                       (mode == 4 && rejected.error.find("recorded resource descriptor") != std::string::npos))));
        foreign.Shutdown();
    }
    renderer.Shutdown();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publisher));
    auto stale = renderer.SubmitRenderFrame(second);
    UNSCOPED_INFO("restart must reject the prior renderer generation's proof");
    REQUIRE((!stale.success && stale.error.find("producer proof") != std::string::npos));
    renderer.Shutdown();
    NativeRendererD3D12 stoppingRenderer;
    UNSCOPED_INFO(error);
    REQUIRE(stoppingRenderer.StartDrawStream(error, provider->GetDevice(), publisher));
    queue = detail::NativeRenderFrameTestAccess::Queue(stoppingRenderer);
    UNSCOPED_INFO("stop complete-frame native gate");
    REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) &&
             SUCCEEDED(queue->Wait(gate.Get(), 1))));
    release.gate = gate.Get();
    firstTicket.reset();
    secondTicket.reset();
    first.textures[1].producerVersion = proof;
    UNSCOPED_INFO("unused stale and foreign carried descriptors must not force imports");
    REQUIRE((stoppingRenderer.QueueRenderFrame(first, firstTicket, error) && firstTicket->Recording().get().success && !firstTicket->Export(9199)));
    proof                              = firstTicket->Export(9101);
    second.textures[0].producerVersion = proof;
    const auto droppedCompletion       = firstTicket->Completion();
    firstTicket.reset();
    ComPtr<ID3D12Fence> secondGate;
    UNSCOPED_INFO("hold a later terminal after the canceled earlier fence registration");
    REQUIRE((SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&secondGate))) &&
             SUCCEEDED(queue->Wait(secondGate.Get(), 1))));
    release.second = secondGate.Get();
    UNSCOPED_INFO(error);
    REQUIRE((stoppingRenderer.QueueRenderFrame(second, secondTicket, error) && secondTicket->Recording().get().success));
    UNSCOPED_INFO(error);
    REQUIRE(stoppingRenderer.QueueRenderFrame(second, thirdTicket, error));
    stoppingRenderer.RequestDrawStreamStop();
    UNSCOPED_INFO("stop must settle unsubmitted capacity tickets while retaining submitted native owners after caller tickets are dropped");
    REQUIRE((thirdTicket->Recording().wait_for(kReadyCeiling) == std::future_status::ready && !thirdTicket->Recording().get().success &&
             thirdTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::ready && !thirdTicket->Completion().get().success &&
             secondTicket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
    UNSCOPED_INFO("release stopped complete-frame owners");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    UNSCOPED_INFO("an older canceled fence event must not settle or fail a later held terminal drain");
    REQUIRE((droppedCompletion.wait_for(kReadyCeiling) == std::future_status::ready && droppedCompletion.get().success &&
             secondTicket->Completion().wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout));
    UNSCOPED_INFO("release the exact later stopped terminal");
    REQUIRE(SUCCEEDED(secondGate->Signal(1)));
    auto repeatedStop = std::async(std::launch::async, [&]
                                   {
                                       for (unsigned index = 0; index < 256; ++index)
                                           stoppingRenderer.RequestDrawStreamStop();
                                   });
    stoppingRenderer.Shutdown();
    repeatedStop.get();
    UNSCOPED_INFO("a submitted frame must retain and prove its actual terminal fence through stop");
    REQUIRE(secondTicket->Completion().get().success);
}

// Holds the draw stream worker at its next loop top, before it takes a
// request, until released. Arming the hook wakes the worker.
class WorkerHold
{
public:
    explicit WorkerHold(NativeRendererD3D12& renderer)
    : renderer_(renderer)
    {
        auto paused  = std::make_shared<std::promise<void>>();
        auto armed   = std::make_shared<std::atomic<bool>>(true);
        auto reached = paused->get_future();
        detail::NativeRenderFrameTestAccess::BeforeRequest(renderer_, [armed, paused, resumed = resumed_]
                                                           {
                                                               if (armed->exchange(false))
                                                               {
                                                                   paused->set_value();
                                                                   resumed.wait();
                                                               }
                                                           });
        UNSCOPED_INFO("the draw stream worker reaches its loop top");
        REQUIRE(reached.wait_for(kReadyCeiling) == std::future_status::ready);
    }

    WorkerHold(const WorkerHold&)            = delete;
    WorkerHold& operator=(const WorkerHold&) = delete;

    void release()
    {
        if (!released_)
        {
            released_ = true;
            resume_.set_value();
        }
    }

    ~WorkerHold()
    {
        release();
        detail::NativeRenderFrameTestAccess::BeforeRequest(renderer_, {});
    }

private:
    NativeRendererD3D12&     renderer_;
    std::promise<void>       resume_;
    std::shared_future<void> resumed_  = resume_.get_future().share();
    bool                     released_ = false;
};

// Two-deep queues admit each request when taken, while the next can queue.
void checkDrawStreamQueue()
{
    using namespace std::chrono_literals;

    struct RestoreDepth
    {
        ~RestoreDepth()
        {
            NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(1);
        }
    } restoreDepth;

    NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(NativeRendererD3D12::kDrawStreamQueueDepth);
    constexpr auto prompt = 10s; // a call the queue must accept without waiting on recording
    auto           red    = makeDraw({ 1, 0, 0, 1 }, false);
    red.sampleCount = red.sampleMask = 1;
    const auto solid                 = [&](std::uint64_t key)
    {
        NativeRenderFrame frame;
        frame.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, key } };
        frame.outputTextureId = 1;
        frame.retainOnly      = true;
        frame.passes          = { { false, { red }, { frameResolve(1, 1280) } } };
        return frame;
    };
    // A prefix chain over one texture: the opening packet, appended packets,
    // and the tail terminal continuing `recorded` passes with `own` of its own.
    const auto opening = [&](std::uint64_t key)
    {
        auto frame            = solid(key);
        frame.outputTextureId = 0;
        frame.retainOnly      = false;
        return frame;
    };
    const auto append = [&]
    {
        NativeRenderFrame frame;
        frame.passes = { { true, { red }, { frameResolve(1, 1280) } } };
        return frame;
    };
    const auto terminal = [&](std::uint64_t key, std::size_t recorded, std::size_t own = 0)
    {
        auto frame = solid(key);
        while (frame.passes.size() < recorded + own)
            frame.passes.push_back({ true, { red }, { frameResolve(1, 1280) } });
        return tailOf(std::move(frame), recorded);
    };
    const auto settled = [](const std::shared_future<NativeDrawReplayResult>& future, std::chrono::milliseconds wait = 0ms)
    {
        return future.valid() && future.wait_for(wait) == std::future_status::ready;
    };
    std::string error;

    // 1. Depth, FIFO and one recording at a time. With the worker held two
    // requests queue at once and a third waits; they record in order, each
    // under a later queue fence than the one before.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
#if REREVVED_DEVELOPER
        const auto before = renderer.ReplayTimingStats();
#endif
        std::shared_future<NativeDrawReplayResult> first, second, third;
        std::string                                thirdError;
        std::future<bool>                          blocked, queued; // outlive the hold, which their waits need released
        {
            WorkerHold hold(renderer);
            queued = std::async(std::launch::async, [&]
                                {
                                    return renderer.QueueRenderFramePasses(opening(9501), first, error) &&
                                           renderer.QueueRenderFramePasses(append(), second, error);
                                });
            UNSCOPED_INFO("two requests queue while the worker is held: " + error);
            REQUIRE((queued.wait_for(prompt) == std::future_status::ready && queued.get()));
            UNSCOPED_INFO("a held worker records neither queued request");
            REQUIRE((!settled(first) && !settled(second)));
            UNSCOPED_INFO("queued requests stage no admission before intake");
            REQUIRE(detail::NativeRenderFrameTestAccess::StagedImports(renderer) == 0);
            blocked = std::async(std::launch::async, [&]
                                 {
                                     return renderer.QueueRenderFramePasses(append(), third, thirdError);
                                 });
            UNSCOPED_INFO("a third request waits while the stream holds two");
            REQUIRE(blocked.wait_for(100ms) == std::future_status::timeout);
            hold.release();
            UNSCOPED_INFO("the third request queues once one records: " + thirdError);
            REQUIRE((blocked.wait_for(kReadyCeiling) == std::future_status::ready && blocked.get()));
            UNSCOPED_INFO("queued requests settle");
            REQUIRE((settled(first, kReadyCeiling) && settled(second, kReadyCeiling) && settled(third, kReadyCeiling)));
        }
        const auto a = first.get(), b = second.get(), c = third.get();
        UNSCOPED_INFO("queued prefix packets record: " + a.error + b.error + c.error);
        REQUIRE((a.success && b.success && c.success));
        // One worker records serially: each recording ends with a signal
        // that covers it before the next request is taken.
        UNSCOPED_INFO("queued requests record FIFO, one recording open at a time");
        REQUIRE((a.completionFenceValue && a.completionFenceValue < b.completionFenceValue && b.completionFenceValue < c.completionFenceValue));
        const auto done = renderer.SubmitRenderFrame(terminal(9501, 3));
        UNSCOPED_INFO(std::format("the chain's terminal joins the queued packets: success {} fence {} after {}: {}", done.success, done.completionFenceValue, c.completionFenceValue, done.error));
        REQUIRE((done.success && done.completionFenceValue >= c.completionFenceValue));
#if REREVVED_DEVELOPER
        const auto after = renderer.ReplayTimingStats();
        UNSCOPED_INFO("a caller blocked on a full queue is counted");
        REQUIRE((after.streamQueueWaits > before.streamQueueWaits && after.streamQueueWaitNanoseconds > before.streamQueueWaitNanoseconds));
#endif
        renderer.Shutdown();
    }

    // 2. Terminals queued behind each other record and complete FIFO.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_ptr<NativeRenderFrameTicket> firstTicket, secondTicket;
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO(error);
            REQUIRE((renderer.QueueRenderFrame(solid(9511), firstTicket, error) && renderer.QueueRenderFrame(solid(9512), secondTicket, error)));
            UNSCOPED_INFO("a held worker records neither queued terminal");
            REQUIRE((!settled(firstTicket->Recording()) && !settled(secondTicket->Recording())));
        }
        UNSCOPED_INFO("queued terminals complete");
        REQUIRE((settled(firstTicket->Completion(), kReadyCeiling) && settled(secondTicket->Completion(), kReadyCeiling)));
        const auto a = firstTicket->Completion().get(), b = secondTicket->Completion().get();
        UNSCOPED_INFO("queued terminals complete FIFO");
        REQUIRE((a.success && b.success && a.completionFenceValue < b.completionFenceValue));
        renderer.Shutdown();
    }

    // 3. Admission at intake: a frame queued behind the request that stores
    // the surface it imports finds it stored, and samples its content.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        auto producer = solid(9521);
        producer.surfaces.push_back({ 1, 9522 });
        NativeRenderFrame consumer;
        consumer.textures             = { { 10, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9522, true },
                                          { 30, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9523 } };
        consumer.outputTextureId      = 30;
        consumer.returnResolvedOutput = true;
        consumer.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        auto sample                   = red;
        sample.textureMask            = 1;
        sample.textures[0]            = frameTexture(10, NativeDrawReplayTextureFormat::Rgba8);
        sample.pixelShaderDxil        = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(p.xy,0)); }", "ps_5_1");
        consumer.passes               = { { false, { sample }, { frameResolve(30, 1280) } } };
        std::shared_ptr<NativeRenderFrameTicket> producerTicket, consumerTicket;
        std::future<bool>                        queued;
        {
            WorkerHold hold(renderer);
            queued = std::async(std::launch::async, [&]
                                {
                                    return renderer.QueueRenderFrame(std::move(producer), producerTicket, error) &&
                                           renderer.QueueRenderFrame(std::move(consumer), consumerTicket, error);
                                });
            UNSCOPED_INFO("a surface consumer queues behind its unrecorded producer: " + error);
            REQUIRE((queued.wait_for(prompt) == std::future_status::ready && queued.get()));
        }
        UNSCOPED_INFO("the consumer completes");
        REQUIRE(settled(consumerTicket->Completion(), kReadyCeiling));
        const auto result = consumerTicket->Completion().get();
        UNSCOPED_INFO("the consumer imports the surface stored ahead of it: " + result.error);
        REQUIRE((producerTicket->Completion().get().success && result.success));
        checkGammaPixels(result.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(311, 31, 47));
        renderer.Shutdown();
    }

    // 4. A terminal queued ahead closes the chain for the caller, so a new
    // chain's opening packet is accepted before that terminal records.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_future<NativeDrawReplayResult> opened;
        std::shared_ptr<NativeRenderFrameTicket>   closing;
        std::string                                nextError;
        std::future<bool>                          next;
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO(error);
            REQUIRE((renderer.QueueRenderFramePasses(opening(9531), opened, error) && renderer.QueueRenderFrame(terminal(9531, 1), closing, error)));
            next = std::async(std::launch::async, [&]
                              {
                                  return renderer.RecordRenderFramePrefix(opening(9532), nextError);
                              });
            UNSCOPED_INFO("the next chain waits for room");
            REQUIRE(next.wait_for(100ms) == std::future_status::timeout);
        }
        UNSCOPED_INFO("a new chain opens behind a queued terminal: " + nextError);
        REQUIRE((next.wait_for(kReadyCeiling) == std::future_status::ready && next.get()));
        UNSCOPED_INFO("the closed chain records and completes");
        REQUIRE((opened.get().success && closing->Completion().get().success));
        const auto done = renderer.SubmitRenderFrame(terminal(9532, 1));
        UNSCOPED_INFO("the new chain's terminal: " + done.error);
        REQUIRE(done.success);
        renderer.Shutdown();
    }

    // 5. A recording failure stops the stream and settles the request queued
    // behind it with the stop error and the fence submitted so far.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_future<NativeDrawReplayResult> opened, failing, behind;
        UNSCOPED_INFO(error);
        REQUIRE((renderer.QueueRenderFramePasses(opening(9541), opened, error) && settled(opened, kReadyCeiling) && opened.get().success));
        {
            WorkerHold hold(renderer);
            auto       changed          = append();
            changed.identityAttachments = true;
            UNSCOPED_INFO(error);
            REQUIRE((renderer.QueueRenderFramePasses(std::move(changed), failing, error) && renderer.QueueRenderFramePasses(append(), behind, error)));
        }
        UNSCOPED_INFO("the failing request and the one behind it settle");
        REQUIRE((settled(failing, kReadyCeiling) && settled(behind, kReadyCeiling)));
        const auto failed = failing.get(), canceled = behind.get();
        UNSCOPED_INFO("a request queued behind a failed recording settles with the stop error: " + canceled.error);
        REQUIRE((!failed.success && !canceled.success && canceled.error.find("stopped before frame submission") != std::string::npos &&
                 canceled.submittedFenceValue >= opened.get().completionFenceValue));
        std::shared_future<NativeDrawReplayResult> refused;
        UNSCOPED_INFO("the failed stream refuses new requests");
        REQUIRE(!renderer.QueueRenderFramePasses(append(), refused, error));
        renderer.Shutdown();
    }

    // Chainless refusal fails one request; packet/prefix failure stops stream.
    {
        // `frame`'s first draw holding `bytes` of vertex capacity.
        const auto heavy = [&](NativeRenderFrame frame, std::size_t bytes)
        {
            auto& draw     = frame.passes.front().draws.front();
            auto  geometry = std::make_shared<std::vector<std::uint8_t>>(std::move(draw.vertexData));
            geometry->reserve(bytes);
            draw.sharedVertexData = std::move(geometry);
            admitFrameDraws(frame);
            return frame;
        };
        constexpr std::size_t kOverBound      = 600U * 1024U * 1024U; // alone past the 512 MiB owned bound
        constexpr std::size_t kHalf           = 300U * 1024U * 1024U; // two exceed it
        const auto            refusedAtIntake = [&](const NativeDrawReplayResult& result, const char* what)
        {
            UNSCOPED_INFO(std::string(what) + " reports the admission error and no submitted fence: " + result.error);
            REQUIRE((!result.success && result.error.find("byte bound") != std::string::npos && result.submittedFenceValue == 0));
        };
        {
            NativeRendererD3D12 renderer;
            UNSCOPED_INFO(error);
            REQUIRE(renderer.StartDrawStream(error));
            const auto                               before = renderer.ReplayCounts();
            std::shared_ptr<NativeRenderFrameTicket> ticket;
            UNSCOPED_INFO("the queue accepts a frame it admits at intake: " + error);
            REQUIRE((renderer.QueueRenderFrame(heavy(solid(9551), kOverBound), ticket, error) && ticket));
            UNSCOPED_INFO("the refused frame settles");
            REQUIRE((settled(ticket->Recording(), kReadyCeiling) && settled(ticket->Completion(), kReadyCeiling)));
            refusedAtIntake(ticket->Recording().get(), "an intake refusal");
            refusedAtIntake(ticket->Completion().get(), "an intake refusal's completion");
            UNSCOPED_INFO("an intake refusal records nothing");
            REQUIRE(renderer.ReplayCounts().queueSubmissions == before.queueSubmissions);
            UNSCOPED_INFO("an intake refusal leaves no staged admission");
            REQUIRE(detail::NativeRenderFrameTestAccess::StagedImports(renderer) == 0);
            std::shared_future<NativeDrawReplayResult> opened;
            UNSCOPED_INFO("the next epoch opens a chain after an intake refusal: " + error);
            REQUIRE((renderer.QueueRenderFramePasses(opening(9552), opened, error) && settled(opened, kReadyCeiling) && opened.get().success));
            const auto done = renderer.SubmitRenderFrame(terminal(9552, 1));
            UNSCOPED_INFO("the next epoch's terminal records after an intake refusal: " + done.error);
            REQUIRE(done.success);
            UNSCOPED_INFO("the attachments of a renderer's first frame count as pool misses");
            REQUIRE(renderer.ReplayCacheStats().attachmentPoolMisses > 0);
            UNSCOPED_INFO(error);
            REQUIRE((renderer.QueueRenderFramePasses(heavy(opening(9555), kHalf), opened, error) && settled(opened, kReadyCeiling) && opened.get().success));
            const auto heavyDone = renderer.SubmitRenderFrame(terminal(9555, 1));
            UNSCOPED_INFO(heavyDone.error);
            REQUIRE(heavyDone.success);
            const auto released = renderer.ReplayCacheStats();
            UNSCOPED_INFO(std::format("a completed epoch's 300 MiB prefix leaves the owned charge: {} now, {} at most", released.ownedChargeBytes, released.ownedChargePeakBytes));
            REQUIRE((released.ownedChargePeakBytes >= kHalf && released.ownedChargeBytes < kHalf));
            renderer.Shutdown();
        }
        for (const bool packet : { false, true })
        {
            const char*         what = packet ? "a refused packet" : "a refused terminal behind an open prefix";
            NativeRendererD3D12 renderer;
            UNSCOPED_INFO(error);
            REQUIRE(renderer.StartDrawStream(error));
            std::shared_future<NativeDrawReplayResult> opened, refused, next;
            if (packet)
            {
                // An epoch's first packet: the caller already holds its passes.
                UNSCOPED_INFO(error);
                REQUIRE(renderer.QueueRenderFramePasses(heavy(opening(9553), kOverBound), refused, error));
            }
            else
            {
                UNSCOPED_INFO(error);
                REQUIRE((renderer.QueueRenderFramePasses(heavy(opening(9553), kHalf), opened, error) && settled(opened, kReadyCeiling) && opened.get().success));
                std::shared_ptr<NativeRenderFrameTicket> ticket;
                UNSCOPED_INFO(error);
                REQUIRE((renderer.QueueRenderFrame(heavy(terminal(9553, 1, 1), kHalf), ticket, error) && ticket));
                refused = ticket->Recording();
            }
            UNSCOPED_INFO(std::string(what) + " settles");
            REQUIRE(settled(refused, kReadyCeiling));
            refusedAtIntake(refused.get(), what);
            if (!packet)
            {
                const auto charged = renderer.ReplayCacheStats();
                UNSCOPED_INFO(std::format("the owned charge peak covers a held 300 MiB prefix: {} now, {} at most", charged.ownedChargeBytes, charged.ownedChargePeakBytes));
                REQUIRE((charged.ownedChargePeakBytes >= kHalf && charged.ownedChargeBytes <= charged.ownedChargePeakBytes));
            }
            UNSCOPED_INFO(std::string(what) + " stops the stream");
            REQUIRE(!renderer.QueueRenderFramePasses(opening(9554), next, error));
            renderer.Shutdown();
        }
    }

    // 7. Stop settles a queued terminal at once and a queued packet when the
    // worker reaches it; shutdown settles every queued request.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_future<NativeDrawReplayResult> packet;
        std::shared_ptr<NativeRenderFrameTicket>   ticket;
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO(error);
            REQUIRE((renderer.QueueRenderFramePasses(opening(9561), packet, error) && renderer.QueueRenderFrame(terminal(9561, 1), ticket, error)));
            renderer.RequestDrawStreamStop();
            UNSCOPED_INFO("stop settles a queued terminal before the worker takes it");
            REQUIRE((settled(ticket->Recording()) && settled(ticket->Completion()) && !ticket->Recording().get().success && !ticket->Completion().get().success));
        }
        UNSCOPED_INFO("stop settles a queued packet when the worker takes it");
        REQUIRE((settled(packet, kReadyCeiling) && packet.get().error.find("stopped before frame submission") != std::string::npos));
        renderer.Shutdown();
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_future<NativeDrawReplayResult> first, second;
        std::future<void>                          stopping;
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO(error);
            REQUIRE((renderer.QueueRenderFramePasses(opening(9562), first, error) && renderer.QueueRenderFramePasses(append(), second, error)));
            stopping = std::async(std::launch::async, [&]
                                  {
                                      renderer.Shutdown();
                                  });
            UNSCOPED_INFO("shutdown waits for the held worker");
            REQUIRE(stopping.wait_for(100ms) == std::future_status::timeout);
        }
        UNSCOPED_INFO("shutdown completes");
        REQUIRE(stopping.wait_for(kReadyCeiling) == std::future_status::ready);
        stopping.get();
        UNSCOPED_INFO("shutdown settles every queued request");
        REQUIRE((settled(first) && settled(second) && !first.get().success && !second.get().success));
    }

    // 8. Synchronous calls drain the queue first, so they record after it.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_ptr<NativeRenderFrameTicket> queuedTicket;
        std::future<NativeDrawReplayResult>      submitted;
#if REREVVED_DEVELOPER
        const auto before = renderer.ReplayTimingStats();
#endif
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO(error);
            REQUIRE(renderer.QueueRenderFrame(solid(9571), queuedTicket, error));
            submitted = std::async(std::launch::async, [&]
                                   {
                                       return renderer.SubmitRenderFrame(solid(9572));
                                   });
            UNSCOPED_INFO("a synchronous call waits while the worker is held");
            REQUIRE(submitted.wait_for(100ms) == std::future_status::timeout);
        }
        UNSCOPED_INFO("the synchronous call completes");
        REQUIRE(submitted.wait_for(kReadyCeiling) == std::future_status::ready);
        const auto result = submitted.get();
        const auto queued = queuedTicket->Completion().get();
        UNSCOPED_INFO("a synchronous call records after the queued request it drained: " + result.error);
        REQUIRE((result.success && queued.success && queued.completionFenceValue < result.completionFenceValue));
#if REREVVED_DEVELOPER
        const auto after = renderer.ReplayTimingStats();
        UNSCOPED_INFO("the drain and the wait on its own result are counted");
        REQUIRE((after.streamFutureWaits > before.streamFutureWaits && after.streamQueueWaits > before.streamQueueWaits));
#endif
        renderer.Shutdown();
    }
}

// Direct stream charges each request; held terminals retire off-thread.
void checkDirectStream()
{
    using namespace std::chrono_literals;

    struct Restore
    {
        ~Restore()
        {
            NativeRendererD3D12::SetDefaultDirectStream(false);
            NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(1);
        }
    } restore;

    NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(NativeRendererD3D12::kDrawStreamQueueDepth);
    // Four stripes, one per pass; a chain sends passes 0 and 1, then pass 2,
    // and its tail carries pass 3.
    const std::array<std::array<float, 4>, 4> colors = { { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } } };
    const auto                                graph  = [&](std::uint64_t key)
    {
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, key } };
        frame.outputTextureId      = 1;
        frame.returnResolvedOutput = true;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        for (std::uint32_t index = 0; index < colors.size(); ++index)
        {
            auto draw       = makeDraw(colors[index], false);
            draw.scissor    = { static_cast<std::int32_t>(index * 320), 0, static_cast<std::int32_t>((index + 1) * 320), 720 };
            auto resolve    = frameResolve(1, 320, index * 320);
            resolve.sourceX = index * 320;
            frame.passes.push_back({ index != 0, { draw }, { resolve } });
        }
        return frame;
    };
    const auto openChain = [&](NativeRendererD3D12& renderer, std::uint64_t key, std::shared_future<NativeDrawReplayResult>& first, std::shared_future<NativeDrawReplayResult>& second)
    {
        auto              full = graph(key);
        NativeRenderFrame opening, append;
        opening.textures = full.textures;
        opening.passes   = { full.passes[0], full.passes[1] };
        append.passes    = { full.passes[2] };
        std::string error;
        callbackChecks.Require((renderer.QueueRenderFramePasses(std::move(opening), first, error) && renderer.QueueRenderFramePasses(std::move(append), second, error)), error);
    };
    const auto settled = [](const auto& future, std::chrono::milliseconds wait)
    {
        return future.valid() && future.wait_for(wait) == std::future_status::ready;
    };
    // The deleter releases a retired frame's charge after destroying it.
    const auto quietCharges = [](const NativeRendererD3D12& renderer)
    {
        const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
        auto       charges  = detail::NativeRenderFrameTestAccess::DirectCharges(renderer);
        while (charges.first && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
            charges = detail::NativeRenderFrameTestAccess::DirectCharges(renderer);
        }
        return charges;
    };
    // A graph whose first draw holds bytes of reserved geometry.
    const auto heavyGraph = [&](std::uint64_t key, std::size_t bytes)
    {
        auto  frame    = graph(key);
        auto& draw     = frame.passes[0].draws[0];
        auto  geometry = std::make_shared<std::vector<std::uint8_t>>(std::move(draw.vertexData));
        geometry->reserve(bytes);
        draw.sharedVertexData = std::move(geometry);
        return frame;
    };
    constexpr std::size_t kMiB = 1024U * 1024U;
    std::string           error;
    NativeRendererD3D12   reference;
    UNSCOPED_INFO(error);
    REQUIRE(reference.StartDrawStream(error));
    const auto expected = reference.SubmitRenderFrame(graph(9901));
    UNSCOPED_INFO(expected.error);
    REQUIRE(expected.success);
    reference.Shutdown();

    NativeRendererD3D12::SetDefaultDirectStream(true);
    // 1. A chain, its tail and a complete frame record the census stream's
    // pixels, walk no owner state and leave no charge once retired.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        const auto                                 walks = detail::NativeRenderFrameTestAccess::OwnedWalks(renderer);
        std::shared_future<NativeDrawReplayResult> first, second;
        openChain(renderer, 9902, first, second);
        const auto joined = renderer.SubmitRenderFrame(tailOf(graph(9902), 3));
        UNSCOPED_INFO("a direct stream records a chain's packets: " + first.get().error + second.get().error);
        REQUIRE((settled(first, kReadyCeiling) && settled(second, kReadyCeiling) && first.get().success && second.get().success));
        UNSCOPED_INFO("a direct stream's tail records the full graph's pixels: " + joined.error);
        REQUIRE((joined.success && joined.resolvedOutput == expected.resolvedOutput));
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        UNSCOPED_INFO(error);
        REQUIRE((renderer.QueueRenderFrame(graph(9903), ticket, error) && settled(ticket->Completion(), kReadyCeiling)));
        const auto complete = ticket->Completion().get();
        UNSCOPED_INFO("a direct stream records a complete frame: " + complete.error);
        REQUIRE((complete.success && complete.resolvedOutput == expected.resolvedOutput));
        UNSCOPED_INFO("a direct stream walks no owner state");
        REQUIRE(detail::NativeRenderFrameTestAccess::OwnedWalks(renderer) == walks);
        const auto [held, persistent] = quietCharges(renderer);
        const auto stats              = renderer.ReplayCacheStats();
        UNSCOPED_INFO(std::format("retired requests release their charge: {} held, {} persistent, {} published, {} at most", held, persistent, stats.ownedChargeBytes, stats.ownedChargePeakBytes));
        REQUIRE((held == 0 && persistent > 0 && stats.ownedChargeBytes == persistent && stats.ownedChargePeakBytes > persistent));
        // The pipeline cache and the structures the frames grew stay charged.
        const auto [cpu, pipelines, structures] = detail::NativeRenderFrameTestAccess::DirectPersistent(renderer);
        UNSCOPED_INFO(std::format("persistent charges: {} CPU textures, {} pipelines, {} structures", cpu, pipelines, structures));
        REQUIRE((pipelines > 0 && structures > 0 && cpu + pipelines + structures == persistent));
        renderer.Shutdown();
    }

    // 2. Queue room counts terminal frames: with the worker held two
    // terminals and a chain's packets queue at once, and a third terminal
    // waits until one records.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_ptr<NativeRenderFrameTicket>   firstTicket, secondTicket;
        std::shared_future<NativeDrawReplayResult> first, second;
        NativeDrawReplayResult                     tail;
        std::future<void>                          queued, blocked;
        bool                                       terminalsQueued = false;
        {
            WorkerHold hold(renderer);
            queued = std::async(std::launch::async, [&]
                                {
                                    terminalsQueued = renderer.QueueRenderFrame(graph(9911), firstTicket, error) && renderer.QueueRenderFrame(graph(9912), secondTicket, error);
                                    if (terminalsQueued)
                                        openChain(renderer, 9913, first, second);
                                });
            UNSCOPED_INFO("two terminals and a chain's packets queue while the worker is held");
            REQUIRE(queued.wait_for(10s) == std::future_status::ready);
            queued.get();
            UNSCOPED_INFO(error);
            REQUIRE(terminalsQueued);
            blocked = std::async(std::launch::async, [&]
                                 {
                                     tail = renderer.SubmitRenderFrame(tailOf(graph(9913), 3));
                                 });
            UNSCOPED_INFO("a third terminal waits while the stream holds two");
            REQUIRE(blocked.wait_for(100ms) == std::future_status::timeout);
            hold.release();
            UNSCOPED_INFO("the third terminal queues once one records");
            REQUIRE(blocked.wait_for(kReadyCeiling) == std::future_status::ready);
            blocked.get();
        }
        UNSCOPED_INFO("the queued requests record");
        REQUIRE((settled(firstTicket->Completion(), kReadyCeiling) && settled(secondTicket->Completion(), kReadyCeiling) && firstTicket->Completion().get().success &&
                 secondTicket->Completion().get().success && first.get().success && second.get().success));
        UNSCOPED_INFO("the chain behind two terminals records its pixels: " + tail.error);
        REQUIRE((tail.success && tail.resolvedOutput == expected.resolvedOutput));
        renderer.Shutdown();
    }

    // 3. Retired frames end off the stream thread.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::atomic<std::thread::id> worker{}, destroyedOn{};
        detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, [&]
                                                           {
                                                               worker = std::this_thread::get_id();
                                                           });
        auto  frame           = graph(9921);
        auto& draw            = frame.passes[0].draws[0];
        draw.sharedVertexData = std::shared_ptr<std::vector<std::uint8_t>>(new std::vector<std::uint8_t>(std::move(draw.vertexData)), [&](std::vector<std::uint8_t>* bytes)
                                                                           {
                                                                               destroyedOn = std::this_thread::get_id();
                                                                               delete bytes;
                                                                           });
        draw.vertexData.clear();
        const auto done = renderer.SubmitRenderFrame(std::move(frame));
        UNSCOPED_INFO("a frame with shared geometry records: " + done.error);
        REQUIRE((done.success && done.resolvedOutput == expected.resolvedOutput));
        renderer.Shutdown();
        const auto on = destroyedOn.load();
        UNSCOPED_INFO("a direct stream destroys a retired frame on its deleter thread");
        REQUIRE((on != std::thread::id{} && on != worker.load() && on != std::this_thread::get_id()));
    }

    // 4. The running total holds a chain's recorded packets: a second 300 MiB
    // packet exceeds the byte bound, is refused as it queues and ends the stream.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        const auto heavy = [&](std::size_t pass)
        {
            auto  full     = graph(9931);
            auto& draw     = full.passes[pass].draws[0];
            auto  geometry = std::make_shared<std::vector<std::uint8_t>>(std::move(draw.vertexData));
            geometry->reserve(300U * 1024U * 1024U);
            draw.sharedVertexData = std::move(geometry);
            NativeRenderFrame packet;
            if (!pass)
                packet.textures = full.textures;
            packet.passes = { full.passes[pass] };
            return packet;
        };
        std::shared_future<NativeDrawReplayResult> opened, refused, next;
        UNSCOPED_INFO("the first 300 MiB packet queues: " + error);
        REQUIRE(renderer.QueueRenderFramePasses(heavy(0), opened, error));
        UNSCOPED_INFO("the first 300 MiB packet records: " + opened.get().error);
        REQUIRE((settled(opened, kReadyCeiling) && opened.get().success));
        UNSCOPED_INFO("a packet past the running total's bound is refused as it queues: " + error);
        REQUIRE((!renderer.QueueRenderFramePasses(heavy(1), refused, error) && error.find("frame bytes exceed the owned byte bound") != std::string::npos));
        UNSCOPED_INFO("a refused packet stops the direct stream");
        REQUIRE(!renderer.QueueRenderFramePasses(graph(9932), next, error));
        renderer.Shutdown();
    }

    // 5. Queued packets are charged as they queue: with the worker held, a
    // chain's third 200 MiB packet cannot fit before its tail and is refused.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        const auto packet = [&](std::size_t pass)
        {
            auto  full     = graph(9941);
            auto& draw     = full.passes[pass].draws[0];
            auto  geometry = std::make_shared<std::vector<std::uint8_t>>(std::move(draw.vertexData));
            geometry->reserve(200 * kMiB);
            draw.sharedVertexData = std::move(geometry);
            NativeRenderFrame value;
            if (!pass)
                value.textures = full.textures;
            value.passes = { full.passes[pass] };
            return value;
        };
        std::array<std::shared_future<NativeDrawReplayResult>, 3> recorded;
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO("two 200 MiB packets queue while the worker is held: " + error);
            REQUIRE((renderer.QueueRenderFramePasses(packet(0), recorded[0], error) && renderer.QueueRenderFramePasses(packet(1), recorded[1], error)));
            UNSCOPED_INFO("a third 200 MiB packet is refused while the worker is held: " + error);
            REQUIRE((!renderer.QueueRenderFramePasses(packet(2), recorded[2], error) && error.find("frame bytes exceed the owned byte bound") != std::string::npos));
            hold.release();
        }
        UNSCOPED_INFO("the refused packet's chain is canceled");
        REQUIRE((settled(recorded[0], kReadyCeiling) && settled(recorded[1], kReadyCeiling) && !recorded[1].get().success));
        const auto stats = renderer.ReplayCacheStats();
        UNSCOPED_INFO(std::format("queued packets stay within the bound: {} at most", stats.ownedChargePeakBytes));
        REQUIRE(stats.ownedChargePeakBytes <= 512 * kMiB);
        renderer.Shutdown();
    }

    // 6. A synchronous frame refused for its output path leaves no charge.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        for (std::uint64_t key = 9951; key < 9954; ++key)
        {
            auto frame       = heavyGraph(key, 200 * kMiB);
            frame.outputPath = std::filesystem::temp_directory_path();
            const auto done  = renderer.SubmitRenderFrame(std::move(frame));
            UNSCOPED_INFO("an existing output path is refused: " + done.error);
            REQUIRE((!done.success && done.error.find("fresh writable path") != std::string::npos));
        }
        UNSCOPED_INFO("a refused synchronous frame leaves no charge");
        REQUIRE(detail::NativeRenderFrameTestAccess::DirectCharges(renderer).first == 0);
        const auto done = renderer.SubmitRenderFrame(heavyGraph(9954, 400 * kMiB));
        UNSCOPED_INFO("a 400 MiB frame records after the refusals: " + done.error);
        REQUIRE((done.success && done.resolvedOutput == expected.resolvedOutput));
        renderer.Shutdown();
    }

    // 7. A packet waits for a held terminal's bytes, not for its own tail:
    // behind a 300 MiB terminal, a 300 MiB packet queues once it retires.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        std::shared_ptr<NativeRenderFrameTicket>   ticket;
        std::shared_future<NativeDrawReplayResult> opened, appended;
        std::future<void>                          blocked;
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO("a 300 MiB terminal queues: " + error);
            REQUIRE(renderer.QueueRenderFrame(heavyGraph(9961, 300 * kMiB), ticket, error));
            bool        packetsQueued = false;
            std::string packetError;
            blocked = std::async(std::launch::async, [&]
                                 {
                                     auto              full = heavyGraph(9962, 300 * kMiB);
                                     NativeRenderFrame opening, append;
                                     opening.textures = full.textures;
                                     opening.passes   = { full.passes[0], full.passes[1] };
                                     append.passes    = { full.passes[2] };
                                     packetsQueued    = renderer.QueueRenderFramePasses(std::move(opening), opened, packetError) &&
                                                        renderer.QueueRenderFramePasses(std::move(append), appended, packetError);
                                 });
            UNSCOPED_INFO("a packet waits while a held terminal's bytes do not leave room");
            REQUIRE(blocked.wait_for(100ms) == std::future_status::timeout);
            hold.release();
            UNSCOPED_INFO("the packet queues once the terminal retires");
            REQUIRE(blocked.wait_for(kReadyCeiling) == std::future_status::ready);
            blocked.get();
            UNSCOPED_INFO("a packet queues once the held terminal retires: " + packetError);
            REQUIRE(packetsQueued);
        }
        UNSCOPED_INFO("the held terminal records: " + ticket->Completion().get().error);
        REQUIRE((settled(ticket->Completion(), kReadyCeiling) && ticket->Completion().get().success));
        const auto tail = renderer.SubmitRenderFrame(tailOf(graph(9962), 3));
        UNSCOPED_INFO("the chain under byte pressure records its pixels: " + opened.get().error + tail.error);
        REQUIRE((settled(opened, kReadyCeiling) && opened.get().success && appended.get().success && tail.success && tail.resolvedOutput == expected.resolvedOutput));
        renderer.Shutdown();
    }

    // 8. Storage a cache keeps past its request stays charged: a retained
    // 64 MiB texture version leaves no room for a 460 MiB frame.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        detail::NativeRenderFrameTestAccess::SetVersionTextureStore(renderer, false);
        NativeDrawReplayTexture texture{ {}, 4096, 4096, NativeDrawReplayTextureFormat::Rgba8 };
        texture.bytes.assign(64 * kMiB, 0xff);
        UNSCOPED_INFO(error);
        REQUIRE(CopyNativeDrawReplayTextureVersion(texture, error));
        auto sample        = makeDraw({ 1, 1, 1, 1 }, false);
        sample.sampleCount = sample.sampleMask = 1;
        sample.textureMask                     = 1;
        sample.textures[0]                     = texture;
        sample.pixelShaderDxil                 = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(p.xy,0)); }", "ps_5_1");
        NativeRenderFrame textured;
        textured.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
        textured.outputTextureId      = 1;
        textured.returnResolvedOutput = true;
        textured.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        textured.passes               = { { false, { std::move(sample) }, { frameResolve(1, 1280) } } };
        admitFrameDraws(textured);
        texture             = {};
        const auto recorded = renderer.SubmitRenderFrame(std::move(textured));
        UNSCOPED_INFO("a 64 MiB texture frame records: " + recorded.error);
        REQUIRE(recorded.success);
        const auto [held, persistent] = quietCharges(renderer);
        UNSCOPED_INFO(std::format("the cache's version stays charged: {} held, {} persistent", held, persistent));
        REQUIRE((held == 0 && persistent >= 64 * kMiB));
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        UNSCOPED_INFO("a 460 MiB frame is refused beside the retained version: " + error);
        REQUIRE((!renderer.QueueRenderFrame(heavyGraph(9971, 460 * kMiB), ticket, error) && error.find("frame bytes exceed the owned byte bound") != std::string::npos));
        UNSCOPED_INFO(error);
        REQUIRE((renderer.QueueRenderFrame(heavyGraph(9972, 400 * kMiB), ticket, error) && settled(ticket->Completion(), kReadyCeiling)));
        const auto done = ticket->Completion().get();
        UNSCOPED_INFO("a 400 MiB frame records beside the retained version: " + done.error);
        REQUIRE((done.success && done.resolvedOutput == expected.resolvedOutput));
        renderer.Shutdown();
    }

    // 9. A raw draw frame is charged: past the bound it is refused, and a
    // frame that fits releases its charge.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        NativeDrawStreamOutput output;
        output.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        output.returnResolvedOutput = true;
        auto draw                   = makeDraw({ 1, 0, 0, 1 }, false);
        auto heavy                  = draw;
        auto geometry               = std::make_shared<std::vector<std::uint8_t>>(std::move(heavy.vertexData));
        geometry->reserve(520 * kMiB);
        heavy.sharedVertexData = std::move(geometry);
        heavy.vertexData.clear();
        const auto refused = renderer.SubmitDrawFrame({ heavy }, {}, output);
        UNSCOPED_INFO("a raw draw frame past the bound is refused: " + refused.error);
        REQUIRE((!refused.success && refused.error.find("frame bytes exceed the owned byte bound") != std::string::npos));
        NativeDrawReplayResult done;
        {
            // The frame is charged before the worker takes it.
            WorkerHold hold(renderer);
            auto       pending  = std::async(std::launch::async, [&]
                                             {
                                          return renderer.SubmitDrawFrame({ draw }, {}, output);
                                             });
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!detail::NativeRenderFrameTestAccess::DirectCharges(renderer).first && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            // Checked after the worker runs, which the pending frame needs.
            const bool charged = detail::NativeRenderFrameTestAccess::DirectCharges(renderer).first > 0;
            hold.release();
            done = pending.get();
            UNSCOPED_INFO("a raw draw frame is charged before the worker takes it");
            REQUIRE(charged);
        }
        UNSCOPED_INFO("a raw draw frame records: " + done.error);
        REQUIRE((done.success && !done.resolvedOutput.empty()));
        UNSCOPED_INFO("a raw draw frame releases its charge");
        REQUIRE(quietCharges(renderer).first == 0);
        renderer.Shutdown();
    }

    // Two frames take the slot after a 400 MiB terminal charge releases; slow
    // intake preserves the first.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        auto  slow     = graph(10001);
        auto& draw     = slow.passes[0].draws[0];
        auto* geometry = new std::vector<std::uint8_t>(std::move(draw.vertexData));
        geometry->reserve(400 * kMiB);
        draw.sharedVertexData = std::shared_ptr<std::vector<std::uint8_t>>(geometry, [](std::vector<std::uint8_t>* bytes)
                                                                           {
                                                                               std::this_thread::sleep_for(500ms);
                                                                               delete bytes;
                                                                           });
        draw.vertexData.clear();
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        std::future<NativeDrawReplayResult>      first, second;
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO("a 400 MiB terminal queues: " + error);
            REQUIRE(renderer.QueueRenderFrame(std::move(slow), ticket, error));
            first  = std::async(std::launch::async, [&]
                                {
                                   return renderer.SubmitRenderFrame(heavyGraph(10002, 200 * kMiB));
                                });
            second = std::async(std::launch::async, [&]
                                {
                                    return renderer.SubmitRenderFrame(heavyGraph(10003, 200 * kMiB));
                                });
            hold.release();
        }
        detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, []
                                                           {
                                                               std::this_thread::sleep_for(300ms);
                                                           });
        const auto one = first.get(), two = second.get();
        detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, {});
        UNSCOPED_INFO("the slowly destroyed terminal records");
        REQUIRE((settled(ticket->Completion(), kReadyCeiling) && ticket->Completion().get().success));
        UNSCOPED_INFO("two synchronous frames woken by one release both record: " + one.error + two.error);
        REQUIRE((one.success && two.success && one.resolvedOutput == expected.resolvedOutput && two.resolvedOutput == expected.resolvedOutput));
        renderer.Shutdown();
    }

    // 11. A raw draw frame waiting for a slowly destroyed terminal's charge
    // is refused once a prefix records meanwhile, leaving the prefix intact.
    {
        NativeRendererD3D12 renderer;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error));
        auto  slow     = graph(10011);
        auto& draw     = slow.passes[0].draws[0];
        auto* geometry = new std::vector<std::uint8_t>(std::move(draw.vertexData));
        geometry->reserve(400 * kMiB);
        draw.sharedVertexData = std::shared_ptr<std::vector<std::uint8_t>>(geometry, [](std::vector<std::uint8_t>* bytes)
                                                                           {
                                                                               std::this_thread::sleep_for(1000ms);
                                                                               delete bytes;
                                                                           });
        draw.vertexData.clear();
        NativeDrawStreamOutput output;
        output.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        output.returnResolvedOutput = true;
        auto raw                    = makeDraw({ 1, 0, 0, 1 }, false);
        auto rawGeometry            = std::make_shared<std::vector<std::uint8_t>>(std::move(raw.vertexData));
        rawGeometry->reserve(200 * kMiB);
        raw.sharedVertexData = std::move(rawGeometry);
        raw.vertexData.clear();
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        std::future<NativeDrawReplayResult>      pending;
        {
            WorkerHold hold(renderer);
            UNSCOPED_INFO("a 400 MiB terminal queues: " + error);
            REQUIRE(renderer.QueueRenderFrame(std::move(slow), ticket, error));
            pending = std::async(std::launch::async, [&]
                                 {
                                     return renderer.SubmitDrawFrame({ raw }, {}, output);
                                 });
            hold.release();
        }
        UNSCOPED_INFO("the slowly destroyed terminal records");
        REQUIRE((settled(ticket->Completion(), kReadyCeiling) && ticket->Completion().get().success));
        // The raw frame passes its first checks and waits for the charge.
        std::this_thread::sleep_for(200ms);
        UNSCOPED_INFO("the raw draw frame waits for the held charge");
        REQUIRE(!settled(pending, 0ms));
        auto                                       full = graph(10012);
        NativeRenderFrame                          opening;
        std::shared_future<NativeDrawReplayResult> recording;
        opening.textures = full.textures;
        opening.passes   = { full.passes[0], full.passes[1] };
        UNSCOPED_INFO("a small prefix queues: " + error);
        REQUIRE(renderer.QueueRenderFramePasses(std::move(opening), recording, error));
        const auto refused = pending.get();
        UNSCOPED_INFO("the prefix records");
        REQUIRE((settled(recording, kReadyCeiling) && recording.get().success));
        UNSCOPED_INFO("a raw draw frame woken after a prefix recorded is refused: " + refused.error);
        REQUIRE((!refused.success && refused.error.find("unfinished render-frame prefix") != std::string::npos));
        renderer.Shutdown();
    }
    std::cout << "Direct stream charges, queue room, deleter and byte bound passed\n";
}

void checkAdmittedImportPins()
{
    using Microsoft::WRL::ComPtr;
    NativeRenderFrame original;
    original.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9601 },
                                 { 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9602 } };
    original.outputTextureId = 1;
    original.retainOnly      = true;
    auto red                 = makeDraw({ 1, 0, 0, 1 }, false);
    red.sampleCount = red.sampleMask = 1;
    original.passes                  = { { false, { red }, { frameResolve(1, 1280), frameResolve(2, 1280) } } };
    std::string error;
    auto        provider = rex::ui::d3d12::D3D12Provider::Create();
    UNSCOPED_INFO("admitted import provider");
    REQUIRE(bool(provider));
    auto presenter = rex::ui::d3d12::D3D12Presenter::Create([](bool, bool)
                                                            {
                                                            },
                                                            *provider);
    UNSCOPED_INFO("admitted import presenter");
    REQUIRE(bool(presenter));
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), cancelingGpuPublisher(*presenter)));
    std::shared_ptr<NativeRenderFrameTicket> originalTicket, replacementTicket, consumerTicket;
    UNSCOPED_INFO("complete exact import sources before admission");
    REQUIRE((renderer.QueueRenderFrame(original, originalTicket, error) && originalTicket->Completion().get().success));
    const auto proof = originalTicket->Export(9601);
    UNSCOPED_INFO("canonical completed import proof");
    REQUIRE(bool(proof));
    auto replacement            = original;
    replacement.textures        = { { 3, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9603 } };
    replacement.outputTextureId = 3;
    replacement.retainOnly      = false;
    replacement.returnGpuOutput = true;
    auto blue                   = makeDraw({ 0, 0, 1, 1 }, false);
    blue.sampleCount = blue.sampleMask = 1;
    auto replacementGeometry           = std::make_shared<std::vector<std::uint8_t>>(std::move(blue.vertexData));
    replacementGeometry->reserve(300U * 1024U * 1024U);
    blue.sharedVertexData = std::move(replacementGeometry);
    replacement.passes    = { { false, { blue }, { frameResolve(3, 1280) } } };
    admitFrameDraws(replacement);
    auto prefix            = replacement;
    prefix.outputTextureId = 0;
    prefix.returnGpuOutput = false;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.RecordRenderFramePrefix(std::move(prefix), error));
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence>  gate, marker;
    auto*                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    UNSCOPED_INFO("hold the registry-replacing native terminal");
    REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))) &&
             SUCCEEDED(queue->Wait(gate.Get(), 1))));
    std::promise<void> pausedPromise, resumePromise;
    auto               paused = pausedPromise.get_future();
    auto               resume = resumePromise.get_future().share();
    std::atomic<bool>  armed{ true };
    const auto         submissionsBefore = renderer.ReplayCounts().queueSubmissions;
    detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, [&]
                                                       {
                                                           if (renderer.ReplayCounts().queueSubmissions > submissionsBefore && armed.exchange(false))
                                                           {
                                                               pausedPromise.set_value();
                                                               resume.wait();
                                                           }
                                                       });
    std::future<bool> consumerAdmission;

    struct ReleasePause
    {
        NativeRendererD3D12& renderer;
        ID3D12Fence*         gate;
        std::promise<void>&  resume;
        bool                 released = false;

        ~ReleasePause()
        {
            gate->Signal(1);
            if (!released)
                resume.set_value();
            renderer.Shutdown();
        }
    } release{ renderer, gate.Get(), resumePromise };

    UNSCOPED_INFO("pause worker before newer quiet FIFO retirement and CPU request dispatch");
    REQUIRE((renderer.QueueRenderFrame(tailOf(replacement, 1), replacementTicket, error) && replacementTicket->Recording().get().success &&
             paused.wait_for(kReadyCeiling) == std::future_status::ready));
    NativeRenderFrame consumer;
    consumer.textures             = { { 10, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9601, true, proof },
                                      { 20, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9602, true },
                                      { 30, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9604 } };
    consumer.outputTextureId      = 30;
    consumer.returnResolvedOutput = true;
    consumer.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    auto sample                   = red;
    sample.textureMask            = 3;
    sample.textures[0]            = frameTexture(10, NativeDrawReplayTextureFormat::Rgba8);
    sample.textures[1]            = frameTexture(20, NativeDrawReplayTextureFormat::Rgba8);
    sample.pixelShaderDxil        = compile(
        "Texture2D<float4> a:register(t0); Texture2D<float4> b:register(t1);"
        "float4 main(float4 p:SV_Position):SV_Target0 { float4 x=a.Load(int3(p.xy,0)); float4 y=b.Load(int3(p.xy,0));"
        "return all(x==y)&&x.r>0.9?float4(1,0,0,1):float4(0,1,0,1); }",
        "ps_5_1");
    auto consumerGeometry = std::make_shared<std::vector<std::uint8_t>>(std::move(sample.vertexData));
    consumerGeometry->reserve(300U * 1024U * 1024U);
    sample.sharedVertexData = std::move(consumerGeometry);
    consumer.passes         = { { false, { sample }, { frameResolve(30, 1280) } } };
    admitFrameDraws(consumer);
    consumerAdmission = std::async(std::launch::async, [&, consumer = std::move(consumer)]() mutable
                                   {
                                       return renderer.QueueRenderFrame(std::move(consumer), consumerTicket, error);
                                   });
    UNSCOPED_INFO("released predecessor recipes permit queued admission while the worker holds consumer Recording");
    REQUIRE((consumerAdmission.wait_for(kReadyCeiling) == std::future_status::ready && consumerAdmission.get() &&
             consumerTicket && consumerTicket->Recording().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
    UNSCOPED_INFO("complete newer terminal before dispatch");
    REQUIRE((SUCCEEDED(queue->Signal(marker.Get(), 1)) && SUCCEEDED(gate->Signal(1))));
    const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
    while (!marker->GetCompletedValue() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    UNSCOPED_INFO("the newer actual terminal must be ready for quiet registry replacement");
    REQUIRE(marker->GetCompletedValue() == 1);
    resumePromise.set_value();
    release.released = true;
    UNSCOPED_INFO("required canonical and bare imports must stay pinned through quiet retirement before recording");
    REQUIRE((consumerTicket->Recording().wait_for(kReadyCeiling) == std::future_status::ready && consumerTicket->Recording().get().success));
    UNSCOPED_INFO("quiet retirement must replace the registry before recording the admitted consumer");
    REQUIRE((replacementTicket->Completion().wait_for(kReadyCeiling) == std::future_status::ready && replacementTicket->Completion().get().success &&
             consumerTicket->Completion().wait_for(kReadyCeiling) == std::future_status::ready));
    const auto result = consumerTicket->Completion().get();
    UNSCOPED_INFO("admitted resource pins must survive completed-registry replacement and preserve exact carried exports");
    REQUIRE((result.success && consumerTicket->Export(9601) == proof && consumerTicket->Export(9602)));
    checkGammaPixels(result.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(311, 31, 47));
    const std::weak_ptr<const NativeRenderFrameTextureVersion> completedProof = consumerTicket->Export(9604);
    UNSCOPED_INFO("readback must export its generated canonical texture proof");
    REQUIRE(!completedProof.expired());
    consumerTicket.reset();
    auto requestReleased = std::make_shared<std::promise<void>>();
    auto nextIteration   = requestReleased->get_future();
    auto barrierArmed    = std::make_shared<std::atomic<bool>>(true);
    detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, [requestReleased, barrierArmed]
                                                       {
                                                           if (barrierArmed->exchange(false))
                                                               requestReleased->set_value();
                                                       });
    UNSCOPED_INFO("worker must release its completed local request before the canonical registry proof lifetime check");
    REQUIRE(nextIteration.wait_for(kReadyCeiling) == std::future_status::ready);
    UNSCOPED_INFO("the completed registry must retain the canonical proof after its readback ticket is dropped");
    REQUIRE(!completedProof.expired());
    detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, {});
    renderer.Shutdown();
}

void checkAggregateOwnedAdmission()
{
    const auto frameWithCapacity = []
    {
        NativeRenderFrame frame;
        frame.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9301 } };
        frame.outputTextureId = 1;
        frame.returnGpuOutput = true;
        auto draw             = makeDraw({ 1, 0, 0, 1 }, false);
        draw.sampleCount = draw.sampleMask = 1;
        auto geometry                      = std::make_shared<std::vector<std::uint8_t>>(std::move(draw.vertexData));
        geometry->reserve(300U * 1024U * 1024U);
        draw.sharedVertexData = std::move(geometry);
        frame.passes          = { { false, { std::move(draw) }, { frameResolve(1, 1280) } } };
        admitFrameDraws(frame);
        return frame;
    };
    auto                frame = frameWithCapacity();
    NativeRendererD3D12 renderer;
    std::string         error;
    auto                provider = rex::ui::d3d12::D3D12Provider::Create();
    UNSCOPED_INFO("owned admission provider");
    REQUIRE(bool(provider));
    auto presenter = rex::ui::d3d12::D3D12Presenter::Create([](bool, bool)
                                                            {
                                                            },
                                                            *provider);
    UNSCOPED_INFO("owned admission presenter");
    REQUIRE(bool(presenter));
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), cancelingGpuPublisher(*presenter)));
    auto prefix            = frame;
    prefix.outputTextureId = 0;
    prefix.returnGpuOutput = false;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.RecordRenderFramePrefix(std::move(prefix), error));
    auto                                     distinct = frameWithCapacity();
    const auto                               before   = renderer.ReplayCounts();
    std::shared_ptr<NativeRenderFrameTicket> ticket;
    auto                                     admission = std::async(std::launch::async, [&]
                                                                    {
                                    return renderer.QueueRenderFrame(std::move(distinct), ticket, error);
                                                                    });
    UNSCOPED_INFO("distinct retained capacities must fail explicitly when an unfinished mandatory prefix cannot retire");
    REQUIRE((admission.wait_for(kReadyCeiling) == std::future_status::ready && !admission.get() &&
             error.find("aggregate retained storage") != std::string::npos && !ticket));
    UNSCOPED_INFO("aggregate CPU admission failure must precede native recording");
    REQUIRE(renderer.ReplayCounts().queueSubmissions == before.queueSubmissions);
    UNSCOPED_INFO("the held prefix payload capacity charges once through its tail terminal: " + error);
    REQUIRE((renderer.QueueRenderFrame(tailOf(frame, 1), ticket, error) && ticket->Recording().get().success && ticket->Completion().get().success));
    renderer.Shutdown();
}

void checkUnprovedTerminalDrain()
{
    using Microsoft::WRL::ComPtr;
    std::string       error;
    NativeRenderFrame frame;
    frame.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 9401 } };
    frame.outputTextureId = 1;
    frame.retainOnly      = true;
    auto draw             = makeDraw({ 1, 0, 0, 1 }, false);
    draw.sampleCount = draw.sampleMask = 1;
    // A stored version the held frame samples, which abandonment must leak.
    auto sampled        = draw;
    sampled.textureMask = 1;
    sampled.textures[0] = { { 0, 0, 255, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    UNSCOPED_INFO(error);
    REQUIRE(CopyNativeDrawReplayTextureVersion(sampled.textures[0], error));
    sampled.pixelShaderDxil = compile("Texture2D<float4> t:register(t0); float4 main():SV_Target0 { return t.Load(int3(0,0,0)); }", "ps_5_1");
    frame.passes            = { { false, { draw, sampled }, { frameResolve(1, 1280) } } };
    admitFrameDraws(frame);
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    // Leave idle pooled uploads beside the leases of the held frame.
    for (const auto size : { 256U, 512U })
    {
        const auto warm = renderer.SubmitRenderFrame(uploadPoolFrame(size, { 255, 0, 0, 255 }, { 0, 0, 255, 255 }));
        UNSCOPED_INFO(warm.error);
        REQUIRE((warm.success && warm.completionFenceValue));
    }
    UNSCOPED_INFO("abandonment starts with idle pooled uploads");
    REQUIRE(renderer.ReplayCacheStats().uploadPoolIdleBytes == 2 * 256 * 1024);
    ComPtr<ID3D12CommandQueue> queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    ComPtr<ID3D12Device>       device;
    ComPtr<ID3D12InfoQueue>    debug;
    ComPtr<ID3D12Fence>        gate, marker;
    UNSCOPED_INFO("healthy native queue held through failed terminal drain");
    REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&debug))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))) && SUCCEEDED(queue->Wait(gate.Get(), 1))));

    struct ReleaseGate
    {
        ID3D12Fence* fence;

        ~ReleaseGate()
        {
            fence->Signal(1);
        }
    } release{ gate.Get() };

    std::shared_ptr<NativeRenderFrameTicket> ticket;
    UNSCOPED_INFO("submit actual terminal storage before failed drain");
    REQUIRE((renderer.QueueRenderFrame(frame, ticket, error) && ticket->Recording().get().success && SUCCEEDED(queue->Signal(marker.Get(), 1))));
    // The held frame drops the 512x512 textures: four idle uploads, its arena
    // lease and its stored version's upload lease.
    const auto pooled = onRendererThread(renderer, [&]
                                         {
                                             return detail::NativeRenderFrameTestAccess::UploadPoolBuffers(renderer);
                                         });
    UNSCOPED_INFO("abandonment holds idle pooled uploads and pending arena and version upload leases");
    REQUIRE(pooled.size() == 6);
    // The held frame samples only its stored version, so all four warm
    // textures are idle; the second warm frame's resolve destination waits
    // in its pool.
    const auto pooledTextures = onRendererThread(renderer, [&]
                                                 {
                                                     return detail::NativeRenderFrameTestAccess::PooledTextures(renderer);
                                                 });
    UNSCOPED_INFO("abandonment holds idle pooled textures");
    REQUIRE(pooledTextures.size() == 5);
    const auto stored = onRendererThread(renderer, [&]
                                         {
                                             return detail::NativeRenderFrameTestAccess::VersionTextures(renderer);
                                         });
    UNSCOPED_INFO("the held frame stores its sampled version");
    REQUIRE(stored.size() == 1);
    debug->ClearStoredMessages();
    auto shutdown = std::async(std::launch::async, [&]
                               {
                                   renderer.Shutdown();
                               });
    UNSCOPED_INFO("stop must not manufacture completion for a held native owner");
    REQUIRE(ticket->Completion().wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    // The bound is the property: the drain abandons after its 5-second fence
    // wait. The test runs serially (RUN_SERIAL) so that bound sees no other test.
    UNSCOPED_INFO("failed healthy-device drain must use bounded abandonment");
    REQUIRE(shutdown.wait_for(std::chrono::seconds(12)) == std::future_status::ready);
    shutdown.get();
    const auto failed = ticket->Completion().get();
    UNSCOPED_INFO("unproved healthy-device owners must fail completion and deny restart while native storage remains submitted");
    REQUIRE((!failed.success && !failed.completionFenceValue && !failed.presenterToken && !failed.gpuOutputTicket &&
             gate->GetCompletedValue() == 0 && marker->GetCompletedValue() == 0 && SUCCEEDED(device->GetDeviceRemovedReason()) &&
             !renderer.StartDrawStream(error)));
    UNSCOPED_INFO("release abandoned native storage after shutdown");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
    while (!marker->GetCompletedValue() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    UNSCOPED_INFO("abandoned submitted owners must remain executable after renderer shutdown");
    REQUIRE(marker->GetCompletedValue() == 1);
    for (auto* buffer : pooled)
    {
        UNSCOPED_INFO("abandonment must leak idle and leased pooled uploads instead of releasing them");
        REQUIRE(buffer->Release() >= 1);
    }
    for (auto* texture : pooledTextures)
    {
        UNSCOPED_INFO("abandonment must leak idle pooled textures instead of releasing them");
        REQUIRE(texture->Release() >= 1);
    }
    UNSCOPED_INFO("abandonment must leak stored version textures instead of releasing them");
    REQUIRE(stored[0]->Release() >= 1);
    for (UINT64 index = 0; index < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
    {
        SIZE_T size = 0;
        UNSCOPED_INFO("failed drain debug message size");
        REQUIRE(SUCCEEDED(debug->GetMessage(index, nullptr, &size)));
        std::vector<std::uint8_t> bytes(size);
        auto*                     message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        UNSCOPED_INFO("failed drain debug message");
        REQUIRE(SUCCEEDED(debug->GetMessage(index, message, &size)));
        UNSCOPED_INFO(message->pDescription);
        REQUIRE((message->Severity != D3D12_MESSAGE_SEVERITY_WARNING && message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
                 message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION));
    }
}

struct ComparisonGpuOwners
{
    Microsoft::WRL::ComPtr<ID3D12Device>                   device;
    Microsoft::WRL::ComPtr<ID3D12Fence>                    fence;
    std::vector<Microsoft::WRL::ComPtr<ID3D12DeviceChild>> objects;

    ~ComparisonGpuOwners()
    {
        const auto completed = fence->GetCompletedValue();
        if (SUCCEEDED(device->GetDeviceRemovedReason()) && (completed == UINT64_MAX || completed < 1))
        {
            for (auto& object : objects)
                (void)object.Detach();
            (void)fence.Detach();
            (void)device.Detach();
        }
    }
};

std::vector<std::uint8_t> readGpuMailbox(const rex::ui::d3d12::D3D12Provider& provider, ID3D12Resource* texture, D3D12_RESOURCE_STATES state)
{
    using Microsoft::WRL::ComPtr;
    auto*                              device      = provider.GetDevice();
    const auto                         description = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64                             bytes = 0;
    device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width     = bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout                                                                        = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource>            readback;
    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence>               fence;
    UNSCOPED_INFO("GPU comparison buffer");
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))));
    UNSCOPED_INFO("GPU comparison allocator");
    REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    UNSCOPED_INFO("GPU comparison list");
    REQUIRE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))));
    UNSCOPED_INFO("GPU comparison fence");
    REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_COPY_SOURCE };
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
    source.pResource            = texture;
    source.Type                 = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource       = readback.Get();
    destination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    UNSCOPED_INFO("GPU comparison close");
    REQUIRE(SUCCEEDED(list->Close()));
    ID3D12CommandList*                      execute = list.Get();
    ComparisonGpuOwners                     owners{ device, fence };
    const std::array<ID3D12DeviceChild*, 5> submittedObjects{ texture, readback.Get(), allocator.Get(), list.Get(), provider.GetDirectQueue() };
    for (auto* object : submittedObjects)
        owners.objects.emplace_back(object);
    provider.GetDirectQueue()->ExecuteCommandLists(1, &execute);
    UNSCOPED_INFO("GPU comparison signal");
    REQUIRE(SUCCEEDED(provider.GetDirectQueue()->Signal(fence.Get(), 1)));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    UNSCOPED_INFO("GPU comparison event");
    REQUIRE((event && SUCCEEDED(fence->SetEventOnCompletion(1, event))));
    const auto waited    = WaitForSingleObject(event, static_cast<DWORD>(std::chrono::milliseconds(kReadyCeiling).count()));
    const auto completed = fence->GetCompletedValue();
    const auto healthy   = SUCCEEDED(device->GetDeviceRemovedReason());
    if (!healthy || (completed != UINT64_MAX && completed >= 1))
        CloseHandle(event);
    UNSCOPED_INFO("GPU comparison completion");
    REQUIRE((waited == WAIT_OBJECT_0 && healthy && completed != UINT64_MAX && completed >= 1));
    void*             mapped = nullptr;
    const D3D12_RANGE range{ 0, static_cast<SIZE_T>(bytes) };
    UNSCOPED_INFO("GPU comparison map");
    REQUIRE(SUCCEEDED(readback->Map(0, &range, &mapped)));
    std::vector<std::uint8_t> pixels(description.Width * description.Height * 4);
    for (UINT row = 0; row < description.Height; ++row)
        std::memcpy(pixels.data() + row * description.Width * 4,
                    static_cast<const std::uint8_t*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch,
                    description.Width * 4);
    const D3D12_RANGE noWrite{};
    readback->Unmap(0, &noWrite);
    return pixels;
}

void checkGpuOutputHandoff()
{
    using Microsoft::WRL::ComPtr;
    using rex::ui::d3d12::D3D12Presenter;
    using rex::ui::d3d12::GpuGuestOutputTicket;
    auto provider = rex::ui::d3d12::D3D12Provider::Create();
    UNSCOPED_INFO("GPU output provider");
    REQUIRE(bool(provider));
    auto presenter = D3D12Presenter::Create([](bool, bool)
                                            {
                                            },
                                            *provider);
    UNSCOPED_INFO("GPU output presenter");
    REQUIRE(bool(presenter));
    ComPtr<ID3D12InfoQueue> debug;
    UNSCOPED_INFO("GPU output debug queue");
    REQUIRE(SUCCEEDED(provider->GetDevice()->QueryInterface(IID_PPV_ARGS(&debug))));
    debug->ClearStoredMessages();
    auto                                  stream = presenter->CreateGpuGuestOutputStream();
    std::shared_ptr<GpuGuestOutputTicket> ticket;
    std::vector<ComPtr<ID3D12Resource>>   sources;
    const auto                            publish = [&](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
    {
        D3D12Presenter::GpuGuestOutputFrame output;
        output.resource    = resource;
        output.ready_fence = fence;
        output.ready_value = value;
        output.width       = 1280;
        output.height      = 720;
        ticket             = presenter->QueueGpuGuestOutputFrame(stream, std::move(output));
        if (ticket)
            sources.emplace_back(resource);
        return ticket;
    };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO("GPU publisher requires an explicit device");
    REQUIRE(!renderer.StartDrawStream(error, nullptr, publish));
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publish));
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.passes               = { { false, { makeDraw({ 1, 0, 0, 1 }, false) }, { frameResolve(1, 1280) } } };
    const auto handoffDraw     = frame.passes[0].draws[0];
    frame.passes[0].draws.resize(D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE / kNativeDrawReplayTextureSlots, handoffDraw);
    auto continuation                        = frame.passes[0];
    continuation.preservePreviousAttachments = true;
    continuation.draws.erase(continuation.draws.begin(), continuation.draws.begin() + continuation.draws.size() / 2);
    frame.passes[0].draws.resize(frame.passes[0].draws.size() / 2);
    frame.passes[0].resolves.clear();
    frame.passes.push_back(std::move(continuation));
    const auto cpuA = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(cpuA.error);
    REQUIRE((cpuA.success && !cpuA.resolvedOutput.empty()));
    admitFrameDraws(frame);
    const auto admittedA = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("admitted draw handles must preserve complete raw frame output before SDK copying");
    REQUIRE((admittedA.success && admittedA.resolvedOutput == cpuA.resolvedOutput));
    frame.gammaTable[255] ^= 1U << 20;
    const auto cpuB   = renderer.SubmitRenderFrame(frame);
    const auto gammaB = frame.gammaTable;
    frame.gammaTable[0] ^= 1U << 10;
    const auto cpuC   = renderer.SubmitRenderFrame(frame);
    const auto gammaC = frame.gammaTable;
    UNSCOPED_INFO("different frame gamma fixtures");
    REQUIRE((cpuB.success && cpuC.success && cpuA.resolvedOutput != cpuB.resolvedOutput && cpuB.resolvedOutput != cpuC.resolvedOutput));
    ComPtr<ID3D12Resource> destination;
    const auto             refresh = [&]
    {
        UNSCOPED_INFO("GPU output refresh");
        REQUIRE(presenter->RefreshGuestOutput(1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& context)
                                              {
                                                  destination = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
                                                  context.SetIs8bpc(true);
                                                  return true;
                                              }));
    };
    frame.returnGpuOutput      = true;
    frame.returnResolvedOutput = false;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    auto gpuA                  = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(gpuA.error);
    REQUIRE((gpuA.success && gpuA.presenterToken && gpuA.resolvedOutput.empty() && gpuA.gpuOutputTicket == ticket));
    auto first = ticket;
    UNSCOPED_INFO("admission is not copy retirement");
    REQUIRE(!first->AwaitRetirement(0));
    ComPtr<ID3D12Fence> gate;
    UNSCOPED_INFO("consumer gate fence");
    REQUIRE(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
    auto* copyQueue = rex::ui::d3d12::detail::GpuGuestOutputTestAccess::CopyQueue(*presenter);
    UNSCOPED_INFO("hold actual SDK consumer copying");
    REQUIRE((copyQueue && SUCCEEDED(copyQueue->Wait(gate.Get(), 1))));

    // Unblock before any asynchronous submit future unwinds on a failed check.
    struct ReleaseGate
    {
        ID3D12Fence* fence;

        ~ReleaseGate()
        {
            if (fence)
                fence->Signal(1);
        }
    };

    ReleaseGate finalRelease{ gate.Get() };
    refresh();
    UNSCOPED_INFO("the held consumer must own a submitted, incomplete copy");
    REQUIRE((first->status() == GpuGuestOutputTicket::Status::kSubmitted && !first->AwaitRetirement(0)));
    frame.gammaTable            = gammaB;
    bool        appendsAccepted = true, copyUntouched = false;
    std::string appendError;
    auto        second = std::async(std::launch::async, [&]
                                    {
                                 for (const auto& pass : frame.passes)
                                 {
                                     NativeRenderFrame packet;
                                     packet.textures = frame.textures;
                                     packet.passes   = { pass };
                                     if (!renderer.AppendRenderFramePasses(std::move(packet), appendError))
                                     {
                                         appendsAccepted = false;
                                         return NativeDrawReplayResult{};
                                     }
                                 }
                                 copyUntouched = ticket == first && first->status() == GpuGuestOutputTicket::Status::kSubmitted;
                                 return renderer.SubmitRenderFrame(tailOf(frame, frame.passes.size()));
                                    });
    {
        ReleaseGate release{ gate.Get() };
        UNSCOPED_INFO("a second native frame must complete while the first SDK copy is pending");
        REQUIRE(second.wait_for(kReadyCeiling) == std::future_status::ready);
        // Keep the gate held beyond this scope on success.
        release.fence = nullptr;
    }
    auto gpuB = second.get();
    UNSCOPED_INFO(appendError);
    REQUIRE(appendsAccepted);
    UNSCOPED_INFO("both appends must leave the preceding SDK copy and output ticket untouched");
    REQUIRE(copyUntouched);
    UNSCOPED_INFO(gpuB.error);
    REQUIRE((gpuB.success && gpuB.resolvedOutput.empty() && gpuB.presenterToken > gpuA.presenterToken &&
             gpuB.gpuOutputTicket == ticket && NativeRendererD3D12::PollGpuOutput(first) == NativeRendererD3D12::GpuOutputStatus::Pending));
    UNSCOPED_INFO("pending frames require independent gamma resources");
    REQUIRE((sources.size() == 2 && sources[0] != sources[1]));
    frame.gammaTable = gammaC;
    auto third       = std::async(std::launch::async, [&]
                                  {
                                return renderer.SubmitRenderFrame(frame);
                                  });
    {
        ReleaseGate release{ gate.Get() };
        UNSCOPED_INFO("two owned output slots must backpressure a third frame");
        REQUIRE(third.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    }
    std::uint64_t copied = 0;
    UNSCOPED_INFO("first output must retire through its own actual copy fence");
    REQUIRE((NativeRendererD3D12::AwaitGpuOutput(first, error, &copied) && copied == first->copy_fence_value() && copied));
    UNSCOPED_INFO("later frame mutation corrupted the first SDK copy");
    REQUIRE(readGpuMailbox(*provider, destination.Get()) == cpuA.resolvedOutput);
    auto gpuC = third.get();
    UNSCOPED_INFO("a slot may be reused only after its consumer retires");
    REQUIRE((gpuC.success && gpuC.gpuOutputTicket == ticket && sources.size() == 3 && sources[0] == sources[2]));
    refresh();
    UNSCOPED_INFO(error);
    REQUIRE((NativeRendererD3D12::AwaitGpuOutput(gpuB.gpuOutputTicket, error, &copied) && copied > first->copy_fence_value()));
    UNSCOPED_INFO("second output gamma bits must survive exactly");
    REQUIRE(readGpuMailbox(*provider, destination.Get()) == cpuB.resolvedOutput);
    refresh();
    UNSCOPED_INFO(error);
    REQUIRE((renderer.AwaitGpuOutput(error, &copied) && copied > gpuB.gpuOutputTicket->copy_fence_value()));
    UNSCOPED_INFO("third output gamma bits must survive slot reuse exactly");
    REQUIRE(readGpuMailbox(*provider, destination.Get()) == cpuC.resolvedOutput);
    frame.returnResolvedOutput = true;
    const auto both            = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(both.error);
    REQUIRE((both.success && both.resolvedOutput == cpuC.resolvedOutput && both.presenterToken));
    refresh();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.AwaitGpuOutput(error));
    UNSCOPED_INFO("first-frame diagnostic and GPU export differ");
    REQUIRE(readGpuMailbox(*provider, destination.Get()) == both.resolvedOutput);
    frame.returnResolvedOutput = false;
    const auto queuedA         = renderer.SubmitRenderFrame(frame);
    const auto queuedB         = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("two queued shutdown offers");
    REQUIRE((queuedA.success && queuedB.success));
    presenter->CloseGpuGuestOutputStream(stream);
    for (const auto& queued : { queuedA, queuedB })
    {
        UNSCOPED_INFO("stop cancels every unclaimed output owner");
        REQUIRE((queued.gpuOutputTicket->status() == GpuGuestOutputTicket::Status::kCanceled && queued.gpuOutputTicket->AwaitRetirement(0)));
    }
    renderer.Shutdown();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publish));
    UNSCOPED_INFO("closed generation rejects new exports");
    REQUIRE(!renderer.SubmitRenderFrame(frame).success);
    renderer.Shutdown();
    presenter.reset();
    for (UINT64 index = 0; index < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
    {
        SIZE_T size = 0;
        UNSCOPED_INFO("GPU output debug message size");
        REQUIRE(SUCCEEDED(debug->GetMessage(index, nullptr, &size)));
        std::vector<std::uint8_t> bytes(size);
        auto*                     message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        UNSCOPED_INFO("GPU output debug message");
        REQUIRE(SUCCEEDED(debug->GetMessage(index, message, &size)));
        UNSCOPED_INFO(message->pDescription);
        REQUIRE((message->Severity != D3D12_MESSAGE_SEVERITY_WARNING && message->Severity != D3D12_MESSAGE_SEVERITY_ERROR &&
                 message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION));
    }
}

void checkQueuedGpuOutputHandoff()
{
    using Microsoft::WRL::ComPtr;
    using rex::ui::d3d12::D3D12Presenter;
    auto provider = rex::ui::d3d12::D3D12Provider::Create();
    UNSCOPED_INFO("queued GPU output provider");
    REQUIRE(bool(provider));
    auto presenter = D3D12Presenter::Create([](bool, bool)
                                            {
                                            },
                                            *provider);
    UNSCOPED_INFO("queued GPU output presenter");
    REQUIRE(bool(presenter));
    const auto                          stream = presenter->CreateGpuGuestOutputStream();
    std::atomic<unsigned>               publications{ 0 };
    std::vector<ComPtr<ID3D12Resource>> sources;
    const auto                          publish = [&](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
    {
        callbackChecks.Require(fence->GetCompletedValue() >= value, "SDK publication must follow its exact actual native terminal fence");
        D3D12Presenter::GpuGuestOutputFrame output;
        output.resource    = resource;
        output.ready_fence = fence;
        output.ready_value = value;
        output.width       = 1280;
        output.height      = 720;
        auto ticket        = presenter->QueueGpuGuestOutputFrame(stream, std::move(output));
        if (ticket)
        {
            sources.emplace_back(resource);
            ++publications;
        }
        return ticket;
    };
    const auto gamma           = makeGammaTable(17, 31, 47, 311, 617, 919);
    auto       frame           = collectorTerminalFrame(true, gamma);
    frame.returnGpuOutput      = true;
    frame.returnResolvedOutput = false;
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publish));
    auto prefix                 = frame;
    prefix.outputTextureId      = 0;
    prefix.returnGpuOutput      = false;
    prefix.returnResolvedOutput = false;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.RecordRenderFramePrefix(std::move(prefix), error));
    auto*               queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    ComPtr<ID3D12Fence> prefixReady, nativeGate, copyGate;
    UNSCOPED_INFO("queued GPU native and copy gates");
    REQUIRE((SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&prefixReady))) &&
             SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nativeGate))) &&
             SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyGate))) &&
             SUCCEEDED(queue->Signal(prefixReady.Get(), 1))));
    const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
    while (!prefixReady->GetCompletedValue() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    UNSCOPED_INFO("hold terminal gamma after completed FirstPass");
    REQUIRE((prefixReady->GetCompletedValue() == 1 && SUCCEEDED(queue->Wait(nativeGate.Get(), 1))));

    struct ReleaseGates
    {
        ID3D12Fence* native;
        ID3D12Fence* copy;

        ~ReleaseGates()
        {
            native->Signal(1);
            copy->Signal(1);
        }
    } release{ nativeGate.Get(), copyGate.Get() };

    std::shared_ptr<NativeRenderFrameTicket> first, second, third;
    UNSCOPED_INFO("record first deferred gamma");
    REQUIRE((renderer.QueueRenderFrame(tailOf(frame, frame.passes.size()), first, error) && first->Recording().wait_for(kReadyCeiling) == std::future_status::ready &&
             first->Recording().get().success));
    auto laterGamma = gamma;
    laterGamma[255] ^= 1U << 20;
    auto later                 = collectorTerminalFrame(true, laterGamma);
    later.returnGpuOutput      = true;
    later.returnResolvedOutput = false;
    UNSCOPED_INFO("two complete gamma owners must record without early native success or SDK publication");
    REQUIRE((renderer.QueueRenderFrame(later, second, error) && second->Recording().wait_for(kReadyCeiling) == std::future_status::ready &&
             second->Recording().get().success && publications.load() == 0 && !first->Recording().get().presenterToken &&
             !first->Recording().get().completionFenceValue && !second->Recording().get().completionFenceValue &&
             first->Recording().get().resolvedOutput.empty() && second->Recording().get().resolvedOutput.empty() &&
             first->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
             second->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
    UNSCOPED_INFO("observe the actual native queue after both complete terminal submissions");
    REQUIRE(SUCCEEDED(queue->Signal(prefixReady.Get(), 2)));
    UNSCOPED_INFO("two prepared terminal graphs backpressure a third while their actual native fences are held");
    REQUIRE((renderer.QueueRenderFrame(later, third, error) && third->Recording().wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout));
    const bool terminalsCompleted = SUCCEEDED(nativeGate->Signal(1)) && first->Completion().wait_for(kReadyCeiling) == std::future_status::ready &&
                                    second->Completion().wait_for(kReadyCeiling) == std::future_status::ready;
    const auto completionState    = [](const auto& ticket)
    {
        if (ticket->Completion().wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return std::string("pending");
        const auto& result = ticket->Completion().get();
        return std::string(result.success ? "success" : "failed") + ", fence=" + std::to_string(result.completionFenceValue) + ", error=" + result.error;
    };
    UNSCOPED_INFO("quiet native worker must publish both actual terminals: native_gate=" + std::to_string(nativeGate->GetCompletedValue()) +
                  ", terminal_queue_tail=" + std::to_string(prefixReady->GetCompletedValue()) + ", publications=" + std::to_string(publications.load()) +
                  ", first=" + completionState(first) + ", second=" + completionState(second) +
                  ", third_recording=" + (third->Recording().wait_for(std::chrono::milliseconds(0)) == std::future_status::ready ? "ready" : "pending"));
    REQUIRE(terminalsCompleted);
    const auto a = first->Completion().get();
    const auto b = second->Completion().get();
    UNSCOPED_INFO("FIFO terminal publication must retain independent exact gamma sources");
    REQUIRE((a.success && b.success && a.completionFenceValue < b.completionFenceValue && a.presenterToken < b.presenterToken &&
             publications.load() == 2 && sources.size() == 2 && sources[0] != sources[1]));
    auto* copyQueue = rex::ui::d3d12::detail::GpuGuestOutputTestAccess::CopyQueue(*presenter);
    UNSCOPED_INFO("hold actual queued SDK copy");
    REQUIRE((copyQueue && SUCCEEDED(copyQueue->Wait(copyGate.Get(), 1))));
    ComPtr<ID3D12Resource> destination;
    const auto             refresh = [&]
    {
        UNSCOPED_INFO("refresh queued SDK output");
        REQUIRE(presenter->RefreshGuestOutput(1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& context)
                                              {
                                                  destination = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
                                                  context.SetIs8bpc(true);
                                                  return true;
                                              }));
    };
    refresh();
    UNSCOPED_INFO("two SDK source owners must backpressure a third complete output even after native completion");
    REQUIRE((third->Recording().wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout &&
             NativeRendererD3D12::PollGpuOutput(a.gpuOutputTicket) == NativeRendererD3D12::GpuOutputStatus::Pending && publications.load() == 2));
    renderer.RequestDrawStreamStop();
    UNSCOPED_INFO("stop must wake SDK output-capacity admission without releasing pending copy sources");
    REQUIRE((third->Recording().wait_for(kReadyCeiling) == std::future_status::ready && !third->Recording().get().success &&
             third->Completion().wait_for(kReadyCeiling) == std::future_status::ready && !third->Completion().get().success));
    UNSCOPED_INFO(error);
    REQUIRE((SUCCEEDED(copyGate->Signal(1)) && NativeRendererD3D12::AwaitGpuOutput(a.gpuOutputTicket, error)));
    checkGammaPixels(readGpuMailbox(*provider, destination.Get()), packRgb10Output(311, 31, 47), packRgb10Output(311, 31, 47));
    refresh();
    UNSCOPED_INFO(error);
    REQUIRE(NativeRendererD3D12::AwaitGpuOutput(b.gpuOutputTicket, error));
    checkGammaPixels(readGpuMailbox(*provider, destination.Get()), packRgb10Output(310, 31, 47), packRgb10Output(310, 31, 47));
    presenter->CloseGpuGuestOutputStream(stream);
    renderer.Shutdown();

    NativeRendererD3D12   rejecting;
    std::atomic<unsigned> rejectedPublications{ 0 };
    UNSCOPED_INFO(error);
    REQUIRE(rejecting.StartDrawStream(error, provider->GetDevice(), [&](ID3D12Resource*, ID3D12Fence*, std::uint64_t)
                                      {
                                          ++rejectedPublications;
                                          throw std::runtime_error("quiet publisher failure");
                                          return std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>{};
                                      }));
    UNSCOPED_INFO("quiet SDK publisher exceptions must settle the exact completion ticket once");
    REQUIRE((rejecting.QueueRenderFrame(frame, first, error) && first->Recording().get().success &&
             first->Completion().wait_for(kReadyCeiling) == std::future_status::ready && !first->Completion().get().success &&
             first->Completion().get().error.find("quiet publisher failure") != std::string::npos && rejectedPublications.load() == 1));
    rejecting.Shutdown();
}

struct CpuTextureTestGates
{
    Microsoft::WRL::ComPtr<ID3D12Fence> native, copy;

    ~CpuTextureTestGates()
    {
        if (native)
            native->Signal(1);
        if (copy)
            copy->Signal(1);
    }
};

struct QueuedCpuTextureFixture
{
    std::unique_ptr<rex::ui::d3d12::D3D12Provider>        provider;
    std::unique_ptr<rex::ui::d3d12::D3D12Presenter>       presenter;
    std::shared_ptr<rex::ui::d3d12::GpuGuestOutputStream> stream;
    std::string                                           error;
    std::atomic<unsigned>                                 publications{ 0 };
    Microsoft::WRL::ComPtr<ID3D12Resource>                destination;
    std::uint64_t                                         lastNativeFence = 0, lastToken = 0, lastCopyFence = 0;
    NativeRendererD3D12                                   renderer;

    QueuedCpuTextureFixture()
    {
        provider = rex::ui::d3d12::D3D12Provider::Create();
        UNSCOPED_INFO("canonical CPU texture provider");
        REQUIRE(bool(provider));
        presenter = rex::ui::d3d12::D3D12Presenter::Create([](bool, bool)
                                                           {
                                                           },
                                                           *provider);
        UNSCOPED_INFO("canonical CPU texture presenter");
        REQUIRE(bool(presenter));
        stream = presenter->CreateGpuGuestOutputStream();
        UNSCOPED_INFO("canonical CPU texture output stream");
        REQUIRE(bool(stream));
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), [this](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
                                         {
                                             const auto completed = fence->GetCompletedValue();
                                             callbackChecks.Require((value && completed != UINT64_MAX && completed >= value), "canonical texture publication requires its actual native terminal fence");
                                             rex::ui::d3d12::D3D12Presenter::GpuGuestOutputFrame output;
                                             output.resource    = resource;
                                             output.ready_fence = fence;
                                             output.ready_value = value;
                                             output.width       = 1280;
                                             output.height      = 720;
                                             auto ticket        = presenter->QueueGpuGuestOutputFrame(stream, std::move(output));
                                             if (ticket)
                                                 ++publications;
                                             return ticket;
                                         }));
    }

    ~QueuedCpuTextureFixture()
    {
        Close();
    }

    void Close()
    {
        presenter->CloseGpuGuestOutputStream(stream);
        renderer.Shutdown();
    }

    Microsoft::WRL::ComPtr<ID3D12Fence> Fence()
    {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        UNSCOPED_INFO("canonical texture actual queue gate");
        REQUIRE(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
        return fence;
    }

    Microsoft::WRL::ComPtr<ID3D12Fence> HoldNative()
    {
        auto gate = Fence();
        UNSCOPED_INFO("hold canonical texture native execution");
        REQUIRE(SUCCEEDED(detail::NativeRenderFrameTestAccess::Queue(renderer)->Wait(gate.Get(), 1)));
        return gate;
    }

    std::shared_ptr<NativeRenderFrameTicket> Queue(NativeRenderFrame frame)
    {
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.QueueRenderFrame(std::move(frame), ticket, error));
        return ticket;
    }

    void Recorded(const std::shared_ptr<NativeRenderFrameTicket>& ticket)
    {
        UNSCOPED_INFO("canonical CPU texture recording must finish within its bounded wait");
        REQUIRE((ticket && ticket->Recording().wait_for(kReadyCeiling) == std::future_status::ready));
        const auto& result = ticket->Recording().get();
        UNSCOPED_INFO((result.success ? "canonical texture Recording must not publish native or SDK completion" : result.error));
        REQUIRE((result.success && !result.completionFenceValue && !result.presenterToken && !result.gpuOutputTicket));
    }

    NativeDrawReplayResult Terminal(const std::shared_ptr<NativeRenderFrameTicket>& ticket)
    {
        UNSCOPED_INFO("canonical texture actual terminal retirement");
        REQUIRE(ticket->Completion().wait_for(kReadyCeiling) == std::future_status::ready);
        auto result = ticket->Completion().get();
        UNSCOPED_INFO((result.success ? "canonical texture native fences and SDK tokens must retire FIFO" : result.error));
        REQUIRE((result.success && result.completionFenceValue > lastNativeFence && result.presenterToken > lastToken && result.gpuOutputTicket));
        lastNativeFence = result.completionFenceValue;
        lastToken       = result.presenterToken;
        return result;
    }

    void StartCopy()
    {
        UNSCOPED_INFO("refresh canonical texture SDK output");
        REQUIRE(presenter->RefreshGuestOutput(1280, 720, 1280, 720, [this](rex::ui::Presenter::GuestOutputRefreshContext& context)
                                              {
                                                  destination = static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
                                                  context.SetIs8bpc(true);
                                                  return true;
                                              }));
    }

    void FinishCopy(const NativeDrawReplayResult& result, std::uint32_t left, std::uint32_t right)
    {
        std::uint64_t copyFence = 0;
        UNSCOPED_INFO(error);
        REQUIRE(NativeRendererD3D12::AwaitGpuOutput(result.gpuOutputTicket, error, &copyFence));
        UNSCOPED_INFO("canonical texture SDK FIFO requires an actual completed copy fence");
        REQUIRE((copyFence > lastCopyFence && NativeRendererD3D12::PollGpuOutput(result.gpuOutputTicket) == NativeRendererD3D12::GpuOutputStatus::Completed));
        lastCopyFence = copyFence;
        checkGammaPixels(readGpuMailbox(*provider, destination.Get()), left, right);
    }

    void Consume(const std::shared_ptr<NativeRenderFrameTicket>& ticket, std::uint32_t left, std::uint32_t right)
    {
        const auto result = Terminal(ticket);
        StartCopy();
        FinishCopy(result, left, right);
    }

    void RequestReleased()
    {
        auto released = std::make_shared<std::promise<void>>();
        auto ready    = released->get_future();
        auto armed    = std::make_shared<std::atomic<bool>>(true);
        detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, [released, armed]
                                                           {
                                                               if (armed->exchange(false))
                                                                   released->set_value();
                                                           });
        UNSCOPED_INFO("release worker-local canonical texture request owners");
        REQUIRE(ready.wait_for(kReadyCeiling) == std::future_status::ready);
        detail::NativeRenderFrameTestAccess::BeforeRequest(renderer, {});
    }
};

struct WeakCpuTextureOwners
{
    std::vector<std::weak_ptr<const NativeDrawReplayTextureVersion>> versions;
    std::vector<std::weak_ptr<const std::vector<std::uint8_t>>>      payloads;

    explicit WeakCpuTextureOwners(std::span<const NativeDrawReplayTexture> textures)
    {
        for (const auto& texture : textures)
        {
            versions.push_back(texture.textureVersion);
            payloads.push_back(texture.sharedBytes);
        }
    }

    bool Present() const
    {
        return std::all_of(versions.begin(), versions.end(), [](const auto& owner)
                           {
                               return !owner.expired();
                           }) &&
               std::all_of(payloads.begin(), payloads.end(), [](const auto& owner)
                           {
                               return !owner.expired();
                           });
    }

    bool Expired() const
    {
        return std::all_of(versions.begin(), versions.end(), [](const auto& owner)
                           {
                               return owner.expired();
                           }) &&
               std::all_of(payloads.begin(), payloads.end(), [](const auto& owner)
                           {
                               return owner.expired();
                           });
    }
};

NativeDrawReplayTexture canonicalCpuTexture(const std::array<std::uint8_t, 4>& color, std::uint32_t width = 1, std::uint32_t height = 1)
{
    NativeDrawReplayTexture texture{ {}, width, height, NativeDrawReplayTextureFormat::Rgba8 };
    texture.bytes.resize(static_cast<std::size_t>(width) * height * color.size());
    for (std::size_t offset = 0; offset < texture.bytes.size(); offset += color.size())
        std::memcpy(texture.bytes.data() + offset, color.data(), color.size());
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(CopyNativeDrawReplayTextureVersion(texture, error));
    return texture;
}

NativeRenderFrame canonicalCpuTextureFrame(const NativeDrawReplayTexture& texture, bool swizzle = false)
{
    auto left        = makeDraw({ 1, 1, 1, 1 }, true);
    left.sampleCount = left.sampleMask = 1;
    left.textureMask                   = 1;
    left.textures[0]                   = texture;
    left.pixelShaderDxil               = compile("Texture2D<float4> t:register(t0); float4 main():SV_Target0 { return t.Load(int3(0,0,0)); }", "ps_5_1");
    auto right                         = left;
    right.scissor                      = { 640, 0, 1280, 720 };
    if (swizzle)
        right.textures[0].swizzle = { 2, 1, 0, 3 };
    NativeRenderFrame frame;
    frame.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId = 1;
    frame.returnGpuOutput = true;
    frame.gammaTable      = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.passes          = { { false, { std::move(left), std::move(right) }, { frameResolve(1, 1280) } } };
    admitFrameDraws(frame);
    return frame;
}

void consumeCanonicalTexturePair(QueuedCpuTextureFixture& fixture,
                                 NativeRenderFrame        firstFrame,
                                 NativeRenderFrame        secondFrame,
                                 std::uint32_t            firstLeft,
                                 std::uint32_t            firstRight,
                                 std::uint32_t            secondLeft,
                                 std::uint32_t            secondRight)
{
    const auto          before = fixture.publications.load();
    CpuTextureTestGates release{ fixture.HoldNative() };
    const auto          first = fixture.Queue(std::move(firstFrame));
    fixture.Recorded(first);
    const auto second = fixture.Queue(std::move(secondFrame));
    fixture.Recorded(second);
    UNSCOPED_INFO("canonical CPU textures must occupy both actual contexts without early native or SDK completion");
    REQUIRE((first->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
             second->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
             fixture.publications.load() == before && release.native->GetCompletedValue() == 0));
    UNSCOPED_INFO("release canonical texture pair");
    REQUIRE(SUCCEEDED(release.native->Signal(1)));
    fixture.Consume(first, firstLeft, firstRight);
    fixture.Consume(second, secondLeft, secondRight);
    fixture.RequestReleased();
}

void checkQueuedCanonicalTextureOwners()
{
    QueuedCpuTextureFixture    fixture;
    const auto                 red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47), blue = packRgb10Output(17, 31, 919);
    std::array                 original{ canonicalCpuTexture({ 255, 0, 0, 255 }) };
    const WeakCpuTextureOwners oldOwners(original);
    auto                       first  = canonicalCpuTextureFrame(original[0]);
    auto                       second = canonicalCpuTextureFrame(original[0], true);
    original                          = {};
    consumeCanonicalTexturePair(fixture, std::move(first), std::move(second), red, red, red, blue);
    UNSCOPED_INFO("stored versions must not retain an owner the producer released");
    REQUIRE(oldOwners.Expired());

    std::array                 replacement{ canonicalCpuTexture({ 255, 0, 0, 255 }), canonicalCpuTexture({ 0, 255, 0, 255 }) };
    const WeakCpuTextureOwners replacedOwners(replacement);
    first       = canonicalCpuTextureFrame(replacement[0]);
    second      = canonicalCpuTextureFrame(replacement[1]);
    replacement = {};
    consumeCanonicalTexturePair(fixture, std::move(first), std::move(second), red, red, green, green);
    UNSCOPED_INFO("distinct canonical replacements must sample their own stored versions on both contexts");
    REQUIRE((oldOwners.Expired() && replacedOwners.Expired()));

    std::array                 grown{ canonicalCpuTexture({ 0, 0, 255, 255 }, 2, 2) };
    const WeakCpuTextureOwners grownOwners(grown);
    first  = canonicalCpuTextureFrame(grown[0]);
    second = canonicalCpuTextureFrame(grown[0]);
    grown  = {};
    consumeCanonicalTexturePair(fixture, std::move(first), std::move(second), blue, blue, blue, blue);
    UNSCOPED_INFO("queued layout growth must sample its stored version on both contexts");
    REQUIRE((replacedOwners.Expired() && grownOwners.Expired()));
    fixture.Close();
    UNSCOPED_INFO("healthy shutdown must release queued canonical cache owners");
    REQUIRE(grownOwners.Expired());
}

// Evicted textures sampled by queued frames return pending their fence.
void checkQueuedVersionTextureEviction()
{
    QueuedCpuTextureFixture fixture;
    const auto              red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47);
    std::array              textures{ canonicalCpuTexture({ 255, 0, 0, 255 }), canonicalCpuTexture({ 0, 255, 0, 255 }) };
    auto                    redFrame = canonicalCpuTextureFrame(textures[0]), greenFrame = canonicalCpuTextureFrame(textures[1]);
    CpuTextureTestGates     release{ fixture.HoldNative() };
    const auto              first = fixture.Queue(std::move(redFrame));
    fixture.Recorded(first);
    const auto stored = fixture.renderer.ReplayCacheStats();
    UNSCOPED_INFO("the held frame stores its version");
    REQUIRE((stored.versionTextureUploads && stored.versionTextureBytes));
    onRendererThread(fixture.renderer, [&]
                     {
                         detail::NativeRenderFrameTestAccess::SetVersionTextureCap(fixture.renderer, stored.versionTextureBytes);
                         return 0;
                     });
    const auto second = fixture.Queue(std::move(greenFrame));
    fixture.Recorded(second);
    const auto evicted = fixture.renderer.ReplayCacheStats();
    UNSCOPED_INFO("a version evicted under a held frame waits in the pool for that frame's fence");
    REQUIRE((evicted.versionTextureEvictions == stored.versionTextureEvictions + 1 &&
             evicted.texturePoolPendingBytes >= stored.texturePoolPendingBytes + stored.versionTextureBytes && release.native->GetCompletedValue() == 0));
    UNSCOPED_INFO("release the held version frames");
    REQUIRE(SUCCEEDED(release.native->Signal(1)));
    fixture.Consume(first, red, red);
    fixture.Consume(second, green, green);
    fixture.RequestReleased();
    fixture.Close();
}

std::vector<NativeDrawReplayTexture> narrowCanonicalTextures(std::size_t count, bool green)
{
    std::vector<NativeDrawReplayTexture> textures;
    textures.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        auto texture = canonicalCpuTexture(green ? std::array<std::uint8_t, 4>{ 0, 255, 0, 255 } : std::array<std::uint8_t, 4>{ 255, 0, 0, 255 }, 1, 4096);
        UNSCOPED_INFO("narrow fixture needs distinct canonical cache keys within each context");
        REQUIRE(std::none_of(textures.begin(), textures.end(), [&](const auto& prior)
                             {
                                 return prior.textureVersion == texture.textureVersion || prior.sharedBytes == texture.sharedBytes;
                             }));
        textures.push_back(std::move(texture));
    }
    return textures;
}

NativeRenderFrame narrowCanonicalTextureFrame(std::span<const NativeDrawReplayTexture> textures, bool green)
{
    UNSCOPED_INFO("supported narrow texture fixture cardinality");
    REQUIRE((textures.size() == 251 || textures.size() == 253));
    auto base        = makeDraw({ 1, 1, 1, 1 }, false);
    base.sampleCount = base.sampleMask = 1;
    const auto shader                  = [&](std::size_t count)
    {
        const auto value  = green ? "float4(0,1,0,1)" : "float4(1,0,0,1)";
        const auto source = "Texture2D<float4> t[" + std::to_string(count) + "]:register(t0);"
                                                                             "float4 main():SV_Target0 { bool ok=true; [unroll] for(int i=0;i<" +
                            std::to_string(count) + ";++i) { ok=ok && all(t[i].Load(int3(0,0,0))==" + value +
                            ") && all(t[i].Load(int3(0,4095,0))==" + value + "); } return ok?" + value + ":float4(1,0,1,1); }";
        return compile(source.c_str(), "ps_5_1");
    };
    const auto        fullShader = shader(kNativeDrawReplayTextureSlots);
    const auto        lastShader = shader(textures.size() % kNativeDrawReplayTextureSlots);
    NativeRenderFrame frame;
    frame.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId = 1;
    frame.returnGpuOutput = true;
    frame.gammaTable      = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.passes          = { { false, {}, { frameResolve(1, 1280) } } };
    for (std::size_t stripe = 0; stripe < 8; ++stripe)
    {
        auto       draw      = base;
        const auto offset    = stripe * kNativeDrawReplayTextureSlots;
        const auto count     = std::min<std::size_t>(kNativeDrawReplayTextureSlots, textures.size() - offset);
        draw.textureMask     = count == kNativeDrawReplayTextureSlots ? UINT32_MAX : (1U << count) - 1;
        draw.scissor         = { static_cast<std::int32_t>(stripe * 160), 0, static_cast<std::int32_t>((stripe + 1) * 160), 720 };
        draw.pixelShaderDxil = count == kNativeDrawReplayTextureSlots ? fullShader : lastShader;
        std::copy_n(textures.begin() + offset, count, draw.textures.begin());
        frame.passes[0].draws.push_back(std::move(draw));
    }
    admitFrameDraws(frame);
    return frame;
}

std::uint64_t narrowCanonicalMetadata(std::span<const NativeDrawReplayTexture> textures)
{
    std::uint64_t metadata = 0;
    for (const auto& texture : textures)
    {
        const auto& payload = texture.textureVersion->Payload();
        UNSCOPED_INFO("narrow canonical payload capacity must match the supported fixture forecast");
        REQUIRE((payload->size() == 16384 && payload->capacity() == payload->size()));
        metadata += texture.textureVersion->MetadataBytes();
    }
    return metadata;
}

void checkNarrowCanonicalFootprints(QueuedCpuTextureFixture& fixture, std::size_t count, std::uint64_t metadata)
{
    D3D12_RESOURCE_DESC description{};
    description.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width            = 1;
    description.Height           = 4096;
    description.DepthOrArraySize = description.MipLevels = description.SampleDesc.Count = 1;
    description.Format                                                                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64                             bytes = 0;
    fixture.provider->GetDevice()->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    UNSCOPED_INFO("narrow fixture requires the actual padded upload width, including its short last row");
    REQUIRE((bytes == 1048324 && footprint.Footprint.RowPitch == 256));
    constexpr std::uint64_t bound   = 512ULL * 1024 * 1024;
    const auto              uploads = 2 * count * bytes;
    // These frames have no initial CPU planes. Each context keeps the exact
    // eight-draw/resolve/terminal arena plus gamma and three helper constants.
    constexpr std::uint64_t otherUploads = 2 * (78592 + sizeof(std::array<std::uint32_t, 256>) + 3 * 256);
    UNSCOPED_INFO("narrow CPU pressure must leave the complete Upload domain within its original bound");
    REQUIRE(uploads + otherUploads < bound);
    // One preparation per context owns a description, three vector objects,
    // two totals and one footprint/row-count/row-width subresource.
    constexpr std::uint64_t preparationMetadata = 2 * (sizeof(D3D12_RESOURCE_DESC) + sizeof(std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>) +
                                                       sizeof(std::vector<UINT>) + sizeof(std::vector<UINT64>) + 2 * sizeof(UINT64) +
                                                       sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) + sizeof(UINT) + sizeof(UINT64));
    if (count == 251)
    {
        UNSCOPED_INFO("optional reservation fixture must fit two retained arrays while exceeding the old/new three-array union");
        REQUIRE((uploads + 2 * count * 16384 + metadata + preparationMetadata < bound && uploads + 3 * count * 16384 > bound));
    }
    else
    {
        UNSCOPED_INFO("shared narrow owners must fit while distinct owners exceed CpuTexture and the near-pressure progress precondition fits");
        REQUIRE((uploads + count * 16384 + metadata + preparationMetadata < bound && uploads + 2 * count * 16384 > bound &&
                 metadata + preparationMetadata < 225312));
    }
}

// The per-context CPU cache serves canonical versions only with the version
// store off, which these CpuTexture pressure fixtures need.
void usePerContextCanonicalCache(NativeRendererD3D12& renderer)
{
    onRendererThread(renderer, [&]
                     {
                         detail::NativeRenderFrameTestAccess::SetVersionTextureStore(renderer, false);
                         return 0;
                     });
}

void checkUnavailableCanonicalTextureReservation()
{
    QueuedCpuTextureFixture fixture;
    usePerContextCanonicalCache(fixture.renderer);
    auto                       oldA = narrowCanonicalTextures(251, false), oldB = narrowCanonicalTextures(251, false), replacement = narrowCanonicalTextures(251, true);
    const WeakCpuTextureOwners ownersA(oldA), ownersB(oldB), ownersC(replacement);
    checkNarrowCanonicalFootprints(fixture, 251, narrowCanonicalMetadata(oldA) + narrowCanonicalMetadata(oldB) + narrowCanonicalMetadata(replacement));
    auto first = narrowCanonicalTextureFrame(oldA, false), second = narrowCanonicalTextureFrame(oldB, false);
    oldA.clear();
    oldB.clear();
    const auto red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47);
    consumeCanonicalTexturePair(fixture, std::move(first), std::move(second), red, red, red, red);
    UNSCOPED_INFO("both idle narrow CPU caches must retain their actual canonical owners");
    REQUIRE((ownersA.Present() && ownersB.Present()));
    auto next = narrowCanonicalTextureFrame(replacement, true);
    replacement.clear();
    const auto          before = fixture.publications.load();
    CpuTextureTestGates release{ fixture.HoldNative() };
    const auto          ticket = fixture.Queue(std::move(next));
    fixture.Recorded(ticket);
    fixture.RequestReleased();
    UNSCOPED_INFO("optional reservation rejection must preserve the successful original replacement path without waiting or evicting the other idle context");
    REQUIRE((ticket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
             fixture.publications.load() == before && release.native->GetCompletedValue() == 0 && ownersA.Expired() && ownersB.Present() && ownersC.Present()));
    UNSCOPED_INFO("release optional-reservation replacement");
    REQUIRE(SUCCEEDED(release.native->Signal(1)));
    fixture.Consume(ticket, green, green);
    fixture.RequestReleased();
    UNSCOPED_INFO("optional reservation failure must preserve idle and replacement cache ownership");
    REQUIRE((ownersB.Present() && ownersC.Present()));
    fixture.Close();
    UNSCOPED_INFO("optional fallback cache owners must retire on healthy shutdown");
    REQUIRE((ownersB.Expired() && ownersC.Expired()));
}

void checkCanonicalTexturePressure()
{
    const auto red = packRgb10Output(311, 31, 47), green = packRgb10Output(17, 617, 47);
    for (bool shared : { true, false })
    {
        QueuedCpuTextureFixture fixture;
        usePerContextCanonicalCache(fixture.renderer);
        auto                       oldA = narrowCanonicalTextures(253, false);
        auto                       next = shared ? oldA : narrowCanonicalTextures(253, true);
        const WeakCpuTextureOwners ownersA(oldA), ownersB(next);
        checkNarrowCanonicalFootprints(fixture, 253, narrowCanonicalMetadata(oldA) + (shared ? 0 : narrowCanonicalMetadata(next)));
        auto firstFrame = narrowCanonicalTextureFrame(oldA, false), secondFrame = narrowCanonicalTextureFrame(next, !shared);
        oldA.clear();
        next.clear();
        if (shared)
        {
            consumeCanonicalTexturePair(fixture, std::move(firstFrame), std::move(secondFrame), red, red, red, red);
            UNSCOPED_INFO("shared narrow canonical owners must remain charged once across two idle contexts");
            REQUIRE(ownersA.Present());
        }
        else
        {
            std::shared_ptr<NativeRenderFrameTicket> second;
            std::string                              admissionError;
            std::future<bool>                        admission;
            // Future and referenced state precede the guard, so failure releases
            // actual native/copy waits before a pending async future can join.
            CpuTextureTestGates release;
            release.native   = fixture.HoldNative();
            release.copy     = fixture.Fence();
            const auto first = fixture.Queue(std::move(firstFrame));
            fixture.Recorded(first);
            admission = std::async(std::launch::async, [&fixture, &second, &admissionError, frame = std::move(secondFrame)]() mutable
                                   {
                                       return fixture.renderer.QueueRenderFrame(std::move(frame), second, admissionError);
                                   });
            UNSCOPED_INFO("distinct canonical texture admission must finish within its bounded wait");
            REQUIRE(admission.wait_for(kReadyCeiling) == std::future_status::ready);
            UNSCOPED_INFO(admissionError);
            REQUIRE(admission.get());
            const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
            while (fixture.renderer.ReplayCacheStats().textureUploadCreations < 2 * 253 - 2 && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            UNSCOPED_INFO("distinct narrow owners must reach actual CpuTexture growth pressure and retain the held first terminal");
            REQUIRE((fixture.renderer.ReplayCacheStats().textureUploadCreations >= 2 * 253 - 2 &&
                     second->Recording().wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout &&
                     first->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
                     release.native->GetCompletedValue() == 0 && ownersA.Present()));
            UNSCOPED_INFO("release distinct canonical texture pressure");
            REQUIRE(SUCCEEDED(release.native->Signal(1)));
            const auto firstDone = fixture.Terminal(first);
            auto*      copyQueue = rex::ui::d3d12::detail::GpuGuestOutputTestAccess::CopyQueue(*fixture.presenter);
            UNSCOPED_INFO("hold SDK copy while native CPU cache pressure resumes");
            REQUIRE((copyQueue && SUCCEEDED(copyQueue->Wait(release.copy.Get(), 1))));
            fixture.StartCopy();
            fixture.Recorded(second);
            fixture.RequestReleased();
            UNSCOPED_INFO("native pressure must release retired CPU owners while the exact SDK output copy remains pending");
            REQUIRE((ownersA.Expired() && ownersB.Present() &&
                     NativeRendererD3D12::PollGpuOutput(firstDone.gpuOutputTicket) == NativeRendererD3D12::GpuOutputStatus::Pending));
            UNSCOPED_INFO("release canonical texture SDK copy");
            REQUIRE(SUCCEEDED(release.copy->Signal(1)));
            fixture.FinishCopy(firstDone, red, red);
            fixture.Consume(second, green, green);
            fixture.RequestReleased();

            std::array fresh{ canonicalCpuTexture({ 0, 255, 0, 255 }) };
            auto       frame  = canonicalCpuTextureFrame(fresh[0]);
            fresh             = {};
            const auto ticket = fixture.Queue(std::move(frame));
            fixture.Recorded(ticket);
            fixture.Consume(ticket, green, green);
            fixture.RequestReleased();
        }
        fixture.Close();
        UNSCOPED_INFO("pressure and shared-context owners must retire on healthy shutdown");
        REQUIRE((ownersA.Expired() && ownersB.Expired()));
    }
}

void checkRenderFrame(const std::filesystem::path& directory)
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 },
                                   { 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 2;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    auto red                   = makeDraw({ 1, 0, 0, 1 }, true);
    auto blue                  = makeDraw({ 0, 0, 1, 1 }, true);
    red.width = blue.width    = 640;
    auto leftResolve          = frameResolve(1, 640);
    leftResolve.samples       = { 0, 3 };
    auto rightResolve         = leftResolve;
    rightResolve.destinationX = 640;
    auto compose              = makeDraw({ 1, 1, 1, 1 }, false);
    compose.sampleCount = compose.sampleMask = 1;
    compose.textureMask                      = 1;
    compose.textures[0]                      = frameTexture(1, NativeDrawReplayTextureFormat::Rgba8);
    compose.pixelShaderDxil                  = compile(
        "Texture2D<float4> source_texture:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return source_texture.Load(int3(int2(p.xy),0)); }",
        "ps_5_1");
    frame.passes = { { false, { red }, { leftResolve } },
                     { false, { blue }, { rightResolve } },
                     { false, { compose }, { frameResolve(2, 1280) } } };
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(frame, error));
    auto incomplete = frame;
    incomplete.passes[1].resolves.clear();
    UNSCOPED_INFO("sampling an unresolved half must fail before GPU work");
    REQUIRE(!ValidateNativeRenderFrame(incomplete, error));
    auto incompatible                                 = frame;
    incompatible.passes[2].draws[0].textures[0].width = 640;
    UNSCOPED_INFO("resolved resource descriptor mismatch must fail");
    REQUIRE(!ValidateNativeRenderFrame(incompatible, error));
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue));
    checkGammaPixels(result.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(17, 31, 919));
    const auto priorPixels   = result.resolvedOutput;
    const auto priorFence    = result.completionFenceValue;
    auto       rgb10         = frame;
    rgb10.textures[0].format = NativeDrawReplayTextureFormat::Rgb10a2;
    for (unsigned half = 0; half < 2; ++half)
    {
        auto& draw           = rgb10.passes[half].draws[0];
        draw.targetFormat    = NativeDrawReplayTargetFormat::Rgb10a2;
        draw.pixelShaderDxil = compile(
            half == 0 ? "float4 main():SV_Target0 { return float4(257.0/1023.0,513.0/1023.0,769.0/1023.0,1.0/3.0); }"
                      : "float4 main():SV_Target0 { return float4(769.0/1023.0,257.0/1023.0,513.0/1023.0,2.0/3.0); }",
            "ps_5_1");
    }
    rgb10.passes[2].draws[0].textures[0].format = NativeDrawReplayTextureFormat::Rgb10a2;
    rgb10.passes[2].draws[0].pixelShaderDxil    = compile(
        "Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 {"
        "float4 v=t.Load(int3(int2(p.xy),0)); float4 expected=p.x<640?"
        "float4(257.0/1023.0,513.0/1023.0,769.0/1023.0,1.0/3.0):"
        "float4(769.0/1023.0,257.0/1023.0,513.0/1023.0,2.0/3.0);"
        "return all(abs(v-expected)<0.00001)?float4(0,1,0,1):float4(1,0,0,1); }",
        "ps_5_1");
    result = renderer.SubmitRenderFrame(rgb10);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 617, 47), packRgb10Output(17, 617, 47));
    auto halfUp                               = frame;
    halfUp.passes[0].draws[0].pixelShaderDxil = compile(
        "float4 main(uint sample:SV_SampleIndex):SV_Target0 { return float4(sample==3?1.0/255.0:0,0,0,1); }", "ps_5_1");
    result = renderer.SubmitRenderFrame(halfUp);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output((frame.gammaTable[1] >> 20) & 1023, 31, 47), packRgb10Output(17, 31, 919));
    frame.outputSwizzle = { 2, 1, 0, 3 };
    result              = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue > priorFence));
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 31, 919), packRgb10Output(311, 31, 47));
    checkGammaPixels(priorPixels, packRgb10Output(311, 31, 47), packRgb10Output(17, 31, 919));
    result = renderer.SubmitDrawFrame({ makeDraw({ 0, 1, 0, 1 }, false) }, directory / "after-render-frame.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "after-render-frame.rgba", { 0, 255, 0, 255 }, { 0, 255, 0, 255 });
    renderer.Shutdown();

    auto depth          = makeDraw({ 1, 0, 0, 1 }, false);
    depth.depth.enabled = depth.depth.writeEnabled = true;
    depth.pixelShaderDxil                          = compile(
        "struct O { float4 c:SV_Target0; float z:SV_Depth; };"
        "O main(uint sample:SV_SampleIndex) { O o; o.c=float4(1,0,0,1);"
        "o.z=sample==3?0.75:0.25; return o; }",
        "ps_5_1");
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        const float z = 0.25F;
        std::memcpy(depth.vertexData.data() + vertex * depth.vertexStrideBytes + 8, &z, sizeof(z));
    }
    auto signedDraw        = makeDraw({ 0.25F, -0.5F, 0, 1 }, false);
    signedDraw.sampleCount = signedDraw.sampleMask = 1;
    signedDraw.targetFormat                        = NativeDrawReplayTargetFormat::Rg16Snorm;
    auto depthResolve                              = frameResolve(1, 1280);
    depthResolve.depth                             = true;
    depthResolve.samples                           = { 0, 0 };
    auto signedResolve                             = frameResolve(2, 1280);
    signedResolve.valueScale                       = 32;
    compose.textureMask                            = 3;
    compose.textures[0]                            = frameTexture(1, NativeDrawReplayTextureFormat::R32Float);
    compose.textures[1]                            = frameTexture(2, NativeDrawReplayTextureFormat::Rg16Float);
    // The k_16_16 resolve scales 0.25 to 8 and clamps -0.5's -16 to -1.0, as
    // the SDK's resolve_full_32bpp does.
    compose.pixelShaderDxil = compile(
        "Texture2D<float4> d:register(t0); Texture2D<float4> s:register(t1);"
        "float4 main(float4 p:SV_Position):SV_Target0 { int3 xy=int3(int2(p.xy),0);"
        "bool ok=abs(d.Load(xy).r-0.25)<0.000001 && all(abs(s.Load(xy).rg-float2(8,-1))<0.01);"
        "return ok?float4(0,1,0,1):float4(1,0,0,1); }",
        "ps_5_1");
    frame.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::R32Float },
                              { 2, 1280, 720, NativeDrawReplayTextureFormat::Rg16Float },
                              { 3, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.passes          = { { false, { depth }, { depthResolve } },
                              { false, { signedDraw }, { signedResolve } },
                              { false, { compose }, { frameResolve(3, 1280) } } };
    frame.outputTextureId = 3;
    frame.outputSwizzle   = { 0, 1, 2, 3 };
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 617, 47), packRgb10Output(17, 617, 47));
    renderer.Shutdown();
}

void checkFrameCommandOwnership()
{
    // Draws fill 16-pixel cells, so three owner batches and one more draw fit the target.
    const std::size_t     commandDraws = detail::NativeRenderFrameTestAccess::FrameCommandDraws();
    const std::size_t     drawCount    = commandDraws * 3 + 1;
    constexpr std::size_t cell = 16, cellsPerRow = 1280 / cell;
    UNSCOPED_INFO("an owner batch holds more draws than one per-draw sampler heap allowed");
    REQUIRE((commandDraws > 31 && drawCount <= cellsPerRow * (720 / cell)));
    auto base        = makeDraw({ 1, 1, 1, 1 }, false);
    base.sampleCount = base.sampleMask = 1;
    base.textureMask                   = 1;
    base.vertexShaderDxil              = compile(
        "cbuffer Vertex : register(b0,space4) { float4 vc[256]; };"
        "struct V { float4 p:SV_Position; float4 c:COLOR0; };"
        "V main(float4 p:TEXCOORD0,float4 c:TEXCOORD1) { V v; v.p=p; v.c=c*vc[0]; return v; }",
        "vs_5_1");
    base.pixelShaderDxil = compile(
        "cbuffer Pixel : register(b1,space4) { float4 pc[224]; };"
        "cbuffer Shared : register(b2,space4) { float4 sc[33]; };"
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target0 {"
        "return float4(c.r,pc[0].g,t.SampleLevel(s,float2(2,2),0).b*sc[0].b,1); }",
        "ps_5_1");
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.passes               = { { false, {}, { frameResolve(1, 1280) } } };
    std::vector<std::uint32_t> expected;
    for (std::size_t index = 0; index < drawCount; ++index)
    {
        auto                       draw = base;
        const bool                 r = index & 1, g = index & 2, b = index & 4, border = index & 8;
        const std::array<float, 4> vertex{ r ? 1.0F : 0.0F, 1, 1, 1 };
        const std::array<float, 4> pixel{ 0, g ? 1.0F : 0.0F, 0, 0 };
        const std::array<float, 4> shared{ 0, 0, b ? 1.0F : 0.0F, 0 };
        std::memcpy(draw.vertexConstants.data(), vertex.data(), sizeof(vertex));
        std::memcpy(draw.pixelConstants.data(), pixel.data(), sizeof(pixel));
        std::memcpy(draw.sharedConstants.data(), shared.data(), sizeof(shared));
        draw.textures[0]             = { {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
        draw.textures[0].sharedBytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{ 0, 0, 255, 255 });
        draw.sampler.address         = border ? std::array<std::uint32_t, 3>{ 4, 4, 4 } : std::array<std::uint32_t, 3>{ 1, 1, 1 };
        draw.sampler.border          = { 0, 0, 0, 1 };
        const auto left = static_cast<std::int32_t>(index % cellsPerRow * cell), top = static_cast<std::int32_t>(index / cellsPerRow * cell);
        draw.scissor = { left, top, left + static_cast<std::int32_t>(cell), top + static_cast<std::int32_t>(cell) };
        frame.passes[0].draws.push_back(std::move(draw));
        expected.push_back(packRgb10Output(r ? 311 : 17, g ? 617 : 31, b && !border ? 919 : 47));
    }
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto first = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(first.error);
    REQUIRE((first.success && first.completionFenceValue));
    for (std::size_t y = 0; y < 720; ++y)
        for (std::size_t x = 0; x < 1280; ++x)
        {
            std::uint32_t word = 0;
            std::memcpy(&word, first.resolvedOutput.data() + (y * 1280 + x) * 4, sizeof(word));
            const auto index = y / cell * cellsPerRow + x / cell;
            UNSCOPED_INFO("command-owner upload or descriptor reuse changed an earlier draw");
            REQUIRE(word == (index < expected.size() ? expected[index] : packRgb10Output(17, 31, 47)));
        }
    // Draws, the resolve and gamma fill four owner batches; the first three end at the draw bound.
    UNSCOPED_INFO("draws, resolve and gamma must share bounded command owners");
    REQUIRE((renderer.ReplayCounts().queueSubmissions == 4 && renderer.ReplayCounts().chunkSubmissions == 3 &&
             renderer.ReplayCacheStats().samplerTableFlushes == 0));
    const auto heaps    = renderer.ReplayCacheStats().descriptorHeapCreations;
    const auto repeated = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("completed owners must reuse bounded descriptor storage across frames");
    REQUIRE((repeated.success && repeated.resolvedOutput == first.resolvedOutput &&
             renderer.ReplayCacheStats().descriptorHeapCreations == heaps));
    const auto draws = std::move(frame.passes[0].draws);
    frame.passes.clear();
    for (const auto& draw : draws)
        frame.passes.push_back({ !frame.passes.empty(), { draw }, {} });
    frame.passes.back().resolves = { frameResolve(1, 1280) };
    const auto before            = renderer.ReplayCounts().queueSubmissions;
    const auto acrossPasses      = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("pass boundaries must preserve frame ownership and exact upload/descriptor contents");
    REQUIRE((acrossPasses.success && acrossPasses.resolvedOutput == first.resolvedOutput &&
             renderer.ReplayCounts().queueSubmissions - before == 4));
    renderer.Shutdown();
}

// Frame draws deduplicate sampler tables; six states expose wrong-table errors.
void checkFrameSamplerTables()
{
    // Mip 0 is red in its left column and green in its right; mip 1 is blue.
    NativeDrawReplayTexture texture{ {}, 2, 2, NativeDrawReplayTextureFormat::Rgba8 };
    texture.mipLevels   = 2;
    texture.sharedBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{ 255, 0, 0, 255, 0, 255, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255 });
    const auto state = [](std::uint32_t address, bool linear, float lod, std::array<float, 4> border)
    {
        NativeDrawReplaySampler sampler;
        sampler.address   = { address, address, address };
        sampler.minLinear = sampler.magLinear = linear;
        sampler.minLod = sampler.maxLod = lod;
        sampler.border                  = border;
        return sampler;
    };
    // Wrap and clamp, point and linear, two border colors, and a minimum LOD.
    const std::array<NativeDrawReplaySampler, 6> states{ state(1, false, 0, {}), state(1, true, 0, {}), state(3, false, 0, {}), state(4, false, 0, { 1, 1, 1, 1 }), state(4, false, 0, { 0, 0, 0, 1 }), state(1, false, 1, {}) };
    auto                                         base = makeDraw({ 1, 1, 1, 1 }, false);
    base.sampleCount = base.sampleMask = 1;
    base.textureMask                   = 1;
    base.textures[0]                   = texture;
    base.pixelShaderDxil               = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main(float4 p:SV_Position):SV_Target0 {"
        "float4 a=t.SampleLevel(s,float2(1.25,0.25),0), b=t.SampleLevel(s,float2(1.5,0.25),0);"
        "return float4(a.r,b.g,a.b,1); }",
        "ps_5_1");
    constexpr std::size_t cell = 16, cellsPerRow = 1280 / cell;
    const auto            frameOf = [&](const std::vector<NativeDrawReplaySampler>& samplers, bool cells)
    {
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
        frame.outputTextureId      = 1;
        frame.returnResolvedOutput = true;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        frame.passes               = { { false, {}, { frameResolve(1, 1280) } } };
        for (std::size_t index = 0; index < samplers.size(); ++index)
        {
            auto draw    = base;
            draw.sampler = samplers[index];
            if (cells)
            {
                const auto left = static_cast<std::int32_t>(index % cellsPerRow * cell), top = static_cast<std::int32_t>(index / cellsPerRow * cell);
                draw.scissor = { left, top, left + static_cast<std::int32_t>(cell), top + static_cast<std::int32_t>(cell) };
            }
            frame.passes[0].draws.push_back(std::move(draw));
        }
        return frame;
    };
    const auto wordAt = [](const NativeDrawReplayResult& result, std::size_t x, std::size_t y)
    {
        std::uint32_t word = 0;
        std::memcpy(&word, result.resolvedOutput.data() + (y * 1280 + x) * 4, sizeof(word));
        return word;
    };
    std::string error;
    std::size_t bytes = 0, subresources = 0;
    UNSCOPED_INFO("the sampled texture packs both mips");
    REQUIRE((SizeNativeDrawReplayTextureLayout({ 2, 2, NativeDrawReplayTextureFormat::Rgba8, 2 }, bytes, subresources) && bytes == texture.sharedBytes->size()));

    // Each state alone in its own frame gives that state's color.
    std::array<std::uint32_t, states.size()> colors{};
    {
        NativeRendererD3D12 reference;
        UNSCOPED_INFO(error);
        REQUIRE(reference.StartDrawStream(error));
        for (std::size_t index = 0; index < states.size(); ++index)
        {
            const auto result = reference.SubmitRenderFrame(frameOf({ states[index] }, false));
            UNSCOPED_INFO(result.error);
            REQUIRE(result.success);
            colors[index] = wordAt(result, 0, 0);
            for (std::size_t other = 0; other < index; ++other)
            {
                UNSCOPED_INFO(std::format("sampler states {} and {} draw the same color", other, index));
                REQUIRE(colors[other] != colors[index]);
            }
        }
        reference.Shutdown();
    }
    const auto checkCells = [&](const NativeDrawReplayResult& result, const std::vector<std::size_t>& order, const char* message)
    {
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        for (std::size_t y = 0; y < 720; ++y)
            for (std::size_t x = 0; x < 1280; ++x)
            {
                const auto index = y / cell * cellsPerRow + x / cell;
                UNSCOPED_INFO(message);
                REQUIRE(wordAt(result, x, y) == (index < order.size() ? colors[order[index]] : packRgb10Output(17, 31, 47)));
            }
    };

    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    // Repeats of the previous table and returns to earlier ones; the
    // presentation's default clamp sampler repeats the third state.
    constexpr std::array<std::size_t, 10> pattern{ 0, 0, 1, 0, 2, 3, 3, 4, 5, 1 };
    std::vector<std::size_t>              order;
    std::vector<NativeDrawReplaySampler>  samplers;
    for (std::size_t index = 0; index < 120; ++index)
    {
        order.push_back(pattern[index % pattern.size()]);
        samplers.push_back(states[order.back()]);
    }
    auto       before = renderer.ReplayCacheStats();
    const auto shared = renderer.SubmitRenderFrame(frameOf(samplers, true));
    checkCells(shared, order, "a draw sampled through another draw's sampler table");
    auto after = renderer.ReplayCacheStats();
    UNSCOPED_INFO("an owner writes each distinct sampler table once");
    REQUIRE((after.samplerTables - before.samplerTables == states.size() && after.samplerTableHits - before.samplerTableHits == samplers.size() + 1 - states.size() &&
             after.samplerTableFlushes == before.samplerTableFlushes));

    // Distinct maximum LODs above the texture's mips make every table new, so
    // the owner heaps fill and their batches end early.
    order.clear();
    samplers.clear();
    for (std::size_t index = 0; index < 2500; ++index)
    {
        order.push_back(index % states.size());
        samplers.push_back(states[order.back()]);
        samplers.back().maxLod = 2.0F + static_cast<float>(index);
    }
    before               = renderer.ReplayCacheStats();
    const auto submitted = renderer.ReplayCounts().queueSubmissions;
    const auto distinct  = renderer.SubmitRenderFrame(frameOf(samplers, true));
    checkCells(distinct, order, "a full sampler heap changed a recorded table");
    after = renderer.ReplayCacheStats();
    UNSCOPED_INFO("full sampler heaps end their batches and start over after the owner's fence");
    REQUIRE((after.samplerTables - before.samplerTables >= samplers.size() && after.samplerTableFlushes - before.samplerTableFlushes >= 2 &&
             renderer.ReplayCounts().queueSubmissions - submitted >= 3));
    renderer.Shutdown();
}

void checkRecordedFramePrefix()
{
    auto red           = makeDraw({ 1, 0, 0, 1 }, true);
    auto green         = makeDraw({ 0, 1, 0, 1 }, false);
    green.scissor.left = 640;
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 717 } };
    frame.outputTextureId      = 1;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    auto right                 = frameResolve(1, 640, 640);
    right.sourceX              = 640;
    // One more draw than an owner batch holds puts the prefix in both owners.
    const auto prefixDraws = detail::NativeRenderFrameTestAccess::FrameCommandDraws() + 1;
    frame.passes           = { { false, std::vector<NativeDrawReplayRecipe>(prefixDraws, red), { frameResolve(1, 640) } },
                               { true, { green }, { right } } };
    for (std::size_t index = 0; index < prefixDraws; ++index)
        placeInCell(frame.passes.front().draws[index], index);
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto expected = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(expected.error);
    REQUIRE((expected.success && expected.completionFenceValue));
    auto makePrefix = [&]()
    {
        NativeRenderFrame prefix;
        prefix.textures = frame.textures;
        prefix.passes   = { frame.passes.front() };
        return prefix;
    };
    auto prefix = makePrefix();
    UNSCOPED_INFO("a sealed prefix must admit partial strips without a completed output claim");
    REQUIRE((ValidateNativeRenderFramePrefix(prefix, error) && !ValidateNativeRenderFrame(prefix, error)));
    auto*                                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Fence>  gate;
    UNSCOPED_INFO("native queue gate");
    REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))));

    struct ReleaseGate
    {
        ID3D12Fence*  gate;
        std::uint64_t value = 1;

        ~ReleaseGate()
        {
            gate->Signal(value);
        }
    } release{ gate.Get() };

    UNSCOPED_INFO("hold actual native prefix execution");
    REQUIRE(SUCCEEDED(queue->Wait(gate.Get(), 1)));
    const auto before = renderer.ReplayCounts();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.RecordRenderFramePrefix(prefix, error));
    const auto submitted = renderer.ReplayCounts();
    UNSCOPED_INFO("prefix recording must submit both descriptor owners before terminal completion");
    REQUIRE((submitted.recordedDraws - before.recordedDraws == prefixDraws && submitted.queueSubmissions - before.queueSubmissions == 2 &&
             gate->GetCompletedValue() == 0));
    UNSCOPED_INFO("an unfinished prefix must reject another prefix and standalone storage mutation");
    REQUIRE((!renderer.RecordRenderFramePrefix(prefix, error) && !renderer.SubmitDrawFrame({ red }).success));
    for (auto& draw : prefix.passes.front().draws)
    {
        std::fill(draw.vertexData.begin(), draw.vertexData.end(), 0);
        std::fill(draw.vertexConstants.begin(), draw.vertexConstants.end(), 0xFF);
        draw.vertexShaderDxil.clear();
    }
    prefix              = {};
    auto       terminal = std::async(std::launch::async, [&]()
                                     {
                                   return renderer.SubmitRenderFrame(tailOf(frame, 1));
                                     });
    const bool waited   = terminal.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    UNSCOPED_INFO("release native prefix");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    const auto completed = terminal.get();
    UNSCOPED_INFO("terminal storage reuse must await the exact prefix fence and preserve its immutable bytes");
    REQUIRE((waited && completed.success && completed.completionFenceValue > expected.completionFenceValue &&
             completed.resolvedOutput == expected.resolvedOutput));
    UNSCOPED_INFO("terminal submission must record the remaining draw and gamma without replaying the prefix");
    REQUIRE(renderer.ReplayCounts().recordedDraws - submitted.recordedDraws == 2);
    renderer.Shutdown();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    device.Reset();
    UNSCOPED_INFO("restart native gate");
    REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)))));
    release.gate      = gate.Get();
    auto admittedStop = makePrefix();
    admitFrameDraws(admittedStop);
    std::weak_ptr<const NativeDrawReplayAdmission> stoppedOwner = admittedStop.passes.front().admittedDraws.front();
    UNSCOPED_INFO(error);
    REQUIRE((SUCCEEDED(queue->Wait(gate.Get(), 1)) && renderer.RecordRenderFramePrefix(admittedStop, error)));
    auto       stopped = std::async(std::launch::async, [&]()
                                    {
                                  renderer.Shutdown();
                                    });
    const bool drained = stopped.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
    // The test's packet, the prefix state and the renderer's held pass.
    const bool retained = stoppedOwner.use_count() == 3;
    UNSCOPED_INFO("release stopped prefix");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    stopped.get();
    UNSCOPED_INFO("shutdown must retain admitted canonical and prefix owners until unfinished native work drains");
    REQUIRE((drained && retained && !renderer.Initialized()));
    UNSCOPED_INFO("native shutdown must release its admitted prefix ownership after drain");
    REQUIRE(stoppedOwner.use_count() == 1);
    admittedStop = {};
    UNSCOPED_INFO("stopped canonical draw must retire when planning tears down");
    REQUIRE(stoppedOwner.expired());
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto restartedFrame = frame;
    admitFrameDraws(restartedFrame);
    const auto restarted = renderer.SubmitRenderFrame(std::move(restartedFrame));
    UNSCOPED_INFO("a discarded prefix must not affect a restarted renderer");
    REQUIRE((restarted.success && restarted.resolvedOutput == expected.resolvedOutput));
    auto signedPrefix               = makePrefix();
    signedPrefix.passes[0].target   = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rg16Snorm };
    signedPrefix.textures[0].format = NativeDrawReplayTextureFormat::Rg16Float;
    for (auto& draw : signedPrefix.passes[0].draws)
        draw.targetFormat = NativeDrawReplayTargetFormat::Rg16Snorm;
    signedPrefix.passes[0].resolves[0].valueScale = 32;
    UNSCOPED_INFO("a k_16_16 pass opens a prefix: " + error);
    REQUIRE(ValidateNativeRenderFramePrefix(signedPrefix, error));
    auto aliased                          = makePrefix();
    auto aliasPass                        = aliased.passes[0];
    aliasPass.colorSourcePass             = 1;
    aliasPass.preservePreviousAttachments = true;
    aliased.passes.push_back(std::move(aliasPass));
    UNSCOPED_INFO("valid raw color alias passes must stay on the terminal graph path");
    REQUIRE((!renderer.RecordRenderFramePrefix(std::move(aliased), error) && error.find("color aliasing") != std::string::npos));
    prefix = makePrefix();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.RecordRenderFramePrefix(prefix, error));
    const auto rejected = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("a complete graph cannot end a recorded chain, and fails before output or version publication: " + rejected.error);
    REQUIRE((!rejected.success && !rejected.completionFenceValue && !rejected.presenterToken &&
             rejected.error.find("as a tail") != std::string::npos));
    renderer.Shutdown();
    UNSCOPED_INFO("a rejected terminal graph must preserve the renderer's terminal failure state");
    REQUIRE(!renderer.StartDrawStream(error));
}

// k_16_16 prefix resolves feed the tail and match the complete graph's pixels.
void checkSignedPrefixChain()
{
    NativeRenderFrame full;
    full.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 821 }, { 2, 1280, 720, NativeDrawReplayTextureFormat::Rg16Float } };
    full.outputTextureId      = 1;
    full.returnResolvedOutput = true;
    full.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    NativeRenderFramePass fog;
    fog.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rg16Snorm };
    NativeRenderFramePass::Clear clear;
    clear.rectangle  = { 0, 0, 1280, 720 };
    clear.color      = true;
    clear.colorValue = { 0.125F, -0.25F, 0, 0 };
    fog.clears.push_back(clear);
    auto fogDraw        = makeDraw({ 0.25F, -0.5F, 0, 1 }, true);
    fogDraw.sampleCount = fogDraw.sampleMask = 1;
    fogDraw.targetFormat                     = NativeDrawReplayTargetFormat::Rg16Snorm;
    fog.draws.push_back(fogDraw);
    auto fogResolve       = frameResolve(2, 1280);
    fogResolve.valueScale = 32;
    fog.resolves.push_back(fogResolve);
    // Codes times 32: red 8 on the left and 4 on the right; green clamps at -1.
    auto read        = makeDraw({ 1, 1, 1, 1 }, false);
    read.sampleCount = read.sampleMask = 1;
    read.textureMask                   = 1;
    read.textures[0]                   = frameTexture(2, NativeDrawReplayTextureFormat::Rg16Float);
    read.pixelShaderDxil               = compile(
        "Texture2D<float4> fog:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { float4 v=fog.Load(int3(p.xy,0)); return float4(saturate(v.r/8),saturate(-v.g),0,1); }",
        "ps_5_1");
    NativeRenderFramePass presented;
    presented.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rgba8 };
    presented.draws  = { read };
    presented.resolves.push_back(frameResolve(1, 1280));
    full.passes = { fog, presented };
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(full, error));

    NativeRendererD3D12 reference;
    UNSCOPED_INFO(error);
    REQUIRE(reference.StartDrawStream(error));
    const auto expected = reference.SubmitRenderFrame(full);
    reference.Shutdown();
    UNSCOPED_INFO(expected.error);
    REQUIRE((expected.success && expected.resolvedOutput.size() == 1280U * 720U * 4U));
    std::uint32_t left = 0, right = 0;
    std::memcpy(&left, expected.resolvedOutput.data() + (360 * 1280 + 320) * 4, sizeof(left));
    std::memcpy(&right, expected.resolvedOutput.data() + (360 * 1280 + 960) * 4, sizeof(right));
    UNSCOPED_INFO("the fog draw and its clear read back apart");
    REQUIRE(left != right);

    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    NativeRenderFrame opening;
    opening.textures = full.textures;
    opening.passes   = { fog };
    std::shared_future<NativeDrawReplayResult> recording;
    UNSCOPED_INFO("a k_16_16 pass opens a chain: " + error);
    REQUIRE(renderer.QueueRenderFramePasses(std::move(opening), recording, error));
    UNSCOPED_INFO("the k_16_16 packet records: " + recording.get().error);
    REQUIRE((recording.wait_for(kReadyCeiling) == std::future_status::ready && recording.get().success));
    const auto joined = renderer.SubmitRenderFrame(tailOf(full, 1));
    UNSCOPED_INFO("a tail samples its k_16_16 packet's float16 resolve: " + joined.error);
    REQUIRE((joined.success && joined.resolvedOutput == expected.resolvedOutput));
    renderer.Shutdown();
}

void checkAppendedFramePasses()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 811 } };
    frame.outputTextureId      = 1;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    auto       red             = makeDraw({ 1, 0, 0, 1 }, true);
    auto       green           = makeDraw({ 0, 1, 0, 1 }, false);
    const auto sampleShader    = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return t.Sample(s,p.xy/float2(1280,720)); }",
        "ps_5_1");
    red.pixelShaderDxil = green.pixelShaderDxil = sampleShader;
    red.textureMask = green.textureMask = 1;
    red.textures[0]                     = { { 255, 0, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    green.textures[0]                   = { { 0, 255, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    auto right                          = frameResolve(1, 640, 640);
    right.sourceX                       = 640;
    // Each packet fills an owner batch and spills into the other one.
    const auto packetDraws = detail::NativeRenderFrameTestAccess::FrameCommandDraws() + 1;
    frame.passes           = { { false, std::vector<NativeDrawReplayRecipe>(packetDraws, red), { frameResolve(1, 640) } },
                               { true, std::vector<NativeDrawReplayRecipe>(packetDraws, green), { right } } };
    for (std::size_t index = 0; index < packetDraws; ++index)
    {
        placeInCell(frame.passes[0].draws[index], index);
        placeInCell(frame.passes[1].draws[index], index, 640);
    }
    const auto packet = [&](std::size_t index)
    {
        NativeRenderFrame next;
        if (!index)
            next.textures = frame.textures;
        next.passes = { frame.passes[index] };
        return next;
    };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto expected = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(expected.error);
    REQUIRE((expected.success && expected.completionFenceValue));
    auto*                                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Fence>  firstGate, secondGate;
    UNSCOPED_INFO("append queue gates");
    REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&firstGate))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&secondGate)))));

    struct ReleaseGates
    {
        ID3D12Fence* first;
        ID3D12Fence* second;

        ~ReleaseGates()
        {
            if (first)
                first->Signal(1);
            if (second)
                second->Signal(1);
        }
    } release{ firstGate.Get(), secondGate.Get() };

    UNSCOPED_INFO("hold first append execution");
    REQUIRE(SUCCEEDED(queue->Wait(firstGate.Get(), 1)));
    const auto before = renderer.ReplayCounts();
    auto       first  = packet(0);
    UNSCOPED_INFO(error);
    REQUIRE(renderer.AppendRenderFramePasses(first, error));
    UNSCOPED_INFO("record the first append once");
    REQUIRE(renderer.ReplayCounts().recordedDraws - before.recordedDraws == packetDraws);
    auto        second = packet(1);
    std::string appendError;
    auto        append = std::async(std::launch::async, [&]
                                    {
                                 return renderer.AppendRenderFramePasses(second, appendError);
                                    });
    {
        ReleaseGates unblock{ firstGate.Get(), secondGate.Get() };
        UNSCOPED_INFO("append command-owner reuse must wait the preceding packet's actual native fence");
        REQUIRE(append.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        UNSCOPED_INFO("release first append and hold the next");
        REQUIRE((SUCCEEDED(queue->Wait(secondGate.Get(), 1)) && SUCCEEDED(firstGate->Signal(1))));
        UNSCOPED_INFO("append must resume after the preceding packet's fence");
        REQUIRE(append.wait_for(kReadyCeiling) == std::future_status::ready);
        const bool appended = append.get();
        UNSCOPED_INFO(appendError);
        REQUIRE(appended);
        UNSCOPED_INFO("the second append must record before its actual completion or terminal swap");
        REQUIRE((secondGate->GetCompletedValue() == 0 && renderer.ReplayCounts().recordedDraws - before.recordedDraws == 2 * packetDraws));
        unblock.first = unblock.second = nullptr;
    }
    for (auto* owned : { &first, &second })
    {
        for (auto& draw : owned->passes.front().draws)
        {
            std::fill(draw.vertexData.begin(), draw.vertexData.end(), 0);
            std::fill(draw.pixelConstants.begin(), draw.pixelConstants.end(), 0xFF);
            draw.vertexShaderDxil.clear();
        }
        *owned = {};
    }
    auto terminal = std::async(std::launch::async, [&]
                               {
                                   return renderer.SubmitRenderFrame(tailOf(frame, 2));
                               });
    {
        ReleaseGates unblock{ nullptr, secondGate.Get() };
        UNSCOPED_INFO("terminal admission must wait the last append's actual native fence");
        REQUIRE(terminal.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        UNSCOPED_INFO("release second append");
        REQUIRE(SUCCEEDED(secondGate->Signal(1)));
        const auto completed = terminal.get();
        UNSCOPED_INFO("appended uploads, preserved attachments and partial resolves must match complete-frame bytes");
        REQUIRE((completed.success && completed.resolvedOutput == expected.resolvedOutput &&
                 completed.completionFenceValue > expected.completionFenceValue));
        UNSCOPED_INFO("terminal submission must record gamma alone without replaying either append");
        REQUIRE(renderer.ReplayCounts().recordedDraws - before.recordedDraws == 2 * packetDraws + 1);
    }
    UNSCOPED_INFO(error);
    REQUIRE((renderer.AppendRenderFramePasses(packet(0), error) && renderer.AppendRenderFramePasses(packet(1), error)));
    const auto rejected = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("a complete graph cannot end an appended chain: " + rejected.error);
    REQUIRE((!rejected.success && !rejected.completionFenceValue && !rejected.presenterToken &&
             rejected.error.find("as a tail") != std::string::npos));
    renderer.Shutdown();

    auto mutablePayload = std::make_shared<std::vector<std::uint8_t>>(4, 255);
    for (auto& draw : frame.passes[0].draws)
    {
        draw.textureMask             = 1;
        draw.textures[0]             = { {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
        draw.textures[0].sharedBytes = mutablePayload;
    }
    auto       extentFrame      = frame;
    auto       mutableGeometry  = std::make_shared<std::vector<std::uint8_t>>(extentFrame.passes[0].draws[0].vertexData);
    auto&      extentDraw       = extentFrame.passes[0].draws[0];
    const auto extentStride     = extentDraw.vertexStrideBytes;
    extentDraw.sharedVertexData = mutableGeometry;
    extentDraw.vertexData.clear();
    admitFrameDraws(extentFrame);
    mutableGeometry->resize(mutableGeometry->size() + extentStride);
    NativeRendererD3D12 extentMutation;
    UNSCOPED_INFO(error);
    REQUIRE(extentMutation.StartDrawStream(error));
    NativeRenderFrame malformedPrefix;
    malformedPrefix.textures                                = extentFrame.textures;
    malformedPrefix.passes                                  = { extentFrame.passes.front() };
    const auto                                 beforeExtent = extentMutation.ReplayCounts();
    std::shared_future<NativeDrawReplayResult> rejectedRecording;
    UNSCOPED_INFO("changed admitted shared geometry extent must reject before any native command recording");
    REQUIRE((extentMutation.QueueRenderFramePasses(std::move(malformedPrefix), rejectedRecording, error) &&
             rejectedRecording.wait_for(kReadyCeiling) == std::future_status::ready && !rejectedRecording.get().success &&
             rejectedRecording.get().error.find("extent changed") != std::string::npos &&
             !rejectedRecording.get().completionFenceValue && !rejectedRecording.get().presenterToken &&
             extentMutation.ReplayCounts().recordedDraws == beforeExtent.recordedDraws &&
             extentMutation.ReplayCounts().queueSubmissions == beforeExtent.queueSubmissions));
    extentMutation.Shutdown();
    NativeRendererD3D12 mixedStorage;
    UNSCOPED_INFO(error);
    REQUIRE(mixedStorage.StartDrawStream(error));
    auto                                             mixed = packet(0);
    std::shared_ptr<const NativeDrawReplayAdmission> mixedOwner;
    UNSCOPED_INFO(error);
    REQUIRE(AdmitNativeDrawReplayRecipe(mixed.passes[0].draws.front(), mixedOwner, error));
    mixed.passes[0].admittedDraws.push_back(std::move(mixedOwner));
    const auto beforeMixed = mixedStorage.ReplayCounts();
    UNSCOPED_INFO("mixed raw and admitted draw storage must reject before native command recording");
    REQUIRE((mixedStorage.QueueRenderFramePasses(std::move(mixed), rejectedRecording, error) &&
             rejectedRecording.wait_for(kReadyCeiling) == std::future_status::ready && !rejectedRecording.get().success &&
             rejectedRecording.get().error.find("mixes raw and admitted") != std::string::npos &&
             !rejectedRecording.get().completionFenceValue && !rejectedRecording.get().presenterToken &&
             mixedStorage.ReplayCounts().recordedDraws == beforeMixed.recordedDraws &&
             mixedStorage.ReplayCounts().queueSubmissions == beforeMixed.queueSubmissions));
    mixedStorage.Shutdown();

    NativeRenderFramePrefixState extents;
    auto                         partial = packet(0);
    UNSCOPED_INFO(error);
    REQUIRE(extents.Append(partial, error));
    NativeRenderFrame missing;
    auto              sampled          = green;
    sampled.textureMask                = 1;
    sampled.textures[0]                = { {}, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 };
    sampled.textures[0].frameTextureId = 1;
    missing.passes                     = { { true, { sampled }, {} } };
    UNSCOPED_INFO("later appends must not sample a partially resolved earlier resource");
    REQUIRE((!extents.Append(missing, error) && error.find("incompletely resolved") != std::string::npos));
    NativeRenderFramePrefixState complete;
    UNSCOPED_INFO(error);
    REQUIRE((complete.Append(packet(0), error) && complete.Append(packet(1), error) && complete.Append(missing, error)));

    NativeRenderFramePrefixState resources;
    NativeRenderFrame            clear;
    clear.passes.resize(1);
    clear.passes[0].target = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    clear.passes[0].clears = { { { 0, 0, 1280, 720 }, true, false, false, 1 } };
    for (std::uint64_t id = 1; id <= 8; ++id)
    {
        clear.passes[0].target.sampleCount = id % 2 ? 4 : 1;
        clear.passes[0].target.format      = id % 2 ? NativeDrawReplayTargetFormat::Rgba8 : NativeDrawReplayTargetFormat::Rgb10a2;
        clear.textures                     = { { id, 4096, 4096, NativeDrawReplayTextureFormat::Rgba8, id, true } };
        UNSCOPED_INFO(error);
        REQUIRE(resources.Append(clear, error));
    }
    clear.textures[0].id = clear.textures[0].persistentKey = 9;
    UNSCOPED_INFO("resource bytes must remain cumulative across unsigned target changes");
    REQUIRE((!resources.Append(clear, error) && error.find("resource byte bound") != std::string::npos));

    NativeRenderFramePrefixState inputs;
    auto                         large = green;
    large.vertexData.clear();
    large.sharedVertexData = std::make_shared<const std::vector<std::uint8_t>>(128U * 1024U);
    NativeRenderFrame draws;
    draws.passes = { { false, std::vector<NativeDrawReplayRecipe>(1024, large), {} } };
    for (unsigned index = 0; index < 3; ++index)
    {
        for (auto& draw : draws.passes[0].draws)
        {
            draw.sampleCount  = index % 2 ? 1 : 4;
            draw.sampleMask   = index % 2 ? 1 : 9;
            draw.targetFormat = index % 2 ? NativeDrawReplayTargetFormat::Rgb10a2 : NativeDrawReplayTargetFormat::Rgba8;
        }
        UNSCOPED_INFO(error);
        REQUIRE(inputs.Append(draws, error));
    }
    UNSCOPED_INFO("logical input bytes must remain cumulative after a packet retires or its unsigned target changes");
    REQUIRE((!inputs.Append(draws, error) && error.find("owned byte bound") != std::string::npos));
}

// Tail terminals keep only own passes while joining held prefix work at intake.
void checkTailTerminals()
{
    struct RestoreDepth
    {
        ~RestoreDepth()
        {
            NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(1);
        }
    } restoreDepth;

    constexpr std::uint64_t kOutputKey = 9801, kSideKey = 9802;
    // Four stripes of the output, one per pass; the first pass also resolves
    // texture 2, which only the prefix declares.
    const std::array<std::array<float, 4>, 4> colors = { { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } } };
    std::vector<NativeDrawReplayRecipe>       stripes;
    for (std::uint32_t index = 0; index < colors.size(); ++index)
    {
        auto draw    = makeDraw(colors[index], false);
        draw.scissor = { static_cast<std::int32_t>(index * 320), 0, static_cast<std::int32_t>((index + 1) * 320), 720 };
        stripes.push_back(std::move(draw));
    }
    const auto full = [&]
    {
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, kOutputKey },
                                       { 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, kSideKey } };
        frame.outputTextureId      = 1;
        frame.returnResolvedOutput = true;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        for (std::uint32_t index = 0; index < stripes.size(); ++index)
        {
            auto resolve    = frameResolve(1, 320, index * 320);
            resolve.sourceX = index * 320;
            frame.passes.push_back({ index != 0, { stripes[index] }, { resolve } });
        }
        frame.passes[0].resolves.push_back(frameResolve(2, 1280));
        return frame;
    };
    // The chain: packet 0 carries passes 0 and 1, packet 1 pass 2, and the
    // tail pass 3, continuing the sum of the packets' pass counts.
    const auto packet = [&](std::size_t index, bool typed = false)
    {
        auto              frame = full();
        NativeRenderFrame result;
        if (!index)
        {
            result.textures = std::move(frame.textures);
            result.passes   = { std::move(frame.passes[0]), std::move(frame.passes[1]) };
        }
        else
            result.passes.push_back(std::move(frame.passes[2]));
        result.typedGuestWords = typed;
        return result;
    };
    const auto tail = [&]
    {
        auto frame = full();
        // It redeclares the output with the same identity; texture 2 joins
        // from the prefix.
        frame.textures.resize(1);
        frame.passes.erase(frame.passes.begin(), frame.passes.begin() + 3);
        frame.continuedPrefixPasses = 3;
        return frame;
    };
    const auto ready = [](const std::shared_future<NativeDrawReplayResult>& future)
    {
        return future.valid() && future.wait_for(kReadyCeiling) == std::future_status::ready;
    };
    const auto openChain = [&](NativeRendererD3D12& renderer, NativeRenderFrame first)
    {
        std::shared_future<NativeDrawReplayResult> opened, appended;
        std::string                                error;
        UNSCOPED_INFO(error);
        REQUIRE((renderer.QueueRenderFramePasses(std::move(first), opened, error) && renderer.QueueRenderFramePasses(packet(1), appended, error)));
        UNSCOPED_INFO("a tail's prefix packets settle");
        REQUIRE((ready(opened) && ready(appended)));
        UNSCOPED_INFO("a tail's prefix packets record: " + opened.get().error + appended.get().error);
        REQUIRE((opened.get().success && appended.get().success));
    };
    // A terminal through SubmitRenderFrame, or the completion of QueueRenderFrame.
    const auto finish = [&](NativeRendererD3D12& renderer, NativeRenderFrame frame, bool queued, std::shared_ptr<NativeRenderFrameTicket>* kept = nullptr) -> NativeDrawReplayResult
    {
        if (!queued)
            return renderer.SubmitRenderFrame(std::move(frame));
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        NativeDrawReplayResult                   refused;
        if (!renderer.QueueRenderFrame(std::move(frame), ticket, refused.error))
            return refused;
        UNSCOPED_INFO("a queued terminal settles");
        REQUIRE(ticket->Completion().wait_for(kReadyCeiling) == std::future_status::ready);
        if (kept)
            *kept = ticket;
        return ticket->Completion().get();
    };
    const auto chainError = [](const NativeDrawReplayResult& result, std::string_view text, const std::string& what)
    {
        UNSCOPED_INFO(what + " fails with the chain error and no submitted fence: " + result.error);
        REQUIRE((!result.success && result.error.find(text) != std::string::npos && !result.submittedFenceValue &&
                 !result.completionFenceValue && !result.presenterToken));
    };
    const auto stopped = [&](NativeRendererD3D12& renderer, const std::string& what)
    {
        std::shared_future<NativeDrawReplayResult> next;
        std::string                                error;
        UNSCOPED_INFO(what + " stops the stream");
        REQUIRE(!renderer.QueueRenderFramePasses(packet(0), next, error));
        renderer.Shutdown();
    };

    for (const std::size_t depth : { std::size_t(1), NativeRendererD3D12::kDrawStreamQueueDepth })
    {
        NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(depth);
        for (const bool queued : { false, true })
        {
            const auto  what = std::format("at depth {} through {}", depth, queued ? "QueueRenderFrame" : "SubmitRenderFrame");
            std::string error;

            // 1. A tail after two packets records the full graph's pixels and
            // retains the same textures, including one only the prefix declares.
            {
                NativeRendererD3D12 renderer;
                UNSCOPED_INFO(error);
                REQUIRE(renderer.StartDrawStream(error));
                std::shared_ptr<NativeRenderFrameTicket> fullTicket, tailTicket;
                const auto                               expected = finish(renderer, full(), true, &fullTicket);
                UNSCOPED_INFO("the full graph records: " + expected.error);
                REQUIRE((expected.success && fullTicket->Export(kOutputKey) && fullTicket->Export(kSideKey)));
                openChain(renderer, packet(0));
                const auto before = renderer.ReplayCounts();
                const auto joined = finish(renderer, tail(), queued, &tailTicket);
                UNSCOPED_INFO("a tail " + what + " records the full graph's pixels: " + joined.error);
                REQUIRE((joined.success && joined.resolvedOutput == expected.resolvedOutput && joined.completionFenceValue > expected.completionFenceValue));
                UNSCOPED_INFO("a tail " + what + " records its own draw and gamma without replaying the prefix");
                REQUIRE(renderer.ReplayCounts().recordedDraws - before.recordedDraws == 2);
                UNSCOPED_INFO("a tail " + what + " retains the full graph's textures, including the prefix-only one");
                REQUIRE((!queued || (tailTicket->Export(kOutputKey) && tailTicket->Export(kSideKey))));
                renderer.Shutdown();
            }

            // 2. A count other than the chain's, here the packet count, and a
            // count with no open prefix, each break the chain.
            for (const bool open : { true, false })
            {
                NativeRendererD3D12 renderer;
                UNSCOPED_INFO(error);
                REQUIRE(renderer.StartDrawStream(error));
                auto wrong = tail();
                if (open)
                {
                    openChain(renderer, packet(0));
                    wrong.continuedPrefixPasses = 2;
                }
                const auto label = std::string(open ? "a wrong count " : "a count with no open prefix ") + what;
                chainError(finish(renderer, std::move(wrong), queued), open ? "does not continue this prefix chain" : "does not hold", label);
                stopped(renderer, label);
            }

            // 3. A tail that redeclares a prefix texture must keep its identity;
            // the same identity joins (1).
            {
                NativeRendererD3D12 renderer;
                UNSCOPED_INFO(error);
                REQUIRE(renderer.StartDrawStream(error));
                openChain(renderer, packet(0));
                auto changed                      = tail();
                changed.textures[0].persistentKey = kOutputKey + 100;
                const auto label                  = "a tail changing a prefix texture's identity " + what;
                chainError(finish(renderer, std::move(changed), queued), "changed a recorded resource descriptor", label);
                stopped(renderer, label);
            }

            // 4. A packet's typed guest-word flag reaches the joined frame: an
            // untyped tail is refused once the device lacks castable storage.
            {
                NativeRendererD3D12 renderer;
                UNSCOPED_INFO(error);
                REQUIRE(renderer.StartDrawStream(error));
                detail::NativeRenderFrameTestAccess::SetCastableGuestWords(renderer, true);
                openChain(renderer, packet(0, true));
                detail::NativeRenderFrameTestAccess::SetCastableGuestWords(renderer, false);
                const auto label = "an untyped tail behind a typed packet " + what;
                chainError(finish(renderer, tail(), queued), "castable storage", label);
                stopped(renderer, label);
            }

            // 5. The owned census charges the passes the renderer holds: a
            // packet's geometry, which the caller no longer owns, stays charged
            // until the tail retires the chain.
            {
                constexpr std::size_t kHeld = 64U * 1024U * 1024U;
                NativeRendererD3D12   renderer;
                UNSCOPED_INFO(error);
                REQUIRE(renderer.StartDrawStream(error));
                auto  first    = packet(0);
                auto& draw     = first.passes[0].draws[0];
                auto  geometry = std::make_shared<std::vector<std::uint8_t>>(std::move(draw.vertexData));
                geometry->reserve(kHeld);
                draw.vertexData.clear();
                draw.sharedVertexData                                     = geometry;
                const std::weak_ptr<const std::vector<std::uint8_t>> held = geometry;
                geometry.reset();
                openChain(renderer, std::move(first));
                const auto charged = renderer.ReplayCacheStats().ownedChargeBytes;
                UNSCOPED_INFO(std::format("the census charges held prefix passes {}: {} bytes", what, charged));
                REQUIRE((!held.expired() && charged >= kHeld));
                const auto done = finish(renderer, tail(), queued);
                UNSCOPED_INFO(done.error);
                REQUIRE(done.success);
                const auto released = renderer.ReplayCacheStats().ownedChargeBytes;
                UNSCOPED_INFO(std::format("a retired tail {} releases the held passes and their charge: {} bytes", what, released));
                REQUIRE((held.expired() && released < kHeld));
                renderer.Shutdown();
            }

            // 6. A tail with far more admitted draws than its small prefix:
            // its validation's owner growth and census are funded before the
            // join.
            {
                NativeRendererD3D12 renderer;
                UNSCOPED_INFO(error);
                REQUIRE(renderer.StartDrawStream(error));
                std::shared_ptr<NativeRenderFrameTicket> fullTicket;
                const auto                               expected = finish(renderer, full(), true, &fullTicket);
                UNSCOPED_INFO("the full graph records: " + expected.error);
                REQUIRE(expected.success);
                openChain(renderer, packet(0));
                auto large = tail();
                large.passes[0].draws.resize(4000, stripes[3]);
                large.passes.resize(4, large.passes[0]);
                admitFrameDraws(large);
                const auto joined = finish(renderer, std::move(large), queued);
                UNSCOPED_INFO("a tail with 16000 admitted draws after a small prefix " + what + " records: " + joined.error);
                REQUIRE((joined.success && joined.resolvedOutput == expected.resolvedOutput));
                renderer.Shutdown();
            }

            // 7. A long chain of admitted packets: the join funds the tail's
            // owner growth from the admission snapshot, so it walks the owned
            // state at most once (a refresh), never once per held owner.
            {
                constexpr std::size_t kPackets = 64, kDraws = 8;
                NativeRendererD3D12   renderer;
                UNSCOPED_INFO(error);
                REQUIRE(renderer.StartDrawStream(error));
                std::shared_ptr<NativeRenderFrameTicket> fullTicket;
                const auto                               expected = finish(renderer, full(), true, &fullTicket);
                UNSCOPED_INFO("the full graph records: " + expected.error);
                REQUIRE(expected.success);
                std::vector<std::shared_future<NativeDrawReplayResult>> recorded(kPackets);
                for (std::size_t index = 0; index < kPackets; ++index)
                {
                    auto next = packet(index ? 1 : 0);
                    for (auto& pass : next.passes)
                        pass.draws.resize(kDraws, pass.draws.front());
                    admitFrameDraws(next);
                    UNSCOPED_INFO(error);
                    REQUIRE(renderer.QueueRenderFramePasses(std::move(next), recorded[index], error));
                }
                for (const auto& future : recorded)
                {
                    UNSCOPED_INFO("a long chain's packets record: " + future.get().error);
                    REQUIRE((ready(future) && future.get().success));
                }
                auto large = tail();
                large.passes[0].draws.resize(4000, stripes[3]);
                admitFrameDraws(large);
                large.continuedPrefixPasses = kPackets + 1;
                const auto walks            = detail::NativeRenderFrameTestAccess::JoinOwnedWalks(renderer);
                const auto joined           = finish(renderer, std::move(large), queued);
                const auto joinWalks        = detail::NativeRenderFrameTestAccess::JoinOwnedWalks(renderer) - walks;
                UNSCOPED_INFO("a long chain's tail " + what + " records: " + joined.error);
                REQUIRE((joined.success && joined.resolvedOutput == expected.resolvedOutput));
                UNSCOPED_INFO(std::format("a long chain's join {} walks the owned state at most once: {} walks", what, joinWalks));
                REQUIRE(joinWalks <= 1);
                renderer.Shutdown();
            }
        }
    }
}

void checkUnsignedTargetChanges(bool admittedDraws = false)
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 1001 },
                                   { 2, 640, 360, NativeDrawReplayTextureFormat::Rgb10a2, 1002 },
                                   { 3, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 1003 } };
    frame.outputTextureId      = 3;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    NativeRenderFramePass first;
    first.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear full;
    full.rectangle = { 0, 0, 1280, 720 };
    full.color = full.depth = full.stencil = true;
    full.depthValue                        = 0;
    full.stencilValue                      = 7;
    first.clears.push_back(full);
    auto red        = makeDraw({ 1, 1, 1, 1 }, false);
    red.sampleCount = red.sampleMask = 1;
    red.textureMask                  = 1;
    red.textures[0]                  = { { 255, 0, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    red.pixelShaderDxil              = compile(
        "Texture2D<float4> t:register(t0);"
        "float4 main():SV_Target0 { return t.Load(int3(0,0,0)); }",
        "ps_5_1");
    first.draws.push_back(red);
    first.resolves.push_back(frameResolve(1, 1280));

    NativeRenderFramePass changed;
    changed.target          = { 640, 360, 4, NativeDrawReplayTargetFormat::Rgb10a2 };
    auto sampled            = makeDraw({ 1, 1, 1, 1 }, false);
    sampled.width           = 640;
    sampled.height          = 360;
    sampled.targetFormat    = NativeDrawReplayTargetFormat::Rgb10a2;
    sampled.viewport        = { 0, 0, 640, 360, 0, 1 };
    sampled.scissor         = { 0, 0, 640, 360 };
    sampled.textureMask     = 1;
    sampled.textures[0]     = frameTexture(1, NativeDrawReplayTextureFormat::Rgba8);
    sampled.pixelShaderDxil = compile(
        "Texture2D<float4> t:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(int2(p.xy)*2,0)).grba; }",
        "ps_5_1");
    changed.draws.push_back(sampled);
    auto smallResolve    = frameResolve(2, 640);
    smallResolve.height  = 360;
    smallResolve.samples = { 0, 3 };
    changed.resolves.push_back(smallResolve);

    NativeRenderFramePass returned;
    returned.target         = first.target;
    auto partial            = full;
    partial.rectangle.right = 640;
    partial.colorValue      = { 0, 0, 1, 1 };
    partial.depthValue      = 1;
    partial.stencilValue    = 0;
    returned.clears.push_back(partial);
    auto right               = red;
    right.scissor.left       = 640;
    right.textures[0]        = frameTexture(2, NativeDrawReplayTextureFormat::Rgb10a2);
    right.textures[0].width  = 640;
    right.textures[0].height = 360;
    right.pixelShaderDxil    = compile(
        "Texture2D<float4> t:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(int2(p.xy)/2,0)); }",
        "ps_5_1");
    right.depth.enabled         = true;
    right.depth.compare         = 2;
    right.stencil.enabled       = true;
    right.stencil.reference     = 0;
    right.stencil.writeMask     = 0;
    right.stencil.front.compare = right.stencil.back.compare = 3;
    returned.draws.push_back(right);
    returned.resolves.push_back(frameResolve(3, 1280));
    frame.passes = { first, changed, returned };
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(frame, error));
    auto wholePrefix                 = frame;
    wholePrefix.outputTextureId      = 0;
    wholePrefix.returnResolvedOutput = false;
    wholePrefix.gammaTable           = {};
    UNSCOPED_INFO("standalone prefix validation must retain its compatible-chain boundary");
    REQUIRE(!ValidateNativeRenderFramePrefix(wholePrefix, error));

    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto expected = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(expected.error);
    REQUIRE((expected.success && expected.completionFenceValue));
    checkGammaPixels(expected.resolvedOutput, packRgb10Output(17, 31, 919), packRgb10Output(17, 617, 47));
    using Payload = std::vector<std::uint8_t>;
    std::weak_ptr<const Payload>                                recipeOwner;
    std::vector<std::weak_ptr<const NativeDrawReplayAdmission>> canonicalOwners;
    std::vector<std::weak_ptr<const Payload>>                   canonicalGeometry;
    if (admittedDraws)
    {
        for (auto& pass : frame.passes)
        {
            auto& draw            = pass.draws.front();
            draw.sharedVertexData = std::make_shared<const Payload>(std::move(draw.vertexData));
            draw.vertexData.clear();
            canonicalGeometry.push_back(draw.sharedVertexData);
        }
        admitFrameDraws(frame);
        for (const auto& pass : frame.passes)
            canonicalOwners.push_back(pass.admittedDraws.front());
        const auto complete = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO("admitted unsigned target changes must preserve every complete raw frame output byte");
        REQUIRE((complete.success && complete.resolvedOutput == expected.resolvedOutput));
        UNSCOPED_INFO("complete submission must retire renderer references while canonical planning owns each draw");
        REQUIRE(std::all_of(canonicalOwners.begin(), canonicalOwners.end(), [](const auto& owner)
                            {
                                return owner.use_count() == 1;
                            }));
    }
    const auto packet = [&](std::size_t index)
    {
        NativeRenderFrame next;
        next.textures                  = { frame.textures[index] };
        next.textures[0].persistentKey = 0;
        next.passes                    = { frame.passes[index] };
        if (!admittedDraws)
        {
            auto& draw            = next.passes[0].draws[0];
            draw.sharedVertexData = std::make_shared<const Payload>(std::move(draw.vertexData));
            draw.vertexData.clear();
            recipeOwner = draw.sharedVertexData;
        }
        else
        {
            callbackChecks.Require(next.passes[0].admittedDraws[0] == frame.passes[index].admittedDraws[0], "unsigned target packets must share the canonical immutable draw owner");
        }
        return next;
    };
    auto*                                              queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    Microsoft::WRL::ComPtr<ID3D12Device>               device;
    std::array<Microsoft::WRL::ComPtr<ID3D12Fence>, 3> gates;
    UNSCOPED_INFO("target-change native queue");
    REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))));
    // A recorded prefix reports the queue fence value that will complete it;
    // the held gates keep that value pending until the test signals them.
    Microsoft::WRL::ComPtr<ID3D12Fence> streamFence;
    streamFence.Attach(renderer.AcquireStreamFence());
    UNSCOPED_INFO("target-change stream fence");
    REQUIRE(streamFence);
    for (auto& gate : gates)
    {
        UNSCOPED_INFO("target-change native gate");
        REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
    }

    struct ReleaseGates
    {
        const std::array<Microsoft::WRL::ComPtr<ID3D12Fence>, 3>& gates;

        ~ReleaseGates()
        {
            for (const auto& gate : gates)
                gate->Signal(1);
        }
    } finalRelease{ gates };

    UNSCOPED_INFO("hold first unsigned target execution");
    REQUIRE(SUCCEEDED(queue->Wait(gates[0].Get(), 1)));
    const auto                                 before = renderer.ReplayCounts();
    std::shared_future<NativeDrawReplayResult> recording;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFramePasses(packet(0), recording, error));
    // Until the terminal an admitted owner has three holders: the test's
    // frame, the prefix state and the renderer's held pass. A raw packet's
    // payload lives on in the held pass alone.
    for (std::size_t index = 0; index < frame.passes.size(); ++index)
    {
        UNSCOPED_INFO("retire unsigned target CPU recording");
        REQUIRE(recording.wait_for(kReadyCeiling) == std::future_status::ready);
        const auto& recorded = recording.get();
        UNSCOPED_INFO("CPU ticket readiness must release its request lease while canonical planning and actual unsigned native work remain pending");
        REQUIRE((recorded.success && recorded.completionFenceValue > streamFence->GetCompletedValue() && !recorded.presenterToken &&
                 !recorded.gpuOutputTicket && recorded.resolvedOutput.empty() &&
                 (admittedDraws ? canonicalOwners[index].use_count() == 3 && canonicalGeometry[index].use_count() == 2 : recipeOwner.use_count() == 1) &&
                 gates[index]->GetCompletedValue() == 0));
        const auto submitted = renderer.ReplayCounts();
        UNSCOPED_INFO("each unsigned target packet must record once before terminal swap");
        REQUIRE(submitted.recordedDraws - before.recordedDraws == index + 1);
        if (index + 1 == frame.passes.size())
            break;
        const auto storage = renderer.ReplayCacheStats();
        UNSCOPED_INFO("unsigned layout retention must share the bounded attachment domain");
        REQUIRE((storage.frameAttachmentByteBound && storage.frameAttachmentByteBound <= 512ULL * 1024 * 1024));
        UNSCOPED_INFO("hold the next unsigned native packet");
        REQUIRE(SUCCEEDED(queue->Wait(gates[index + 1].Get(), 1)));
        UNSCOPED_INFO(error);
        REQUIRE(renderer.QueueRenderFramePasses(packet(index + 1), recording, error));
        UNSCOPED_INFO("a changed target must retain immutable storage and record before the preceding packet's native fence retires");
        REQUIRE((recording.wait_for(kReadyCeiling) == std::future_status::ready && recording.get().success &&
                 (admittedDraws ? canonicalOwners[index + 1].use_count() == 3 : recipeOwner.use_count() == 1) &&
                 gates[index]->GetCompletedValue() == 0));
        const auto waiting = renderer.ReplayCacheStats();
        const auto pending = renderer.ReplayCounts();
        UNSCOPED_INFO("pending native packets must allocate retained upload storage rather than recycle the preceding arena");
        REQUIRE((waiting.drawUploadCreations == storage.drawUploadCreations + 1 && waiting.drawUploadReuses == storage.drawUploadReuses &&
                 pending.recordedDraws == submitted.recordedDraws + 1 && pending.queueSubmissions == submitted.queueSubmissions + 1));
        UNSCOPED_INFO("retire the preceding unsigned target packet");
        REQUIRE(SUCCEEDED(gates[index]->Signal(1)));
    }
    const auto beforeTerminal = renderer.ReplayCounts();
    auto       terminal       = std::async(std::launch::async, [&]
                                           {
                                   return renderer.SubmitRenderFrame(tailOf(frame, frame.passes.size()));
                                           });
    {
        ReleaseGates unblock{ gates };
        UNSCOPED_INFO("terminal target publication must wait the last actual native packet fence");
        REQUIRE(terminal.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        UNSCOPED_INFO("release final unsigned target execution");
        REQUIRE(SUCCEEDED(gates.back()->Signal(1)));
        const auto completed = terminal.get();
        UNSCOPED_INFO("dimension, sample, format and fresh-layout transitions must preserve every complete-frame output byte");
        REQUIRE((completed.success && completed.completionFenceValue > expected.completionFenceValue &&
                 completed.resolvedOutput == expected.resolvedOutput));
        UNSCOPED_INFO("terminal target submission must record gamma alone without replaying any sealed pass");
        REQUIRE(renderer.ReplayCounts().recordedDraws == beforeTerminal.recordedDraws + 1);
    }
    if (admittedDraws)
    {
        UNSCOPED_INFO("terminal native completion must release prefix admission ownership and preserve the canonical graph");
        REQUIRE(std::all_of(canonicalOwners.begin(), canonicalOwners.end(), [](const auto& owner)
                            {
                                return owner.use_count() == 1;
                            }));
        frame = {};
        UNSCOPED_INFO("canonical draw owners must retire after terminal completion and planning teardown");
        REQUIRE((std::all_of(canonicalOwners.begin(), canonicalOwners.end(), [](const auto& owner)
                             {
                                 return owner.expired();
                             }) &&
                 std::all_of(canonicalGeometry.begin(), canonicalGeometry.end(), [](const auto& owner)
                             {
                                 return owner.expired();
                             })));
    }
    renderer.Shutdown();
}

void checkFrameUploadHazards()
{
    using Microsoft::WRL::ComPtr;
    using rex::ui::d3d12::D3D12Presenter;
    using rex::ui::d3d12::GpuGuestOutputTicket;
    for (const bool pendingUploads : { false, true })
    {
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
        frame.outputTextureId      = 1;
        frame.returnResolvedOutput = true;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        NativeRenderFramePass first;
        first.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rgba8 };
        NativeRenderFramePass::Clear clear;
        clear.rectangle  = { 0, 0, 1280, 720 };
        clear.color      = true;
        clear.colorValue = { 1, 0, 0, 1 };
        auto red         = makeDraw({ 1, 0, 0, 1 }, true);
        red.sampleCount = red.sampleMask = 1;
        if (pendingUploads)
            first.draws.push_back(red);
        else
            first.clears.push_back(clear);
        frame.passes.push_back(first);
        if (pendingUploads)
        {
            NativeRenderFramePass emptyUploads;
            emptyUploads.target                      = first.target;
            emptyUploads.preservePreviousAttachments = true;
            clear.rectangle                          = { 600, 0, 640, 720 };
            clear.colorValue                         = { 0, 0, 1, 1 };
            emptyUploads.clears.push_back(clear);
            frame.passes.push_back(std::move(emptyUploads));
        }
        auto green        = makeDraw({ 0, 1, 0, 1 }, false);
        green.sampleCount = green.sampleMask = 1;
        green.scissor.left                   = 640;
        frame.passes.push_back({ true, { green }, { frameResolve(1, 1280) } });

        auto provider = rex::ui::d3d12::D3D12Provider::Create();
        UNSCOPED_INFO("upload hazard provider");
        REQUIRE(bool(provider));
        auto presenter = D3D12Presenter::Create([](bool, bool)
                                                {
                                                },
                                                *provider);
        UNSCOPED_INFO("upload hazard presenter");
        REQUIRE(bool(presenter));
        const auto stream = presenter->CreateGpuGuestOutputStream();
        UNSCOPED_INFO("upload hazard output stream");
        REQUIRE(bool(stream));
        std::atomic<unsigned>               publications{ 0 };
        std::vector<ComPtr<ID3D12Resource>> sources;
        const auto                          publish = [&](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
        {
            const auto completed = fence->GetCompletedValue();
            callbackChecks.Require((value && completed != UINT64_MAX && completed >= value && SUCCEEDED(provider->GetDevice()->GetDeviceRemovedReason())), "upload hazard publication requires actual healthy native completion");
            D3D12Presenter::GpuGuestOutputFrame output;
            output.resource    = resource;
            output.ready_fence = fence;
            output.ready_value = value;
            output.width       = 1280;
            output.height      = 720;
            auto ticket        = presenter->QueueGpuGuestOutputFrame(stream, std::move(output));
            if (ticket)
            {
                sources.emplace_back(resource);
                ++publications;
            }
            return ticket;
        };
        NativeRendererD3D12 renderer;
        std::string         error;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publish));
        const auto expected = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(expected.error);
        REQUIRE((expected.success && expected.completionFenceValue && !expected.resolvedOutput.empty()));
        auto* queue     = detail::NativeRenderFrameTestAccess::Queue(renderer);
        auto* copyQueue = rex::ui::d3d12::detail::GpuGuestOutputTestAccess::CopyQueue(*presenter);
        UNSCOPED_INFO("upload hazard native and SDK queues");
        REQUIRE((queue && copyQueue));
        std::array<ComPtr<ID3D12Fence>, 4> gates;
        for (auto& gate : gates)
        {
            UNSCOPED_INFO("upload hazard queue gate");
            REQUIRE(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
        }

        struct ReleaseGates
        {
            const std::array<ComPtr<ID3D12Fence>, 4>&                    gates;
            D3D12Presenter&                                              presenter;
            const std::shared_ptr<rex::ui::d3d12::GpuGuestOutputStream>& stream;

            ~ReleaseGates()
            {
                // Release holds and unclaimed outputs before the renderer drains.
                for (const auto& gate : gates)
                    gate->Signal(1);
                presenter.CloseGpuGuestOutputStream(stream);
            }
        } release{ gates, *presenter, stream };

        ComPtr<ID3D12Resource> destination;
        const auto             refresh = [&]
        {
            UNSCOPED_INFO("refresh upload hazard SDK output");
            REQUIRE(presenter->RefreshGuestOutput(1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& context)
                                                  {
                                                      destination = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
                                                      context.SetIs8bpc(true);
                                                      return true;
                                                  }));
        };
        std::uint64_t lastCopyFence = 0;
        const auto    copiedPixels  = [&](const NativeDrawReplayResult& result)
        {
            std::uint64_t copied = 0;
            UNSCOPED_INFO(error);
            REQUIRE((result.gpuOutputTicket && NativeRendererD3D12::AwaitGpuOutput(result.gpuOutputTicket, error, &copied)));
            UNSCOPED_INFO("upload hazard output requires its actual SDK copy fence");
            REQUIRE((copied > lastCopyFence && copied == result.gpuOutputTicket->copy_fence_value() &&
                     NativeRendererD3D12::PollGpuOutput(result.gpuOutputTicket) == NativeRendererD3D12::GpuOutputStatus::Completed));
            lastCopyFence = copied;
            return readGpuMailbox(*provider, destination.Get());
        };

        auto warm                 = frame;
        warm.returnResolvedOutput = false;
        warm.returnGpuOutput      = true;
        if (pendingUploads)
        {
            auto blue        = makeDraw({ 0, 0, 1, 1 }, true);
            blue.sampleCount = blue.sampleMask = 1;
            warm.passes[0].draws[0]            = std::move(blue);
        }
        else
            warm.passes[0].clears[0].colorValue = { 0, 0, 1, 1 };
        UNSCOPED_INFO("hold both upload arena warm contexts");
        REQUIRE(SUCCEEDED(queue->Wait(gates[0].Get(), 1)));
        std::array<std::shared_ptr<NativeRenderFrameTicket>, 2> warmTickets;
        for (auto& ticket : warmTickets)
        {
            UNSCOPED_INFO("both upload contexts must record before native warm completion");
            REQUIRE((renderer.QueueRenderFrame(warm, ticket, error) && ticket->Recording().wait_for(kReadyCeiling) == std::future_status::ready &&
                     ticket->Recording().get().success));
        }
        UNSCOPED_INFO("both upload contexts must retain pending complete native frames");
        REQUIRE((gates[0]->GetCompletedValue() == 0 && publications.load() == 0 &&
                 warmTickets[0]->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
                 warmTickets[1]->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
        UNSCOPED_INFO("release both warm native contexts");
        REQUIRE(SUCCEEDED(gates[0]->Signal(1)));
        std::array<NativeDrawReplayResult, 2> warmed;
        for (std::size_t index = 0; index < warmed.size(); ++index)
        {
            UNSCOPED_INFO("retire warm native contexts");
            REQUIRE(warmTickets[index]->Completion().wait_for(kReadyCeiling) == std::future_status::ready);
            warmed[index] = warmTickets[index]->Completion().get();
            UNSCOPED_INFO(warmed[index].error);
            REQUIRE((warmed[index].success && warmed[index].gpuOutputTicket && warmed[index].completionFenceValue));
        }
        UNSCOPED_INFO("both warm contexts must publish independent FIFO gamma sources");
        REQUIRE((publications.load() == 2 && sources.size() == 2 && sources[0] != sources[1] &&
                 warmed[0].completionFenceValue < warmed[1].completionFenceValue && warmed[0].presenterToken < warmed[1].presenterToken));
        for (const auto& result : warmed)
        {
            refresh();
            const auto pixels = copiedPixels(result);
            checkGammaPixels(pixels, packRgb10Output(17, 31, 919), packRgb10Output(17, 617, 47));
            UNSCOPED_INFO("warm contents must differ from the measured frame");
            REQUIRE(pixels != expected.resolvedOutput);
        }
        sources.clear();
        publications.store(0);
        const auto storage = renderer.ReplayCacheStats();
        const auto before  = renderer.ReplayCounts();
        UNSCOPED_INFO("both native contexts must have cached upload storage");
        REQUIRE(storage.drawUploadCreations);
        using Payload = std::vector<std::uint8_t>;
        std::vector<std::weak_ptr<const NativeDrawReplayAdmission>> canonicalOwners;
        std::vector<std::weak_ptr<const Payload>>                   canonicalGeometry;
        for (auto& pass : frame.passes)
            for (auto& draw : pass.draws)
            {
                draw.sharedVertexData = std::make_shared<const Payload>(std::move(draw.vertexData));
                draw.vertexData.clear();
                canonicalGeometry.push_back(draw.sharedVertexData);
            }
        admitFrameDraws(frame);
        for (const auto& pass : frame.passes)
            for (const auto& admission : pass.admittedDraws)
                canonicalOwners.push_back(admission);
        UNSCOPED_INFO("hold measured upload prefix execution");
        REQUIRE(SUCCEEDED(queue->Wait(gates[1].Get(), 1)));
        NativeRenderFrame prefix;
        prefix.textures = frame.textures;
        prefix.passes   = { frame.passes.front() };
        UNSCOPED_INFO(error);
        REQUIRE(renderer.RecordRenderFramePrefix(std::move(prefix), error));
        const auto prefixStorage = renderer.ReplayCacheStats();
        const auto submitted     = renderer.ReplayCounts();
        UNSCOPED_INFO("prefix must submit once with only its actual upload demand");
        REQUIRE((gates[1]->GetCompletedValue() == 0 && submitted.queueSubmissions == before.queueSubmissions + 1 &&
                 submitted.recordedDraws == before.recordedDraws + (pendingUploads ? 1 : 0) &&
                 prefixStorage.drawUploadCreations == storage.drawUploadCreations &&
                 prefixStorage.drawUploadReuses == storage.drawUploadReuses + (pendingUploads ? 1 : 0)));
        if (pendingUploads)
        {
            UNSCOPED_INFO("hold zero-upload incremental clear execution");
            REQUIRE(SUCCEEDED(queue->Wait(gates[2].Get(), 1)));
            NativeRenderFrame emptyUploads;
            emptyUploads.passes = { frame.passes[1] };
            UNSCOPED_INFO(error);
            REQUIRE(renderer.AppendRenderFramePasses(std::move(emptyUploads), error));
            const auto cleared      = renderer.ReplayCounts();
            const auto clearStorage = renderer.ReplayCacheStats();
            UNSCOPED_INFO("zero-upload incremental clear must leave preceding upload storage pending");
            REQUIRE((gates[1]->GetCompletedValue() == 0 && gates[2]->GetCompletedValue() == 0 &&
                     cleared.queueSubmissions == submitted.queueSubmissions + 1 && cleared.recordedDraws == submitted.recordedDraws &&
                     clearStorage.drawUploadCreations == prefixStorage.drawUploadCreations && clearStorage.drawUploadReuses == prefixStorage.drawUploadReuses));
        }
        const auto beforeTerminal  = renderer.ReplayCounts();
        frame.returnResolvedOutput = false;
        frame.returnGpuOutput      = true;
        std::shared_ptr<NativeRenderFrameTicket> terminal;
        UNSCOPED_INFO(error);
        REQUIRE(renderer.QueueRenderFrame(tailOf(std::move(frame), pendingUploads ? 2 : 1), terminal, error));
        frame                        = {};
        const auto expectedCreations = prefixStorage.drawUploadCreations + (pendingUploads ? 1 : 0);
        const auto expectedReuses    = prefixStorage.drawUploadReuses + (pendingUploads ? 0 : 1);
        if (pendingUploads)
        {
            // Observe replacement before the command-owner wait can independently
            // protect the earlier upload from a lost arena hazard.
            const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
            while (renderer.ReplayCacheStats().drawUploadCreations == prefixStorage.drawUploadCreations && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            const auto reserved = renderer.ReplayCacheStats();
            UNSCOPED_INFO("zero-upload preparation must preserve the earlier arena hazard before command-owner retirement");
            REQUIRE((reserved.drawUploadCreations == expectedCreations && reserved.drawUploadReuses == expectedReuses &&
                     gates[1]->GetCompletedValue() == 0 && gates[2]->GetCompletedValue() == 0 &&
                     terminal->Recording().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
            UNSCOPED_INFO("retire the actual earlier upload owner");
            REQUIRE(SUCCEEDED(gates[1]->Signal(1)));
        }
        UNSCOPED_INFO("terminal recording must finish while the remaining native prefix stays held");
        REQUIRE(terminal->Recording().wait_for(kReadyCeiling) == std::future_status::ready);
        const auto recorded        = terminal->Recording().get();
        auto*      pendingGate     = gates[pendingUploads ? 2 : 1].Get();
        const auto recordedStorage = renderer.ReplayCacheStats();
        const auto recordedTiming  = renderer.ReplayCounts();
        UNSCOPED_INFO((pendingUploads ? "pending earlier uploads require retained replacement storage across a zero-upload packet"
                                      : "a clear-only prefix must reuse its warm arena without publishing native completion"));
        REQUIRE((recorded.success && !recorded.completionFenceValue && !recorded.presenterToken && !recorded.gpuOutputTicket && recorded.resolvedOutput.empty() &&
                 pendingGate->GetCompletedValue() == 0 && terminal->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout &&
                 publications.load() == 0 && recordedStorage.drawUploadCreations == expectedCreations && recordedStorage.drawUploadReuses == expectedReuses &&
                 recordedTiming.recordedDraws == beforeTerminal.recordedDraws + 2 && recordedTiming.queueSubmissions == beforeTerminal.queueSubmissions + 1));
        UNSCOPED_INFO("a retired arena stays leased while its frame is pending");
        REQUIRE((recordedStorage.uploadPoolIdleBytes == storage.uploadPoolIdleBytes && recordedStorage.uploadPoolHits == storage.uploadPoolHits));
        UNSCOPED_INFO("successful Recording retires obsolete canonical admission and unique decoded geometry inventories while native completion remains held");
        REQUIRE((!canonicalOwners.empty() && canonicalOwners.size() == canonicalGeometry.size() &&
                 std::all_of(canonicalOwners.begin(), canonicalOwners.end(), [](const auto& owner)
                             {
                                 return owner.expired();
                             }) &&
                 std::all_of(canonicalGeometry.begin(), canonicalGeometry.end(), [](const auto& owner)
                             {
                                 return owner.expired();
                             })));
        UNSCOPED_INFO("retire the exact measured native terminal");
        REQUIRE((SUCCEEDED(pendingGate->Signal(1)) && terminal->Completion().wait_for(kReadyCeiling) == std::future_status::ready));
        const auto completed = terminal->Completion().get();
        UNSCOPED_INFO("measured output must publish once after actual native completion");
        REQUIRE((completed.success && completed.completionFenceValue > warmed[1].completionFenceValue &&
                 completed.presenterToken > warmed[1].presenterToken && completed.gpuOutputTicket && publications.load() == 1 && sources.size() == 1));
        const auto retiredIdle = renderer.ReplayCacheStats().uploadPoolIdleBytes;
        UNSCOPED_INFO("a retired arena returns to the pool with its completed frame");
        REQUIRE((pendingUploads ? retiredIdle > recordedStorage.uploadPoolIdleBytes : retiredIdle == recordedStorage.uploadPoolIdleBytes));
        UNSCOPED_INFO("hold the actual measured SDK copy");
        REQUIRE(SUCCEEDED(copyQueue->Wait(gates[3].Get(), 1)));
        refresh();
        UNSCOPED_INFO("measured SDK copy must stay submitted until its actual consumer fence completes");
        REQUIRE((gates[3]->GetCompletedValue() == 0 && completed.gpuOutputTicket->status() == GpuGuestOutputTicket::Status::kSubmitted &&
                 completed.gpuOutputTicket->copy_fence_value() && !completed.gpuOutputTicket->AwaitRetirement(0) &&
                 NativeRendererD3D12::PollGpuOutput(completed.gpuOutputTicket) == NativeRendererD3D12::GpuOutputStatus::Pending));
        UNSCOPED_INFO("release the measured SDK copy");
        REQUIRE(SUCCEEDED(gates[3]->Signal(1)));
        UNSCOPED_INFO("upload hazard retirement must preserve every complete-frame output byte and arena allocation");
        REQUIRE((copiedPixels(completed) == expected.resolvedOutput && renderer.ReplayCacheStats().drawUploadCreations == expectedCreations));
        presenter->CloseGpuGuestOutputStream(stream);
        renderer.Shutdown();
    }
}

void checkResolveOnlyTargetChanges()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 },
                                   { 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 },
                                   { 3, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    NativeRenderFramePass first;
    first.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear red;
    red.rectangle  = { 0, 0, 1280, 720 };
    red.color      = true;
    red.colorValue = { 1, 0, 0, 1 };
    first.clears.push_back(red);
    NativeRenderFramePass preserved;
    preserved.target                      = first.target;
    preserved.preservePreviousAttachments = true;
    preserved.resolves.push_back(frameResolve(1, 1280));
    NativeRenderFramePass changed;
    changed.target = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    auto msaa      = frameResolve(2, 1280);
    msaa.samples   = { 0, 3 };
    changed.resolves.push_back(msaa);
    NativeRenderFramePass returned;
    returned.target = first.target;
    returned.resolves.push_back(frameResolve(3, 1280));
    frame.passes = { first, preserved, changed, returned };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    for (const std::uint64_t output : { 1, 2, 3 })
    {
        frame.outputTextureId = output;
        UNSCOPED_INFO(error);
        REQUIRE(ValidateNativeRenderFrame(frame, error));
        const auto expected = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(expected.error);
        REQUIRE((expected.success && expected.completionFenceValue));
        const auto color = output == 1 ? packRgb10Output(311, 31, 47) : packRgb10Output(17, 31, 47);
        checkGammaPixels(expected.resolvedOutput, color, color);
        for (std::size_t index = 0; index < frame.passes.size(); ++index)
        {
            NativeRenderFrame packet;
            if (!index)
                packet.textures = frame.textures;
            packet.passes = { frame.passes[index] };
            UNSCOPED_INFO(error);
            REQUIRE(renderer.AppendRenderFramePasses(std::move(packet), error));
        }
        const auto appended = renderer.SubmitRenderFrame(tailOf(frame, frame.passes.size()));
        UNSCOPED_INFO("complete and appended resolve-only passes must preserve existing color and initialize every fresh target byte");
        REQUIRE((appended.success && appended.completionFenceValue > expected.completionFenceValue &&
                 appended.resolvedOutput == expected.resolvedOutput));
    }
    renderer.Shutdown();
}

void checkQueuedFramePasses()
{
    // With the resolve, the first pass takes one slot more than both owner batches hold.
    const auto drawCount = 2 * detail::NativeRenderFrameTestAccess::FrameCommandDraws();
    auto       base      = makeDraw({ 1, 1, 1, 1 }, false);
    base.textureMask     = 1;
    base.pixelShaderDxil = compile(
        "Texture2D<float4> t:register(t0); SamplerState s:register(s0,space3);"
        "float4 main():SV_Target0 { return t.SampleLevel(s,float2(0.5,0.5),0); }",
        "ps_5_1");
    using Payload = std::vector<std::uint8_t>;
    std::vector<std::weak_ptr<const Payload>>                        recipes;
    std::vector<std::weak_ptr<const NativeDrawReplayTextureVersion>> textureVersions;
    const auto                                                       makeFrame = [&](bool observeOwners = false)
    {
        NativeRenderFrame frame;
        frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, 912 } };
        frame.outputTextureId      = 1;
        frame.returnResolvedOutput = true;
        frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
        frame.passes               = { { false, {}, { frameResolve(1, 640) } }, { true, {}, {} } };
        for (std::size_t index = 0; index < drawCount; ++index)
        {
            auto draw = base;
            placeInCell(draw, index);
            draw.sharedVertexData = std::make_shared<const Payload>(std::move(draw.vertexData));
            draw.vertexData.clear();
            draw.textures[0]             = { {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
            draw.textures[0].sharedBytes = std::make_shared<const Payload>(Payload{
                static_cast<std::uint8_t>((index & 1) ? 255 : 0),
                static_cast<std::uint8_t>((index & 2) ? 255 : 0),
                static_cast<std::uint8_t>((index & 4) ? 255 : 0),
                255 });
            std::string textureError;
            callbackChecks.Require(CopyNativeDrawReplayTextureVersion(draw.textures[0], textureError), textureError);
            if (observeOwners)
            {
                recipes.push_back(draw.sharedVertexData);
                textureVersions.push_back(draw.textures[0].textureVersion);
            }
            frame.passes[0].draws.push_back(std::move(draw));
        }
        auto right         = base;
        right.scissor.left = 640;
        right.textures[0]  = { { 0, 255, 0, 255 }, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
        frame.passes[1].draws.push_back(std::move(right));
        auto resolve    = frameResolve(1, 640, 640);
        resolve.sourceX = 640;
        frame.passes[1].resolves.push_back(resolve);
        return frame;
    };
    const auto packet = [&](std::size_t index, bool observeOwners = false)
    {
        auto              frame = makeFrame(observeOwners);
        NativeRenderFrame result;
        if (!index)
            result.textures = std::move(frame.textures);
        result.passes.push_back(std::move(frame.passes[index]));
        return result;
    };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto expected = renderer.SubmitRenderFrame(makeFrame());
    UNSCOPED_INFO(expected.error);
    REQUIRE((expected.success && expected.completionFenceValue));
    auto*                                queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Fence>  firstGate, terminalGate;
    UNSCOPED_INFO("queued recording native fences");
    REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&firstGate))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&terminalGate)))));

    struct ReleaseGates
    {
        ID3D12Fence* first;
        ID3D12Fence* terminal;

        ~ReleaseGates()
        {
            if (first)
                first->Signal(1);
            if (terminal)
                terminal->Signal(1);
        }
    } finalRelease{ firstGate.Get(), terminalGate.Get() };

    UNSCOPED_INFO("hold native descriptor owners across wrap");
    REQUIRE(SUCCEEDED(queue->Wait(firstGate.Get(), 1)));
    const auto                                 before = renderer.ReplayCounts();
    auto                                       caller = packet(0, true);
    std::shared_future<NativeDrawReplayResult> firstRecording;
    auto                                       admission = std::async(std::launch::async, [&]
                                                                      {
                                    return renderer.QueueRenderFramePasses(caller, firstRecording, error);
                                                                      });
    {
        // This guard must outlive any failing assertion and precede the async
        // future's destructor, which otherwise waits on the held native fence.
        ReleaseGates unblock{ firstGate.Get(), terminalGate.Get() };
        UNSCOPED_INFO(error);
        REQUIRE((admission.wait_for(kReadyCeiling) == std::future_status::ready && admission.get()));
        UNSCOPED_INFO("owned admission must return while descriptor wrap still blocks CPU recording");
        REQUIRE((firstRecording.valid() && firstRecording.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout));
        const auto wrapDeadline = std::chrono::steady_clock::now() + kReadyCeiling;
        while (renderer.ReplayCounts().queueSubmissions < before.queueSubmissions + 2 &&
               std::chrono::steady_clock::now() < wrapDeadline)
            std::this_thread::yield();
        UNSCOPED_INFO("both native descriptor owners must be submitted before CPU recording waits on their actual reuse fence");
        REQUIRE((renderer.ReplayCounts().queueSubmissions >= before.queueSubmissions + 2 &&
                 firstRecording.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout && firstGate->GetCompletedValue() == 0));
        for (auto& draw : caller.passes[0].draws)
        {
            std::fill(draw.pixelConstants.begin(), draw.pixelConstants.end(), 0xFF);
            draw.pixelShaderDxil.clear();
            draw.sharedVertexData.reset();
            draw.textures[0].sharedBytes.reset();
            draw.textures[0].textureVersion.reset();
        }
        caller = {};
        UNSCOPED_INFO("the pending request must own recipes after caller mutation and destruction");
        REQUIRE(std::all_of(recipes.begin(), recipes.end(), [](const auto& owner)
                            {
                                return !owner.expired();
                            }));
        UNSCOPED_INFO("queued recording must retain immutable texture versions after the producer releases them");
        REQUIRE(std::all_of(textureVersions.begin(), textureVersions.end(), [](const auto& owner)
                            {
                                return !owner.expired();
                            }));
        std::shared_future<NativeDrawReplayResult> secondRecording;
        std::string                                secondError;
        auto                                       secondAdmission = std::async(std::launch::async, [&]
                                                                                {
                                              return renderer.QueueRenderFramePasses(packet(1), secondRecording, secondError);
                                                                                });
        {
            ReleaseGates secondUnblock{ firstGate.Get(), terminalGate.Get() };
            UNSCOPED_INFO("a later request must backpressure the single active CPU slot");
            REQUIRE(secondAdmission.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
            UNSCOPED_INFO("release wrapped owners and hold native completion after recording");
            REQUIRE((SUCCEEDED(queue->Wait(terminalGate.Get(), 1)) && SUCCEEDED(firstGate->Signal(1))));
            UNSCOPED_INFO("first recording retirement");
            REQUIRE(firstRecording.wait_for(kReadyCeiling) == std::future_status::ready);
            const auto& recorded = firstRecording.get();
            UNSCOPED_INFO("CPU retirement reports its completion fence and makes no output claim");
            REQUIRE((recorded.success && recorded.completionFenceValue && !recorded.presenterToken &&
                     !recorded.gpuOutputTicket && recorded.resolvedOutput.empty()));
            UNSCOPED_INFO("the renderer must hold a recorded packet's geometry recipes until its terminal");
            REQUIRE(std::all_of(recipes.begin(), recipes.end(), [](const auto& owner)
                                {
                                    return !owner.expired();
                                }));
            UNSCOPED_INFO("CPU ticket readiness must retain uploaded texture versions through the actual native fence");
            REQUIRE(std::all_of(textureVersions.begin(), textureVersions.end(), [](const auto& owner)
                                {
                                    return !owner.expired();
                                }));
            UNSCOPED_INFO(secondError);
            REQUIRE((secondAdmission.wait_for(kReadyCeiling) == std::future_status::ready && secondAdmission.get()));
            UNSCOPED_INFO("the admitted append must record in the completed command owner while preceding immutable uploads remain held");
            REQUIRE((secondRecording.valid() && secondRecording.wait_for(kReadyCeiling) == std::future_status::ready && secondRecording.get().success));
            UNSCOPED_INFO("recording readiness must precede actual native completion");
            REQUIRE(terminalGate->GetCompletedValue() == 0);
            auto terminal = std::async(std::launch::async, [&]
                                       {
                                           return renderer.SubmitRenderFrame(tailOf(makeFrame(), 2));
                                       });
            {
                ReleaseGates terminalUnblock{ firstGate.Get(), terminalGate.Get() };
                UNSCOPED_INFO("terminal submission must await the admitted prefix's actual native fence");
                REQUIRE(terminal.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
                UNSCOPED_INFO("release terminal native work");
                REQUIRE(SUCCEEDED(terminalGate->Signal(1)));
                UNSCOPED_INFO("the completed append recording must remain successful after exact native retirement");
                REQUIRE((secondRecording.wait_for(kReadyCeiling) == std::future_status::ready && secondRecording.get().success));
                const auto completed = terminal.get();
                UNSCOPED_INFO("queued recording must preserve ordered textured draws and exact terminal output");
                REQUIRE((completed.success && completed.completionFenceValue > expected.completionFenceValue &&
                         completed.resolvedOutput == expected.resolvedOutput));
                UNSCOPED_INFO("the retired terminal must release the held packet's geometry recipes");
                REQUIRE(std::all_of(recipes.begin(), recipes.end(), [](const auto& owner)
                                    {
                                        return owner.expired();
                                    }));
            }
        }
    }
    renderer.Shutdown();
    UNSCOPED_INFO("native shutdown must release retained immutable texture versions after drain");
    REQUIRE(std::all_of(textureVersions.begin(), textureVersions.end(), [](const auto& owner)
                        {
                            return owner.expired();
                        }));
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    device.Reset();
    queue              = detail::NativeRenderFrameTestAccess::Queue(renderer);
    finalRelease.first = nullptr;
    UNSCOPED_INFO("queued shutdown native fence");
    REQUIRE((queue && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&firstGate)))));
    finalRelease.first = firstGate.Get();
    UNSCOPED_INFO("hold active recording through shutdown");
    REQUIRE(SUCCEEDED(queue->Wait(firstGate.Get(), 1)));
    recipes.clear();
    textureVersions.clear();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFramePasses(packet(0, true), firstRecording, error));
    UNSCOPED_INFO("shutdown fixture must own an unfinished admitted request");
    REQUIRE(firstRecording.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    std::shared_future<NativeDrawReplayResult> canceledRecording;
    auto                                       blocked = std::async(std::launch::async, [&]
                                                                    {
                                  std::string unavailable;
                                  return renderer.QueueRenderFramePasses(packet(1), canceledRecording, unavailable);
                                                                    });
    {
        ReleaseGates unblock{ firstGate.Get(), nullptr };
        UNSCOPED_INFO("shutdown starts with a waiting submitter");
        REQUIRE(blocked.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
        auto stopping = std::async(std::launch::async, [&]
                                   {
                                       renderer.Shutdown();
                                   });
        {
            ReleaseGates stopUnblock{ firstGate.Get(), nullptr };
            UNSCOPED_INFO("closing admission must cancel a waiter without issuing a recording ticket");
            REQUIRE((blocked.wait_for(kReadyCeiling) == std::future_status::ready && !blocked.get() && !canceledRecording.valid()));
            UNSCOPED_INFO("shutdown must keep actual native owners until their submitted fences retire");
            REQUIRE(stopping.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
            UNSCOPED_INFO("shutdown must retain queued immutable texture versions while the actual native queue is held");
            REQUIRE(std::all_of(textureVersions.begin(), textureVersions.end(), [](const auto& owner)
                                {
                                    return !owner.expired();
                                }));
            UNSCOPED_INFO("release admitted recording for shutdown");
            REQUIRE(SUCCEEDED(firstGate->Signal(1)));
            stopping.get();
            UNSCOPED_INFO("shutdown must settle the admitted CPU ticket after recipe retirement and native drain");
            REQUIRE((firstRecording.wait_for(kReadyCeiling) == std::future_status::ready &&
                     std::all_of(recipes.begin(), recipes.end(), [](const auto& owner)
                                 {
                                     return owner.expired();
                                 }) &&
                     std::all_of(textureVersions.begin(), textureVersions.end(), [](const auto& owner)
                                 {
                                     return owner.expired();
                                 }) &&
                     !renderer.Initialized()));
        }
    }
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto restarted = renderer.SubmitRenderFrame(makeFrame());
    UNSCOPED_INFO("healthy restart must discard the abandoned prefix");
    REQUIRE((restarted.success && restarted.resolvedOutput == expected.resolvedOutput));
    renderer.Shutdown();

    NativeRendererD3D12 invalid;
    UNSCOPED_INFO(error);
    REQUIRE(invalid.StartDrawStream(error));
    auto malformed = packet(0);
    malformed.passes[0].draws[0].vertexShaderDxil.assign(4, 0);
    std::shared_future<NativeDrawReplayResult> failure;
    UNSCOPED_INFO("invalid executable bytes must still return an owned admission ticket");
    REQUIRE((invalid.QueueRenderFramePasses(std::move(malformed), failure, error) && failure.valid()));
    UNSCOPED_INFO("worker rejection must settle a deferred failure without completion or output");
    REQUIRE((failure.wait_for(kReadyCeiling) == std::future_status::ready && !failure.get().success &&
             !failure.get().completionFenceValue && !failure.get().presenterToken));
    UNSCOPED_INFO("deferred recording failure must latch unavailable admission");
    REQUIRE(!invalid.QueueRenderFramePasses(packet(0), failure, error));
    invalid.Shutdown();
    UNSCOPED_INFO("a recording failure must survive shutdown and reject restart");
    REQUIRE(!invalid.StartDrawStream(error));

    NativeRendererD3D12 changed;
    UNSCOPED_INFO(error);
    REQUIRE(changed.StartDrawStream(error));
    UNSCOPED_INFO(error);
    REQUIRE((changed.QueueRenderFramePasses(packet(0), firstRecording, error) && firstRecording.get().success));
    const auto rejected = changed.SubmitRenderFrame(makeFrame());
    UNSCOPED_INFO("a complete graph must reject the admitted prefix before publication: " + rejected.error);
    REQUIRE((!rejected.success && !rejected.completionFenceValue && !rejected.presenterToken &&
             !rejected.gpuOutputTicket && rejected.error.find("as a tail") != std::string::npos));
    changed.Shutdown();
}

void checkOrderedFrameResolves()
{
    auto source        = makeDraw({ 1, 0, 0, 1 }, false);
    auto first         = frameResolve(1, 1280);
    auto second        = frameResolve(2, 1280);
    second.swapRedBlue = true;
    second.samples     = { 3, 3 };
    auto sample        = source;
    sample.sampleCount = sample.sampleMask = 1;
    sample.textureMask                     = 1;
    sample.textures[0]                     = { {}, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 };
    sample.textures[0].frameTextureId      = 1;
    sample.pixelShaderDxil                 = compile("Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(p.xy,0)); }", "ps_5_1");
    sample.scissor.right                   = 640;
    auto right                             = sample;
    right.scissor.left                     = 640;
    right.scissor.right                    = 1280;
    right.textures[0].frameTextureId       = 2;
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 },
                                   { 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 },
                                   { 3, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 3;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.passes               = { { false, { source }, { first, second } },
                                   { false, { sample }, {} },
                                   { true, { right }, { frameResolve(3, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(17, 31, 919));
    UNSCOPED_INFO("ordered resolve constants, descriptors, attachment switches and gamma must survive one submission");
    REQUIRE(renderer.ReplayCounts().queueSubmissions == 1);

    NativeRenderFramePass clearPass;
    clearPass.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear clear;
    clear.rectangle  = { 0, 0, 1, 1 };
    clear.color      = true;
    clear.colorValue = { 0, 0, 1, 1 };
    clearPass.clears.resize(NativeRenderBatchCommands, clear);
    frame.textures.resize(1);
    frame.outputTextureId      = 1;
    frame.passes               = { clearPass, { true, {}, { frameResolve(1, 1280) } } };
    frame.passes.back().target = clearPass.target;
    const auto before          = renderer.ReplayCounts().queueSubmissions;
    const auto bounded         = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(bounded.error);
    REQUIRE((bounded.success && renderer.ReplayCounts().queueSubmissions - before == 2));
    for (std::size_t pixel = 0; pixel < 1280U * 720U; ++pixel)
    {
        std::uint32_t actual = 0;
        std::memcpy(&actual, bounded.resolvedOutput.data() + pixel * 4, sizeof(actual));
        UNSCOPED_INFO("command-cap submission must preserve ordered clears and output");
        REQUIRE(actual == (pixel ? packRgb10Output(17, 31, 47) : packRgb10Output(17, 31, 919)));
    }

    renderer.Shutdown();
}

void checkRenderFrameBatch()
{
    auto red        = makeDraw({ 1, 1, 1, 1 }, false);
    red.sampleCount = red.sampleMask = 1;
    red.textureMask                  = 1;
    red.pixelShaderDxil              = compile(
        "Texture2D<float4> t:register(t0);"
        "float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(0,0,0)); }",
        "ps_5_1");
    red.textures[0]              = { {}, 1, 1, NativeDrawReplayTextureFormat::Rgba8 };
    red.textures[0].sharedBytes  = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{ 255, 0, 0, 255 });
    auto blue                    = red;
    blue.scissor.right           = 640;
    blue.textures[0].sharedBytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{ 0, 0, 255, 255 });
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    frame.passes               = { { false, { red, blue, red, blue, red, blue }, { frameResolve(1, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 31, 919), packRgb10Output(311, 31, 47));
    UNSCOPED_INFO("compatible full-frame draws, resolve and present must share one submission");
    REQUIRE(renderer.ReplayCounts().queueSubmissions == 1);
    UNSCOPED_INFO("repeated immutable texture versions must upload once per frame");
    REQUIRE(renderer.ReplayCacheStats().textureUploadCreations == 2);
    for (auto& draw : frame.passes[0].draws)
        draw.textures[0].sharedBytes = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{ 0, 255, 0, 255 });
    result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 617, 47), packRgb10Output(17, 617, 47));
    auto manyStates = frame;
    manyStates.passes[0].draws.clear();
    // A complete scene can use more pipeline states than a small draw stream.
    for (unsigned state = 0; state < 34; ++state)
    {
        auto draw            = frame.passes[0].draws.front();
        draw.blend.writeMask = state % 16;
        draw.rasterizer.cull = state / 16;
        manyStates.passes[0].draws.push_back(std::move(draw));
    }
    manyStates.passes[0].draws.push_back(frame.passes[0].draws.front());
    result = renderer.SubmitRenderFrame(manyStates);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    const auto warmedPipelines = renderer.ReplayCacheStats().pipelineCreations;
    result                     = renderer.SubmitRenderFrame(manyStates);
    UNSCOPED_INFO("successive full frames must reuse their complete pipeline working set");
    REQUIRE((result.success && renderer.ReplayCacheStats().pipelineCreations == warmedPipelines));
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 617, 47), packRgb10Output(17, 617, 47));
    // A guest frame with off-screen resolves submits each as its own terminal
    // frame with a few pipelines, and the queue may drain between them.
    auto offscreen = frame;
    offscreen.passes[0].draws.resize(1);
    offscreen.passes[0].draws.front().rasterizer.frontCounterClockwise = !offscreen.passes[0].draws.front().rasterizer.frontCounterClockwise;
    result                                                             = renderer.SubmitRenderFrame(offscreen);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    const auto alternating = renderer.ReplayCacheStats().pipelineCreations;
    const auto started     = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < 3; ++round)
    {
        result = renderer.SubmitRenderFrame(manyStates);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        result = renderer.SubmitRenderFrame(offscreen);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
    }
    std::cout << std::format("alternating terminal frames: {} pipeline creations in {} us\n",
                             renderer.ReplayCacheStats().pipelineCreations - alternating,
                             std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
    UNSCOPED_INFO("terminal frames of one guest frame must not evict each other's pipelines");
    REQUIRE(renderer.ReplayCacheStats().pipelineCreations == alternating);
    const auto submissions = renderer.ReplayCounts().queueSubmissions;
    frame.passes[0].draws.back().vertexShaderDxil.assign(4, 0);
    result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("a failed draw must not submit an incomplete pass");
    REQUIRE((!result.success && renderer.ReplayCounts().queueSubmissions == submissions));
    renderer.Shutdown();
}

void checkLargeRenderFrame()
{
    auto red        = makeDraw({ 1, 0, 0, 1 }, false);
    red.sampleCount = red.sampleMask = 1;
    auto blue                        = makeDraw({ 0, 0, 1, 1 }, true);
    blue.sampleCount = blue.sampleMask = 1;
    blue.scissor.right                 = 320;
    auto green                         = makeDraw({ 0, 1, 0, 1 }, false);
    green.sampleCount = green.sampleMask = 1;
    green.scissor.left                   = 640;
    auto sample                          = red;
    sample.textureMask                   = 1;
    sample.textures[0]                   = { {}, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 };
    sample.textures[0].frameTextureId    = 1;
    sample.pixelShaderHash               = 3;
    sample.pixelShaderDxil               = compile(
        "Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position):SV_Target0 { return t.Load(int3(p.xy,0)); }", "ps_5_1");
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 },
                                   { 2, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 2;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.passes               = { { false, { red, green }, { frameResolve(1, 1280) } },
                                   { false, { sample, blue }, { frameResolve(2, 1280) } } };
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto reference = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(reference.error);
    REQUIRE(reference.success);
    auto tiny    = red;
    tiny.scissor = { 0, 0, 1, 1 };
    frame.passes[0].draws.insert(frame.passes[0].draws.end() - 1, NativeRenderBatchCommands, tiny);
    UNSCOPED_INFO("an oversized unsplit pass must fail before GPU submission");
    REQUIRE(!ValidateNativeRenderFrame(frame, error));
    UNSCOPED_INFO(error);
    REQUIRE((BatchNativeRenderFrame(frame, error) && ValidateNativeRenderFrame(frame, error)));
    UNSCOPED_INFO("native batches preserve attachments and delay the resolve until all preceding draws");
    REQUIRE((frame.passes.size() == 3 && frame.passes[0].draws.size() == NativeRenderBatchCommands &&
             frame.passes[1].preservePreviousAttachments && frame.passes[0].resolves.empty()));
    const auto beforeDraws = renderer.ReplayCounts().recordedDraws;
    const auto result      = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("large batched producer/consumer output must match the unsplit reference: " + result.error);
    REQUIRE((result.success && result.resolvedOutput == reference.resolvedOutput));
    UNSCOPED_INFO("every large-frame draw and its final presentation must execute exactly once");
    REQUIRE(renderer.ReplayCounts().recordedDraws - beforeDraws == NativeRenderBatchCommands + 5);
    const auto heaps    = renderer.ReplayCacheStats().descriptorHeapCreations;
    const auto repeated = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("completed draw groups must reuse descriptor storage without changing output");
    REQUIRE((repeated.success && repeated.resolvedOutput == reference.resolvedOutput &&
             renderer.ReplayCacheStats().descriptorHeapCreations == heaps));

    auto producer = frame;
    producer.passes.pop_back();
    producer.textures.resize(1);
    producer.textures[0].persistentKey = 73;
    producer.outputTextureId           = 1;
    producer.returnResolvedOutput      = false;
    producer.retainOnly                = true;
    const auto retained                = renderer.SubmitRenderFrame(producer);
    UNSCOPED_INFO("large producer must retain only after its final fence");
    REQUIRE((retained.success && retained.resolvedOutput.empty()));
    auto consumer                      = frame;
    consumer.passes                    = { frame.passes.back() };
    consumer.textures[0].persistentKey = 73;
    consumer.textures[0].imported      = true;
    const auto carried                 = renderer.SubmitRenderFrame(consumer);
    UNSCOPED_INFO("batched native versions must survive into the next frame: " + carried.error);
    REQUIRE((carried.success && carried.resolvedOutput == reference.resolvedOutput));

    NativeRendererD3D12 stopping;
    UNSCOPED_INFO(error);
    REQUIRE(stopping.StartDrawStream(error));
    auto pending = std::async(std::launch::async, [&]
                              {
                                  return stopping.SubmitRenderFrame(frame);
                              });
    while (!stopping.ReplayCounts().recordedDraws && pending.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
    {
    }
    stopping.Shutdown();
    UNSCOPED_INFO("shutdown must release an active large frame");
    REQUIRE(pending.wait_for(kReadyCeiling) == std::future_status::ready);
    const auto stopped = pending.get();
    UNSCOPED_INFO("stop between draw groups must not publish a partial frame");
    REQUIRE((!stopped.success && stopped.resolvedOutput.empty()));

    frame.passes[1].draws[0].vertexShaderDxil.assign(4, 0);
    const auto failed = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("failure after a batch must never publish partial output");
    REQUIRE((!failed.success && failed.resolvedOutput.empty()));
    renderer.Shutdown();
    std::cout << "Large frame output, descriptor reuse, retained versions, failure and stop passed\n";
}

void checkFrameClearsAndStencil()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    NativeRenderFramePass start;
    start.target = { 1280, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear full;
    full.rectangle = { 0, 0, 1280, 720 };
    full.color = full.depth = full.stencil = true;
    full.colorValue                        = { 0, 0, 1, 1 };
    start.clears.push_back(full);
    auto left            = full;
    left.rectangle.right = 640;
    left.color = left.depth = false;
    left.stencilValue       = 1;
    start.clears.push_back(left);
    auto green                  = makeDraw({ 0, 1, 0, 1 }, false);
    green.stencil.enabled       = true;
    green.stencil.reference     = 1;
    green.stencil.writeMask     = 0;
    green.stencil.front.compare = green.stencil.back.compare = 3;
    NativeRenderFramePass erase;
    erase.preservePreviousAttachments = true;
    erase.target                      = start.target;
    full.color = full.depth = false;
    erase.clears.push_back(full);
    auto red        = makeDraw({ 1, 0, 0, 1 }, false);
    red.stencil     = green.stencil;
    auto resolve    = frameResolve(1, 1280);
    resolve.samples = { 0, 3 };
    frame.passes    = { start, { true, { green }, {} }, erase, { true, { red }, { resolve } } };
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(frame, error));
    auto invalid                                = frame;
    invalid.passes[0].clears[0].rectangle.right = 1281;
    UNSCOPED_INFO("out-of-bounds clears must fail before GPU work");
    REQUIRE(!ValidateNativeRenderFrame(invalid, error));
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 617, 47), packRgb10Output(17, 31, 919));
    frame.passes[1].draws[0].stencil.reference = 0;
    result                                     = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 31, 919), packRgb10Output(17, 617, 47));
    auto mrt = frame;
    start.clears.clear();
    full.color        = true;
    full.stencil      = false;
    full.colorTargets = 0xF;
    full.colorValue   = { 1, 0, 0, 1 };
    start.clears.push_back(full);
    auto right           = full;
    right.rectangle.left = 640;
    right.colorTargets   = 2;
    right.colorValue     = { 0, 0, 1, 1 };
    NativeRenderFramePass partial;
    partial.target                      = start.target;
    partial.preservePreviousAttachments = true;
    partial.clears.push_back(right);
    resolve.colorTarget = 1;
    partial.resolves.push_back(resolve);
    mrt.passes = { start, partial };
    result     = renderer.SubmitRenderFrame(mrt);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(17, 31, 919));
    mrt.passes.back().resolves[0].colorTarget = 0;
    result                                    = renderer.SubmitRenderFrame(mrt);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(311, 31, 47));
    mrt.passes[0].target.format = mrt.passes[1].target.format = NativeDrawReplayTargetFormat::Rg16Snorm;
    UNSCOPED_INFO("signed targets must not silently resolve through an RGBA8 clamp");
    REQUIRE((!ValidateNativeRenderFrame(mrt, error) && error.find("conversion") != std::string::npos));
    renderer.Shutdown();
}

void checkColorAliasBits()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.returnResolvedOutput = true;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    NativeRenderFramePass source;
    source.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rg16Snorm };
    NativeRenderFramePass::Clear clear;
    clear.rectangle = { 0, 0, 1280, 720 };
    clear.color     = true;
    // Raw 0x00FF in the first signed channel becomes red RGBA8 bytes.
    clear.colorValue = { 255.0F / 32767.0F, 0, 0, 0 };
    source.clears.push_back(clear);
    auto overwrite        = makeDraw({ 0, 0, 1, 1 }, false);
    overwrite.sampleCount = overwrite.sampleMask = 1;
    auto empty                                   = makeDraw({ 0, 1, 0, 1 }, false);
    empty.sampleCount = empty.sampleMask = 1;
    empty.scissor                        = { 0, 0, 0, 0 };
    frame.passes                         = { source, { false, { overwrite }, {} }, { false, { empty }, { frameResolve(1, 1280) } } };
    frame.passes.back().colorSourcePass  = 1;
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(frame, error));
    auto invalid                          = frame;
    invalid.passes.back().colorSourcePass = 3;
    UNSCOPED_INFO("forward color aliases must be rejected");
    REQUIRE(!ValidateNativeRenderFrame(invalid, error));
    invalid                                   = frame;
    invalid.passes.front().target.sampleCount = 4;
    UNSCOPED_INFO("multisample color aliases must be rejected");
    REQUIRE(!ValidateNativeRenderFrame(invalid, error));
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    auto result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(311, 31, 47));
    clear.rectangle.right = 640;
    clear.colorValue      = { 0, 1, 0, 1 };
    frame.passes.back().clears.push_back(clear);
    result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 617, 47), packRgb10Output(311, 31, 47));
    renderer.Shutdown();
}

void checkFrameAttachmentAllocationReuse()
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    NativeRenderFramePass first;
    first.target = { 1280, 720, 1, NativeDrawReplayTargetFormat::Rgba8 };
    NativeRenderFramePass::Clear full;
    full.rectangle = { 0, 0, 1280, 720 };
    full.color = full.depth = full.stencil = true;
    full.colorValue                        = { 1, 0, 0, 1 };
    full.depthValue                        = 0;
    full.stencilValue                      = 7;
    first.clears.push_back(full);
    auto other                      = first;
    other.target                    = { 640, 720, 4, NativeDrawReplayTargetFormat::Rgba8 };
    other.clears[0].rectangle.right = 640;
    other.clears[0].colorValue      = { 0, 1, 0, 1 };
    NativeRenderFramePass returned;
    returned.target         = first.target;
    auto partial            = full;
    partial.rectangle.right = 640;
    partial.colorValue      = { 0, 0, 1, 1 };
    partial.depthValue      = 1;
    partial.stencilValue    = 0;
    returned.clears.push_back(partial);
    auto right        = makeDraw({ 0, 1, 0, 1 }, false);
    right.sampleCount = right.sampleMask = 1;
    right.scissor.left                   = 640;
    right.depth.enabled                  = true;
    right.depth.compare                  = 2;
    right.stencil.enabled                = true;
    right.stencil.reference              = 0;
    right.stencil.writeMask              = 0;
    right.stencil.front.compare = right.stencil.back.compare = 3;
    returned.draws.push_back(right);
    returned.resolves.push_back(frameResolve(1, 1280));
    frame.passes = { first, other, returned };
    std::string error;
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(frame, error));
    NativeRendererD3D12 renderer;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    std::uint64_t fence  = 0;
    const auto    submit = [&](const NativeRenderFrame& value)
    {
        const auto result = renderer.SubmitRenderFrame(value);
        UNSCOPED_INFO(result.error);
        REQUIRE((result.success && result.completionFenceValue > fence));
        fence = result.completionFenceValue;
        // The old right-side depth zero and stencil seven must not survive reuse.
        checkGammaPixels(result.resolvedOutput, packRgb10Output(17, 31, 919), packRgb10Output(17, 617, 47));
    };
    submit(frame);
    const auto cold = renderer.ReplayCacheStats();
    UNSCOPED_INFO("returning to a completed attachment layout must reuse bounded allocations");
    REQUIRE((cold.frameAttachmentReuses > 0 && cold.frameAttachmentByteBound > 0 &&
             cold.frameAttachmentByteBound <= 512ULL * 1024 * 1024));
    submit(frame);
    const auto warm = renderer.ReplayCacheStats();
    UNSCOPED_INFO("the next frame must reuse completed attachment allocations");
    REQUIRE(warm.frameAttachmentReuses > cold.frameAttachmentReuses);
    const auto attachmentCensus = [&]
    {
        return onRendererThread(renderer, [&]
                                {
                                    return detail::NativeRenderFrameTestAccess::RetainedTextureStorage(renderer, true);
                                });
    };
    const auto warmCensus = attachmentCensus();
    auto       pruned     = frame;
    pruned.passes         = { returned };
    submit(pruned);
    const auto prunedStats = renderer.ReplayCacheStats();
    UNSCOPED_INFO("layouts absent from the current frame must retire after completion");
    REQUIRE(prunedStats.frameAttachmentByteBound < warm.frameAttachmentByteBound);
    UNSCOPED_INFO("a retired layout's attachments wait in the pool, still charged as Attachment storage");
    REQUIRE((prunedStats.attachmentPoolIdleBytes > 0 && attachmentCensus() == warmCensus));
    // Four MSAA color slots and the depth target of the 640x720 layout return.
    submit(frame);
    const auto restored = renderer.ReplayCacheStats();
    UNSCOPED_INFO("a returning layout takes its pooled attachments instead of creating them");
    REQUIRE((restored.attachmentPoolHits == prunedStats.attachmentPoolHits + 5 && restored.attachmentPoolIdleBytes == 0 && attachmentCensus() == warmCensus));
    submit(pruned);
    auto fallback = pruned;
    fallback.passes.clear();
    // Distinct supported MSAA layouts exceed the retained allocation budget,
    // but ordinary sequential allocation still holds only one target set.
    for (std::uint32_t index = 0; index < 12; ++index)
    {
        auto large                       = first;
        large.target.sampleCount         = 4;
        large.target.height              = 720 + index;
        large.clears[0].rectangle.bottom = static_cast<std::int32_t>(large.target.height);
        fallback.passes.push_back(std::move(large));
    }
    fallback.passes.push_back(returned);
    UNSCOPED_INFO(error);
    REQUIRE(ValidateNativeRenderFrame(fallback, error));
    const auto beforeFallback = renderer.ReplayCacheStats();
    submit(fallback);
    const auto afterFallback = renderer.ReplayCacheStats();
    UNSCOPED_INFO("over-budget layouts must retain sequential allocation without caching");
    REQUIRE((afterFallback.frameAttachmentByteBound == 0 &&
             afterFallback.frameAttachmentReuses == beforeFallback.frameAttachmentReuses));
    renderer.Shutdown();
    UNSCOPED_INFO("shutdown must clear the attachment working-set bound");
    REQUIRE(renderer.ReplayCacheStats().frameAttachmentByteBound == 0);
    const auto beforeRestart = renderer.ReplayCacheStats();
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    fence = 0;
    submit(frame);
    UNSCOPED_INFO("restart must rebuild the cold attachment domain on the new device");
    REQUIRE(renderer.ReplayCacheStats().frameAttachmentReuses - beforeRestart.frameAttachmentReuses == cold.frameAttachmentReuses);
    renderer.Shutdown();
}

#ifdef REREVVED_PREPARED_RG16_TEST
class Rg16CaptureObserver final : public rex::graphics::diagnostic::DrawCaptureFrameInputObserver
{
public:
    using CopyView = rex::graphics::diagnostic::DrawCaptureFrameInputCopyView;
    using DrawView = rex::graphics::diagnostic::DrawCaptureFrameInputDrawView;
    using SwapView = rex::graphics::diagnostic::DrawCaptureFrameInputSwapView;

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
        initialBoundary = boundary.id;
        return inputs.BeginPrepared(boundary, error);
    }

    bool OnDrawPrepared(const DrawView& draw) override
    {
        return inputs.PreparedDraw(draw, error);
    }

    bool OnCopyPrepared(const CopyView& copy) override
    {
        return inputs.PreparedCopy(copy, error);
    }

    bool OnSwapPrepared(const SwapView& boundary) override
    {
        if (boundary.id == initialBoundary)
            return true;
        finished = inputs.PreparedSwap(boundary, error);
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
    std::uint64_t                                        initialBoundary = 0;
    bool                                                 finished        = false;
    std::string                                          error;
};

void checkCompleteFloatBytes(std::span<const std::uint8_t>       bytes,
                             const std::array<std::uint16_t, 2>& expected)
{
    UNSCOPED_INFO("complete linear RG16 float plane required");
    REQUIRE(bytes.size() == 1280U * 720U * 4U);
    UNSCOPED_INFO("float proof requires distinct finite nonzero values with opposite signs");
    REQUIRE((((expected[0] ^ expected[1]) & 0x8000U) &&
             (expected[0] & 0x7C00U) != 0x7C00U && (expected[1] & 0x7C00U) != 0x7C00U &&
             (expected[0] & 0x7FFFU) && (expected[1] & 0x7FFFU)));
    for (std::size_t offset = 0; offset < bytes.size(); offset += 4)
    {
        std::array<std::uint16_t, 2> actual;
        std::memcpy(actual.data(), bytes.data() + offset, 4);
        UNSCOPED_INFO("full raw RG16 float output differs from expected half-float channels");
        REQUIRE(actual == expected);
    }
}

std::vector<std::uint8_t> rg16FixtureIdentity(const rex::graphics::testing::Rg16ControlResult& result)
{
    std::vector<std::uint8_t> bytes;
    const auto                append = [&](std::uint32_t value)
    {
        for (std::uint32_t shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto words = [&](const auto& values)
    {
        append(static_cast<std::uint32_t>(values.size()));
        for (const auto value : values)
            append(value);
    };
    UNSCOPED_INFO("fixture identity requires every actual Prepared draw input");
    REQUIRE((result.draw_registers.size() == 3 && result.draw_vertex_shader_words.size() == 3 &&
             result.draw_pixel_shader_words.size() == 3));
    for (std::size_t index = 0; index < 3; ++index)
    {
        words(result.draw_registers[index]);
        words(result.draw_vertex_shader_words[index]);
        words(result.draw_pixel_shader_words[index]);
    }
    words(result.consumer_registers);
    words(result.producer_registers);
    for (const auto* texture : { &result.producer_a, &result.producer_b })
        for (const auto value : { texture->address, texture->width, texture->height, texture->pitch, texture->fetch_pitch, texture->format, std::uint32_t(texture->signs), texture->extent })
            append(value);
    UNSCOPED_INFO("fixture identity requires both float resolves and the terminal resolve");
    REQUIRE(result.original_copies.size() == 3);
    for (const auto& copy : result.original_copies)
    {
        append(copy.written_address);
        append(copy.written_length);
        append(copy.planned_address);
        append(copy.planned_length);
    }
    return bytes;
}

std::vector<std::uint8_t> readRg16ProofBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    UNSCOPED_INFO("raw companion proof file is missing");
    REQUIRE(bool(stream));
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

struct Rg16FenceGate
{
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    bool                                released = false;

    ~Rg16FenceGate()
    {
        if (fence && !released)
            (void)fence->Signal(1);
    }

    void Release()
    {
        UNSCOPED_INFO("RG16 proof gate signal");
        REQUIRE((fence && SUCCEEDED(fence->Signal(1))));
        released = true;
    }
};

NativeRenderFrame mutableRg16Control(const NativeRenderFrame& admitted)
{
    auto result = admitted;
    for (auto& pass : result.passes)
    {
        for (const auto& draw : pass.admittedDraws)
        {
            UNSCOPED_INFO("counter control requires its immutable source recipe");
            REQUIRE(bool(draw));
            pass.draws.push_back(draw->Recipe());
        }
        pass.admittedDraws.clear();
        for (auto& draw : pass.draws)
        {
            const auto                vertex = draw.vertexShaderBytes();
            const auto                pixel  = draw.pixelShaderBytes();
            std::vector<std::uint8_t> vertexCopy(vertex.begin(), vertex.end());
            std::vector<std::uint8_t> pixelCopy(pixel.begin(), pixel.end());
            draw.sharedVertexShaderDxil.reset();
            draw.sharedPixelShaderDxil.reset();
            draw.shaderVersion.reset();
            draw.vertexShaderDxil = std::move(vertexCopy);
            draw.pixelShaderDxil  = std::move(pixelCopy);
        }
    }
    result.returnGpuOutput      = false;
    result.returnResolvedOutput = true;
    return result;
}
#endif

#ifdef REREVVED_PREPARED_RG16_TEST
void checkPreparedRg16()
{
    using rex::ui::d3d12::D3D12Presenter;
    using rex::ui::d3d12::GpuGuestOutputTicket;
    const auto* rootText = std::getenv("REREVVED_PREPARED_RG16_OUTPUT");
    UNSCOPED_INFO("RG16 proof requires a fresh output directory");
    REQUIRE((rootText && *rootText));
    const auto root = std::filesystem::path(rootText);
    UNSCOPED_INFO("fresh RG16 proof directory required");
    REQUIRE(std::filesystem::create_directory(root));
    std::string error;
    if (const auto* rawOnly = std::getenv("REREVVED_PREPARED_CONTROL_RAW"); rawOnly && std::string_view(rawOnly) == "1")
    {
        rex::graphics::testing::Rg16ControlResult original;
        UNSCOPED_INFO(error);
        REQUIRE(rex::graphics::testing::RunRg16Control(nullptr, original, error) == 0);
        UNSCOPED_INFO("raw diagnostic requires an actual completed Xenos fence trace");
        REQUIRE((original.readback_mode && original.raw_gpu_proof && original.raw_fence_value &&
                 !original.raw_fence_trace_path.empty() && original.terminal_capture));
        const std::array<const rex::graphics::testing::Rg16RawTextureRecord*, 2> raw      = { &original.producer_a, &original.producer_b };
        const std::array<std::array<std::uint16_t, 2>, 2>                        expected = { { { 0x4800, 0xB800 }, { 0xBA00, 0x4A00 } } };
        for (std::size_t index = 0; index < raw.size(); ++index)
        {
            UNSCOPED_INFO("raw float plane requires its actual completed fence");
            REQUIRE((raw[index]->raw_gpu_proof && raw[index]->completed_fence == original.raw_fence_value));
            checkCompleteFloatBytes(raw[index]->linear_rg16_float, expected[index]);
            writeClearProofBytes(root / (index ? "original-b.rg16" : "original-a.rg16"), raw[index]->linear_rg16_float);
        }
        writeClearProofBytes(root / "fixture.bin", rg16FixtureIdentity(original));
        writeClearProofBytes(root / "original.rgba", original.terminal_pixels);
        const auto witness = "completed_fence=" + std::to_string(original.raw_fence_value) + " trace=" + original.raw_fence_trace_path + "\n";
        writeClearProofBytes(root / "raw-fence.txt", { reinterpret_cast<const std::uint8_t*>(witness.data()), witness.size() });
        std::cout << "Separate original RG16 readback diagnostic completed\n";
        return;
    }
    if (const auto* captureOnly = std::getenv("REREVVED_PREPARED_RG16_CAPTURE_ONLY");
        captureOnly && std::string_view(captureOnly) == "1")
    {
        auto                                      observer = std::make_shared<Rg16CaptureObserver>();
        rex::graphics::testing::Rg16ControlResult original;
        const auto                                status = rex::graphics::testing::RunRg16Control(observer, original, error);
        UNSCOPED_INFO((observer->error.empty() ? error : observer->error));
        REQUIRE((status == 0 && observer->finished && observer->error.empty()));
        UNSCOPED_INFO("actual SDK capture did not complete original GPU output");
        REQUIRE((original.progressed && original.completion_observed && original.terminal_capture));
        UNSCOPED_INFO(error);
        REQUIRE(observer->inputs.WriteSnapshot(root / "inputs", error));
        writeClearProofBytes(root / "original.rgba", original.terminal_pixels);
        writeClearProofBytes(root / "original-a.rg16", original.producer_a.linear_rg16_float);
        writeClearProofBytes(root / "original-b.rg16", original.producer_b.linear_rg16_float);
        std::cout << "RG16 constructor inputs and completed original output captured\n";
        return;
    }
    const auto* shaderText = std::getenv("REREVVED_PREPARED_NATIVE_SHADER_ROOT");
    UNSCOPED_INFO("RG16 proof requires its exact generated shader catalog");
    REQUIRE((shaderText && *shaderText));
    std::vector<NativeGameplayShaderInterface> shaders;
    UNSCOPED_INFO(error);
    REQUIRE(LoadNativeGameplayShaders(std::filesystem::path(shaderText), shaders, error));
    auto observer = std::make_shared<MainClearGraphObserver>(shaders);
    if (const auto* omit = std::getenv("REREVVED_PREPARED_RG16_DROP_PRODUCER"); omit && *omit)
    {
        UNSCOPED_INFO("bounded producer omission required");
        REQUIRE((std::string_view(omit) == "1" || std::string_view(omit) == "2"));
        observer->dropPreparedCopy = static_cast<std::size_t>(*omit - '0');
        rex::graphics::testing::Rg16ControlResult original;
        (void)rex::graphics::testing::RunRg16Control(observer, original, error);
        const auto journal = observer->journal.str();
        writeClearProofBytes(root / "callback-journal.txt", { reinterpret_cast<const std::uint8_t*>(journal.data()), journal.size() });
        UNSCOPED_INFO("omitted producer must reject actual Prepared construction before native submission");
        REQUIRE((!observer->finished && !observer->firstError.empty()));
        UNSCOPED_INFO("native omission must preserve actual original GPU resolve success");
        REQUIRE((original.original_copies.size() >= observer->dropPreparedCopy &&
                 original.original_copies[observer->dropPreparedCopy - 1].original_succeeded));
        writeClearProofBytes(root / "rejection.txt", { reinterpret_cast<const std::uint8_t*>(observer->firstError.data()), observer->firstError.size() });
        std::cout << "Actual Prepared consumer rejected omitted native producer before renderer construction\n";
        return;
    }
    rex::graphics::testing::Rg16ControlResult original;
    const auto                                status  = rex::graphics::testing::RunRg16Control(observer, original, error);
    const auto                                journal = observer->journal.str();
    writeClearProofBytes(root / "callback-journal.txt", { reinterpret_cast<const std::uint8_t*>(journal.data()), journal.size() });
    UNSCOPED_INFO((observer->error.empty() ? error : observer->error));
    REQUIRE((status == 0 && observer->finished && observer->error.empty()));
    UNSCOPED_INFO("original GPU control must complete its full terminal");
    REQUIRE((original.progressed && original.completion_observed && original.terminal_capture &&
             original.terminal_width == 1280 && original.terminal_height == 720 && original.terminal_stride == 1280 * 4));
    UNSCOPED_INFO("positive Prepared run requires ordinary original GPU execution without diagnostic readback");
    REQUIRE((!original.readback_mode && !original.raw_gpu_proof && !original.raw_fence_value && original.raw_fence_trace_path.empty() &&
             original.producer_a.linear_rg16_float.empty() && original.producer_b.linear_rg16_float.empty()));
    const auto* rawRootText = std::getenv("REREVVED_PREPARED_RG16_RAW_ROOT");
    UNSCOPED_INFO("positive proof requires its separate immutable raw companion");
    REQUIRE((rawRootText && *rawRootText));
    const auto rawRoot  = std::filesystem::path(rawRootText);
    const auto identity = rg16FixtureIdentity(original);
    UNSCOPED_INFO("ordinary and readback processes must use identical actual fixture inputs and complete terminal output");
    REQUIRE((identity == readRg16ProofBytes(rawRoot / "fixture.bin") &&
             original.terminal_pixels == readRg16ProofBytes(rawRoot / "original.rgba")));
    writeClearProofBytes(root / "fixture.bin", identity);
    UNSCOPED_INFO("two float resolves and one terminal resolve each require one exact Prepared/original pair");
    REQUIRE((original.prepared_copies.size() == 3 && original.original_copies.size() == 3 && observer->preparedCopyIds.size() == 3));
    for (std::size_t index = 0; index < 3; ++index)
    {
        const auto& prepared = original.prepared_copies[index];
        const auto& issued   = original.original_copies[index];
        UNSCOPED_INFO("prospective Prepared copy must retain the exact later original success and physical extent");
        REQUIRE((prepared.id == issued.id && prepared.id == observer->preparedCopyIds[index] &&
                 prepared.frame == issued.frame && prepared.submission == issued.submission && prepared.cpu_data_complete &&
                 !prepared.original_succeeded && !prepared.written_address && !prepared.written_length &&
                 issued.original_succeeded && prepared.planned_address == issued.written_address &&
                 prepared.planned_length == issued.written_length && prepared.planned_length == 0x398000U));
    }
    UNSCOPED_INFO(error);
    REQUIRE(observer->inputs.WriteSnapshot(root / "inputs", error));
    writeClearProofBytes(root / "original.rgba", original.terminal_pixels);
    const std::array<const rex::graphics::testing::Rg16RawTextureRecord*, 2> raw = { &original.producer_a, &original.producer_b };
    // These exact half values correspond to the controlled guest exports.
    // The installed fixture closes its logger during shutdown. Restore the
    // harness logger so native validation failures retain their D3D12 details.
    rex::LogConfig nativeLogging;
    nativeLogging.log_to_console = true;
    rex::InitLogging(nativeLogging);
    const std::array<std::array<std::uint16_t, 2>, 2> expectedHalf = { { { 0x4800, 0xB800 }, { 0xBA00, 0x4A00 } } };
    std::array<std::vector<std::uint8_t>, 2>          rawBytes;
    for (std::size_t index = 0; index < raw.size(); ++index)
    {
        UNSCOPED_INFO("ordinary producer metadata must match the exact raw companion fetch layout");
        REQUIRE((!raw[index]->completed_fence && raw[index]->width == 1280 && raw[index]->height == 720 &&
                 raw[index]->pitch == 1280 && raw[index]->format == 31 && raw[index]->signs == 0x55 && raw[index]->extent == 0x398000));
        rawBytes[index] = readRg16ProofBytes(rawRoot / (index ? "original-b.rg16" : "original-a.rg16"));
        checkCompleteFloatBytes(rawBytes[index], expectedHalf[index]);
    }
    auto                                  frame = observer->frame;
    std::vector<NativeRenderFrameTexture> floatTextures;
    for (const auto& pass : frame.passes)
        for (const auto& resolve : pass.resolves)
        {
            const auto found = std::ranges::find_if(frame.textures, [&](const auto& texture)
                                                    {
                                                        return texture.id == resolve.textureId;
                                                    });
            UNSCOPED_INFO("resolve requires its descriptor");
            REQUIRE(found != frame.textures.end());
            if (found->format != NativeDrawReplayTextureFormat::Rg16Float)
                continue;
            UNSCOPED_INFO("float resources must be full same-frame resolves with the selected signed conversion");
            REQUIRE((!found->imported && found->persistentKey && !found->producerVersion &&
                     resolve.width == 1280 && resolve.height == 720 && !resolve.sourceX && !resolve.sourceY && !resolve.destinationX &&
                     resolve.valueScale == 32 && !resolve.swapRedBlue && resolve.samples == std::array<std::uint32_t, 2>{ 0, 0 }));
            floatTextures.push_back(*found);
        }
    UNSCOPED_INFO("consumer requires two distinct ordered float producers");
    REQUIRE((floatTextures.size() == 2 && floatTextures[0].id != floatTextures[1].id));
    auto provider = rex::ui::d3d12::D3D12Provider::Create();
    UNSCOPED_INFO("RG16 proof provider");
    REQUIRE(bool(provider));
    auto presenter = D3D12Presenter::Create([](bool, bool)
                                            {
                                            },
                                            *provider);
    UNSCOPED_INFO("RG16 proof SDK presenter");
    REQUIRE(bool(presenter));
    const auto                            stream = presenter->CreateGpuGuestOutputStream();
    std::shared_ptr<GpuGuestOutputTicket> published;
    const auto                            publish = [&](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
    {
        D3D12Presenter::GpuGuestOutputFrame output;
        output.resource    = resource;
        output.ready_fence = fence;
        output.ready_value = value;
        output.width       = 1280;
        output.height      = 720;
        published          = presenter->QueueGpuGuestOutputFrame(stream, std::move(output));
        return published;
    };
    NativeRendererD3D12 renderer;
    Rg16FenceGate       nativeGate, copyGate;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error, provider->GetDevice(), publish));
    UNSCOPED_INFO("native float gate allocation");
    REQUIRE(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nativeGate.fence))));
    UNSCOPED_INFO("copy float gate allocation");
    REQUIRE(SUCCEEDED(provider->GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyGate.fence))));
    auto* queue = detail::NativeRenderFrameTestAccess::Queue(renderer);
    UNSCOPED_INFO("hold actual native float completion");
    REQUIRE((queue && SUCCEEDED(queue->Wait(nativeGate.fence.Get(), 1))));
    frame.returnGpuOutput      = true;
    frame.returnResolvedOutput = false;
    std::shared_ptr<NativeRenderFrameTicket> ticket;
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(frame, ticket, error));
    UNSCOPED_INFO("float recording readiness");
    REQUIRE(ticket->Recording().wait_for(kReadyCeiling) == std::future_status::ready);
    UNSCOPED_INFO("Recording must fund preparation without completion or publication");
    REQUIRE((ticket->Recording().get().success && !published && ticket->Completion().wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout));
    for (const auto& texture : floatTextures)
    {
        ID3D12Resource* resource = nullptr;
        UNSCOPED_INFO("raw float borrow must reject a pending native fence");
        REQUIRE((!detail::NativeRenderFrameTestAccess::CompletedTexture(renderer, ticket->Export(texture.persistentKey), resource) && !resource));
    }
    nativeGate.Release();
    UNSCOPED_INFO("float native completion after actual gate");
    REQUIRE(ticket->Completion().wait_for(kReadyCeiling) == std::future_status::ready);
    const auto completed = ticket->Completion().get();
    UNSCOPED_INFO(completed.error);
    REQUIRE((completed.success && completed.completionFenceValue && published && completed.gpuOutputTicket == published));
    for (std::size_t index = 0; index < floatTextures.size(); ++index)
    {
        ID3D12Resource* resource = nullptr;
        const auto      owner    = detail::NativeRenderFrameTestAccess::CompletedTexture(renderer, ticket->Export(floatTextures[index].persistentKey), resource);
        UNSCOPED_INFO("completed native float resource requires its strong owner");
        REQUIRE((owner && resource && resource->GetDesc().Format == DXGI_FORMAT_R16G16_FLOAT));
        const auto bytes = readGpuMailbox(*provider, resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        checkCompleteFloatBytes(bytes, expectedHalf[index]);
        UNSCOPED_INFO("every native float pixel/channel must equal completed original GPU samples");
        REQUIRE(bytes == rawBytes[index]);
        writeClearProofBytes(root / (index ? "prepared-b.rg16" : "prepared-a.rg16"), bytes);
    }
    auto* sdkQueue = rex::ui::d3d12::detail::GpuGuestOutputTestAccess::CopyQueue(*presenter);
    UNSCOPED_INFO("hold actual SDK float terminal copy retirement");
    REQUIRE((sdkQueue && SUCCEEDED(sdkQueue->Wait(copyGate.fence.Get(), 1))));
    Microsoft::WRL::ComPtr<ID3D12Resource> copied;
    UNSCOPED_INFO("queue actual SDK consumer copy of float terminal");
    REQUIRE(presenter->RefreshGuestOutput(1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& context)
                                          {
                                              copied = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context).resource_uav_capable();
                                              context.SetIs8bpc(true);
                                              return true;
                                          }));
    std::uint64_t copyFence = 0;
    UNSCOPED_INFO("native completion must not retire SDK output borrowing");
    REQUIRE((NativeRendererD3D12::PollGpuOutput(published, &copyFence) == NativeRendererD3D12::GpuOutputStatus::Pending && !copyFence));
    copyGate.Release();
    UNSCOPED_INFO(error);
    REQUIRE((NativeRendererD3D12::AwaitGpuOutput(published, error, &copyFence) && copyFence));
    const auto positive = unpackTerminalPixels(readGpuMailbox(*provider, copied.Get()));
    writeClearProofBytes(root / "prepared.rgba", positive);
    UNSCOPED_INFO("every Prepared terminal pixel/channel must match original GPU output");
    REQUIRE(positive == original.terminal_pixels);
    renderer.Shutdown();
    auto control    = mutableRg16Control(observer->frame);
    auto wrongScale = control;
    for (auto& pass : wrongScale.passes)
        for (auto& resolve : pass.resolves)
            if (std::ranges::any_of(floatTextures, [&](const auto& texture)
                                    {
                                        return texture.id == resolve.textureId;
                                    }))
                resolve.valueScale = 1;
    NativeRendererD3D12 scaleRenderer;
    UNSCOPED_INFO(error);
    REQUIRE(scaleRenderer.StartDrawStream(error));
    const auto scaleResult = scaleRenderer.SubmitRenderFrame(wrongScale);
    UNSCOPED_INFO("unsupported float resolve scale must reject before completion");
    REQUIRE((!scaleResult.success && !scaleResult.completionFenceValue));
    scaleRenderer.Shutdown();
    NativeRendererD3D12 controlRenderer;
    UNSCOPED_INFO(error);
    REQUIRE(controlRenderer.StartDrawStream(error));
    const auto controlResult = controlRenderer.SubmitRenderFrame(control);
    UNSCOPED_INFO(controlResult.error);
    REQUIRE((controlResult.success && controlResult.completionFenceValue));
    const auto controlPixels = unpackTerminalPixels(controlResult.resolvedOutput);
    writeClearProofBytes(root / "control.rgba", controlPixels);
    UNSCOPED_INFO("complete independent native control must match Prepared and original output");
    REQUIRE(controlPixels == positive);
    controlRenderer.Shutdown();
    std::ostringstream proof;
    proof << "full_terminal_pixels=921600 channels=4 match=true raw_float_pixels=1843200 channels=2 match=true\n"
          << "native_completion_fence=" << completed.completionFenceValue << " sdk_copy_fence=" << copyFence << '\n';
    for (std::size_t producer = 0; producer < 2; ++producer)
    {
        const std::array<std::array<float, 2>, 2> source = { { { 0.25F, -0.015625F }, { -0.0234375F, 0.375F } } };
        for (std::size_t mode = 0; mode < 5; ++mode)
        {
            auto counter = mutableRg16Control(observer->frame);
            auto pass    = std::ranges::find_if(counter.passes, [&](const auto& candidate)
                                                {
                                                 return std::ranges::any_of(candidate.resolves, [&](const auto& resolve)
                                                                            {
                                                                                return resolve.textureId == floatTextures[producer].id;
                                                                            });
                                                });
            UNSCOPED_INFO("counter requires one actual attachment producer");
            REQUIRE((pass != counter.passes.end() && pass->draws.size() == 1));
            auto values = source[producer];
            if (mode == 0)
                for (auto& value : values)
                    value = std::max(value, 0.0F);
            if (mode == 1)
                std::swap(values[0], values[1]);
            if (mode == 2)
                for (auto& value : values)
                    value /= 32;
            if (mode == 3)
                values[0] *= 0.5F;
            if (mode == 4)
                values[1] *= 0.5F;
            const auto* counterRoot = std::getenv("REREVVED_PREPARED_RG16_COUNTER_ROOT");
            UNSCOPED_INFO("counter controls require pinned Shader Model 6.0 bytecode");
            REQUIRE((counterRoot && *counterRoot));
            const auto         counterName = "counter-" + std::to_string(producer) + '-' + std::to_string(mode);
            std::ostringstream shader;
            shader << std::setprecision(9) << "float4 main():SV_Target0 { return float4(" << values[0] << ',' << values[1] << ",0,1); }";
            const auto expectedSource = shader.str();
            auto       actualSource   = readRg16ProofBytes(std::filesystem::path(counterRoot) / (counterName + ".hlsl"));
            UNSCOPED_INFO("counter source must end with a newline");
            REQUIRE((!actualSource.empty() && actualSource.back() == '\n'));
            actualSource.pop_back();
            if (!actualSource.empty() && actualSource.back() == '\r')
                actualSource.pop_back();
            UNSCOPED_INFO("counter source must describe its exact sign/channel/scale mutation");
            REQUIRE(std::string(actualSource.begin(), actualSource.end()) == expectedSource);
            pass->draws[0].pixelShaderDxil = readRg16ProofBytes(std::filesystem::path(counterRoot) / (counterName + ".dxil"));
            NativeRendererD3D12 counterRenderer;
            UNSCOPED_INFO(error);
            REQUIRE(counterRenderer.StartDrawStream(error));
            const auto result = counterRenderer.SubmitRenderFrame(counter);
            UNSCOPED_INFO(result.error);
            REQUIRE((result.success && result.completionFenceValue));
            const auto pixels = unpackTerminalPixels(result.resolvedOutput);
            const auto name   = "counter-" + std::to_string(producer) + '-' + std::to_string(mode) + ".rgba";
            writeClearProofBytes(root / name, pixels);
            std::size_t changed = 0;
            for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
                changed += !std::equal(pixels.begin() + offset, pixels.begin() + offset + 4, positive.begin() + offset);
            UNSCOPED_INFO("every terminal pixel must expose each wrong sign, channel, scale or producer control");
            REQUIRE(changed == 921600);
            proof << "counter producer=" << producer << " mode=" << mode << " changed_pixels=" << changed << " native_fence=" << result.completionFenceValue << '\n';
            counterRenderer.Shutdown();
        }
    }
    const auto text = proof.str();
    writeClearProofBytes(root / "proof-journal.txt", { reinterpret_cast<const std::uint8_t*>(text.data()), text.size() });
    presenter->CloseGpuGuestOutputStream(stream);
    std::cout << "Prepared signed RG16 float full terminal and raw sample proof passed\n";
}
#endif

// native_vertex_residency device cases. An owner is the shared vertex span of
// one colored triangle (96 bytes); frame draws cover distinct 160-pixel
// scissor columns, so each draw's pixels show which bytes it drew.
using ResidentOwner = std::shared_ptr<const std::vector<std::uint8_t>>;

constexpr std::uint64_t kResidentSpan       = 96;
constexpr std::uint64_t kResidentChunkBytes = 16ULL * 1024 * 1024;
constexpr std::uint64_t kResidentFootprint  = 4 * kResidentChunkBytes;

ResidentOwner residentOwner(const std::array<float, 4>& color)
{
    return std::make_shared<const std::vector<std::uint8_t>>(makeDraw(color, true).vertexData);
}

NativeDrawReplayRecipe residentDraw(const ResidentOwner& owner, std::int32_t column, bool repeatedIndices = false)
{
    auto draw        = makeDraw({ 1, 1, 1, 1 }, true);
    draw.sampleCount = draw.sampleMask = 1;
    draw.sharedVertexData              = owner;
    draw.vertexData.clear();
    draw.scissor = { column * 160, 0, column * 160 + 160, 720 };
    if (repeatedIndices)
    {
        draw.indices    = { 0, 1, 2, 0, 1, 2 };
        draw.indexCount = 6;
    }
    return draw;
}

NativeRenderFrame residentFrame(std::vector<NativeDrawReplayRecipe> draws)
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    frame.returnResolvedOutput = true;
    frame.passes               = { { false, std::move(draws), { frameResolve(1, 1280) } } };
    return frame;
}

// A frame that only retains its target, so it neither presents nor swaps.
NativeRenderFrame residentRetainedFrame(std::vector<NativeDrawReplayRecipe> draws, std::uint64_t key)
{
    NativeRenderFrame frame;
    frame.textures        = { { 1, 1280, 720, NativeDrawReplayTextureFormat::Rgba8, key } };
    frame.outputTextureId = 1;
    frame.retainOnly      = true;
    frame.passes          = { { false, std::move(draws), { frameResolve(1, 1280) } } };
    return frame;
}

#if REREVVED_DEVELOPER
std::size_t residentReason(std::string_view name)
{
    const auto& names = NativeDrawReplayTimingStats::kResidentFallbackReasons;
    for (std::size_t index = 0; index < names.size(); ++index)
        if (name == names[index])
            return index;
    throw std::runtime_error("unknown resident fallback reason");
}
#endif

// Starts the stream with the options set for its initialization, as the
// options are read there, and restores them afterwards.
void startResident(NativeRendererD3D12& renderer, bool on, bool exact, int fault = 0)
{
    const bool savedOn = REXCVAR_GET(native_vertex_residency), savedExact = REXCVAR_GET(native_vertex_residency_exact);
    REXCVAR_SET(native_vertex_residency, on);
    REXCVAR_SET(native_vertex_residency_exact, exact);
    if (fault)
        detail::NativeRenderFrameTestAccess::ResidencyFault(renderer, fault);
    std::string error;
    const bool  started = renderer.StartDrawStream(error);
    REXCVAR_SET(native_vertex_residency, savedOn);
    REXCVAR_SET(native_vertex_residency_exact, savedExact);
    UNSCOPED_INFO(error);
    REQUIRE(started);
    DEV_DO(renderer.SetPipelineTiming(true));
}

struct ResidentCounts
{
    std::uint64_t                 hits = 0, avoided = 0, misses = 0, copied = 0, fallbacks = 0;
    std::array<std::uint64_t, 10> reasons{};
};

// The hit, miss and fallback observations are developer telemetry; a shipping
// build reads zeros here and checks the residency seams instead.
ResidentCounts residentCounts(const NativeRendererD3D12& renderer)
{
    ResidentCounts counts;
#if REREVVED_DEVELOPER
    const auto stats = renderer.ReplayTimingStats();
    counts.hits      = stats.residentHits;
    counts.avoided   = stats.residentBytesAvoided;
    counts.misses    = stats.residentMisses;
    counts.copied    = stats.residentCopyBytes;
    counts.fallbacks = stats.residentFallbacks;
    counts.reasons   = stats.residentFallbackReasons;
#else
    static_cast<void>(renderer);
#endif
    return counts;
}

ResidentCounts operator-(ResidentCounts after, const ResidentCounts& before)
{
    after.hits -= before.hits;
    after.avoided -= before.avoided;
    after.misses -= before.misses;
    after.copied -= before.copied;
    after.fallbacks -= before.fallbacks;
    for (std::size_t reason = 0; reason < after.reasons.size(); ++reason)
        after.reasons[reason] -= before.reasons[reason];
    return after;
}

// Every outcome counts once; avoided and copied bytes follow the outcomes.
// Each presented terminal frame also draws its owner-less presentation, which
// falls back as owner.
void expectResident([[maybe_unused]] const ResidentCounts& got, [[maybe_unused]] std::uint64_t hits, [[maybe_unused]] std::uint64_t misses, [[maybe_unused]] std::uint64_t fallbacks, [[maybe_unused]] const char* what, [[maybe_unused]] std::uint64_t presented = 0, [[maybe_unused]] std::uint64_t span = kResidentSpan)
{
#if REREVVED_DEVELOPER
    UNSCOPED_INFO(std::format("{}: the presentation draw falls back as owner", what));
    REQUIRE(got.reasons[residentReason("owner")] >= presented);
    fallbacks += presented;
    std::uint64_t reasons = 0;
    for (const auto count : got.reasons)
        reasons += count;
    UNSCOPED_INFO(std::format("{}: hits {} avoided {} misses {} copied {} fallbacks {} by reason {}", what, got.hits, got.avoided, got.misses, got.copied, got.fallbacks, reasons));
    REQUIRE((got.hits == hits && got.avoided == hits * span && got.misses == misses && got.copied == misses * span && got.fallbacks == fallbacks &&
             reasons == fallbacks));
#endif
}

struct ResidencyState
{
    std::uint64_t entries, pending, retired, allocated, barriers, preBarriers, admissions, swap;
};

ResidencyState residencyState(NativeRendererD3D12& renderer)
{
    return onRendererThread(renderer, [&]
                            {
                                using Access = detail::NativeRenderFrameTestAccess;
                                return ResidencyState{ Access::VertexResidencyEntries(renderer), Access::VertexResidencyPending(renderer), Access::VertexResidencyRetiredBytes(renderer), Access::VertexResidencyAllocatedBytes(renderer), Access::VertexResidencyBarriers(renderer), Access::VertexResidencyPreBarriers(renderer), Access::VertexResidencyAdmissions(renderer), Access::VertexResidencySwap(renderer) };
                            });
}

// The device's debug layer reports no warning or error since construction.
struct ResidentValidation
{
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;

    explicit ResidentValidation(NativeRendererD3D12& renderer)
    {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        UNSCOPED_INFO("the debug layer must be available");
        REQUIRE((SUCCEEDED(detail::NativeRenderFrameTestAccess::Queue(renderer)->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)))));
        queue->ClearStoredMessages();
    }

    void check() const
    {
        for (UINT64 index = 0; index < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
        {
            SIZE_T size = 0;
            UNSCOPED_INFO("residency debug message size");
            REQUIRE(SUCCEEDED(queue->GetMessage(index, nullptr, &size)));
            std::vector<std::uint8_t> bytes(size);
            auto*                     message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            UNSCOPED_INFO("residency debug message");
            REQUIRE(SUCCEEDED(queue->GetMessage(index, message, &size)));
            // Resource state misuse is an error. Two warnings come from the recorder
            // itself with and without residency: an unoptimized render target clear
            // and a comparison function on a point sampler.
            const bool recorderWarning = message->Severity == D3D12_MESSAGE_SEVERITY_WARNING &&
                                         (message->ID == D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE || message->ID == D3D12_MESSAGE_ID_CREATE_SAMPLER_COMPARISON_FUNC_IGNORED);
            UNSCOPED_INFO(message->pDescription);
            REQUIRE((recorderWarning || (message->Severity != D3D12_MESSAGE_SEVERITY_WARNING && message->Severity != D3D12_MESSAGE_SEVERITY_ERROR && message->Severity != D3D12_MESSAGE_SEVERITY_CORRUPTION)));
        }
    }
};

// The pixels a renderer without residency draws.
std::vector<std::uint8_t> residentReference(const NativeRenderFrame& frame)
{
    NativeRendererD3D12 renderer;
    startResident(renderer, false, false);
    ResidentValidation validation(renderer);
    const auto         result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    validation.check();
    renderer.Shutdown();
    return result.resolvedOutput;
}

struct ResidentSequence
{
    std::vector<std::vector<std::uint8_t>> pixels;
    std::vector<ResidentCounts>            deltas;
    NativeDrawReplayCounts                 counts;
#if REREVVED_DEVELOPER
    NativeDrawReplayTimingStats total;
#endif
    ResidencyState state{};
};

// Submits the frames in order on a fresh renderer. A clear fault arms before
// the frame at clearBefore.
ResidentSequence residentSequence(bool on, bool exact, const std::vector<NativeRenderFrame>& frames, std::size_t clearBefore = ~std::size_t(0))
{
    NativeRendererD3D12 renderer;
    startResident(renderer, on, exact);
    ResidentValidation validation(renderer);
    ResidentSequence   sequence;
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        if (index == clearBefore)
            detail::NativeRenderFrameTestAccess::ResidencyFault(renderer, 7);
        const auto before = residentCounts(renderer);
        const auto result = renderer.SubmitRenderFrame(frames[index]);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        sequence.pixels.push_back(result.resolvedOutput);
        sequence.deltas.push_back(residentCounts(renderer) - before);
    }
    sequence.counts = renderer.ReplayCounts();
    DEV_DO(sequence.total = renderer.ReplayTimingStats());
    sequence.state = residencyState(renderer);
    validation.check();
    renderer.Shutdown();
    return sequence;
}

// Both modes draw like Off; barriers count copies and hits skip bytes.
void residentCompare()
{
    const auto a        = residentOwner({ 1, 0, 0, 1 });
    const auto b        = residentOwner({ 0, 1, 0, 1 });
    const auto bPrime   = residentOwner({ 0, 1, 1, 1 });
    const auto c        = residentOwner({ 0, 0, 1, 1 });
    const auto equalToA = std::make_shared<const std::vector<std::uint8_t>>(*a);
    // Misses, then hits; an equal-sized owner staged at the rewound offset
    // before a hit; equal bytes under a new owner; changed indices; and after a
    // cleared table the same owner copies again from its new staging offset.
    const std::vector<NativeRenderFrame> frames = {
        residentFrame({ residentDraw(a, 0), residentDraw(b, 1) }),
        residentFrame({ residentDraw(a, 0), residentDraw(b, 1) }),
        residentFrame({ residentDraw(bPrime, 0), residentDraw(a, 1), residentDraw(c, 2) }),
        residentFrame({ residentDraw(equalToA, 0) }),
        residentFrame({ residentDraw(a, 0, true), residentDraw(c, 1, true) }),
        residentFrame({ residentDraw(a, 0) }),
        residentFrame({ residentDraw(a, 0) }),
    };
    const std::array<std::array<std::uint64_t, 2>, 7> outcomes = { { { 0, 2 }, { 2, 0 }, { 1, 2 }, { 0, 1 }, { 2, 0 }, { 0, 1 }, { 1, 0 } } };
    const auto                                        off      = residentSequence(false, false, frames);
    for (const auto& delta : off.deltas)
        expectResident(delta, 0, 0, 0, "off counts nothing");
    UNSCOPED_INFO("off keeps no residency");
    REQUIRE((off.state.entries == 0 && off.state.allocated == 0 && off.state.admissions == 0 && off.state.barriers == 0 && off.state.swap == 0));
    for (const bool exact : { false, true })
    {
        const auto on = residentSequence(true, exact, frames, 5);
        for (std::size_t index = 0; index < frames.size(); ++index)
        {
            UNSCOPED_INFO(std::format("frame {} must draw what off draws", index));
            REQUIRE(on.pixels[index] == off.pixels[index]);
            expectResident(on.deltas[index], outcomes[index][0], outcomes[index][1], 0, std::format("frame {}", index).c_str(), 1);
        }
        UNSCOPED_INFO("residency changes no draw, submission, constant or index upload");
        REQUIRE((on.counts.recordedDraws == off.counts.recordedDraws && on.counts.queueSubmissions == off.counts.queueSubmissions &&
                 DEV_CHECK(on.total.drawUploadBytes[0] == off.total.drawUploadBytes[0] && on.total.drawUploadBytes[2] == off.total.drawUploadBytes[2])));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("exactly the hit vertex payload skips upload");
        REQUIRE(off.total.drawUploadBytes[1] - on.total.drawUploadBytes[1] == 6 * kResidentSpan);
#endif
        // Only a copy into a chunk this list already read or wrote, here the
        // second admission of the first frame and the third draw of the third,
        // transitions first.
        UNSCOPED_INFO(std::format("the pre-copy transition rule: {} transitions", on.state.preBarriers));
        REQUIRE(on.state.preBarriers == 2);
        UNSCOPED_INFO(std::format("one post-copy transition per admission: {} copies, {} transitions", on.state.admissions, on.state.barriers));
        REQUIRE((on.state.admissions == 6 && on.state.barriers == on.state.admissions));
        UNSCOPED_INFO(std::format("the table ends with the re-admitted owner: {} entries, {} bytes, {} pending, {} swaps", on.state.entries, on.state.allocated, on.state.pending, on.state.swap));
        REQUIRE((on.state.entries == 1 && on.state.allocated == kResidentFootprint && on.state.pending == 0 && on.state.swap == frames.size()));
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("operational events and gauges publish");
        REQUIRE((on.total.residentClears == 1 && on.total.residentEvictions == 0 && on.total.residentExpiries == 0 && on.total.residentEntries == 1 &&
                 on.total.residentBytes == kResidentFootprint && on.total.residentUploadFailures == 0 && on.total.residentInitFailures == 0));
#endif
    }
    // Admitted draws carry their owners too: every outcome counts once and the
    // pixels are unchanged.
    auto admittedFrames = std::vector<NativeRenderFrame>{ frames[0], frames[1] };
    for (auto& frame : admittedFrames)
        admitFrameDraws(frame);
    const auto admittedOff = residentSequence(false, false, admittedFrames);
    const auto admittedOn  = residentSequence(true, false, admittedFrames);
    for (std::size_t index = 0; index < admittedFrames.size(); ++index)
    {
        const auto& delta = admittedOn.deltas[index];
        UNSCOPED_INFO("admitted draws draw what off draws and count once each");
        REQUIRE((admittedOn.pixels[index] == admittedOff.pixels[index] && DEV_CHECK(delta.hits + delta.misses + delta.fallbacks == 3)));
    }
}

// Residency defaults off; requesting exact alone leaves frames unchanged.
void residentDefaults()
{
    UNSCOPED_INFO("residency and its exact mode default off");
    REQUIRE((!REXCVAR_GET(native_vertex_residency) && !REXCVAR_GET(native_vertex_residency_exact)));
    const auto a      = residentOwner({ 1, 0, 0, 1 });
    const auto frames = std::vector<NativeRenderFrame>{ residentFrame({ residentDraw(a, 0) }), residentFrame({ residentDraw(a, 0) }) };
    const auto plain  = residentSequence(false, false, frames);
    const auto inert  = residentSequence(false, true, frames);
    UNSCOPED_INFO("exact without residency is inert");
    REQUIRE((plain.pixels == inert.pixels && inert.state.entries == 0 && inert.state.allocated == 0 && inert.state.admissions == 0 && inert.state.swap == 0));
    for (const auto& delta : inert.deltas)
        expectResident(delta, 0, 0, 0, "inert exact counts nothing");
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("inert exact publishes no gauges");
    REQUIRE((inert.total.residentBytes == 0 && inert.total.residentEntries == 0 && inert.total.residentInitFailures == 0));
#endif
}

// Exact residency rejects changed owner bytes and hits unchanged bytes.
void residentExact()
{
    auto                bytes = std::make_shared<std::vector<std::uint8_t>>(makeDraw({ 1, 0, 0, 1 }, true).vertexData);
    const auto          alias = ResidentOwner(bytes);
    const auto          frame = residentFrame({ residentDraw(alias, 0) });
    const auto          blue  = makeDraw({ 0, 0, 1, 1 }, true).vertexData;
    const auto          drawn = residentReference(residentFrame({ residentDraw(residentOwner({ 0, 0, 1, 1 }), 0) }));
    NativeRendererD3D12 renderer;
    startResident(renderer, true, true);
    const auto submit = [&]
    {
        const auto before = residentCounts(renderer);
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        return std::pair{ result.resolvedOutput, residentCounts(renderer) - before };
    };
    const auto first = submit();
    expectResident(first.second, 0, 1, 0, "first frame", 1);
    const auto second = submit();
    expectResident(second.second, 1, 0, 0, "unchanged owner", 1);
    UNSCOPED_INFO("a hit draws the admitted bytes");
    REQUIRE(second.first == first.first);
    std::memcpy(bytes->data(), blue.data(), blue.size());
    const auto changed = submit();
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("a changed owner falls back as mutated");
    REQUIRE((changed.second.hits == 0 && changed.second.fallbacks == 2 && changed.second.reasons[residentReason("mutated")] == 1));
#endif
    UNSCOPED_INFO("a changed owner draws its current bytes");
    REQUIRE(changed.first == drawn);
    renderer.Shutdown();
}

// Outside-frame A,B,A draws bypass residency and count outside_frame.
void residentStandalone()
{
    const auto             a = residentOwner({ 1, 0, 0, 1 });
    const auto             b = residentOwner({ 0, 1, 0, 1 });
    NativeDrawStreamOutput output;
    output.gammaTable           = makeGammaTable(17, 31, 47, 311, 617, 919);
    output.returnResolvedOutput = true;
    const auto run              = [&](bool on)
    {
        NativeRendererD3D12 renderer;
        startResident(renderer, on, false);
        const auto before = residentCounts(renderer);
        auto       result = renderer.SubmitDrawFrame({ residentDraw(a, 0), residentDraw(b, 1), residentDraw(a, 2) }, {}, output);
        UNSCOPED_INFO(result.error);
        REQUIRE(result.success);
        auto repeated = renderer.SubmitDrawFrame({ residentDraw(a, 0) }, {}, output);
        UNSCOPED_INFO(repeated.error);
        REQUIRE(repeated.success);
        const auto delta = residentCounts(renderer) - before;
        const auto state = residencyState(renderer);
        renderer.Shutdown();
        return std::tuple{ result.resolvedOutput, repeated.resolvedOutput, delta, state };
    };
    const auto off = run(false);
    const auto on  = run(true);
    UNSCOPED_INFO("standalone draws match off");
    REQUIRE((std::get<0>(on) == std::get<0>(off) && std::get<1>(on) == std::get<1>(off)));
    const auto& delta = std::get<2>(on);
    expectResident(delta, 0, 0, 4, "standalone draws");
    UNSCOPED_INFO("standalone draws are outside_frame fallbacks with nothing admitted");
    REQUIRE((DEV_CHECK(delta.reasons[residentReason("outside_frame")] == 4) && std::get<3>(on).entries == 0 && std::get<3>(on).admissions == 0));
}

// Refusals preserve uploads/pixels and never retain an uncopied entry.
void residentFaults()
{
    using Access         = detail::NativeRenderFrameTestAccess;
    const auto a         = residentOwner({ 1, 0, 0, 1 });
    const auto b         = residentOwner({ 0, 1, 0, 1 });
    const auto frameAB   = residentFrame({ residentDraw(a, 0), residentDraw(b, 1) });
    const auto frameA    = residentFrame({ residentDraw(a, 0) });
    const auto expectAB  = residentReference(frameAB);
    const auto expectA   = residentReference(frameA);
    const auto submitted = [](NativeRendererD3D12& renderer, const NativeRenderFrame& frame)
    {
        const auto before = residentCounts(renderer);
        auto       result = renderer.SubmitRenderFrame(frame);
        return std::pair{ std::move(result), residentCounts(renderer) - before };
    };
    // 1 and 2: initialization refusals leave off behavior and one operational failure.
    for (const int fault : { 1, 2 })
    {
        NativeRendererD3D12 renderer;
        startResident(renderer, true, false, fault);
        ResidentValidation validation(renderer);
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("an initialization refusal counts one failure");
        REQUIRE(renderer.ReplayTimingStats().residentInitFailures == 1);
#endif
        const auto [result, delta] = submitted(renderer, frameAB);
        UNSCOPED_INFO("a refused initialization draws what off draws");
        REQUIRE((result.success && result.resolvedOutput == expectAB));
        expectResident(delta, 0, 0, 0, "refused initialization");
        const auto state = residencyState(renderer);
        UNSCOPED_INFO("a refused initialization keeps no table or chunks");
        REQUIRE((state.entries == 0 && state.allocated == 0 && state.retired == 0 && state.admissions == 0));
        validation.check();
        renderer.Shutdown();
    }
    // Two keys precede each fault; failed stream retries on a fresh renderer.
    for (const int fault : { 3, 4, 5 })
    {
        NativeRendererD3D12 renderer;
        startResident(renderer, true, false);
        const auto warm = renderer.SubmitRenderFrame(frameAB);
        UNSCOPED_INFO("two keys are live before a failure");
        REQUIRE((warm.success && warm.resolvedOutput == expectAB && residencyState(renderer).entries == 2));
        const auto before = residentCounts(renderer);
        Access::ResidencyFault(renderer, fault);
        const auto failed = renderer.SubmitRenderFrame(residentFrame({ residentDraw(residentOwner({ 0, 0, 1, 1 }), 0) }));
        UNSCOPED_INFO("an injected failure fails the frame");
        REQUIRE(!failed.success);
        renderer.Shutdown();
        UNSCOPED_INFO("a failed recording leaves no live key");
        REQUIRE(Access::VertexResidencySeamEntries(renderer, 3) == 0);
        if (fault == 3)
        {
            expectResident(residentCounts(renderer) - before, 0, 0, 0, "failed staging");
#if REREVVED_DEVELOPER
            UNSCOPED_INFO("failed staging counts an upload failure");
            REQUIRE(renderer.ReplayTimingStats().residentUploadFailures == 1);
#endif
        }
        NativeRendererD3D12 retry;
        startResident(retry, true, false);
        const auto result = retry.SubmitRenderFrame(frameAB);
        UNSCOPED_INFO("a fresh renderer draws what off draws");
        REQUIRE((result.success && result.resolvedOutput == expectAB));
        retry.Shutdown();
    }
    // 8: an exception while recording, after a hit, still ends the packet as
    // failed: no key survives it, and the unfinished draw publishes no hit.
    {
        NativeRendererD3D12 renderer;
        startResident(renderer, true, false);
        const auto warm = renderer.SubmitRenderFrame(frameAB);
        UNSCOPED_INFO("two keys are live before an exception");
        REQUIRE((warm.success && warm.resolvedOutput == expectAB && residencyState(renderer).entries == 2));
        const auto before = residentCounts(renderer);
        Access::ResidencyFault(renderer, 8);
        const auto failed = renderer.SubmitRenderFrame(frameA);
        UNSCOPED_INFO("the injected exception fails the frame: " + failed.error);
        REQUIRE((!failed.success && failed.error.find("native vertex residency test fault") != std::string::npos));
        const auto after = residentCounts(renderer);
        UNSCOPED_INFO("an exception while recording clears every live key at once");
        REQUIRE(Access::VertexResidencySeamEntries(renderer, 3) == 0);
        expectResident(after - before, 0, 0, 0, "exception after a hit");
        renderer.Shutdown();
    }
    // 6: a refused plan falls back as scheduling with the normal upload.
    {
        NativeRendererD3D12 renderer;
        startResident(renderer, true, false);
        ResidentValidation validation(renderer);
        Access::ResidencyFault(renderer, 6);
        const auto [result, delta] = submitted(renderer, frameA);
        UNSCOPED_INFO("a scheduling refusal draws what off draws");
        REQUIRE((result.success && result.resolvedOutput == expectA));
        expectResident(delta, 0, 0, 1, "scheduling refusal", 1);
        const auto state = residencyState(renderer);
        UNSCOPED_INFO("a scheduling refusal reserves nothing");
        REQUIRE((DEV_CHECK(delta.reasons[residentReason("scheduling")] == 1) && state.entries == 0 && state.admissions == 0));
        const auto [retry, copied] = submitted(renderer, frameA);
        UNSCOPED_INFO(retry.error);
        REQUIRE(retry.success);
        expectResident(copied, 0, 1, 0, "retry after scheduling", 1);
        validation.check();
        renderer.Shutdown();
    }
}

// The required clear seams each report zero live keys.
void residentSeams()
{
    using Access       = detail::NativeRenderFrameTestAccess;
    const auto a       = residentOwner({ 1, 0, 0, 1 });
    const auto b       = residentOwner({ 0, 1, 0, 1 });
    const auto c       = residentOwner({ 0, 0, 1, 1 });
    const auto unknown = ~std::uint64_t(0);
    // Shutdown clears two live keys.
    {
        NativeRendererD3D12 renderer;
        startResident(renderer, true, false);
        UNSCOPED_INFO("initialization clears its table before any other seam runs");
        REQUIRE((Access::VertexResidencySeamEntries(renderer, 0) == 0 && Access::VertexResidencySeamEntries(renderer, 1) == unknown && Access::VertexResidencySeamEntries(renderer, 2) == unknown &&
                 Access::VertexResidencySeamEntries(renderer, 3) == unknown));
        const auto warm = renderer.SubmitRenderFrame(residentFrame({ residentDraw(a, 0), residentDraw(b, 1) }));
        UNSCOPED_INFO("two keys are live before shutdown");
        REQUIRE((warm.success && residencyState(renderer).entries == 2));
        renderer.Shutdown();
        UNSCOPED_INFO("shutdown clears every live key");
        REQUIRE((Access::VertexResidencySeamEntries(renderer, 2) == 0 && Access::VertexResidencySeamEntries(renderer, 3) == unknown));
    }
    // A failed recording clears two live keys.
    {
        NativeRendererD3D12 renderer;
        startResident(renderer, true, false);
        const auto warm = renderer.SubmitRenderFrame(residentFrame({ residentDraw(a, 0), residentDraw(b, 1) }));
        UNSCOPED_INFO("two keys are live before a failure");
        REQUIRE((warm.success && residencyState(renderer).entries == 2));
        Access::ResidencyFault(renderer, 3);
        const auto failed = renderer.SubmitRenderFrame(residentFrame({ residentDraw(c, 0) }));
        UNSCOPED_INFO("an injected failure fails the frame");
        REQUIRE(!failed.success);
        renderer.Shutdown();
        UNSCOPED_INFO("a failed recording clears every live key");
        REQUIRE(Access::VertexResidencySeamEntries(renderer, 3) == 0);
    }
}

// Later lists hit queued copies; cleared chunks retire only after their fence.
void residentGate(bool clearBetween)
{
    using Access = detail::NativeRenderFrameTestAccess;
    using Microsoft::WRL::ComPtr;
    const auto          a = residentOwner({ 1, 0, 0, 1 });
    const auto          b = residentOwner({ 0, 1, 0, 1 });
    const auto          c = residentOwner({ 0, 0, 1, 1 });
    NativeRendererD3D12 renderer;
    startResident(renderer, true, false);
    ResidentValidation         validation(renderer);
    ComPtr<ID3D12CommandQueue> queue = Access::Queue(renderer);
    ComPtr<ID3D12Device>       device;
    ComPtr<ID3D12Fence>        gate;
    UNSCOPED_INFO("the native queue is held");
    REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) && SUCCEEDED(queue->Wait(gate.Get(), 1))));

    struct ReleaseGate
    {
        ID3D12Fence* fence;

        ~ReleaseGate()
        {
            fence->Signal(1);
        }
    } release{ gate.Get() };

    std::string                              error;
    std::shared_ptr<NativeRenderFrameTicket> first, second;
    const auto                               before = residentCounts(renderer);
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(residentRetainedFrame({ residentDraw(a, 0), residentDraw(b, 1) }, 9501), first, error));
    UNSCOPED_INFO("the first list records while held");
    REQUIRE((first->Recording().wait_for(kReadyCeiling) == std::future_status::ready && first->Recording().get().success));
    auto state = residencyState(renderer);
    UNSCOPED_INFO(std::format("the first list's copies are live: {} entries, {} pending, {} retired, {} swaps", state.entries, state.pending, state.retired, state.swap));
    REQUIRE((state.entries == 2 && state.pending == 0 && state.retired == 0 && state.allocated == kResidentFootprint && state.swap == 0));
    if (clearBetween)
        Access::ResidencyFault(renderer, 7);
    UNSCOPED_INFO(error);
    REQUIRE(renderer.QueueRenderFrame(residentRetainedFrame({ residentDraw(a, 0), residentDraw(c, 1) }, 9502), second, error));
    UNSCOPED_INFO("the second list records while held");
    REQUIRE((second->Recording().wait_for(kReadyCeiling) == std::future_status::ready && second->Recording().get().success));
    const auto counts = residentCounts(renderer) - before;
    state             = residencyState(renderer);
    UNSCOPED_INFO("nothing completes while the queue is held");
    REQUIRE(gate->GetCompletedValue() == 0);
    if (clearBetween)
    {
        expectResident(counts, 0, 4, 0, "cleared table");
        UNSCOPED_INFO(std::format("the cleared chunk stays retired while its list is pending: {} entries, {} retired", state.entries, state.retired));
        REQUIRE((state.entries == 2 && state.retired == kResidentChunkBytes));
    }
    else
    {
        expectResident(counts, 1, 3, 0, "later list");
        UNSCOPED_INFO(std::format("the later list reuses the first list's copy: {} entries, {} retired", state.entries, state.retired));
        REQUIRE((state.entries == 3 && state.retired == 0));
    }
    UNSCOPED_INFO("release the held queue");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    UNSCOPED_INFO("held lists complete");
    REQUIRE((first->Completion().wait_for(kReadyCeiling) == std::future_status::ready && second->Completion().wait_for(kReadyCeiling) == std::future_status::ready));
    const auto frame  = residentFrame({ residentDraw(a, 0) });
    const auto result = renderer.SubmitRenderFrame(frame);
    UNSCOPED_INFO("a completed table draws what off draws");
    REQUIRE((result.success && result.resolvedOutput == residentReference(frame)));
    const auto settled = residencyState(renderer);
    UNSCOPED_INFO(std::format("a completed fence frees the retired chunk: {} retired, {} pending, {} swaps", settled.retired, settled.pending, settled.swap));
    REQUIRE((settled.retired == 0 && settled.pending == 0 && settled.swap == 1 && settled.entries == (clearBetween ? 2u : 3u)));
    validation.check();
    renderer.Shutdown();
}

// Healthy-device drain failures clear keys and abandon resident chunks.
void residentAbandon()
{
    using Access = detail::NativeRenderFrameTestAccess;
    using Microsoft::WRL::ComPtr;
    const auto                 a = residentOwner({ 1, 0, 0, 1 });
    const auto                 b = residentOwner({ 0, 1, 0, 1 });
    NativeRendererD3D12        renderer;
    std::string                error;
    ComPtr<ID3D12CommandQueue> queue;
    startResident(renderer, true, false);
    queue = Access::Queue(renderer);
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence>  gate, marker;
    UNSCOPED_INFO("the native queue is held through the failed drain");
    REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) &&
             SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&marker))) && SUCCEEDED(queue->Wait(gate.Get(), 1))));

    struct ReleaseGate
    {
        ID3D12Fence* fence;

        ~ReleaseGate()
        {
            fence->Signal(1);
        }
    } release{ gate.Get() };

    std::shared_ptr<NativeRenderFrameTicket> ticket;
    UNSCOPED_INFO("submit resident copies before the failed drain");
    REQUIRE((renderer.QueueRenderFrame(residentRetainedFrame({ residentDraw(a, 0), residentDraw(b, 1) }, 9601), ticket, error) && ticket->Recording().get().success &&
             SUCCEEDED(queue->Signal(marker.Get(), 1))));
    const auto chunks = onRendererThread(renderer, [&]
                                         {
                                             return Access::VertexResidencyChunks(renderer);
                                         });
    UNSCOPED_INFO("four chunks hold two keys");
    REQUIRE((chunks.size() == 4 && residencyState(renderer).entries == 2));
    auto shutdown = std::async(std::launch::async, [&]
                               {
                                   renderer.Shutdown();
                               });
    // The drain abandons after its fence wait; the test runs serially.
    UNSCOPED_INFO("a failed healthy-device drain must use bounded abandonment");
    REQUIRE(shutdown.wait_for(std::chrono::seconds(12)) == std::future_status::ready);
    shutdown.get();
    UNSCOPED_INFO("abandonment clears the table at shutdown and at detach");
    REQUIRE((Access::VertexResidencySeamEntries(renderer, 1) == 0 && Access::VertexResidencySeamEntries(renderer, 2) == 0));
    UNSCOPED_INFO("release abandoned native storage after shutdown");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
    while (!marker->GetCompletedValue() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    UNSCOPED_INFO("abandoned submitted owners remain executable");
    REQUIRE(marker->GetCompletedValue() == 1);
    for (auto* chunk : chunks)
    {
        UNSCOPED_INFO("abandonment must leak the resident chunks instead of releasing them");
        REQUIRE(chunk->Release() >= 1);
    }
}

// Chunks charge Texture; CPU metadata/exact ceiling charge owned bytes.
void residentAccounting()
{
    using Access = detail::NativeRenderFrameTestAccess;

    struct Restore
    {
        ~Restore()
        {
            NativeRendererD3D12::SetDefaultDirectStream(false);
        }
    } restore;

    const auto a     = residentOwner({ 1, 0, 0, 1 });
    const auto b     = residentOwner({ 0, 1, 0, 1 });
    const auto frame = residentFrame({ residentDraw(a, 0), residentDraw(b, 1) });

    struct Row
    {
        std::uint64_t texture, attachment, upload, structures, owned;
    };

    const auto metadata = vertex_residency::VertexResidency(vertex_residency::Mode::Owner).MetadataBytes();
    const auto ceiling  = vertex_residency::VertexResidency(vertex_residency::Mode::Exact).SnapshotCeilingBytes();
    UNSCOPED_INFO("the table has CPU metadata and the exact ceiling is 64 MiB");
    REQUIRE((metadata > 0 && ceiling == 64ULL * 1024 * 1024));
    for (const bool direct : { false, true })
    {
        NativeRendererD3D12::SetDefaultDirectStream(direct);
        const auto measure = [&](bool on, bool exact)
        {
            NativeRendererD3D12 renderer;
            startResident(renderer, on, exact);
            const auto read = [&]
            {
                // The deleter releases a retired request's charge after destroying it.
                const auto deadline = std::chrono::steady_clock::now() + kReadyCeiling;
                while (Access::DirectCharges(renderer).first && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return onRendererThread(renderer, [&]
                                        {
                                            return Row{ Access::RetainedTextureStorage(renderer, false), Access::RetainedTextureStorage(renderer, true), Access::RetainedUploadStorage(renderer), Access::DirectPersistent(renderer)[2], direct ? 0 : Access::VertexResidencyOwnedBytes(renderer) };
                                        });
            };
            std::array<Row, 3> rows{};
            rows[0] = read();
            for (int pass = 0; pass < 2; ++pass)
            {
                const auto result = renderer.SubmitRenderFrame(frame);
                UNSCOPED_INFO(result.error);
                REQUIRE(result.success);
            }
            rows[1] = read();
            if (on)
                Access::ResidencyFault(renderer, 7);
            const auto result = renderer.SubmitRenderFrame(frame);
            UNSCOPED_INFO(result.error);
            REQUIRE(result.success);
            rows[2] = read();
            renderer.Shutdown();
            return rows;
        };
        const auto off   = measure(false, false);
        const auto owner = measure(true, false);
        const auto exact = measure(true, true);
        for (std::size_t row = 0; row < off.size(); ++row)
        {
            UNSCOPED_INFO(std::format("the Texture domain charges the fixed footprint in row {}: off {} owner {}", row, off[row].texture, owner[row].texture));
            REQUIRE((owner[row].texture - off[row].texture == kResidentFootprint && exact[row].texture - off[row].texture == kResidentFootprint));
            UNSCOPED_INFO("no other domain changes");
            REQUIRE((owner[row].attachment == off[row].attachment && owner[row].upload == off[row].upload));
            if (!direct)
            {
                UNSCOPED_INFO(std::format("ordinary owned bytes charge the metadata and the exact ceiling in row {}: off {} owner {} exact {}", row, off[row].owned, owner[row].owned, exact[row].owned));
                REQUIRE((owner[row].owned - off[row].owned == metadata && exact[row].owned - owner[row].owned == ceiling));
            }
            if (direct)
            {
                UNSCOPED_INFO(std::format("direct structures charge the metadata and the exact ceiling in row {}: off {} owner {} exact {}", row, off[row].structures, owner[row].structures, exact[row].structures));
                REQUIRE((owner[row].structures - off[row].structures == metadata && exact[row].structures - owner[row].structures == ceiling));
            }
        }
    }
}

// A 16 MiB owner fills one chunk. Its first 96 bytes are the triangle.
ResidentOwner residentLargeOwner(const std::array<float, 4>& color)
{
    auto bytes = makeDraw(color, true).vertexData;
    bytes.resize(kResidentChunkBytes);
    return std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
}

#if REREVVED_DEVELOPER
struct ResidentEvents
{
    std::uint64_t evictions, expiries, clears;

    bool operator==(const ResidentEvents&) const = default;
};

ResidentEvents residentEvents(const NativeDrawReplayTimingStats& stats)
{
    return { stats.residentEvictions, stats.residentExpiries, stats.residentClears };
}
#endif

// Shuts the renderer down and starts it again on a new device. Cumulative
// event totals only grow across the pair, and the restart keeps them.
void restartResident(NativeRendererD3D12& renderer, [[maybe_unused]] const char* what)
{
#if REREVVED_DEVELOPER
    const auto before = residentEvents(renderer.ReplayTimingStats());
#endif
    renderer.Shutdown();
#if REREVVED_DEVELOPER
    const auto shutdown = residentEvents(renderer.ReplayTimingStats());
    UNSCOPED_INFO(std::format("{}: shutdown keeps the totals", what));
    REQUIRE((shutdown.evictions == before.evictions && shutdown.expiries == before.expiries && shutdown.clears >= before.clears));
#endif
    startResident(renderer, true, false);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO(std::format("{}: a restart keeps the cumulative totals", what));
    REQUIRE(residentEvents(renderer.ReplayTimingStats()) == shutdown);
#endif
}

// Cumulative resident events survive restart; timing deltas do not underflow.
void residentRestart()
{
    using Access                = detail::NativeRenderFrameTestAccess;
    const auto          a       = residentOwner({ 1, 0, 0, 1 });
    const auto          b       = residentOwner({ 0, 1, 0, 1 });
    const auto          frameA  = residentFrame({ residentDraw(a, 0) });
    const auto          frameAB = residentFrame({ residentDraw(a, 0), residentDraw(b, 1) });
    NativeRendererD3D12 renderer;
    startResident(renderer, true, false);
    ResidentValidation validation(renderer);
    UNSCOPED_INFO("first frame");
    REQUIRE(renderer.SubmitRenderFrame(frameAB).success);
    // A clear that removes keys, then an owner that expires.
    Access::ResidencyFault(renderer, 7);
    UNSCOPED_INFO("frame after a clear");
    REQUIRE(renderer.SubmitRenderFrame(frameAB).success);
    {
        const auto c = residentOwner({ 0, 0, 1, 1 });
        UNSCOPED_INFO("frame with an owner that expires");
        REQUIRE(renderer.SubmitRenderFrame(residentFrame({ residentDraw(c, 0) })).success);
    }
    UNSCOPED_INFO("frame that sweeps the expired owner");
    REQUIRE(renderer.SubmitRenderFrame(frameA).success);
#if REREVVED_DEVELOPER
    const auto first = residentEvents(renderer.ReplayTimingStats());
    UNSCOPED_INFO(std::format("the first run clears {} and expires {}", first.clears, first.expiries));
    REQUIRE((first.clears >= 1 && first.expiries >= 1));
#endif
    validation.check();
    restartResident(renderer, "restart");
    const ResidentValidation again(renderer);
#if REREVVED_DEVELOPER
    const auto before = renderer.ReplayTimingStats();
#endif
    UNSCOPED_INFO("frame after a restart");
    REQUIRE(renderer.SubmitRenderFrame(frameAB).success);
#if REREVVED_DEVELOPER
    const auto after = renderer.ReplayTimingStats();
    UNSCOPED_INFO("totals only grow after a restart");
    REQUIRE((after.residentEvictions >= before.residentEvictions && after.residentExpiries >= before.residentExpiries && after.residentClears >= before.residentClears));
#endif
    Access::ResidencyFault(renderer, 7);
    UNSCOPED_INFO("clear after a restart");
    REQUIRE(renderer.SubmitRenderFrame(frameAB).success);
#if REREVVED_DEVELOPER
    UNSCOPED_INFO("a clear after a restart adds to the cumulative total");
    REQUIRE(renderer.ReplayTimingStats().residentClears == after.residentClears + 1);
#endif
    again.check();
    renderer.Shutdown();
}

#if REREVVED_DEVELOPER
// Snapshot lock blocks draw publication; gauge uses the same timing lock.
void residentGaugeLock()
{
    using Access                                   = detail::NativeRenderFrameTestAccess;
    const auto                               a     = residentOwner({ 1, 0, 0, 1 });
    const auto                               frame = residentRetainedFrame({ residentDraw(a, 0) }, 9801);
    NativeRendererD3D12                      renderer;
    std::shared_ptr<NativeRenderFrameTicket> ticket;
    std::string                              error;
    startResident(renderer, true, false);
    UNSCOPED_INFO("warm the pipelines and admit the owner");
    REQUIRE(renderer.SubmitRenderFrame(residentFrame({ residentDraw(a, 0) })).success);
    renderer.SetPipelineTiming(false);
    {
        auto lock = Access::ReplayTimingLock(renderer);
        UNSCOPED_INFO(error);
        REQUIRE(renderer.QueueRenderFrame(frame, ticket, error));
        UNSCOPED_INFO("gauge publication waits for the snapshot lock");
        REQUIRE(ticket->Recording().wait_for(std::chrono::seconds(2)) == std::future_status::timeout);
    }
    UNSCOPED_INFO("recording finishes once the snapshot lock is free");
    REQUIRE((ticket->Recording().wait_for(kReadyCeiling) == std::future_status::ready && ticket->Recording().get().success));
    UNSCOPED_INFO("the gauges published");
    REQUIRE(renderer.ReplayTimingStats().residentEntries == 1);
    renderer.Shutdown();
}
#endif

// Cap eviction respects swap protection and waits for held-queue fences.
void residentPressure()
{
    using Access = detail::NativeRenderFrameTestAccess;
    using Microsoft::WRL::ComPtr;
    const std::array<float, 4> colors[5] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 0, 1 }, { 0, 1, 1, 1 } };
    // Part one: a chunk of recent use is not evicted, and one past the swap
    // protection is, whole and oldest first.
    {
        ResidentOwner big[5];
        for (std::size_t index = 0; index < 5; ++index)
            big[index] = residentLargeOwner(colors[index]);
        NativeRendererD3D12 renderer;
        startResident(renderer, true, false);
        ResidentValidation validation(renderer);
        const auto         step = [&](const NativeRenderFrame& frame, std::uint64_t hits, std::uint64_t misses, std::uint64_t fallbacks, [[maybe_unused]] std::uint64_t evictions, const char* what)
        {
            const auto before = residentCounts(renderer);
#if REREVVED_DEVELOPER
            const auto evictionsBefore = renderer.ReplayTimingStats().residentEvictions;
#endif
            const auto result = renderer.SubmitRenderFrame(frame);
            UNSCOPED_INFO(std::format("{}: {}", what, result.error));
            REQUIRE(result.success);
            expectResident(residentCounts(renderer) - before, hits, misses, fallbacks, what, 1, kResidentChunkBytes);
#if REREVVED_DEVELOPER
            const auto evicted = renderer.ReplayTimingStats().residentEvictions - evictionsBefore;
            UNSCOPED_INFO(std::format("{}: {} evictions", what, evicted));
            REQUIRE(evicted == evictions);
#endif
            UNSCOPED_INFO(std::format("{}: draws what off draws", what));
            REQUIRE(result.resolvedOutput == residentReference(frame));
            return result;
        };
        step(residentFrame({ residentDraw(big[0], 0), residentDraw(big[1], 1), residentDraw(big[2], 2), residentDraw(big[3], 3) }), 0, 4, 0, 0, "fill every chunk");
        auto state = residencyState(renderer);
        UNSCOPED_INFO(std::format("four chunks are full: {} entries, {} swaps", state.entries, state.swap));
        REQUIRE((state.entries == 4 && state.allocated == kResidentFootprint && state.swap == 1));
        const auto fifth = residentFrame({ residentDraw(big[4], 0) });
        step(fifth, 0, 0, 1, 0, "recent chunks are protected");
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("a full table of recent chunks falls back as cap");
        REQUIRE(renderer.ReplayTimingStats().residentFallbackReasons[residentReason("cap")] == 1);
#endif
        step(fifth, 0, 1, 0, 1, "an old chunk is evicted");
        state = residencyState(renderer);
        UNSCOPED_INFO(std::format("the eviction replaced one entry: {} entries, {} swaps", state.entries, state.swap));
        REQUIRE((state.entries == 4 && state.swap == 3));
        // The second chunk survived: a hit. The first owner lost its key and misses.
        step(residentFrame({ residentDraw(big[1], 0), residentDraw(big[0], 1) }), 1, 1, 0, 1, "the oldest chunk went whole");
        validation.check();
        restartResident(renderer, "after evictions");
#if REREVVED_DEVELOPER
        UNSCOPED_INFO("evictions stay cumulative across a restart");
        REQUIRE(renderer.ReplayTimingStats().residentEvictions == 2);
#endif
        renderer.Shutdown();
    }
    // Part two: a hit in a held list keeps its chunk from being reclaimed
    // while the insertion list's fence alone has completed.
    {
        ResidentOwner                            keep[3], later;
        NativeRendererD3D12                      renderer;
        std::shared_ptr<NativeRenderFrameTicket> hitTicket, pressureTicket;
        std::string                              error;
        startResident(renderer, true, false);
        ResidentValidation         validation(renderer);
        ComPtr<ID3D12CommandQueue> queue = Access::Queue(renderer);
        ComPtr<ID3D12Device>       device;
        ComPtr<ID3D12Fence>        gate;
        for (std::size_t index = 0; index < 3; ++index)
            keep[index] = residentLargeOwner(colors[index + 1]);
        later = residentLargeOwner(colors[4]);
        {
            // The first owner lives only in these frames.
            const auto first = residentLargeOwner(colors[0]);
            UNSCOPED_INFO("fill every chunk");
            REQUIRE(renderer.SubmitRenderFrame(residentFrame({ residentDraw(first, 0), residentDraw(keep[0], 1), residentDraw(keep[1], 2), residentDraw(keep[2], 3) })).success);
            UNSCOPED_INFO("the native queue is held");
            REQUIRE((SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))) && SUCCEEDED(queue->Wait(gate.Get(), 1))));
            UNSCOPED_INFO(error);
            REQUIRE(renderer.QueueRenderFrame(residentRetainedFrame({ residentDraw(first, 0) }, 9701), hitTicket, error));
            UNSCOPED_INFO("the held list records its hit");
            REQUIRE((hitTicket->Recording().wait_for(kReadyCeiling) == std::future_status::ready && hitTicket->Recording().get().success));
        }

        struct ReleaseGate
        {
            ID3D12Fence* fence;

            ~ReleaseGate()
            {
                fence->Signal(1);
            }
        } release{ gate.Get() };

        const auto before           = residentCounts(renderer);
        const auto admissionsBefore = residencyState(renderer).admissions;
#if REREVVED_DEVELOPER
        const auto beforeStats = renderer.ReplayTimingStats();
#endif
        UNSCOPED_INFO(error);
        REQUIRE(renderer.QueueRenderFrame(residentRetainedFrame({ residentDraw(later, 0) }, 9702), pressureTicket, error));
        UNSCOPED_INFO("the pressure list records while held");
        REQUIRE((pressureTicket->Recording().wait_for(kReadyCeiling) == std::future_status::ready && pressureTicket->Recording().get().success));
        expectResident(residentCounts(renderer) - before, 0, 0, 1, "held chunk");
        UNSCOPED_INFO("a chunk whose last use is pending is not reclaimed, so the pressure list admits nothing");
        REQUIRE(residencyState(renderer).admissions == admissionsBefore);
#if REREVVED_DEVELOPER
        const auto held = renderer.ReplayTimingStats();
        UNSCOPED_INFO(std::format("a chunk whose last use is pending is not reclaimed: {} evictions, {} expiries", held.residentEvictions - beforeStats.residentEvictions, held.residentExpiries - beforeStats.residentExpiries));
        REQUIRE((held.residentFallbackReasons[residentReason("cap")] == beforeStats.residentFallbackReasons[residentReason("cap")] + 1 && held.residentEvictions == beforeStats.residentEvictions &&
                 held.residentExpiries == beforeStats.residentExpiries + 1));
#endif
        UNSCOPED_INFO("nothing completes while the queue is held");
        REQUIRE(gate->GetCompletedValue() == 0);
        UNSCOPED_INFO("release the held queue");
        REQUIRE(SUCCEEDED(gate->Signal(1)));
        UNSCOPED_INFO("held lists complete");
        REQUIRE((hitTicket->Completion().wait_for(kReadyCeiling) == std::future_status::ready && pressureTicket->Completion().wait_for(kReadyCeiling) == std::future_status::ready));
        const auto frame  = residentFrame({ residentDraw(later, 0) });
        const auto result = renderer.SubmitRenderFrame(frame);
        UNSCOPED_INFO("a completed fence allows the reclaim and draws what off draws");
        REQUIRE((result.success && result.resolvedOutput == residentReference(frame)));
        UNSCOPED_INFO("the reclaim admits the owner once");
        REQUIRE(residencyState(renderer).admissions == admissionsBefore + 1);
#if REREVVED_DEVELOPER
        const auto settled = renderer.ReplayTimingStats();
        UNSCOPED_INFO("the reclaim counts one miss and one eviction");
        REQUIRE((settled.residentMisses == held.residentMisses + 1 && settled.residentEvictions == held.residentEvictions + 1));
#endif
        validation.check();
        renderer.Shutdown();
    }
}

// Run one named case or all; print the case name before failures.
bool runVertexResidencyCases(std::string_view only)
{
    const auto directory = std::filesystem::temp_directory_path() / ("native-vertex-residency-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const std::vector<std::pair<const char*, std::function<void()>>> cases = {
        { "residentDefaults", residentDefaults },
        { "residentCompare", residentCompare },
        { "residentExact", residentExact },
        { "residentStandalone", residentStandalone },
        { "residentFaults", residentFaults },
        { "residentSeams", residentSeams },
        { "residentGate", []
          {
              residentGate(false);
          } },
        { "residentGateClear", []
          {
              residentGate(true);
          } },
        { "residentAbandon", residentAbandon },
        { "residentAccounting", residentAccounting },
        { "residentRestart", residentRestart },
#if REREVVED_DEVELOPER
        { "residentGaugeLock", residentGaugeLock },
#endif
        { "residentPressure", residentPressure },
    };
    bool ok = true, ran = false;
    for (const auto& [name, body] : cases)
    {
        if (!only.empty() && only != name)
            continue;
        ran = true;
        try
        {
            body();
            std::cout << name << ": passed\n";
        }
        catch (const std::exception& exception)
        {
            std::cerr << name << ": " << exception.what() << '\n';
            ok = false;
        }
    }
    if (!ran)
    {
        std::cerr << "unknown vertex residency case: " << only << '\n';
        ok = false;
    }
    std::error_code removeError;
    std::filesystem::remove_all(directory, removeError);
    return ok;
}

} // namespace

TEST_CASE("native_frame_stream", "[.][renderer-research][native-frame-stream][native_gpu][timeout-600]")
{
    char enabled[2]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) != 1 || enabled[0] != '1')
    {
        SKIP("Set REREVVED_TEST_NATIVE_D3D12=1 to run the D3D12 submission lifecycle check");
    }
    rerevved::tests::ScopedLogging logging;
    std::filesystem::path          outputDirectory;
    OutputCleanup                  cleanup(outputDirectory);
    NativeRendererD3D12::ConfigureReplayDiagnostics();
#ifdef REREVVED_PREPARED_RG16_TEST
    if (const auto* rg16Only = std::getenv("REREVVED_PREPARED_RG16_ONLY");
        rg16Only && std::string_view(rg16Only) == "1")
    {
        checkPreparedRg16();
        return;
    }
#endif
#ifdef REREVVED_PREPARED_MAIN_CLEAR_TEST
    if (const auto* mainClearOnly = std::getenv("REREVVED_PREPARED_MAIN_CLEAR_ONLY");
        mainClearOnly && std::string_view(mainClearOnly) == "1")
    {
        checkPreparedMainClear();
        return;
    }
#endif
#ifdef REREVVED_PREPARED_FRAME_BRIDGE_TEST
    if (const auto* bridgeOnly = std::getenv("REREVVED_PREPARED_BRIDGE_ONLY");
        bridgeOnly && std::string_view(bridgeOnly) == "1")
    {
        checkPreparedSdkBridge();
        return;
    }
#endif
    const auto red = []()
    {
        auto draw             = makeDraw({ 1, 0, 0, 1 }, false);
        draw.sharedVertexData = std::make_shared<const std::vector<std::uint8_t>>(std::move(draw.vertexData));
        return draw;
    }();
    const auto blue                     = makeDraw({ 0, 0, 1, 1 }, true);
    const auto green                    = makeDraw({ 0, 1, 0, 1 }, true);
    auto       blendVariant             = red;
    blendVariant.blend.enabled          = true;
    blendVariant.blend.sourceColor      = NativeDrawReplayBlendFactor::One;
    blendVariant.blend.destinationColor = NativeDrawReplayBlendFactor::Zero;
    auto shaderVariant                  = red;
    shaderVariant.pixelShaderDxil       = compile(
        "float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target0 { "
        "return float4(c.b, c.g, c.r, c.a); }",
        "ps_5_1");
    UNSCOPED_INFO("shader-byte cache discriminator must differ despite equal metadata hash");
    REQUIRE(shaderVariant.pixelShaderDxil != red.pixelShaderDxil);
    const auto gammaA = makeGammaTable(17, 31, 47, 311, 617, 919);
    const auto gammaB = makeGammaTable(97, 149, 211, 701, 809, 997);
    // Each run writes about 0.5 GB of images; keep them out of the working
    // directory and remove them on success.
    const auto directory = std::filesystem::temp_directory_path() /
                           ("native-frame-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    UNSCOPED_INFO("fresh test output directory required");
    REQUIRE(std::filesystem::create_directory(directory));
    outputDirectory = directory;
    checkCentroidEdgeRamp(directory);
    checkSolidDestinationSamples(directory);
    NativeDrawStreamOutput firstOutput;
    firstOutput.outputPath           = directory / "first.rgb10";
    firstOutput.gammaTable           = gammaA;
    firstOutput.returnResolvedOutput = true;
    NativeDrawStreamOutput secondOutput;
    secondOutput.outputPath = directory / "second.rgb10";
    secondOutput.gammaTable = gammaB;
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO("submission before startup must fail");
    REQUIRE(!renderer.SubmitDrawFrame({ red }).success);
    UNSCOPED_INFO(error);
    REQUIRE(renderer.StartDrawStream(error));
    UNSCOPED_INFO("empty frame must fail admission");
    REQUIRE(!renderer.SubmitDrawFrame({}).success);
    auto result = renderer.SubmitDrawFrame({ red });
    UNSCOPED_INFO("draw without requested output must complete its native fence");
    REQUIRE((result.success && result.completionFenceValue));
    const auto cacheAfterNoOutput = renderer.ReplayCacheStats();
    UNSCOPED_INFO("draw without requested output must avoid resolve and readback allocation");
    REQUIRE((cacheAfterNoOutput.outputBundleCreations == 0 &&
             cacheAfterNoOutput.outputBundleReuses == 0 &&
             cacheAfterNoOutput.immutableConstantCreations == 1));
    UNSCOPED_INFO("first draw must create one upload buffer per input role");
    REQUIRE((cacheAfterNoOutput.drawUploadCreations == 5 && cacheAfterNoOutput.drawUploadReuses == 0));
    result = renderer.SubmitDrawFrame({ red, blue }, directory / "first.rgba", firstOutput);
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue));
    auto fence       = result.completionFenceValue;
    auto ownedPixels = std::move(result.resolvedOutput);
    checkGammaPixels(ownedPixels, packRgb10Output(17, 31, 919), packRgb10Output(311, 31, 47));
    checkImage(directory / "first.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
    checkGammaImage(directory / "first.rgb10",
                    packRgb10Output(17, 31, 919),
                    packRgb10Output(311, 31, 47));
    const auto cacheAfterFirst = renderer.ReplayCacheStats();
    UNSCOPED_INFO("batched draws must have independent retained upload buffers");
    REQUIRE((cacheAfterFirst.drawUploadCreations == 10 && cacheAfterFirst.drawUploadReuses == 5));
    UNSCOPED_INFO("first native frame must populate the renderer-owned immutable cache");
    REQUIRE((cacheAfterFirst.helperShaderCompilations >= 6 &&
             cacheAfterFirst.rootSignatureCreations >= 2 &&
             cacheAfterFirst.pipelineCreations >= 8 &&
             cacheAfterFirst.descriptorHeapCreations >= 3 &&
             cacheAfterFirst.immutableConstantCreations == 3));
    NativeDrawStreamOutput repeatedOutput;
    repeatedOutput.outputPath           = directory / "repeated.rgb10";
    repeatedOutput.gammaTable           = gammaB;
    repeatedOutput.returnResolvedOutput = true;
    result                              = renderer.SubmitDrawFrame({ red, blue }, directory / "repeated.rgba", repeatedOutput);
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue > fence));
    fence = result.completionFenceValue;
    checkImage(directory / "repeated.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
    checkGammaImage(directory / "repeated.rgb10",
                    packRgb10Output(97, 149, 997),
                    packRgb10Output(701, 149, 211));
    checkGammaPixels(result.resolvedOutput,
                     packRgb10Output(97, 149, 997),
                     packRgb10Output(701, 149, 211));
    const auto cacheAfterOutputReuse = renderer.ReplayCacheStats();
    UNSCOPED_INFO("compatible output requests must reuse the retained bundle");
    REQUIRE((cacheAfterOutputReuse.outputBundleCreations == cacheAfterFirst.outputBundleCreations &&
             cacheAfterOutputReuse.outputBundleReuses == cacheAfterFirst.outputBundleReuses + 1));
    result = renderer.SubmitDrawFrame({ blendVariant });
    UNSCOPED_INFO("alternate effective blend state must complete its native fence");
    REQUIRE((result.success && result.completionFenceValue > fence));
    const auto cacheAfterBlend = renderer.ReplayCacheStats();
    UNSCOPED_INFO("alternate effective PSO state must create one guest pipeline");
    REQUIRE(cacheAfterBlend.pipelineCreations == cacheAfterFirst.pipelineCreations + 1);
    fence  = result.completionFenceValue;
    result = renderer.SubmitDrawFrame({ shaderVariant }, directory / "shader-variant.rgba");
    UNSCOPED_INFO("shader-byte variant with an equal metadata hash must complete");
    REQUIRE((result.success && result.completionFenceValue > fence));
    checkImage(directory / "shader-variant.rgba", { 0, 0, 255, 255 }, { 0, 0, 255, 255 });
    const auto cacheAfterShader = renderer.ReplayCacheStats();
    UNSCOPED_INFO("shader bytes must participate in the guest pipeline cache key");
    REQUIRE(cacheAfterShader.pipelineCreations == cacheAfterBlend.pipelineCreations + 1);
    fence  = result.completionFenceValue;
    result = renderer.SubmitDrawFrame({ red });
    UNSCOPED_INFO("revisited guest pipeline variant must complete its native fence");
    REQUIRE((result.success && result.completionFenceValue > fence));
    const auto cacheAfterReuse = renderer.ReplayCacheStats();
    UNSCOPED_INFO("revisited variants must reuse helper and immutable cache entries");
    REQUIRE((cacheAfterReuse.pipelineCreations == cacheAfterShader.pipelineCreations &&
             cacheAfterReuse.helperShaderCompilations == cacheAfterFirst.helperShaderCompilations &&
             cacheAfterReuse.rootSignatureCreations == cacheAfterFirst.rootSignatureCreations &&
             cacheAfterReuse.descriptorHeapCreations == cacheAfterFirst.descriptorHeapCreations &&
             cacheAfterReuse.immutableConstantCreations == cacheAfterFirst.immutableConstantCreations));
    UNSCOPED_INFO("submission must not overwrite an existing output");
    REQUIRE(!renderer.SubmitDrawFrame({ red }, directory / "first.rgba").success);
    UNSCOPED_INFO("submission must not overwrite an existing gamma output");
    REQUIRE(!renderer.SubmitDrawFrame({ red }, {}, firstOutput).success);
    result = renderer.SubmitDrawFrame({ red, blue }, directory / "second.rgba", secondOutput);
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue > fence));
    fence = result.completionFenceValue;
    checkImage(directory / "second.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
    checkGammaImage(directory / "second.rgb10",
                    packRgb10Output(97, 149, 997),
                    packRgb10Output(701, 149, 211));
    UNSCOPED_INFO("unrequested returned pixels must remain empty");
    REQUIRE(result.resolvedOutput.empty());
    checkGammaPixels(ownedPixels, packRgb10Output(17, 31, 919), packRgb10Output(311, 31, 47));
    const auto cacheAfterGammaRecreate = renderer.ReplayCacheStats();
    UNSCOPED_INFO("gamma presence changes must recreate the output bundle");
    REQUIRE(cacheAfterGammaRecreate.outputBundleCreations ==
            cacheAfterOutputReuse.outputBundleCreations + 2);
    NativeDrawStreamOutput returnedOutput;
    returnedOutput.gammaTable           = gammaB;
    returnedOutput.returnResolvedOutput = true;
    result                              = renderer.SubmitDrawFrame({ red, blue }, {}, returnedOutput);
    UNSCOPED_INFO(result.error);
    REQUIRE((result.success && result.completionFenceValue > fence));
    fence = result.completionFenceValue;
    checkGammaPixels(result.resolvedOutput, packRgb10Output(97, 149, 997), packRgb10Output(701, 149, 211));
    NativeDrawStreamOutput invalidOutput;
    invalidOutput.gammaTable = gammaA;
    UNSCOPED_INFO("gamma output without a path must be rejected");
    REQUIRE(!renderer.SubmitDrawFrame({ red }, {}, invalidOutput).success);
    NativeDrawStreamOutput aliasedOutput;
    aliasedOutput.outputPath = directory / "." / "alias.rgb10";
    aliasedOutput.gammaTable = gammaA;
    UNSCOPED_INFO("sample and gamma output aliases must be rejected");
    REQUIRE(!renderer.SubmitDrawFrame({ red }, directory / "alias.rgb10", aliasedOutput).success);
    NativeDrawStreamOutput caseAliasedOutput;
    caseAliasedOutput.outputPath = directory / "FIRST.RGBA";
    caseAliasedOutput.gammaTable = gammaA;
    UNSCOPED_INFO("case-equivalent sample and gamma output aliases must be rejected");
    REQUIRE(!renderer.SubmitDrawFrame({ red }, directory / "first.rgba", caseAliasedOutput).success);
    checkImage(directory / "first.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
    NativeDrawStreamOutput gammaOnlyOutput;
    gammaOnlyOutput.outputPath = directory / "gamma-only.rgb10";
    gammaOnlyOutput.gammaTable = gammaA;
    result                     = renderer.SubmitDrawFrame({ red }, {}, gammaOnlyOutput);
    UNSCOPED_INFO("gamma-only output must complete its native fence");
    REQUIRE((result.success && result.completionFenceValue > fence));
    fence = result.completionFenceValue;
    checkGammaImage(directory / "gamma-only.rgb10",
                    packRgb10Output(311, 31, 47),
                    packRgb10Output(311, 31, 47));
    result = renderer.SubmitDrawFrame({ green }, directory / "third.rgba");
    UNSCOPED_INFO("render with sample readback must complete its native fence");
    REQUIRE((result.success && result.completionFenceValue > fence));
    fence = result.completionFenceValue;
    checkImage(directory / "third.rgba", { 0, 255, 0, 255 }, { 0, 0, 0, 0 });
    const auto cacheBeforeOutputVariants = renderer.ReplayCacheStats();
    auto       depthVariant              = red;
    depthVariant.sampleMask              = 0xF;
    depthVariant.depth.enabled           = true;
    depthVariant.depth.writeEnabled      = true;
    result                               = renderer.SubmitDrawFrame({ depthVariant }, directory / "depth.rgba");
    UNSCOPED_INFO("depth-layout output variant must complete its native fence");
    REQUIRE((result.success && result.completionFenceValue > fence));
    fence = result.completionFenceValue;
    checkSize(directory / "depth.rgba", 1280U * 720U * 4U * 2U);
    const auto cacheAfterOutputVariants = renderer.ReplayCacheStats();
    UNSCOPED_INFO("depth-layout changes must recreate the output bundle");
    REQUIRE(cacheAfterOutputVariants.outputBundleCreations ==
            cacheBeforeOutputVariants.outputBundleCreations + 1);
    result = renderer.SubmitDrawFrame({ red });
    UNSCOPED_INFO("render without readback must complete its native fence");
    REQUIRE((result.success && result.completionFenceValue > fence));
    auto firstQueued  = std::async(std::launch::async, [&]()
                                   {
                                      return renderer.SubmitDrawFrame({ red, blue });
                                   });
    auto secondQueued = std::async(std::launch::async, [&]()
                                   {
                                       return renderer.SubmitDrawFrame({ green });
                                   });
    UNSCOPED_INFO("serialized submitters must wake the renderer and complete");
    REQUIRE((firstQueued.wait_for(kReadyCeiling) == std::future_status::ready &&
             secondQueued.wait_for(kReadyCeiling) == std::future_status::ready));
    const auto firstResult  = firstQueued.get();
    const auto secondResult = secondQueued.get();
    UNSCOPED_INFO("serialized frames must have separate completed native fences");
    REQUIRE((firstResult.success && secondResult.success && firstResult.completionFenceValue &&
             secondResult.completionFenceValue && firstResult.completionFenceValue != secondResult.completionFenceValue));
    const auto uploadsBeforeGrowth = renderer.ReplayCacheStats();
    auto       largerGeometry      = red;
    largerGeometry.vertexData.assign(red.vertexBytes().begin(), red.vertexBytes().end());
    largerGeometry.sharedVertexData.reset();
    largerGeometry.vertexData.insert(largerGeometry.vertexData.end(), blue.vertexData.begin(), blue.vertexData.end());
    largerGeometry.indices    = { 3, 4, 5, 3, 4, 5 };
    largerGeometry.indexCount = 6;
    result                    = renderer.SubmitDrawFrame({ largerGeometry }, directory / "larger-geometry.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "larger-geometry.rgba", { 0, 0, 255, 255 }, { 0, 0, 255, 255 });
    UNSCOPED_INFO("larger geometry must grow only its vertex and index uploads");
    REQUIRE(renderer.ReplayCacheStats().drawUploadCreations == uploadsBeforeGrowth.drawUploadCreations + 2);
    largerGeometry.indices = { 0, 1, 2, 0, 1, 2 };
    result                 = renderer.SubmitDrawFrame({ largerGeometry }, directory / "changed-indices.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "changed-indices.rgba", { 255, 0, 0, 255 }, { 255, 0, 0, 255 });
    result = renderer.SubmitDrawFrame({ green }, directory / "smaller-geometry.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "smaller-geometry.rgba", { 0, 255, 0, 255 }, { 0, 0, 0, 0 });
    const auto uploadsAfterGeometry = renderer.ReplayCacheStats();
    UNSCOPED_INFO("changed indices and smaller geometry must refresh retained uploads without allocation");
    REQUIRE((uploadsAfterGeometry.drawUploadCreations == uploadsBeforeGrowth.drawUploadCreations + 2 &&
             uploadsAfterGeometry.drawUploadReuses == uploadsBeforeGrowth.drawUploadReuses + 13));

    auto constants             = red;
    constants.vertexShaderDxil = compile(
        "cbuffer Vertex : register(b0, space4) { float4 vc[257]; };"
        "struct V { float4 p:SV_Position; float4 c:COLOR0; };"
        "V main(float4 p:TEXCOORD0,float4 c:TEXCOORD1) { V v; v.p=p; v.c=vc[0]+vc[256]; return v; }",
        "vs_5_1");
    constants.pixelShaderDxil = compile(
        "cbuffer Pixel : register(b1, space4) { float4 pc[225]; };"
        "cbuffer Shared : register(b2, space4) { float4 sc[33]; };"
        "float4 main(float4 p:SV_Position,float4 c:COLOR0):SV_Target0 { "
        "return c+pc[0]+pc[224]+sc[0]+sc[21]+sc[32]; }",
        "ps_5_1");
    constants.vertexConstants.resize(4112);
    constants.pixelConstants.resize(3600);
    constants.sharedConstants.resize(528);
    const auto setConstant = [](std::vector<std::uint8_t>& bytes, std::size_t index, const std::array<float, 4>& value)
    {
        std::memcpy(bytes.data() + index * 16, value.data(), 16);
    };
    setConstant(constants.vertexConstants, 256, { 1, 0, 0, 0 });
    setConstant(constants.pixelConstants, 224, { 0, 1, 0, 0 });
    setConstant(constants.sharedConstants, 21, { 0, 0, 1, 0 });
    setConstant(constants.sharedConstants, 32, { 0, 0, 0, 1 });
    result = renderer.SubmitDrawFrame({ constants }, directory / "larger-constants.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "larger-constants.rgba", { 255, 255, 255, 255 }, { 255, 255, 255, 255 });
    UNSCOPED_INFO("larger constants must grow all three stage uploads");
    REQUIRE(renderer.ReplayCacheStats().drawUploadCreations == uploadsAfterGeometry.drawUploadCreations + 3);
    constants.vertexConstants.resize(4096);
    constants.pixelConstants.resize(3584);
    constants.sharedConstants.resize(336);
    setConstant(constants.vertexConstants, 0, { 0, 1, 0, 0 });
    setConstant(constants.pixelConstants, 0, { 0, 0, 1, 0 });
    setConstant(constants.sharedConstants, 0, { 0, 0, 0, 1 });
    result = renderer.SubmitDrawFrame({ constants }, directory / "smaller-constants.rgba");
    UNSCOPED_INFO(result.error);
    REQUIRE(result.success);
    checkImage(directory / "smaller-constants.rgba", { 0, 255, 255, 255 }, { 0, 255, 255, 255 });
    const auto uploadsAfterConstants = renderer.ReplayCacheStats();
    UNSCOPED_INFO("smaller constants must refresh all stages and clear padding and retained tails without allocation");
    REQUIRE((uploadsAfterConstants.drawUploadCreations == uploadsAfterGeometry.drawUploadCreations + 3 &&
             uploadsAfterConstants.drawUploadReuses == uploadsAfterGeometry.drawUploadReuses + 7));
    auto invalidShader = red;
    invalidShader.vertexShaderDxil.assign(4, 0);
    UNSCOPED_INFO("invalid executable shader must fail on the renderer thread");
    REQUIRE(!renderer.SubmitDrawFrame({ invalidShader }).success);
    UNSCOPED_INFO("failed stream must reject later submissions");
    REQUIRE(!renderer.SubmitDrawFrame({ red }).success);
    renderer.Shutdown();
    renderer.Shutdown();
    UNSCOPED_INFO("shutdown must clear initialized state");
    REQUIRE(!renderer.Initialized());

    NativeRendererD3D12 stopping;
    UNSCOPED_INFO(error);
    REQUIRE(stopping.StartDrawStream(error));
    NativeDrawStreamOutput stoppingOutput;
    stoppingOutput.outputPath = directory / "shutdown.rgb10";
    stoppingOutput.gammaTable = gammaA;
    auto pending              = std::async(std::launch::async, [&]()
                                           {
                                  return stopping.SubmitDrawFrame({ red, blue }, {}, stoppingOutput);
                                           });
    stopping.Shutdown();
    UNSCOPED_INFO("shutdown must resolve a concurrent submitter");
    REQUIRE(pending.wait_for(kReadyCeiling) == std::future_status::ready);
    const auto stopped = pending.get();
    UNSCOPED_INFO("accepted work must finish before shutdown returns success");
    REQUIRE((!stopped.success || stopped.completionFenceValue));
    if (stopped.success)
        checkGammaImage(directory / "shutdown.rgb10",
                        packRgb10Output(17, 31, 919),
                        packRgb10Output(311, 31, 47));
    const auto cacheBeforeRestart = stopping.ReplayCacheStats();
    UNSCOPED_INFO(error);
    REQUIRE(stopping.StartDrawStream(error));
    NativeDrawStreamOutput restartOutput;
    restartOutput.outputPath = directory / "restart.rgb10";
    restartOutput.gammaTable = gammaA;
    result                   = stopping.SubmitDrawFrame({ red }, {}, restartOutput);
    UNSCOPED_INFO("restarted stream must rebuild and complete native output resources");
    REQUIRE((result.success && result.completionFenceValue));
    checkGammaImage(directory / "restart.rgb10",
                    packRgb10Output(311, 31, 47),
                    packRgb10Output(311, 31, 47));
    const auto cacheAfterRestart = stopping.ReplayCacheStats();
    UNSCOPED_INFO("stream restart must invalidate the prior device cache");
    REQUIRE((cacheAfterRestart.helperShaderCompilations > cacheBeforeRestart.helperShaderCompilations &&
             cacheAfterRestart.rootSignatureCreations > cacheBeforeRestart.rootSignatureCreations &&
             cacheAfterRestart.pipelineCreations > cacheBeforeRestart.pipelineCreations &&
             cacheAfterRestart.descriptorHeapCreations > cacheBeforeRestart.descriptorHeapCreations &&
             cacheAfterRestart.immutableConstantCreations > cacheBeforeRestart.immutableConstantCreations &&
             cacheAfterRestart.outputBundleCreations > cacheBeforeRestart.outputBundleCreations &&
             cacheAfterRestart.drawUploadCreations == cacheBeforeRestart.drawUploadCreations + 5));
    stopping.Shutdown();
    NativeFrameReplayRecipe halves;
    auto                    leftHalf = makeDraw({ 1, 0, 0, 1 }, true);
    leftHalf.width                   = 640;
    auto rightHalf                   = makeDraw({ 0, 0, 1, 1 }, true);
    rightHalf.width                  = 640;
    rightHalf.viewport.x             = -640;
    halves.halves[0].push_back(std::move(leftHalf));
    halves.halves[1].push_back(std::move(rightHalf));
    halves.gammaTable           = gammaA;
    halves.returnResolvedOutput = true;
    NativeRendererD3D12 halfRenderer;
    halves.outputPath       = directory / "alias.rgb10";
    halves.sampleOutputPath = directory / "." / "alias.rgb10";
    const auto aliased      = halfRenderer.ReplayFrame(halves);
    UNSCOPED_INFO("frame output aliases rejected before GPU submission");
    REQUIRE((!aliased.success && aliased.error.find("distinct output paths") != std::string::npos));
    halves.outputPath.clear();
    halves.sampleOutputPath.clear();
    const auto halfResult = halfRenderer.ReplayFrame(std::move(halves));
    UNSCOPED_INFO(halfResult.error);
    REQUIRE((halfResult.success && halfResult.completionFenceValue));
    checkGammaPixels(halfResult.resolvedOutput, packRgb10Output(311, 31, 47), packRgb10Output(17, 31, 919));
    halfRenderer.Shutdown();
    checkTextureReuse(directory);
    checkMipCubeAndFormats(directory);
    checkAttachmentReuse(directory);
    checkDrawBatching(directory);
    checkGameplayTextureBindings(directory);
    checkSingleSample(directory);
    checkDepthClip(directory);
    checkSignedTarget(directory);
    checkPreparedPipelines();
    checkPreparedCollectorPixels();
#ifdef REREVVED_PREPARED_FRAME_BRIDGE_TEST
    checkPreparedSdkBridge();
#endif
    checkFrameTextureStorageReuse();
    checkUploadPoolLeases();
    checkTexturePoolReuse();
    checkResolvePoolReuse();
    checkImportedSurfaceViews();
    checkContentAdoptionOwnership();
#if REREVVED_DEVELOPER
    checkRecordingPhaseTiming();
    checkDrawUploadBytes();
    checkResidentGeometryBytes();
    checkStreamBusyPublished();
    checkConstantReuse();
    checkRepeatCounters();
    checkTextureKeyOutcomes();
#endif
    checkAdmittedTextureVersions();
    checkVersionTextureStore();
    checkVersionTexturePressure();
    checkAdmittedTextureLayouts();
    checkRenderFrame(directory);
    checkRenderFrameBatch();
    checkFrameCommandOwnership();
    checkFrameSamplerTables();
    checkRecordedFramePrefix();
    checkSignedPrefixChain();
    checkAppendedFramePasses();
    checkTailTerminals();
    checkUnsignedTargetChanges();
    checkUnsignedTargetChanges(true);
    checkFrameUploadHazards();
    checkResolveOnlyTargetChanges();
    checkQueuedFramePasses();
    checkOrderedFrameResolves();
    checkLargeRenderFrame();
    checkRetainedRenderFrame();
    checkQueuedRenderFrames();
    checkAdmittedImportPins();
    checkAggregateOwnedAdmission();
    checkDrawStreamQueue();
    checkDirectStream();
    checkUnprovedTerminalDrain();
    checkFrameClearsAndStencil();
    checkColorAliasBits();
    checkFrameAttachmentAllocationReuse();
    checkGpuOutputHandoff();
    checkQueuedGpuOutputHandoff();
    checkQueuedCanonicalTextureOwners();
    checkQueuedVersionTextureEviction();
    checkUnavailableCanonicalTextureReservation();
    checkCanonicalTexturePressure();
    UNSCOPED_INFO("vertex residency device cases");
    REQUIRE(runVertexResidencyCases({}));
    std::cout << "Persistent native frame cache, preservation, clears, fences, rejection and shutdown passed\n";
    callbackChecks.Verify();
}

// Runs the vertex residency device cases, all or the one named:
//   native-frame-stream.vertex-residency [case]
TEST_CASE("native_frame_stream_vertex_residency", "[.][manual][native-frame-stream]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() > 1)
    {
        FAIL("usage: native-frame-stream.vertex-residency [case]");
    }
    char enabled[2]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) != 1 || enabled[0] != '1')
    {
        SKIP("Set REREVVED_TEST_NATIVE_D3D12=1 to run the D3D12 submission lifecycle check");
    }
    rerevved::tests::ScopedLogging logging;
    NativeRendererD3D12::ConfigureReplayDiagnostics();
    UNSCOPED_INFO("the vertex residency device cases pass");
    REQUIRE(runVertexResidencyCases(args.argc() == 1 ? args.argv()[1] : std::string_view{}));
}

REREVVED_MANUAL_HELPER(nativeFrameStreamVertexResidency, "native-frame-stream.vertex-residency", "native_frame_stream_vertex_residency")
