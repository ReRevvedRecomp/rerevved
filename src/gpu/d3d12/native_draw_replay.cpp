#include "native_draw_replay.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>

#include <toml++/toml.hpp>

namespace rerevved::gpu
{
namespace
{

constexpr std::size_t   kMaxShaderBytes     = 4U * 1024U * 1024U;
constexpr std::size_t   kMaxGeometryBytes   = 16U * 1024U * 1024U;
constexpr std::size_t   kMaxConstantBytes   = 64U * 1024U;
constexpr std::size_t   kMaxTextureBytes    = 64U * 1024U * 1024U;
constexpr std::size_t   kMinVertexConstants = 256U * 16U;
constexpr std::size_t   kMinPixelConstants  = 224U * 16U;
constexpr std::size_t   kMinSharedConstants = 336U;
constexpr std::uint32_t kMaxTargetExtent    = 8192;
constexpr std::uint64_t kMaxTargetBytes     = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t   kMaxCombinedTargetBytes =
    static_cast<std::size_t>(2ULL * kMaxTargetBytes);
// The replay initializes and reads guest sample planes at host samples 0 and 3.
// Schema 2 also admits four-sample raster coverage when the supplied pixel
// shader restricts output coverage to 0x9 after attribute interpolation.
constexpr std::uint32_t kExpectedSampleMask = 0x9;
constexpr std::uint64_t kExpectedVertexHash = 0x11213E38D7154104ULL;
constexpr std::uint64_t kExpectedPixelHash  = 0x3A92D78FE55C7B83ULL;

template <typename T>
bool readRequired(const toml::table& table,
                  std::string_view   key,
                  T&                 result,
                  std::string&       error)
{
    const auto value = table[key].value<T>();
    if (!value)
    {
        error = "missing or invalid TOML key '" + std::string(key) + "'";
        return false;
    }
    result = *value;
    return true;
}

bool readRequiredPath(const toml::table&     table,
                      std::string_view       key,
                      std::filesystem::path& result,
                      std::string&           error)
{
    std::string value;
    if (!readRequired(table, key, value, error))
    {
        return false;
    }
    if (value.empty())
    {
        error = "TOML path '" + std::string(key) + "' is empty";
        return false;
    }
    result = std::filesystem::path(value);
    return true;
}

bool readFloat(const toml::table& table,
               std::string_view   key,
               float&             result,
               std::string&       error)
{
    double value = 0.0;
    if (!readRequired(table, key, value, error) || !std::isfinite(value))
    {
        if (error.empty())
        {
            error = "TOML float '" + std::string(key) + "' is not finite";
        }
        return false;
    }
    result = static_cast<float>(value);
    if (!std::isfinite(result))
    {
        error = "TOML float '" + std::string(key) + "' exceeds the float32 range";
        return false;
    }
    return true;
}

bool readColor(const toml::table&           table,
               std::string_view             key,
               std::array<std::uint8_t, 4>& result,
               std::string&                 error)
{
    const toml::array* values = table[key].as_array();
    if (!values || values->size() != result.size())
    {
        error = "TOML key '" + std::string(key) + "' must contain four byte values";
        return false;
    }
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const auto value = (*values)[index].value<std::int64_t>();
        if (!value || *value < 0 || *value > 255)
        {
            error = "TOML key '" + std::string(key) + "' contains an invalid byte";
            return false;
        }
        result[index] = static_cast<std::uint8_t>(*value);
    }
    return true;
}

bool readHash(const toml::table& table,
              std::string_view   key,
              std::uint64_t&     result,
              std::string&       error)
{
    std::string text;
    if (!readRequired(table, key, text, error))
    {
        return false;
    }
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.erase(0, 2);
    }
    if (text.empty() || text.size() > 16)
    {
        error = "TOML shader hash '" + std::string(key) + "' is not a 64-bit hexadecimal value";
        return false;
    }
    const auto* first  = text.data();
    const auto* last   = first + text.size();
    const auto  parsed = std::from_chars(first, last, result, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != last)
    {
        error = "TOML shader hash '" + std::string(key) + "' is not a 64-bit hexadecimal value";
        return false;
    }
    return true;
}

bool resolveRecipePath(const std::filesystem::path& root,
                       const std::filesystem::path& relative,
                       std::filesystem::path&       resolved,
                       std::string&                 error)
{
    if (relative.empty() || relative.is_absolute())
    {
        error = "recipe paths must be non-empty and relative to the recipe directory";
        return false;
    }
    const auto normalized = relative.lexically_normal();
    for (const auto& component : normalized)
    {
        if (component == "..")
        {
            error = "recipe paths may not escape the recipe directory";
            return false;
        }
    }
    resolved = root / normalized;
    return true;
}

bool readBytes(const std::filesystem::path& path,
               std::size_t                  maxBytes,
               std::vector<std::uint8_t>&   result,
               std::string&                 error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        error = "could not open input file '" + path.string() + "'";
        return false;
    }
    const std::streampos end = file.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > maxBytes)
    {
        error = "input file exceeds the replay byte bound: '" + path.string() + "'";
        return false;
    }
    result.resize(static_cast<std::size_t>(end));
    file.seekg(0, std::ios::beg);
    if (!result.empty())
    {
        file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    }
    if (!file)
    {
        error = "could not read input file '" + path.string() + "'";
        result.clear();
        return false;
    }
    return true;
}

bool readU32File(const std::filesystem::path& path,
                 std::vector<std::uint32_t>&  result,
                 std::string&                 error)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, kMaxGeometryBytes, bytes, error))
    {
        return false;
    }
    if (bytes.empty() || (bytes.size() % sizeof(std::uint32_t)) != 0)
    {
        error = "decoded index input must contain a non-empty uint32 little-endian stream";
        return false;
    }
    result.resize(bytes.size() / sizeof(std::uint32_t));
    for (std::size_t index = 0; index < result.size(); ++index)
    {
        const auto offset = index * sizeof(std::uint32_t);
        result[index]     = static_cast<std::uint32_t>(bytes[offset]) |
                            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    }
    return true;
}

bool parseBlendFactor(std::string_view             text,
                      NativeDrawReplayBlendFactor& result,
                      std::string&                 error)
{
    if (text == "zero")
        result = NativeDrawReplayBlendFactor::Zero;
    else if (text == "one")
        result = NativeDrawReplayBlendFactor::One;
    else if (text == "source_color")
        result = NativeDrawReplayBlendFactor::SourceColor;
    else if (text == "inverse_source_color")
        result = NativeDrawReplayBlendFactor::InverseSourceColor;
    else if (text == "source_alpha")
        result = NativeDrawReplayBlendFactor::SourceAlpha;
    else if (text == "inverse_source_alpha")
        result = NativeDrawReplayBlendFactor::InverseSourceAlpha;
    else if (text == "destination_alpha")
        result = NativeDrawReplayBlendFactor::DestinationAlpha;
    else if (text == "inverse_destination_alpha")
        result = NativeDrawReplayBlendFactor::InverseDestinationAlpha;
    else if (text == "destination_color")
        result = NativeDrawReplayBlendFactor::DestinationColor;
    else if (text == "inverse_destination_color")
        result = NativeDrawReplayBlendFactor::InverseDestinationColor;
    else if (text == "source_alpha_saturated")
        result = NativeDrawReplayBlendFactor::SourceAlphaSaturated;
    else if (text == "blend_factor")
        result = NativeDrawReplayBlendFactor::BlendFactor;
    else if (text == "inverse_blend_factor")
        result = NativeDrawReplayBlendFactor::InverseBlendFactor;
    else
    {
        error = "unsupported blend factor '" + std::string(text) + "'";
        return false;
    }
    return true;
}

bool parseBlendOp(std::string_view         text,
                  NativeDrawReplayBlendOp& result,
                  std::string&             error)
{
    if (text == "add")
        result = NativeDrawReplayBlendOp::Add;
    else if (text == "subtract")
        result = NativeDrawReplayBlendOp::Subtract;
    else if (text == "reverse_subtract")
        result = NativeDrawReplayBlendOp::ReverseSubtract;
    else if (text == "minimum")
        result = NativeDrawReplayBlendOp::Minimum;
    else if (text == "maximum")
        result = NativeDrawReplayBlendOp::Maximum;
    else
    {
        error = "unsupported blend operation '" + std::string(text) + "'";
        return false;
    }
    return true;
}

bool readBlendFactor(const toml::table&           table,
                     std::string_view             key,
                     NativeDrawReplayBlendFactor& result,
                     std::string&                 error)
{
    std::string text;
    return readRequired(table, key, text, error) && parseBlendFactor(text, result, error);
}

bool readBlendOp(const toml::table&       table,
                 std::string_view         key,
                 NativeDrawReplayBlendOp& result,
                 std::string&             error)
{
    std::string text;
    return readRequired(table, key, text, error) && parseBlendOp(text, result, error);
}

} // namespace

bool ValidateNativeDrawReplayRecipe(const NativeDrawReplayRecipe& recipe,
                                    std::string&                  error)
{
    if (recipe.width == 0 || recipe.height == 0 ||
        recipe.width > kMaxTargetExtent || recipe.height > kMaxTargetExtent)
    {
        error = "replay target extent is outside the bounded RGBA8 range";
        return false;
    }
    const std::uint64_t targetBytes =
        static_cast<std::uint64_t>(recipe.width) * recipe.height * 4ULL;
    if (targetBytes > kMaxTargetBytes)
    {
        error = "replay target extent exceeds the bounded RGBA8 byte range";
        return false;
    }
    if (recipe.sampleCount != 4 ||
        (recipe.sampleMask != kExpectedSampleMask &&
         !(recipe.schemaVersion == 2 && recipe.sampleMask == 0xF)))
    {
        error = "unsupported four-sample replay coverage mask";
        return false;
    }
    if (recipe.schemaVersion != 1 && recipe.schemaVersion != 2)
    {
        error = "unsupported replay recipe schema";
        return false;
    }
    if ((recipe.schemaVersion == 1 &&
         (recipe.textureMask != 0 || recipe.vertexShaderHash != kExpectedVertexHash ||
          recipe.pixelShaderHash != kExpectedPixelHash)) ||
        recipe.vertexShaderHash == 0 || recipe.pixelShaderHash == 0)
    {
        error = "recipe shader hashes do not match the supported captured draw";
        return false;
    }
    if (recipe.textureMask > 1 || (recipe.textureMask == 0 && !recipe.texture.bytes.empty()))
    {
        error = "replay supports only the explicitly supplied texture at fetch slot zero";
        return false;
    }
    if (recipe.textureMask == 1)
    {
        const auto& texture = recipe.texture;
        if (texture.width == 0 || texture.height == 0 ||
            texture.width > kMaxTargetExtent || texture.height > kMaxTargetExtent)
        {
            error = "replay texture extent is outside the supported range";
            return false;
        }
        std::uint64_t expectedBytes = 0;
        switch (texture.format)
        {
            case NativeDrawReplayTextureFormat::Rgba8:
                expectedBytes = static_cast<std::uint64_t>(texture.width) * texture.height * 4;
                break;
            case NativeDrawReplayTextureFormat::R8:
                expectedBytes = static_cast<std::uint64_t>(texture.width) * texture.height;
                break;
            case NativeDrawReplayTextureFormat::Bc1:
            case NativeDrawReplayTextureFormat::Bc2:
                expectedBytes = static_cast<std::uint64_t>((texture.width + 3) / 4) *
                                ((texture.height + 3) / 4) *
                                (texture.format == NativeDrawReplayTextureFormat::Bc1 ? 8 : 16);
                break;
            default:
                error = "unsupported replay texture format";
                return false;
        }
        if (expectedBytes > kMaxTextureBytes || texture.bytes.size() != expectedBytes ||
            std::any_of(texture.swizzle.begin(), texture.swizzle.end(), [](auto value)
                        {
                            return value > 5;
                        }))
        {
            error = "replay texture payload or component mapping is invalid";
            return false;
        }
        const auto invalidAddress = [](auto value)
        {
            return value < 1 || value > 5;
        };
        const auto nonFinite = [](auto value)
        {
            return !std::isfinite(value);
        };
        if (std::any_of(recipe.sampler.address.begin(), recipe.sampler.address.end(), invalidAddress) ||
            !std::isfinite(recipe.sampler.minLod) || !std::isfinite(recipe.sampler.maxLod) ||
            !std::isfinite(recipe.sampler.mipBias) || recipe.sampler.minLod > recipe.sampler.maxLod ||
            recipe.sampler.mipBias < -16.0F || recipe.sampler.mipBias > 15.99F ||
            std::any_of(recipe.sampler.border.begin(), recipe.sampler.border.end(), nonFinite))
        {
            error = "replay sampler state is invalid";
            return false;
        }
    }
    if (recipe.vertexShaderDxil.empty() || recipe.pixelShaderDxil.empty() ||
        recipe.vertexShaderDxil.size() > kMaxShaderBytes ||
        recipe.pixelShaderDxil.size() > kMaxShaderBytes ||
        (recipe.vertexShaderDxil.size() % 4) != 0 ||
        (recipe.pixelShaderDxil.size() % 4) != 0)
    {
        error = "DXIL input is empty or exceeds the shader byte bound";
        return false;
    }
    if (recipe.vertexAttributeCount == 0 || recipe.vertexAttributeCount > 16 ||
        recipe.vertexStrideBytes != recipe.vertexAttributeCount * 16 ||
        (recipe.schemaVersion == 1 && recipe.vertexAttributeCount != 2) ||
        recipe.vertexData.empty() || recipe.vertexData.size() > kMaxGeometryBytes ||
        recipe.vertexData.size() % recipe.vertexStrideBytes != 0)
    {
        error = "replay vertex input must contain one to sixteen packed float4 attributes";
        return false;
    }
    if (recipe.indices.empty() || recipe.indexCount != recipe.indices.size() ||
        recipe.indexCount > (kMaxGeometryBytes / sizeof(std::uint32_t)))
    {
        error = "replay index input is empty, mismatched, or exceeds the geometry bound";
        return false;
    }
    const auto vertexCount = recipe.vertexData.size() / recipe.vertexStrideBytes;
    for (std::size_t offset = 0; offset < recipe.vertexData.size(); offset += sizeof(float))
    {
        float value = 0.0F;
        std::memcpy(&value, recipe.vertexData.data() + offset, sizeof(value));
        if (!std::isfinite(value))
        {
            error = "decoded vertex attributes contain a non-finite float32";
            return false;
        }
    }
    for (const std::uint32_t index : recipe.indices)
    {
        if (index >= vertexCount)
        {
            error = "replay index references a vertex outside the decoded vertex array";
            return false;
        }
    }
    if (recipe.indexCount < 3 || (recipe.indexCount % 3) != 0)
    {
        error = "triangle-list replay index count must be a positive multiple of three";
        return false;
    }
    if (recipe.vertexConstants.empty() || recipe.pixelConstants.empty() ||
        recipe.sharedConstants.empty() ||
        recipe.vertexConstants.size() < kMinVertexConstants ||
        recipe.pixelConstants.size() < kMinPixelConstants ||
        recipe.sharedConstants.size() < kMinSharedConstants ||
        recipe.vertexConstants.size() > kMaxConstantBytes ||
        recipe.pixelConstants.size() > kMaxConstantBytes ||
        recipe.sharedConstants.size() > kMaxConstantBytes)
    {
        error = "stage and shared constant files must be present and bounded";
        return false;
    }
    if ((recipe.vertexConstants.size() % 16) != 0 ||
        (recipe.pixelConstants.size() % 16) != 0 ||
        (recipe.sharedConstants.size() % 16) != 0)
    {
        error = "stage and shared constant files must contain complete float4 register values";
        return false;
    }
    if (recipe.initialSample0.size() != targetBytes ||
        recipe.initialSample1.size() != targetBytes)
    {
        error = "initial sample files must be exactly width * height * 4 RGBA8 bytes";
        return false;
    }
    if (!std::isfinite(recipe.viewport.x) || !std::isfinite(recipe.viewport.y) ||
        !std::isfinite(recipe.viewport.width) || !std::isfinite(recipe.viewport.height) ||
        !std::isfinite(recipe.viewport.minDepth) || !std::isfinite(recipe.viewport.maxDepth) ||
        recipe.viewport.width <= 0.0F || recipe.viewport.height <= 0.0F ||
        recipe.viewport.minDepth < 0.0F || recipe.viewport.maxDepth > 1.0F ||
        recipe.viewport.minDepth > recipe.viewport.maxDepth)
    {
        error = "captured viewport is invalid";
        return false;
    }
    if (recipe.scissor.left < 0 || recipe.scissor.top < 0 ||
        recipe.scissor.right <= recipe.scissor.left || recipe.scissor.bottom <= recipe.scissor.top ||
        static_cast<std::uint32_t>(recipe.scissor.right) > recipe.width ||
        static_cast<std::uint32_t>(recipe.scissor.bottom) > recipe.height)
    {
        error = "captured scissor is outside the replay target";
        return false;
    }
    if (recipe.blend.writeMask > 0x0F)
    {
        error = "captured blend write mask contains unsupported bits";
        return false;
    }
    if (recipe.blend.logicOpEnabled)
    {
        error = "logic-op blending is outside the supported replay subset";
        return false;
    }
    const auto usesUncapturedBlendFactor = [](NativeDrawReplayBlendFactor factor)
    {
        return factor == NativeDrawReplayBlendFactor::BlendFactor ||
               factor == NativeDrawReplayBlendFactor::InverseBlendFactor;
    };
    if (usesUncapturedBlendFactor(recipe.blend.sourceColor) ||
        usesUncapturedBlendFactor(recipe.blend.destinationColor) ||
        usesUncapturedBlendFactor(recipe.blend.sourceAlpha) ||
        usesUncapturedBlendFactor(recipe.blend.destinationAlpha))
    {
        error = "constant blend factors are outside the captured replay state";
        return false;
    }
    return true;
}

bool LoadNativeDrawReplayRecipe(const std::filesystem::path& recipePath,
                                NativeDrawReplayRecipe&      recipe,
                                std::string&                 error)
{
    recipe = {};
    error.clear();
    toml::table table;
    try
    {
        table = toml::parse_file(recipePath.string());
    }
    catch (const toml::parse_error& parseError)
    {
        error = "could not parse replay recipe: " + std::string(parseError.description());
        return false;
    }

    std::int64_t schemaVersion = 0;
    if (!readRequired(table, "schema_version", schemaVersion, error) ||
        (schemaVersion != 1 && schemaVersion != 2))
    {
        if (error.empty())
        {
            error = "replay recipe schema_version must be 1 or 2";
        }
        return false;
    }
    recipe.schemaVersion = static_cast<std::uint32_t>(schemaVersion);

    const toml::table* paths    = table["paths"].as_table();
    const toml::table* target   = table["target"].as_table();
    const toml::table* draw     = table["draw"].as_table();
    const toml::table* shader   = table["shader"].as_table();
    const toml::table* viewport = table["viewport"].as_table();
    const toml::table* scissor  = table["scissor"].as_table();
    const toml::table* blend    = table["blend"].as_table();
    if (!paths || !target || !draw || !shader || !viewport || !scissor || !blend)
    {
        error = "replay recipe is missing one or more required TOML tables";
        return false;
    }

    std::int64_t value = 0;
    if (!readRequired(*target, "width", value, error) || value <= 0 || value > std::numeric_limits<std::uint32_t>::max())
        return false;
    recipe.width = static_cast<std::uint32_t>(value);
    if (!readRequired(*target, "height", value, error) || value <= 0 || value > std::numeric_limits<std::uint32_t>::max())
        return false;
    recipe.height = static_cast<std::uint32_t>(value);
    if (recipe.width == 0 || recipe.height == 0 ||
        recipe.width > kMaxTargetExtent || recipe.height > kMaxTargetExtent)
    {
        error = "replay target extent is outside the bounded RGBA8 range";
        return false;
    }
    const auto targetBytes =
        static_cast<std::uint64_t>(recipe.width) * recipe.height * 4ULL;
    if (targetBytes > kMaxTargetBytes)
    {
        error = "replay target extent exceeds the bounded RGBA8 byte range";
        return false;
    }
    if (!readRequired(*target, "sample_count", value, error) || value < 0 || value > std::numeric_limits<std::uint32_t>::max())
        return false;
    recipe.sampleCount = static_cast<std::uint32_t>(value);
    if (!readRequired(*target, "sample_mask", value, error) || value < 0 || value > std::numeric_limits<std::uint32_t>::max())
        return false;
    recipe.sampleMask = static_cast<std::uint32_t>(value);
    if (!readColor(*target, "clear_color_rgba8", recipe.clearColor, error))
        return false;

    std::string topology;
    bool        indexed = false;
    if (!readRequired(*draw, "topology", topology, error) || topology != "triangle_list")
        return false;
    if (!readRequired(*draw, "indexed", indexed, error) || !indexed)
        return false;
    if (!readRequired(*draw, "vertex_stride_bytes", value, error) || value < 0 || value > std::numeric_limits<std::uint32_t>::max())
        return false;
    recipe.vertexStrideBytes = static_cast<std::uint32_t>(value);
    if (!readRequired(*draw, "vertex_attribute_count", value, error) || value < 0 || value > std::numeric_limits<std::uint32_t>::max())
        return false;
    recipe.vertexAttributeCount = static_cast<std::uint32_t>(value);
    if (!readRequired(*draw, "index_count", value, error) || value < 0 || value > std::numeric_limits<std::uint32_t>::max())
        return false;
    recipe.indexCount = static_cast<std::uint32_t>(value);
    if (!readRequired(*draw, "texture_mask", value, error) || value < 0 || value > std::numeric_limits<std::uint32_t>::max())
        return false;
    recipe.textureMask = static_cast<std::uint32_t>(value);

    if (!readHash(*shader, "vertex_hash", recipe.vertexShaderHash, error) ||
        !readHash(*shader, "pixel_hash", recipe.pixelShaderHash, error))
        return false;

    if (!readFloat(*viewport, "x", recipe.viewport.x, error) ||
        !readFloat(*viewport, "y", recipe.viewport.y, error) ||
        !readFloat(*viewport, "width", recipe.viewport.width, error) ||
        !readFloat(*viewport, "height", recipe.viewport.height, error) ||
        !readFloat(*viewport, "min_depth", recipe.viewport.minDepth, error) ||
        !readFloat(*viewport, "max_depth", recipe.viewport.maxDepth, error))
        return false;
    std::int64_t scissorValue = 0;
    if (!readRequired(*scissor, "left", scissorValue, error) ||
        scissorValue < std::numeric_limits<std::int32_t>::min() ||
        scissorValue > std::numeric_limits<std::int32_t>::max())
        return false;
    recipe.scissor.left = static_cast<std::int32_t>(scissorValue);
    if (!readRequired(*scissor, "top", scissorValue, error) ||
        scissorValue < std::numeric_limits<std::int32_t>::min() ||
        scissorValue > std::numeric_limits<std::int32_t>::max())
        return false;
    recipe.scissor.top = static_cast<std::int32_t>(scissorValue);
    if (!readRequired(*scissor, "right", scissorValue, error) ||
        scissorValue < std::numeric_limits<std::int32_t>::min() ||
        scissorValue > std::numeric_limits<std::int32_t>::max())
        return false;
    recipe.scissor.right = static_cast<std::int32_t>(scissorValue);
    if (!readRequired(*scissor, "bottom", scissorValue, error) ||
        scissorValue < std::numeric_limits<std::int32_t>::min() ||
        scissorValue > std::numeric_limits<std::int32_t>::max())
        return false;
    recipe.scissor.bottom = static_cast<std::int32_t>(scissorValue);

    if (!readRequired(*blend, "enabled", recipe.blend.enabled, error) ||
        !readRequired(*blend, "alpha_to_coverage", recipe.blend.alphaToCoverage, error) ||
        !readRequired(*blend, "logic_op_enabled", recipe.blend.logicOpEnabled, error) ||
        !readBlendFactor(*blend, "source_color", recipe.blend.sourceColor, error) ||
        !readBlendFactor(*blend, "destination_color", recipe.blend.destinationColor, error) ||
        !readBlendOp(*blend, "color_op", recipe.blend.colorOp, error) ||
        !readBlendFactor(*blend, "source_alpha", recipe.blend.sourceAlpha, error) ||
        !readBlendFactor(*blend, "destination_alpha", recipe.blend.destinationAlpha, error) ||
        !readBlendOp(*blend, "alpha_op", recipe.blend.alphaOp, error))
        return false;
    if (!readRequired(*blend, "write_mask", value, error) || value < 0 || value > 255)
        return false;
    recipe.blend.writeMask = static_cast<std::uint8_t>(value);

    const auto            root = std::filesystem::absolute(recipePath).parent_path();
    std::filesystem::path vertexDxilPath;
    std::filesystem::path pixelDxilPath;
    std::filesystem::path verticesPath;
    std::filesystem::path indicesPath;
    std::filesystem::path vertexConstantsPath;
    std::filesystem::path pixelConstantsPath;
    std::filesystem::path sharedConstantsPath;
    std::filesystem::path initialSample0Path;
    std::filesystem::path initialSample1Path;
    std::filesystem::path initialSamplesPath;
    if (!readRequiredPath(*paths, "vertex_dxil", vertexDxilPath, error) ||
        !readRequiredPath(*paths, "pixel_dxil", pixelDxilPath, error) ||
        !readRequiredPath(*paths, "vertices", verticesPath, error) ||
        !readRequiredPath(*paths, "indices_u32", indicesPath, error) ||
        !readRequiredPath(*paths, "vertex_constants", vertexConstantsPath, error) ||
        !readRequiredPath(*paths, "pixel_constants", pixelConstantsPath, error) ||
        !readRequiredPath(*paths, "shared_constants", sharedConstantsPath, error))
        return false;
    std::filesystem::path resolved;
    if (recipe.textureMask != 0)
    {
        const auto* texture = table["texture"].as_table();
        const auto* sampler = table["sampler"].as_table();
        if (recipe.schemaVersion != 2 || recipe.textureMask != 1 || !texture || !sampler)
        {
            error = "textured replay requires schema 2 and texture/sampler tables";
            return false;
        }
        std::filesystem::path texturePath;
        std::string           format;
        if (!readRequiredPath(*texture, "file", texturePath, error) ||
            !resolveRecipePath(root, texturePath, resolved, error) ||
            !readBytes(resolved, kMaxTextureBytes, recipe.texture.bytes, error) ||
            !readRequired(*texture, "format", format, error))
            return false;
        if (format == "rgba8_unorm")
            recipe.texture.format = NativeDrawReplayTextureFormat::Rgba8;
        else if (format == "r8_unorm")
            recipe.texture.format = NativeDrawReplayTextureFormat::R8;
        else if (format == "bc1_unorm")
            recipe.texture.format = NativeDrawReplayTextureFormat::Bc1;
        else if (format == "bc2_unorm")
            recipe.texture.format = NativeDrawReplayTextureFormat::Bc2;
        else
        {
            error = "unsupported replay texture format";
            return false;
        }
        if (!readRequired(*texture, "width", value, error))
            return false;
        if (value <= 0 || value > kMaxTargetExtent)
        {
            error = "texture width is outside the supported range";
            return false;
        }
        recipe.texture.width = static_cast<std::uint32_t>(value);
        if (!readRequired(*texture, "height", value, error))
            return false;
        if (value <= 0 || value > kMaxTargetExtent)
        {
            error = "texture height is outside the supported range";
            return false;
        }
        recipe.texture.height = static_cast<std::uint32_t>(value);
        if (!readColor(*texture, "swizzle", recipe.texture.swizzle, error) ||
            !readRequired(*sampler, "min_linear", recipe.sampler.minLinear, error) ||
            !readRequired(*sampler, "mag_linear", recipe.sampler.magLinear, error) ||
            !readRequired(*sampler, "mip_linear", recipe.sampler.mipLinear, error) ||
            !readFloat(*sampler, "min_lod", recipe.sampler.minLod, error) ||
            !readFloat(*sampler, "max_lod", recipe.sampler.maxLod, error) ||
            !readFloat(*sampler, "mip_bias", recipe.sampler.mipBias, error))
            return false;
        const std::array<std::string_view, 3> addressKeys = { "address_u", "address_v", "address_w" };
        for (std::size_t axis = 0; axis < addressKeys.size(); ++axis)
        {
            if (!readRequired(*sampler, addressKeys[axis], value, error))
                return false;
            if (value < 1 || value > 5)
            {
                error = "sampler '" + std::string(addressKeys[axis]) + "' must be a D3D12 address mode from 1 to 5";
                return false;
            }
            recipe.sampler.address[axis] = static_cast<std::uint32_t>(value);
        }
        const auto* border = (*sampler)["border"].as_array();
        if (!border || border->size() != 4)
        {
            error = "sampler border must contain four finite floats";
            return false;
        }
        for (std::size_t component = 0; component < 4; ++component)
        {
            const auto number = (*border)[component].value<double>();
            if (!number || !std::isfinite(*number))
            {
                error = "sampler border must contain four finite floats";
                return false;
            }
            recipe.sampler.border[component] = static_cast<float>(*number);
        }
    }
    if (!resolveRecipePath(root, vertexDxilPath, resolved, error) || !readBytes(resolved, kMaxShaderBytes, recipe.vertexShaderDxil, error))
        return false;
    if (!resolveRecipePath(root, pixelDxilPath, resolved, error) || !readBytes(resolved, kMaxShaderBytes, recipe.pixelShaderDxil, error))
        return false;
    if (!resolveRecipePath(root, verticesPath, resolved, error) || !readBytes(resolved, kMaxGeometryBytes, recipe.vertexData, error))
        return false;
    if (!resolveRecipePath(root, indicesPath, resolved, error) || !readU32File(resolved, recipe.indices, error))
        return false;
    if (!resolveRecipePath(root, vertexConstantsPath, resolved, error) || !readBytes(resolved, kMaxConstantBytes, recipe.vertexConstants, error))
        return false;
    if (!resolveRecipePath(root, pixelConstantsPath, resolved, error) || !readBytes(resolved, kMaxConstantBytes, recipe.pixelConstants, error))
        return false;
    if (!resolveRecipePath(root, sharedConstantsPath, resolved, error) || !readBytes(resolved, kMaxConstantBytes, recipe.sharedConstants, error))
        return false;
    const auto* combinedSamplesNode = paths->get("initial_samples");
    const auto  combinedSamples     = combinedSamplesNode
                                          ? combinedSamplesNode->value<std::string>()
                                          : std::optional<std::string>{};
    if (combinedSamples)
    {
        initialSamplesPath = std::filesystem::path(*combinedSamples);
        if (!resolveRecipePath(root, initialSamplesPath, resolved, error))
            return false;
        std::vector<std::uint8_t> combined;
        const std::uint64_t       combinedBytes =
            static_cast<std::uint64_t>(recipe.width) * recipe.height * 8ULL;
        if (!readBytes(resolved, kMaxCombinedTargetBytes, combined, error) ||
            combined.size() != combinedBytes)
        {
            error = "combined initial_samples must contain two complete RGBA8 planes";
            return false;
        }
        const auto planeBytes = combined.size() / 2;
        recipe.initialSample0.assign(combined.begin(), combined.begin() + planeBytes);
        recipe.initialSample1.assign(combined.begin() + planeBytes, combined.end());
    }
    else
    {
        if (!readRequiredPath(*paths, "initial_sample0", initialSample0Path, error) ||
            !readRequiredPath(*paths, "initial_sample1", initialSample1Path, error))
            return false;
        if (!resolveRecipePath(root, initialSample0Path, resolved, error) ||
            !readBytes(resolved, static_cast<std::size_t>(kMaxTargetBytes), recipe.initialSample0, error))
            return false;
        if (!resolveRecipePath(root, initialSample1Path, resolved, error) ||
            !readBytes(resolved, static_cast<std::size_t>(kMaxTargetBytes), recipe.initialSample1, error))
            return false;
    }
    // The destination is deliberately absent from recipe state. The harness
    // supplies it from the mandatory second CLI argument before replay.
    recipe.sourcePath = std::filesystem::absolute(recipePath);

    if (!ValidateNativeDrawReplayRecipe(recipe, error))
    {
        return false;
    }
    return true;
}

} // namespace rerevved::gpu
