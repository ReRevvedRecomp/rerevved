#include "gameplay_input.h"
#include "gpu/d3d12/native_draw_replay.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "sequence.h"
#include "solid_adapter.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <toml++/toml.hpp>

#include <rex/logging.h>

namespace
{

void printUsage()
{
    std::cerr << "usage: native_draw_replay <recipe-file> <output-file>\n";
    std::cerr << "       native_draw_replay --solid-adapter <capture-directory> <gameplay-shader-catalog-directory> <fresh-comparison-directory> [reference-directory] [--destination reference-before]\n";
    std::cerr << "       native_draw_replay --solid-adapter-reference <capture-directory> <fresh-reference-directory>\n";
}

} // namespace

int main(int argc, char** argv)
{
    const bool              comparison     = argc >= 2 && std::string(argv[1]) == "--solid-adapter";
    const bool              reference      = argc >= 2 && std::string(argv[1]) == "--solid-adapter-reference";
    int                     comparisonArgs = argc;
    SolidAdapterDestination destination    = SolidAdapterDestination::TransparentBlack;
    if (comparison && argc >= 7 && std::string(argv[argc - 2]) == "--destination" &&
        std::string(argv[argc - 1]) == "reference-before")
    {
        comparisonArgs -= 2;
        destination = SolidAdapterDestination::ReferenceBefore;
    }
    if ((comparison && comparisonArgs != 5 && comparisonArgs != 6) ||
        (comparison && comparisonArgs == 6 && std::string(argv[5]).starts_with("--")) || (reference && argc != 4) ||
        (!comparison && !reference && argc != 3))
    {
        printUsage();
        return 2;
    }

    rex::LogConfig logging;
    logging.log_to_console = true;
    rex::InitLogging(logging);
    if (comparison || reference)
    {
        std::string error;
        const bool  success = reference ? IngestSolidAdapterReference(argv[2], argv[3], error) : CompareSolidAdapterCapture(argv[2], argv[3], argv[4], error, comparisonArgs == 6 ? std::filesystem::path(argv[5]) : std::filesystem::path{}, destination);
        if (!success)
            std::cerr << "native_draw_replay: " << error << '\n';
        else
            std::cout << (reference ? "native_draw_replay: solid adapter reference saved\n" : "native_draw_replay: solid adapter comparison passed\n");
        rex::ShutdownLogging();
        return success ? 0 : 1;
    }
    const std::filesystem::path            recipePath = argv[1];
    const std::filesystem::path            outputPath = argv[2];
    rerevved::gpu::NativeDrawReplayRecipe  recipe;
    rerevved::gpu::NativeFrameReplayRecipe frame;
    rerevved::gpu::NativeRenderFrame       renderFrame;
    rerevved::gpu::NativeRenderFrame       producer;
    std::string                            error;
    bool                                   isFrame       = false;
    bool                                   isRenderFrame = false;
    bool                                   loaded        = false;
    try
    {
        if (std::filesystem::file_size(recipePath) > 1024U * 1024U)
            throw std::runtime_error("recipe exceeds metadata byte bound");
        const auto table = toml::parse_file(recipePath.string());
        if (table.contains("gameplay_sequence_schema_version"))
        {
            const bool success = ReplayGameplaySequence(recipePath, outputPath, error);
            if (!success)
                std::cerr << "native_draw_replay: " << error << '\n';
            else
                std::cout << "native_draw_replay: wrote sequence " << outputPath.string() << '\n';
            rex::ShutdownLogging();
            return success ? 0 : 1;
        }
        const bool isGameplay = table.contains("gameplay_frame_schema_version");
        isRenderFrame         = table.contains("gameplay_render_frame_schema_version");
        isFrame               = isGameplay || table.contains("frame_schema_version");
        loaded                = isRenderFrame ? LoadGameplayRenderFrameRecipe(recipePath, renderFrame, error, &producer)
                                : isGameplay  ? LoadGameplayCaptureRecipe(recipePath, frame, error)
                                : isFrame     ? rerevved::gpu::LoadNativeFrameReplayRecipe(recipePath, frame, error)
                                              : rerevved::gpu::LoadNativeDrawReplayRecipe(recipePath, recipe, error);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }
    if (!loaded)
    {
        std::cerr << "native_draw_replay: " << error << '\n';
        rex::ShutdownLogging();
        return 1;
    }
    recipe.outputPath      = outputPath;
    frame.outputPath       = outputPath;
    renderFrame.outputPath = outputPath;
    frame.sampleOutputPath = outputPath.string() + ".samples";
    if (!recipe.outputPath.parent_path().empty())
    {
        std::error_code directoryError;
        std::filesystem::create_directories(recipe.outputPath.parent_path(), directoryError);
        if (directoryError)
        {
            std::cerr << "native_draw_replay: could not create output directory: "
                      << directoryError.message() << '\n';
            rex::ShutdownLogging();
            return 1;
        }
    }

    rerevved::gpu::NativeRendererD3D12::ConfigureReplayDiagnostics();
    rerevved::gpu::NativeRendererD3D12    renderer;
    rerevved::gpu::NativeDrawReplayResult result;
    if (isRenderFrame)
    {
        if (renderer.StartDrawStream(error))
        {
            if (!producer.passes.empty())
                result = renderer.SubmitRenderFrame(std::move(producer));
            if (result.error.empty())
                result = renderer.SubmitRenderFrame(std::move(renderFrame));
        }
        else
            result.error = error;
    }
    else
        result = isFrame ? renderer.ReplayFrame(std::move(frame)) : renderer.ReplayOffscreen(recipe);
    renderer.Shutdown();
    if (!result.success)
    {
        std::cerr << "native_draw_replay: " << result.error << '\n';
        rex::ShutdownLogging();
        return 1;
    }
    std::cout << "native_draw_replay: wrote " << result.outputPath.string() << '\n';
    rex::ShutdownLogging();
    return 0;
}
