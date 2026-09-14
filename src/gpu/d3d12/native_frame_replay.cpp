#include "native_frame_replay.h"

#include <fstream>
#include <optional>
#include <toml++/toml.hpp>

namespace rerevved::gpu
{
namespace
{

constexpr std::size_t kMaxDraws      = 1024;
constexpr std::size_t kMaxFrameBytes = 512U * 1024U * 1024U;

bool validateNativeDrawSpan(std::span<const NativeDrawReplayRecipe> draws,
                            std::uint32_t                           width,
                            std::size_t&                            totalDraws,
                            std::size_t&                            totalBytes,
                            std::string&                            error)
{
    if (draws.empty() ||
        (width == 1280 && draws.size() > 256) ||
        draws.size() > kMaxDraws - totalDraws)
    {
        error = width == 1280 ? "native menu frame exceeds its draw count bound"
                              : "frame halves must be nonempty and fit the draw bound";
        return false;
    }
    totalDraws += draws.size();
    for (std::size_t index = 0; index < draws.size(); ++index)
    {
        const auto& draw        = draws[index];
        const auto  nativeClear = std::array<std::uint8_t, 4>{ 0, 0, 0, 0 };
        const auto  opaqueClear = std::array<std::uint8_t, 4>{ 0, 0, 0, 255 };
        if (draw.schemaVersion != 2 || draw.width != width || draw.height != 720 ||
            draw.targetFormat != NativeDrawReplayTargetFormat::Rgba8 ||
            !draw.initialSample0.empty() || !draw.initialSample1.empty() ||
            !draw.depth.initialSamples.empty() || draw.depth.initialClear != 1.0F ||
            (draw.clearColor != nativeClear && !(index == 0 && draw.clearColor == opaqueClear)))
        {
            error = width == 1280 ? "native menu frames require full-width color and native black/far clears"
                                  : "frame draws require the menu extent and native black/far clears without captured attachments";
            return false;
        }
        if (!ValidateNativeDrawReplayRecipe(draw, error))
            return false;
        for (const auto size : { draw.vertexShaderDxil.size(),
                                 draw.pixelShaderDxil.size(),
                                 draw.vertexData.size(),
                                 draw.indices.size() * sizeof(std::uint32_t),
                                 draw.vertexConstants.size(),
                                 draw.pixelConstants.size(),
                                 draw.sharedConstants.size(),
                                 draw.textures[0].bytes.size() + draw.textures[1].bytes.size() +
                                     draw.textures[2].bytes.size() })
        {
            if (size > kMaxFrameBytes - totalBytes)
            {
                error = width == 1280 ? "native menu frame inputs exceed the owned byte bound"
                                      : "frame inputs exceed the owned byte bound";
                return false;
            }
            totalBytes += size;
        }
    }
    return true;
}

bool resolveInput(const std::filesystem::path& root, const toml::node* node, std::filesystem::path& result, std::string& error)
{
    const auto name = node ? node->value<std::string>() : std::nullopt;
    if (!name || name->empty() || std::filesystem::path(*name).is_absolute())
    {
        error = "frame input path must be a nonempty relative path";
        return false;
    }
    std::error_code ec;
    result = std::filesystem::weakly_canonical(root / *name, ec);
    if (ec)
    {
        error = "could not resolve frame input path";
        return false;
    }
    const auto relative = result.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
    {
        error = "frame input path escapes its recipe directory";
        return false;
    }
    return true;
}

} // namespace

bool ValidateNativeDrawFramesRecipe(const NativeDrawFramesRecipe& recipe, std::string& error)
{
    error.clear();
    if (recipe.frames.empty() || recipe.frames.size() > 8)
    {
        error = "native menu replay requires one through eight consecutive frames";
        return false;
    }
    std::size_t bytes = 0;
    std::size_t count = 0;
    for (const auto& frame : recipe.frames)
    {
        if (!validateNativeDrawSpan(frame, 1280, count, bytes, error))
            return false;
    }
    return true;
}

bool ValidateNativeDrawFrame(std::span<const NativeDrawReplayRecipe> draws, std::string& error)
{
    error.clear();
    std::size_t count = 0;
    std::size_t bytes = 0;
    return validateNativeDrawSpan(draws, 1280, count, bytes, error);
}

bool ValidateNativeFrameReplayRecipe(const NativeFrameReplayRecipe& recipe,
                                     std::string&                   error)
{
    error.clear();
    std::size_t count = 0;
    std::size_t bytes = 0;
    for (const auto& half : recipe.halves)
    {
        if (!validateNativeDrawSpan(half, 640, count, bytes, error))
            return false;
    }
    return true;
}

bool LoadNativeFrameReplayRecipe(const std::filesystem::path& path,
                                 NativeFrameReplayRecipe&     recipe,
                                 std::string&                 error)
{
    recipe = {};
    try
    {
        if (std::filesystem::file_size(path) > 1024U * 1024U)
        {
            error = "frame recipe exceeds the metadata byte bound";
            return false;
        }
        const auto  root   = std::filesystem::canonical(path).parent_path();
        const auto  table  = toml::parse_file(path.string());
        const auto* halves = table["halves"].as_array();
        if (table["frame_schema_version"].value_or(0) != 1 || !halves || halves->size() != 2)
        {
            error = "frame recipe requires schema 1 and exactly two halves";
            return false;
        }
        std::filesystem::path gammaPath;
        if (!resolveInput(root, table.get("gamma_table"), gammaPath, error))
            return false;
        if (std::filesystem::file_size(gammaPath) != 1024)
        {
            error = "frame gamma table must contain 256 little-endian packed entries";
            return false;
        }
        std::ifstream                   gamma(gammaPath, std::ios::binary);
        std::array<unsigned char, 1024> gammaBytes{};
        if (!gamma.read(reinterpret_cast<char*>(gammaBytes.data()), gammaBytes.size()))
        {
            error = "could not read frame gamma table";
            return false;
        }
        for (std::size_t i = 0; i < recipe.gammaTable.size(); ++i)
            recipe.gammaTable[i] = std::uint32_t(gammaBytes[i * 4]) |
                                   (std::uint32_t(gammaBytes[i * 4 + 1]) << 8) |
                                   (std::uint32_t(gammaBytes[i * 4 + 2]) << 16) |
                                   (std::uint32_t(gammaBytes[i * 4 + 3]) << 24);
        std::size_t total = 0;
        std::size_t bytes = 0;
        for (std::size_t half = 0; half < 2; ++half)
        {
            const auto* halfTable = (*halves)[half].as_table();
            const auto* draws     = halfTable ? (*halfTable)["draws"].as_array() : nullptr;
            if (!draws || draws->empty() || draws->size() > kMaxDraws - total)
            {
                error = "frame half draw list is missing or exceeds the bound";
                return false;
            }
            total += draws->size();
            for (const auto& node : *draws)
            {
                std::filesystem::path drawPath;
                if (!resolveInput(root, &node, drawPath, error))
                    return false;
                if (std::filesystem::file_size(drawPath) > 1024U * 1024U)
                {
                    error = "frame draw metadata exceeds the byte bound";
                    return false;
                }
                const auto  drawTable = toml::parse_file(drawPath.string());
                const auto* paths     = drawTable["paths"].as_table();
                if (!paths || paths->contains("initial_samples") || paths->contains("initial_sample0") ||
                    paths->contains("initial_sample1") || drawTable["depth"]["initial_samples"])
                {
                    error = "frame recipes cannot load captured color or depth attachments";
                    return false;
                }
                NativeDrawReplayRecipe draw;
                if (!LoadNativeDrawReplayRecipe(drawPath, draw, error))
                    return false;
                // Bound accumulation during loading, before subsequent files allocate.
                const auto drawBytes = draw.vertexShaderDxil.size() + draw.pixelShaderDxil.size() +
                                       draw.vertexData.size() + draw.indices.size() * sizeof(std::uint32_t) +
                                       draw.vertexConstants.size() + draw.pixelConstants.size() +
                                       draw.sharedConstants.size() + draw.textures[0].bytes.size() + draw.textures[1].bytes.size() + draw.textures[2].bytes.size();
                if (drawBytes > kMaxFrameBytes - bytes)
                {
                    error = "loaded frame inputs exceed the byte bound";
                    return false;
                }
                bytes += drawBytes;
                recipe.halves[half].push_back(std::move(draw));
            }
        }
        return ValidateNativeFrameReplayRecipe(recipe, error);
    }
    catch (const std::exception& exception)
    {
        error = std::string("could not load frame recipe: ") + exception.what();
        return false;
    }
}

} // namespace rerevved::gpu
