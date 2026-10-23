// Depth-only then color draw into the world's 1024x1024 off-screen pass 0.
// Default checks validation; d3d12 renders natively against a CPU reference,
// including VS user-plane 0 from shared constant c29.

#include "gpu/d3d12/native_render_frame.h"
#include "gpu/d3d12/native_renderer_d3d12.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace rerevved::gpu;

namespace
{

constexpr std::uint32_t kSize = 1024;
// Depths whose 24-bit codes neither round nor truncate differently.
constexpr float kOccluderDepth = 0.625F, kColorDepth = 0.75F;
// Color codes as UNORM bytes, R G B A.
constexpr std::array<std::uint8_t, 4> kClearCode{ 51, 102, 153, 204 }, kDrawCode{ 26, 200, 7, 255 };

// Positions as float4 at stride 16, two triangles per quad.
NativeDrawReplayRecipe quad(float left, float right, float depth)
{
    NativeDrawReplayRecipe draw;
    draw.schemaVersion    = 2;
    draw.width            = kSize;
    draw.height           = kSize;
    draw.sampleCount      = 1;
    draw.sampleMask       = 1;
    draw.vertexShaderHash = 1;
    draw.vertexShaderDxil.assign(4, 0);
    draw.vertexAttributeCount                           = 1;
    draw.vertexStrideBytes                              = 16;
    const std::array<std::array<float, 4>, 4> positions = { { { left, -1, depth, 1 }, { left, 1, depth, 1 }, { right, 1, depth, 1 }, { right, -1, depth, 1 } } };
    draw.vertexData.resize(sizeof(positions));
    std::memcpy(draw.vertexData.data(), positions.data(), sizeof(positions));
    draw.indices    = { 0, 1, 2, 0, 2, 3 };
    draw.indexCount = 6;
    draw.vertexConstants.resize(4096);
    draw.pixelConstants.resize(3584);
    draw.sharedConstants.resize(336);
    draw.viewport = { 0, 0, float(kSize), float(kSize), 0, 1 };
    draw.scissor  = { 0, 0, std::int32_t(kSize), std::int32_t(kSize) };
    return draw;
}

// The left half at kOccluderDepth, z:always with depth write and no color.
NativeDrawReplayRecipe occluder()
{
    auto draw               = quad(-1, 0, kOccluderDepth);
    draw.depthOnly          = true;
    draw.blend.writeMask    = 0;
    draw.depth.enabled      = true;
    draw.depth.writeEnabled = true;
    draw.depth.compare      = 8;
    return draw;
}

// The whole target at kColorDepth under a less-than test.
NativeDrawReplayRecipe colorDraw()
{
    auto draw            = quad(-1, 1, kColorDepth);
    draw.pixelShaderHash = 2;
    draw.pixelShaderDxil.assign(4, 0);
    draw.depth.enabled      = true;
    draw.depth.writeEnabled = true;
    draw.depth.compare      = 2;
    return draw;
}

NativeRenderFrame frameOf(std::vector<NativeDrawReplayRecipe> draws)
{
    NativeRenderFrame frame;
    frame.textures.push_back({ 1, kSize, kSize, NativeDrawReplayTextureFormat::Rgba8, 1 });
    NativeRenderFramePass pass;
    // RT0 k_8_8_8_8 at EDRAM 0 over depth at 0x4C0, as pass 0 binds them.
    pass.target = { kSize, kSize, 1, NativeDrawReplayTargetFormat::Rgba8, 0, 0x4C0 };
    NativeRenderFramePass::Clear clear;
    clear.rectangle = { 0, 0, std::int32_t(kSize), std::int32_t(kSize) };
    clear.color = clear.depth = clear.stencil = true;
    for (int c = 0; c < 4; ++c)
        clear.colorValue[c] = kClearCode[c] / 255.0F;
    clear.depthValue = 1.0F;
    pass.clears.push_back(clear);
    pass.draws = std::move(draws);
    NativeRenderFrameResolve resolve;
    resolve.textureId = 1;
    resolve.width = resolve.height = kSize;
    pass.resolves.push_back(resolve);
    frame.passes.push_back(std::move(pass));
    frame.outputTextureId = 1;
    frame.retainOnly = frame.returnRawOutput = true;
    return frame;
}

void runValidation()
{
    std::string error;
    UNSCOPED_INFO("the depth-only then color frame is admitted: " + error);
    REQUIRE(ValidateNativeRenderFrame(frameOf({ occluder(), colorDraw() }), error));
    std::shared_ptr<const NativeDrawReplayAdmission> depthAdmission, colorAdmission;
    UNSCOPED_INFO("admission: " + error);
    REQUIRE((AdmitNativeDrawReplayRecipe(occluder(), depthAdmission, error) && AdmitNativeDrawReplayRecipe(colorDraw(), colorAdmission, error)));
    UNSCOPED_INFO("the depth-only key marks its null pixel shader");
    REQUIRE((depthAdmission->PipelineStateKey() != colorAdmission->PipelineStateKey() && depthAdmission->PipelineStateKey().flags.back() == 1 &&
             colorAdmission->PipelineStateKey().flags.back() == 0));
    auto withPixel = occluder();
    withPixel.pixelShaderDxil.assign(4, 0);
    UNSCOPED_INFO("a depth-only draw with pixel DXIL is refused");
    REQUIRE(!ValidateNativeRenderFrame(frameOf({ withPixel, colorDraw() }), error));
    auto writes            = occluder();
    writes.blend.writeMask = 0xF;
    UNSCOPED_INFO("a depth-only draw with a color mask is refused: " + error);
    REQUIRE((!ValidateNativeRenderFrame(frameOf({ writes, colorDraw() }), error) &&
             error.find("depth-only replay writes color, samples textures or has no depth or stencil state") != std::string::npos));
    std::cout << "Depth-only frame validation admits a null-PS draw before a color draw and refuses pixel DXIL and color writes\n";
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

std::vector<std::uint8_t> render(NativeRendererD3D12& renderer, std::vector<NativeDrawReplayRecipe> draws)
{
    auto        frame = frameOf(std::move(draws));
    std::string error;
    UNSCOPED_INFO("frame validation: " + error);
    REQUIRE(ValidateNativeRenderFrame(frame, error));
    const auto result = renderer.SubmitRenderFrame(std::move(frame));
    UNSCOPED_INFO("depth-only frame: " + result.error);
    REQUIRE((result.success && result.rawRowPitch == kSize * 4 && result.rawOutput.size() == std::size_t(kSize) * kSize * 4));
    return result.rawOutput;
}

// Every texel against the CPU reference; the first mismatch is named.
void compare(const char* name, const std::vector<std::uint8_t>& image, bool occluded)
{
    std::size_t mismatches = 0;
    std::string first;
    for (std::uint32_t y = 0; y < kSize; ++y)
        for (std::uint32_t x = 0; x < kSize; ++x)
        {
            // The occluder covers pixel centers x + 0.5 < 512; the color draw
            // passes its less-than test only where depth is still the clear's.
            const auto& code  = occluded && x < kSize / 2 ? kClearCode : kDrawCode;
            const auto* texel = image.data() + (std::size_t(y) * kSize + x) * 4;
            if (std::memcmp(texel, code.data(), 4) != 0 && !mismatches++)
                first = std::format(" first at ({},{}): {} {} {} {}", x, y, texel[0], texel[1], texel[2], texel[3]);
        }
    UNSCOPED_INFO(std::format("{}: {} texels differ{}", name, mismatches, first));
    REQUIRE(mismatches == 0);
}

void runD3D12()
{
    char enabled[8]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) != 1 || enabled[0] != '1')
    {
        SKIP("Set REREVVED_TEST_NATIVE_D3D12=1 to run the D3D12 depth-only checks");
    }
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
    {
        SKIP("No D3D12 device; the depth-only frames were not rendered");
    }
    NativeRendererD3D12 renderer;
    std::string         error;
    UNSCOPED_INFO("native stream: " + error);
    REQUIRE(renderer.StartDrawStream(error));
    const auto vertex      = compile("float4 main(float4 p:TEXCOORD0):SV_Position { return p; }", "vs_5_1");
    const auto pixel       = compile(std::format("float4 main(float4 p:SV_Position):SV_Target0 {{ return float4({}, {}, {}, {}) / 255.0; }}", kDrawCode[0], kDrawCode[1], kDrawCode[2], kDrawCode[3]).c_str(), "ps_5_1");
    auto       depth       = occluder();
    auto       color       = colorDraw();
    depth.vertexShaderDxil = color.vertexShaderDxil = vertex;
    color.pixelShaderDxil                           = pixel;
    try
    {
        // Without the occluder the color draw covers the target.
        compare("color draw alone", render(renderer, { color }), false);
        // The occluder writes depth and no color; the color draw then fails
        // its test on the left half, which keeps the clear color.
        compare("depth-only then color", render(renderer, { depth, color }), true);
        // The occluder alone leaves the clear color everywhere.
        const auto alone = render(renderer, { depth });
        for (std::size_t i = 0; i < alone.size(); i += 4)
        {
            UNSCOPED_INFO(std::format("the depth-only draw changed texel {}", i / 4));
            REQUIRE(std::memcmp(alone.data() + i, kClearCode.data(), 4) == 0);
        }
        // The clip distance trails the interpolators, and the plane is bound
        // at bytes 464..479 of a 480-byte shared buffer. Plane (1,0,0,0)
        // removes x < 0, leaving the clear on the left half; (0,0,0,1) none.
        auto clipped             = colorDraw();
        clipped.vertexShaderDxil = compile("cbuffer SharedConstants : register(b2, space4) { float4 g_UserClipPlane0 : packoffset(c29); };\n"
                                           "void main(float4 p : TEXCOORD0, out float4 oPos : SV_Position, out float4 oTexCoord0 : TEXCOORD0, out float oClipDistance0 : SV_ClipDistance0)\n"
                                           "{ oPos = p; oTexCoord0 = p; oClipDistance0 = dot(oPos, g_UserClipPlane0); }",
                                           "vs_5_1");
        clipped.pixelShaderDxil  = compile(std::format("float4 main(float4 p : SV_Position, float4 t : TEXCOORD0) : SV_Target0 {{ return float4({}, {}, {}, {}) / 255.0 + 0.0 * t; }}", kDrawCode[0], kDrawCode[1], kDrawCode[2], kDrawCode[3]).c_str(), "ps_5_1");
        clipped.sharedConstants.resize(480);
        const auto plane = [&](std::array<float, 4> value)
        {
            auto draw = clipped;
            std::memcpy(draw.sharedConstants.data() + 464, value.data(), sizeof(value));
            return draw;
        };
        compare("user clip plane (1,0,0,0)", render(renderer, { plane({ 1, 0, 0, 0 }) }), true);
        compare("user clip plane (0,0,0,1)", render(renderer, { plane({ 0, 0, 0, 1 }) }), false);
    }
    catch (...)
    {
        renderer.Shutdown();
        throw;
    }
    renderer.Shutdown();
    std::cout << "A null-PS depth-only draw writes depth and no color into the 1024x1024 target; the color draw after it matches the CPU reference\n";
    std::cout << "A VS clipping by user plane 0 at shared c29 keeps x >= 0 for plane (1,0,0,0) and the whole target for (0,0,0,1)\n";
}

} // namespace

TEST_CASE("native_depth_only_frame", "[ctest][native-depth-only-frame][timeout-120]")
{
    runValidation();
}

TEST_CASE("native_depth_only_frame_d3d12", "[ctest][release-smoke][native-depth-only-frame][native_gpu][timeout-120]")
{
    runD3D12();
}
