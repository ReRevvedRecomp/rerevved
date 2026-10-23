#include "native_gameplay_shaders.h"

#include <charconv>
#include <fstream>
#include <set>
#include <stdexcept>
#include <tuple>

#include <rex/crypto/sha256.h>
#include <toml++/toml.hpp>

namespace rerevved::gpu
{
namespace
{

std::string requiredString(const toml::table& table, std::string_view name)
{
    const auto value = table[name].value<std::string>();
    if (!value || value->empty())
        throw std::runtime_error("missing gameplay shader string: " + std::string(name));
    return *value;
}

std::uint32_t requiredWord(const toml::table& table, std::string_view name)
{
    const auto value = table[name].value<std::int64_t>();
    if (!value || *value < 0 || std::uint64_t(*value) > UINT32_MAX)
        throw std::runtime_error("invalid gameplay shader word: " + std::string(name));
    return static_cast<std::uint32_t>(*value);
}

std::uint32_t optionalWord(const toml::table& table, std::string_view name, std::uint32_t fallback)
{
    if (!table.contains(name))
        return fallback;
    const auto value = table[name].value<std::int64_t>();
    if (!value || *value < 0 || std::uint64_t(*value) > UINT32_MAX)
        throw std::runtime_error("invalid gameplay shader specialization word: " + std::string(name));
    return static_cast<std::uint32_t>(*value);
}

std::int32_t optionalSignedWord(const toml::table& table, std::string_view name, std::int32_t fallback)
{
    if (!table.contains(name))
        return fallback;
    const auto value = table[name].value<std::int64_t>();
    if (!value || *value < INT32_MIN || *value > INT32_MAX)
        throw std::runtime_error("invalid gameplay shader specialization value: " + std::string(name));
    return static_cast<std::int32_t>(*value);
}

bool requiredBool(const toml::table& table, std::string_view name)
{
    const auto value = table[name].value<bool>();
    if (!value)
        throw std::runtime_error("missing gameplay shader boolean: " + std::string(name));
    return *value;
}

std::uint64_t requiredHash(const toml::table& table, std::string_view name)
{
    const auto    text   = requiredString(table, name);
    std::uint64_t result = 0;
    const auto    parsed = std::from_chars(text.data(), text.data() + text.size(), result, 16);
    if (text.size() != 16 || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !result)
        throw std::runtime_error("invalid gameplay shader identity");
    return result;
}

std::string requiredDigest(const toml::table& table, std::string_view name)
{
    const auto value = requiredString(table, name);
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("invalid gameplay shader digest");
    return value;
}

std::vector<std::uint8_t> loadArtifact(const std::filesystem::path& root,
                                       const toml::table&           table,
                                       std::string_view             field,
                                       const std::string&           expected,
                                       NativeShaderFormat           format)
{
    const std::string_view      formatName = format == NativeShaderFormat::Dxil ? "DXIL" : "SPIR-V";
    const std::filesystem::path name(requiredString(table, field));
    if (name.is_absolute())
        throw std::runtime_error("gameplay shader path must be relative");
    const auto path     = std::filesystem::canonical(root / name);
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        throw std::runtime_error("gameplay shader path escapes its directory");
    const auto size = std::filesystem::file_size(path);
    if (!size || size > 4U * 1024U * 1024U)
        throw std::runtime_error("gameplay shader exceeds the " + std::string(formatName) + " byte bound");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream             file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) ||
        rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())) != expected)
        throw std::runtime_error("gameplay " + std::string(formatName) + " does not match its interface digest");
    return bytes;
}

std::int64_t catalogSchema(const toml::table& table, std::string_view refusal)
{
    const auto schema = table["schema_version"].value_or<std::int64_t>(0);
    if (schema != 1 && schema != 2)
        throw std::runtime_error(std::string(refusal));
    return schema;
}

// Names an entry in a shader_format_missing refusal.
std::string entryName(const toml::table& entry)
{
    if (const auto id = entry["id"].value<std::string>())
        return *id;
    return entry["vertex_hash"].value_or<std::string>("") + "-" + entry["pixel_hash"].value_or<std::string>("");
}

struct StageArtifact
{
    std::string               digest;
    std::vector<std::uint8_t> bytes;
};

// Schema 1 uses <stage>_file and <stage>_dxil_sha256; schema 2 uses
// <stage>_dxil or <stage>_spirv tables for the requested format.
StageArtifact loadStage(const std::filesystem::path& root,
                        const toml::table&           entry,
                        std::int64_t                 schema,
                        std::string_view             stage,
                        NativeShaderFormat           format)
{
    const std::string prefix(stage);
    StageArtifact     artifact;
    if (schema == 1)
    {
        if (format != NativeShaderFormat::Dxil)
            throw std::runtime_error("shader_format_missing: " + entryName(entry));
        artifact.digest = requiredDigest(entry, prefix + "_dxil_sha256");
        artifact.bytes  = loadArtifact(root, entry, prefix + "_file", artifact.digest, format);
        return artifact;
    }
    if (entry.contains(prefix + "_file") || entry.contains(prefix + "_dxil_sha256"))
        throw std::runtime_error("gameplay shader schema 2 entry carries schema 1 artifact keys");
    const auto* table = entry[prefix + (format == NativeShaderFormat::Dxil ? "_dxil" : "_spirv")].as_table();
    if (!table || table->empty())
        throw std::runtime_error("shader_format_missing: " + entryName(entry));
    artifact.digest = requiredDigest(*table, "sha256");
    artifact.bytes  = loadArtifact(root, *table, "file", artifact.digest, format);
    return artifact;
}

} // namespace

bool LoadNativeGameplayShaders(const std::filesystem::path&                directory,
                               std::vector<NativeGameplayShaderInterface>& shaders,
                               std::string&                                error,
                               NativeShaderFormat                          format)
{
    shaders.clear();
    error.clear();
    try
    {
        const auto root = std::filesystem::canonical(directory);
        const auto path = root / "shaders.toml";
        if (std::filesystem::file_size(path) > 1024U * 1024U)
            throw std::runtime_error("gameplay shader catalog exceeds the metadata bound");
        const auto        table   = toml::parse_file(path.string());
        const auto* const entries = table["shaders"].as_array();
        const auto        schema  = catalogSchema(table, "gameplay shader catalog requires schema 1 or 2 and shader entries");
        if (!entries || entries->empty())
            throw std::runtime_error("gameplay shader catalog requires schema 1 or 2 and shader entries");
        using Key = std::tuple<std::uint64_t,
                               std::uint64_t,
                               std::uint32_t,
                               std::uint32_t,
                               std::uint32_t,
                               bool,
                               std::uint32_t,
                               std::uint32_t,
                               std::int32_t,
                               std::uint32_t,
                               std::uint32_t,
                               std::uint32_t,
                               std::uint32_t,
                               bool,
                               bool>;
        std::set<Key> identities;
        for (const auto& node : *entries)
        {
            const auto* entry = node.as_table();
            if (!entry)
                throw std::runtime_error("gameplay shader entry must be a table");
            NativeGameplayShaderInterface shader;
            shader.vertexShaderHash      = requiredHash(*entry, "vertex_hash");
            shader.pixelShaderHash       = requiredHash(*entry, "pixel_hash");
            shader.vertexGuestWordDigest = requiredDigest(*entry, "vertex_word_sha256");
            shader.pixelGuestWordDigest  = requiredDigest(*entry, "pixel_word_sha256");
            auto vertex                  = loadStage(root, *entry, schema, "vertex", format);
            auto pixel                   = loadStage(root, *entry, schema, "pixel", format);
            if (format == NativeShaderFormat::Dxil)
            {
                shader.vertexDxilDigest = std::move(vertex.digest);
                shader.pixelDxilDigest  = std::move(pixel.digest);
                shader.vertexShaderDxil = std::move(vertex.bytes);
                shader.pixelShaderDxil  = std::move(pixel.bytes);
            }
            else
            {
                shader.vertexSpirvDigest = std::move(vertex.digest);
                shader.pixelSpirvDigest  = std::move(pixel.digest);
                shader.vertexShaderSpirv = std::move(vertex.bytes);
                shader.pixelShaderSpirv  = std::move(pixel.bytes);
            }
            shader.alphaTestEnabled    = requiredBool(*entry, "alpha_test");
            shader.alphaTestCompare    = requiredWord(*entry, "alpha_compare");
            shader.rasterizedDepth     = requiredBool(*entry, "rasterized_depth");
            shader.independentSamplers = requiredBool(*entry, "independent_samplers");
            if (!shader.independentSamplers)
                throw std::runtime_error("gameplay shaders require independent samplers");
            shader.colorTargetFormat = optionalWord(*entry, "color_format", 0);
            shader.colorExpBias      = optionalSignedWord(*entry, "color_exp_bias", 0);
            shader.sampleCount       = optionalWord(*entry, "sample_count", 4);
            shader.sampleMask        = optionalWord(*entry, "sample_mask", 9);
            if (shader.colorTargetFormat != 0 && shader.colorTargetFormat != 2 && shader.colorTargetFormat != 4)
                throw std::runtime_error("unsupported gameplay shader target format");
            if (shader.colorExpBias < -32 || shader.colorExpBias > 31 ||
                (shader.colorTargetFormat != 4 && shader.colorExpBias != 0))
                throw std::runtime_error("unsupported gameplay shader color exponent bias");
            if (!((shader.sampleCount == 1 && shader.sampleMask == 1) ||
                  (shader.sampleCount == 4 && shader.sampleMask == 9)))
                throw std::runtime_error("unsupported gameplay shader sample state");
            // build-native-shaders.py admits a second target of k_8_8_8_8 or
            // k_2_10_10_10; its format exists only with that target.
            shader.colorTargetCount = optionalWord(*entry, "color_targets", 1);
            shader.color1Format     = optionalWord(*entry, "color1_format", 0);
            if ((shader.colorTargetCount != 1 && shader.colorTargetCount != 2) ||
                (shader.colorTargetCount == 1 ? entry->contains("color1_format")
                                              : !entry->contains("color1_format") || (shader.color1Format != 0 && shader.color1Format != 2)))
                throw std::runtime_error("unsupported gameplay shader color targets");
            const auto program              = requiredWord(*entry, "program");
            const auto context              = requiredWord(*entry, "context");
            const auto interpolators        = requiredWord(*entry, "interpolators");
            shader.generatedPixelParameters = entry->contains("generated_pixel_parameters") &&
                                              requiredBool(*entry, "generated_pixel_parameters");
            if (shader.generatedPixelParameters != bool(program & (1U << 18)))
                throw std::runtime_error("gameplay shader generated pixel parameter specialization is missing or mismatched");
            // Absent means the VS expects the guest viewport transform.
            shader.screenSpacePosition = entry->contains("screen_space_position") &&
                                         requiredBool(*entry, "screen_space_position");
            // Absent means guest clipping without user clip planes.
            shader.userClipPlane = entry->contains("user_clip_plane") && requiredBool(*entry, "user_clip_plane");
            if (shader.userClipPlane && shader.screenSpacePosition)
                throw std::runtime_error("gameplay shader user clip plane requires guest clipping");
            shader.shaderProgram       = program;
            shader.shaderContext       = context;
            shader.shaderInterpolators = interpolators;
            if (!identities.emplace(shader.vertexShaderHash,
                                    shader.pixelShaderHash,
                                    program,
                                    context,
                                    interpolators,
                                    shader.alphaTestEnabled,
                                    shader.alphaTestCompare,
                                    shader.colorTargetFormat,
                                    shader.colorExpBias,
                                    shader.sampleCount,
                                    shader.sampleMask,
                                    shader.colorTargetCount,
                                    shader.color1Format,
                                    shader.screenSpacePosition,
                                    shader.userClipPlane)
                     .second)
                throw std::runtime_error("duplicate gameplay shader specialization");
            const auto* attributes = (*entry)["attributes"].as_array();
            const auto* slots      = (*entry)["texture_fetches"].as_array();
            const auto* dimensions = (*entry)["texture_dimensions"].as_array();
            if (!attributes || attributes->empty() || attributes->size() > 16 || !slots || slots->size() > kNativeDrawReplayTextureSlots ||
                !dimensions || dimensions->size() != slots->size())
                throw std::runtime_error("invalid gameplay shader input interface");
            for (const auto& attributeNode : *attributes)
            {
                const auto* attribute = attributeNode.as_table();
                if (!attribute)
                    throw std::runtime_error("gameplay vertex attribute must be a table");
                NativeGameplayVertexAttribute value;
                value.fetchConstant = requiredWord(*attribute, "fetch");
                value.strideBytes   = requiredWord(*attribute, "stride_bytes");
                value.format        = requiredWord(*attribute, "format");
                const auto offset   = (*attribute)["offset_bytes"].value<std::int64_t>();
                if (!offset || *offset < INT32_MIN || *offset > INT32_MAX)
                    throw std::runtime_error("invalid gameplay vertex attribute offset");
                value.offsetBytes = static_cast<std::int32_t>(*offset);
                value.isSigned    = requiredBool(*attribute, "signed");
                value.normalized  = requiredBool(*attribute, "normalized");
                shader.vertexAttributes.push_back(value);
            }
            std::uint32_t mask = 0;
            for (const auto& slotNode : *slots)
            {
                const auto slot = slotNode.value<std::int64_t>();
                if (!slot || *slot < 0 || *slot > 31 || (mask & (1U << *slot)))
                    throw std::runtime_error("invalid or duplicate gameplay texture slot");
                mask |= 1U << *slot;
                shader.textureFetchSlots.push_back(static_cast<std::uint32_t>(*slot));
            }
            for (const auto& dimensionNode : *dimensions)
            {
                const auto dimension = dimensionNode.value<std::string>();
                if (!dimension || (*dimension != "2d" && *dimension != "cube"))
                    throw std::runtime_error("unsupported gameplay texture dimension");
                shader.textureDimensions.push_back(*dimension == "cube" ? NativeDrawReplayTextureDimension::TextureCube
                                                                        : NativeDrawReplayTextureDimension::Texture2D);
            }
            shaders.push_back(std::move(shader));
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        shaders.clear();
        error = exception.what();
        return false;
    }
}

bool LoadNativeDepthOnlyShaders(const std::filesystem::path&                directory,
                                std::vector<NativeGameplayShaderInterface>& shaders,
                                std::string&                                error,
                                NativeShaderFormat                          format)
{
    shaders.clear();
    error.clear();
    try
    {
        const auto root = std::filesystem::canonical(directory);
        const auto path = root / "depth-only.toml";
        if (!std::filesystem::exists(path))
            return true;
        if (std::filesystem::file_size(path) > 1024U * 1024U)
            throw std::runtime_error("depth-only shader list exceeds the metadata bound");
        const auto        table   = toml::parse_file(path.string());
        const auto* const entries = table["depth_only"].as_array();
        const auto        schema  = catalogSchema(table, "depth-only shader list requires schema 1 or 2 and entries");
        if (!entries || entries->empty())
            throw std::runtime_error("depth-only shader list requires schema 1 or 2 and entries");
        std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t>> identities;
        for (const auto& node : *entries)
        {
            const auto* entry = node.as_table();
            if (!entry)
                throw std::runtime_error("depth-only shader entry must be a table");
            NativeGameplayShaderInterface shader;
            shader.depthOnly             = true;
            shader.vertexShaderHash      = requiredHash(*entry, "vertex_hash");
            shader.vertexGuestWordDigest = requiredDigest(*entry, "vertex_word_sha256");
            requiredString(*entry, "id");
            // A vertex-only entry has no pixel artifact in either schema.
            if (schema == 2 && (entry->contains("pixel_dxil") || entry->contains("pixel_spirv") || entry->contains("pixel_file") ||
                                entry->contains("pixel_dxil_sha256")))
                throw std::runtime_error("depth-only shader entry carries a pixel artifact");
            auto vertex = loadStage(root, *entry, schema, "vertex", format);
            if (format == NativeShaderFormat::Dxil)
            {
                shader.vertexDxilDigest = std::move(vertex.digest);
                shader.vertexShaderDxil = std::move(vertex.bytes);
            }
            else
            {
                shader.vertexSpirvDigest = std::move(vertex.digest);
                shader.vertexShaderSpirv = std::move(vertex.bytes);
            }
            requiredString(*entry, "caller");
            // Without a pixel shader nothing tests alpha or samples a texture.
            shader.rasterizedDepth     = true;
            shader.independentSamplers = true;
            shader.shaderProgram       = requiredWord(*entry, "program");
            shader.shaderContext       = requiredWord(*entry, "context");
            shader.shaderInterpolators = requiredWord(*entry, "interpolators");
            if (requiredWord(*entry, "mode_control") != 5 || requiredWord(*entry, "color_mask") != 0)
                throw std::runtime_error("depth-only shader entry requires mode_control 5 and color_mask 0");
            if (shader.shaderProgram & (1U << 18))
                throw std::runtime_error("depth-only shader entry generates pixel parameters");
            if (!identities.emplace(shader.vertexShaderHash, shader.shaderProgram, shader.shaderContext, shader.shaderInterpolators).second)
                throw std::runtime_error("duplicate depth-only shader entry");
            shaders.push_back(std::move(shader));
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        shaders.clear();
        error = exception.what();
        return false;
    }
}

} // namespace rerevved::gpu
