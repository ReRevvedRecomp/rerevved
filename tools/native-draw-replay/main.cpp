#include "gpu/d3d12/native_draw_replay.h"
#include "gpu/d3d12/native_renderer_d3d12.h"

#include <filesystem>
#include <iostream>
#include <string>

#include <rex/logging.h>

namespace
{

void printUsage()
{
    std::cerr << "usage: native_draw_replay <recipe-file> <output-samples-file>\n";
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
    const std::filesystem::path           recipePath = argv[1];
    const std::filesystem::path           outputPath = argv[2];
    rerevved::gpu::NativeDrawReplayRecipe recipe;
    std::string                           error;
    if (!rerevved::gpu::LoadNativeDrawReplayRecipe(recipePath, recipe, error))
    {
        std::cerr << "native_draw_replay: " << error << '\n';
        rex::ShutdownLogging();
        return 1;
    }
    recipe.outputPath = outputPath;
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

    rerevved::gpu::NativeRendererD3D12 renderer;
    const auto                         result = renderer.ReplayOffscreen(recipe);
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
