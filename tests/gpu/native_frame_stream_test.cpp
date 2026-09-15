#include "gpu/d3d12/native_renderer_d3d12.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include <rex/logging.h>

using namespace rerevved::gpu;

namespace
{

void require(bool value, const std::string& message)
{
    if (!value)
        throw std::runtime_error(message);
}

std::vector<std::uint8_t> compile(const char* source, const char* target)
{
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    const auto                       hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", target, 0, 0, &code, &errors);
    require(SUCCEEDED(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "shader compilation failed");
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

void checkImage(const std::filesystem::path&       path,
                const std::array<std::uint8_t, 4>& left,
                const std::array<std::uint8_t, 4>& right)
{
    std::ifstream             file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file), {} };
    require(bytes.size() == 1280U * 720U * 4U * 2U, "complete sample planes required");
    for (std::size_t plane = 0; plane < 2; ++plane)
        for (std::size_t y = 0; y < 720; ++y)
            for (std::size_t x = 0; x < 1280; ++x)
            {
                const auto  offset   = ((plane * 720 + y) * 1280 + x) * 4;
                const auto& expected = x < 640 ? left : right;
                require(std::memcmp(bytes.data() + offset, expected.data(), 4) == 0,
                        "draw preservation or next-frame clear differs from expected samples");
            }
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

void checkGammaImage(const std::filesystem::path& path,
                     std::uint32_t                left,
                     std::uint32_t                right)
{
    std::ifstream             file(path, std::ios::binary);
    std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file), {} };
    require(bytes.size() == 1280U * 720U * 4U, "complete RGB10 output required");
    for (std::size_t y = 0; y < 720; ++y)
        for (std::size_t x = 0; x < 1280; ++x)
        {
            const auto    offset   = (y * 1280 + x) * 4;
            const auto    expected = x < 640 ? left : right;
            std::uint32_t actual   = 0;
            std::memcpy(&actual, bytes.data() + offset, sizeof(actual));
            require(actual == expected, "gamma output channels or preservation differ from expected values");
        }
}

} // namespace

int main()
{
    char enabled[2]{};
    if (GetEnvironmentVariableA("REREVVED_TEST_NATIVE_D3D12", enabled, sizeof(enabled)) != 1 || enabled[0] != '1')
    {
        std::cout << "Set REREVVED_TEST_NATIVE_D3D12=1 to run the D3D12 submission lifecycle check\n";
        return 77;
    }
    rex::LogConfig logging;
    logging.log_to_console = true;
    rex::InitLogging(logging);
    int status = 0;
    try
    {
        NativeRendererD3D12::ConfigureReplayDiagnostics();
        const auto red       = makeDraw({ 1, 0, 0, 1 }, false);
        const auto blue      = makeDraw({ 0, 0, 1, 1 }, true);
        const auto green     = makeDraw({ 0, 1, 0, 1 }, true);
        const auto gammaA    = makeGammaTable(17, 31, 47, 311, 617, 919);
        const auto gammaB    = makeGammaTable(97, 149, 211, 701, 809, 997);
        const auto directory = std::filesystem::current_path() /
                               ("native-frame-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory), "fresh test output directory required");
        NativeDrawStreamOutput firstOutput;
        firstOutput.outputPath = directory / "first.rgb10";
        firstOutput.gammaTable = gammaA;
        NativeDrawStreamOutput secondOutput;
        secondOutput.outputPath = directory / "second.rgb10";
        secondOutput.gammaTable = gammaB;
        NativeRendererD3D12 renderer;
        std::string         error;
        require(!renderer.SubmitDrawFrame({ red }).success, "submission before startup must fail");
        require(renderer.StartDrawStream(error), error);
        require(!renderer.SubmitDrawFrame({}).success, "empty frame must fail admission");
        auto result = renderer.SubmitDrawFrame({ red, blue }, directory / "first.rgba", firstOutput);
        require(result.success && result.completionFenceValue, result.error);
        auto fence = result.completionFenceValue;
        checkImage(directory / "first.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
        checkGammaImage(directory / "first.rgb10",
                        packRgb10Output(17, 31, 919),
                        packRgb10Output(311, 31, 47));
        require(!renderer.SubmitDrawFrame({ red }, directory / "first.rgba").success,
                "submission must not overwrite an existing output");
        require(!renderer.SubmitDrawFrame({ red }, {}, firstOutput).success,
                "submission must not overwrite an existing gamma output");
        result = renderer.SubmitDrawFrame({ red, blue }, directory / "second.rgba", secondOutput);
        require(result.success && result.completionFenceValue > fence, result.error);
        fence = result.completionFenceValue;
        checkImage(directory / "second.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
        checkGammaImage(directory / "second.rgb10",
                        packRgb10Output(97, 149, 997),
                        packRgb10Output(701, 149, 211));
        NativeDrawStreamOutput invalidOutput;
        invalidOutput.gammaTable = gammaA;
        require(!renderer.SubmitDrawFrame({ red }, {}, invalidOutput).success,
                "gamma output without a path must be rejected");
        NativeDrawStreamOutput aliasedOutput;
        aliasedOutput.outputPath = directory / "." / "alias.rgb10";
        aliasedOutput.gammaTable = gammaA;
        require(!renderer.SubmitDrawFrame({ red }, directory / "alias.rgb10", aliasedOutput).success,
                "sample and gamma output aliases must be rejected");
        NativeDrawStreamOutput caseAliasedOutput;
        caseAliasedOutput.outputPath = directory / "FIRST.RGBA";
        caseAliasedOutput.gammaTable = gammaA;
        require(!renderer.SubmitDrawFrame({ red }, directory / "first.rgba", caseAliasedOutput).success,
                "case-equivalent sample and gamma output aliases must be rejected");
        checkImage(directory / "first.rgba", { 0, 0, 255, 255 }, { 255, 0, 0, 255 });
        NativeDrawStreamOutput gammaOnlyOutput;
        gammaOnlyOutput.outputPath = directory / "gamma-only.rgb10";
        gammaOnlyOutput.gammaTable = gammaA;
        result                     = renderer.SubmitDrawFrame({ red }, {}, gammaOnlyOutput);
        require(result.success && result.completionFenceValue > fence,
                "gamma-only output must complete its native fence");
        fence = result.completionFenceValue;
        checkGammaImage(directory / "gamma-only.rgb10",
                        packRgb10Output(311, 31, 47),
                        packRgb10Output(311, 31, 47));
        result = renderer.SubmitDrawFrame({ green }, directory / "third.rgba");
        require(result.success && result.completionFenceValue > fence,
                "render with sample readback must complete its native fence");
        fence = result.completionFenceValue;
        checkImage(directory / "third.rgba", { 0, 255, 0, 255 }, { 0, 0, 0, 0 });
        result = renderer.SubmitDrawFrame({ red });
        require(result.success && result.completionFenceValue > fence, "render without readback must complete its native fence");
        auto firstQueued  = std::async(std::launch::async, [&]()
                                       {
                                          return renderer.SubmitDrawFrame({ red, blue });
                                       });
        auto secondQueued = std::async(std::launch::async, [&]()
                                       {
                                           return renderer.SubmitDrawFrame({ green });
                                       });
        require(firstQueued.wait_for(std::chrono::seconds(10)) == std::future_status::ready &&
                    secondQueued.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
                "serialized submitters must wake the renderer and complete");
        const auto firstResult  = firstQueued.get();
        const auto secondResult = secondQueued.get();
        require(firstResult.success && secondResult.success && firstResult.completionFenceValue &&
                    secondResult.completionFenceValue && firstResult.completionFenceValue != secondResult.completionFenceValue,
                "serialized frames must have separate completed native fences");
        auto invalidShader = red;
        invalidShader.vertexShaderDxil.assign(4, 0);
        require(!renderer.SubmitDrawFrame({ invalidShader }).success, "invalid executable shader must fail on the renderer thread");
        require(!renderer.SubmitDrawFrame({ red }).success, "failed stream must reject later submissions");
        renderer.Shutdown();
        renderer.Shutdown();
        require(!renderer.Initialized(), "shutdown must clear initialized state");

        NativeRendererD3D12 stopping;
        require(stopping.StartDrawStream(error), error);
        NativeDrawStreamOutput stoppingOutput;
        stoppingOutput.outputPath = directory / "shutdown.rgb10";
        stoppingOutput.gammaTable = gammaA;
        auto pending              = std::async(std::launch::async, [&]()
                                               {
                                      return stopping.SubmitDrawFrame({ red, blue }, {}, stoppingOutput);
                                               });
        stopping.Shutdown();
        require(pending.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
                "shutdown must resolve a concurrent submitter");
        const auto stopped = pending.get();
        require(!stopped.success || stopped.completionFenceValue, "accepted work must finish before shutdown returns success");
        if (stopped.success)
            checkGammaImage(directory / "shutdown.rgb10",
                            packRgb10Output(17, 31, 919),
                            packRgb10Output(311, 31, 47));
        std::cout << "Persistent native frame preservation, clears, fences, rejection and shutdown passed\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << "native_frame_stream_test: " << exception.what() << '\n';
        status = 1;
    }
    rex::ShutdownLogging();
    return status;
}
