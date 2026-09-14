#include "gpu/d3d12/native_draw_replay.h"
#include "gpu/d3d12/native_renderer_d3d12.h"

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
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        printUsage();
        return 2;
    }

    rex::LogConfig logging;
    logging.log_to_console = true;
    rex::InitLogging(logging);
    const std::filesystem::path            recipePath = argv[1];
    const std::filesystem::path            outputPath = argv[2];
    rerevved::gpu::NativeDrawReplayRecipe  recipe;
    rerevved::gpu::NativeFrameReplayRecipe frame;
    std::string                            error;
    bool                                   isFrame = false;
    bool                                   loaded  = false;
    try
    {
        if (std::filesystem::file_size(recipePath) > 1024U * 1024U)
            throw std::runtime_error("recipe exceeds metadata byte bound");
        isFrame = toml::parse_file(recipePath.string()).contains("frame_schema_version");
        loaded  = isFrame ? rerevved::gpu::LoadNativeFrameReplayRecipe(recipePath, frame, error)
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
    rerevved::gpu::NativeRendererD3D12 renderer;
    const auto                         result = isFrame ? renderer.ReplayFrame(frame) : renderer.ReplayOffscreen(recipe);
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
