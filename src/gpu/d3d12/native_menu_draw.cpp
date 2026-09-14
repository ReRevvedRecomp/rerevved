#include "native_menu_draw.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>

#include <rex/crypto/sha256.h>

namespace rerevved::gpu
{
namespace
{

constexpr std::size_t kMaxGeometryBytes = 16U * 1024U * 1024U;

struct Fetch
{
    std::uint32_t format, offset;
};

struct ShaderContract
{
    std::uint64_t        vs, ps;
    std::string_view     vsUcode, psUcode, vsDxil, psDxil;
    std::uint32_t        stride, attributes, program, context, interpolators;
    std::array<Fetch, 3> fetches;
    bool                 scene;
};

// The input order and digests bind these layouts to the translated shader
// interfaces. Destination swizzles remain in the vertex shader.
constexpr ShaderContract kShaders[] = {
    { 0x11213E38D7154104ULL, 0x3A92D78FE55C7B83ULL, "7a8473e246709b45e6895b7e20ebff38949e02da014ac760cfc6caa71d13cb77", "f15792acab07e97384dbaff573da53b2641cedd0e4ecbf1c5f3e1b34d2171921", "3eee5d345eb1f1b858d073d77cb00e8296d0462eef164f44cac62c67018abd26", "f92f9c5b3d9c98bb399fbf2e92cc5d950a0fbd40de95f49c594e70b36f7d8b9a", 2, 2, 0x10010001, 0, 1, { { { 25, 0 }, { 6, 1 }, {} } }, false },
    { 0x5F6EB3BC96CE8FC0ULL, 0x6831098A8316F932ULL, "6f1c41b00c0798638e9ac75401220379d4ed389d93a729136d50dccd34d4bad9", "f2c35b1e6655282cedb7022de9b6374fc98069e78c930ec51dd3f6b313739e7e", "c472d7e6a574984403b7e02021993c93b5c19de2f3c02c91a9f8ba96374309e8", "6b24ef4d69fcf1df47cd921bdad7efad2719b0bf47f511d1eaf103e63c7ce906", 1, 1, 0x10010000, 4, 0x10001, { { { 25, 0 }, {}, {} } }, false },
    { 0x1EE55F3AB5213177ULL, 0x47F2D46F3B8F1668ULL, "e2cde9a88d5760b5b4291c5148306356f5182c53549dad55e2c1a30267536a03", "873ad152186e5cb71b2c3178f333a0468f932d34439265b434477f89e50536d2", "6cbc31bb7931421b290bb9972497613c204c3ef32b22b870ffe6c4be55566007", "4d11dbc023caf7150a434c5e91f6527befa346cb68169d20242dc6eaf6f05d3a", 3, 3, 0x10210202, 8, 0x10007, { { { 25, 0 }, { 6, 1 }, { 6, 2 } } }, false },
    { 0x2BA2325A7EA93DE3ULL, 0xC3BEC99768EF0D6BULL, "80e55d6dee0e08d066bbfdb6bdc6f1ee077f27f04862c57cce91decc76b113b4", "da7bca278e09ebf26d765b3dfc7a755a47fb04ef955476f352028ddfbaaa488b", "a3b53ebb450ef4c13485f0673fa80f09f486fd39160165cdb08c53bb2dd32d5b", "b67ad629276d3089eaa0e9fc890038e514609c1cdb9fbf634a789adafddaed3a", 7, 3, 0x10110201, 8, 0x10003, { { { 38, 0 }, { 37, 4 }, { 6, 6 } } }, false },
    { 0xC3BAB94E67553E12ULL, 0x64D8E6A475FFCB2DULL, "ca4917e94fd85d07331bfcb1949fe134234c59f1229953de8227af39dd236b91", "2cac681fefe9253f28a78bd8e5e0b5e523a62f2ff768b2ed343e163c56de3757", "9a7d0273718864046019aab013aec91532d80503a0628ff3b7d8b1bf7e44c1ce", "17fa2fb9075504e4fd7239ddab65c21ad05014d007fcfe1f2016aba896100cb8", 8, 3, 0x10210204, 8, 0x10007, { { { 57, 0 }, { 37, 3 }, { 57, 5 } } }, true },
};

const ShaderContract* contract(const NativeMenuDrawView& view)
{
    for (const auto& shader : kShaders)
        if (shader.vs == view.vertexShaderHash && shader.ps == view.pixelShaderHash)
            return &shader;
    return nullptr;
}

bool fail(std::string& error, std::string_view message)
{
    error = message;
    return false;
}

std::string digest(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

void store(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    for (std::size_t c = 0; c < 4; ++c)
        bytes[offset + c] = static_cast<std::uint8_t>(value >> (8 * c));
}

std::string wordDigest(std::span<const std::uint32_t> words)
{
    if (words.empty() || words.size_bytes() > 4U * 1024U * 1024U)
        return {};
    std::vector<std::uint8_t> bytes(words.size_bytes());
    for (std::size_t i = 0; i < words.size(); ++i)
        store(bytes, i * 4, words[i]);
    return digest(bytes);
}

std::uint32_t bigWord(const std::uint8_t* bytes)
{
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
           (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

std::int32_t signedHalf(std::uint32_t word)
{
    return static_cast<std::int32_t>((word & 0xFFFF) ^ 0x8000) - 0x8000;
}

bool state(const NativeMenuDrawView& view, const ShaderContract& shader, NativeDrawReplayRecipe& recipe, std::string& error)
{
    const auto                                    r          = view.registers;
    const bool                                    right      = r[0x2080] == 0x7D80;
    const bool                                    full       = view.fullViewportTarget;
    const std::pair<std::uint32_t, std::uint32_t> required[] = {
        { 0x2000, 0x0A010280 },
        { 0x2001, 0 },
        { 0x2002, 0x530 },
        { 0x200E, 0 },
        { 0x200F, 0x20002000 },
        { 0x2080, !full && right ? 0x7D80U : 0U },
        { 0x2081, !full && right ? 640U : 0U },
        { 0x2082, full || right ? 0x02D00500U : 0x02D00280U },
        { 0x2100, 0xFFFFFF },
        { 0x2101, 0 },
        { 0x2102, 0 },
        { 0x2104, 15 },
        { 0x210F, 0x44200000 },
        { 0x2110, 0x44200000 },
        { 0x2111, 0xC3B40000 },
        { 0x2112, 0x43B40000 },
        { 0x2113, shader.scene ? 0x3F800000U : 0U },
        { 0x2114, 0 },
        { 0x2180, shader.program },
        { 0x2181, shader.context },
        { 0x2182, shader.interpolators },
        { 0x2200, shader.scene ? 0x24F00736U : 0x24F00770U },
        { 0x2201, shader.scene ? 0x00010706U : 0x07060706U },
        { 0x2202, shader.scene ? 0x8700000CU : 0x87000004U },
        { 0x2204, 0x80000 },
        { 0x2205, shader.scene ? 0x18002U : 0x18000U },
        { 0x2206, 0x43F },
        { 0x2208, 4 },
        { 0x2302, 4 },
        { 0x2307, 0xFF000 },
        { 0x2308, 0xFF100 },
    };
    // RB_COLORCONTROL's low bits select the comparison for a disabled alpha
    // test in both the menu and copyright UI states.
    for (const auto [index, value] : required)
        if (r[index] != value && !(index == 0x2202 && !shader.scene && r[index] == 0x87000007U))
        {
            std::ostringstream text;
            text << "unsupported menu register 0x" << std::hex << index
                 << ": 0x" << r[index] << ", expected 0x" << value;
            return fail(error, text.str());
        }
    if (!view.halfPixelOffset)
        return fail(error, "menu shader requires the captured half-pixel convention");
    recipe.schemaVersion = 2;
    recipe.width         = full ? 1280 : 640;
    recipe.height        = 720;
    recipe.sampleCount   = 4;
    recipe.sampleMask    = shader.scene ? 9 : 15;
    recipe.clearColor    = { 0, 0, 0, 0 };
    recipe.viewport      = { !full && right ? -640.0F : 0.0F, 0, 1280, 720, 0, shader.scene ? 1.0F : 0.0F };
    recipe.scissor       = { 0, 0, static_cast<std::int32_t>(recipe.width), 720 };
    recipe.depth.enabled = recipe.depth.writeEnabled = shader.scene;
    recipe.rasterizer.cull                           = shader.scene ? 2 : 0;
    recipe.rasterizer.frontCounterClockwise          = shader.scene;
    recipe.blend.enabled                             = true;
    recipe.blend.sourceColor                         = NativeDrawReplayBlendFactor::SourceAlpha;
    recipe.blend.destinationColor                    = NativeDrawReplayBlendFactor::InverseSourceAlpha;
    recipe.blend.sourceAlpha                         = shader.scene ? NativeDrawReplayBlendFactor::One : NativeDrawReplayBlendFactor::SourceAlpha;
    recipe.blend.destinationAlpha                    = shader.scene ? NativeDrawReplayBlendFactor::Zero : NativeDrawReplayBlendFactor::InverseSourceAlpha;
    const auto copy                                  = [&](std::size_t first, std::size_t count, std::vector<std::uint8_t>& bytes, std::size_t offset = 0)
    {
        bytes.resize(std::max(bytes.size(), offset + count * 4));
        for (std::size_t i = 0; i < count; ++i)
            store(bytes, offset + i * 4, r[first + i]);
    };
    copy(0x4000, 256 * 4, recipe.vertexConstants);
    copy(0x4400, 224 * 4, recipe.pixelConstants);
    recipe.sharedConstants.resize(336);
    copy(0x4900, 8, recipe.sharedConstants, 256);
    store(recipe.sharedConstants, 292, std::bit_cast<std::uint32_t>(1.0F / 1280.0F));
    store(recipe.sharedConstants, 296, std::bit_cast<std::uint32_t>(-1.0F / 720.0F));
    if (shader.scene)
        store(recipe.sharedConstants, 300, r[0x210E]);
    return true;
}

} // namespace

bool LoadNativeMenuShaders(const std::filesystem::path& directory,
                           NativeMenuShaders&           shaders,
                           std::string&                 error)
{
    shaders = {};
    try
    {
        for (std::size_t i = 0; i < shaders.size(); ++i)
        {
            const auto& expected = kShaders[i];
            auto&       shader   = shaders[i];
            shader.vertexHash    = expected.vs;
            shader.pixelHash     = expected.ps;
            const auto load      = [&](std::string_view prefix, std::uint64_t hash, std::string_view sha, std::vector<std::uint8_t>& bytes)
            {
                std::ostringstream name;
                name << prefix << '_' << std::hex << std::uppercase << std::setw(16)
                     << std::setfill('0') << hash << ".dxil";
                const auto path = directory / name.str();
                const auto size = std::filesystem::file_size(path);
                if (!size || size > 4U * 1024U * 1024U)
                    return fail(error, "menu DXIL exceeds shader bounds");
                bytes.resize(static_cast<std::size_t>(size));
                std::ifstream file(path, std::ios::binary);
                if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || digest(bytes) != sha)
                    return fail(error, "menu DXIL does not match the pinned shader interface");
                return true;
            };
            if (!load("vs", expected.vs, expected.vsDxil, shader.vertexDxil) ||
                !load("ps", expected.ps, expected.psDxil, shader.pixelDxil))
                return false;
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        return fail(error, std::string("could not load menu shaders: ") + exception.what());
    }
}

bool DecodeNativeMenuDrawGeometry(const NativeMenuDrawView& view,
                                  NativeDrawReplayRecipe&   recipe,
                                  std::string&              error)
{
    error.clear();
    recipe.vertexData.clear();
    recipe.indices.clear();
    const auto* shader = contract(view);
    if (!shader || view.registers.size() != 0x5003 || !view.indexCount ||
        view.indexCount % 3 || view.indexCount > kMaxGeometryBytes / 4)
        return fail(error, "unsupported menu geometry identity, registers or triangle count");
    const auto r    = view.registers;
    const auto size = view.guestSourceVertices ? view.vertexBytes.size() : std::uint64_t((r[0x48BF] >> 2) & 0xFFFFFF) * 4;
    if (!size || size > kMaxGeometryBytes)
        return fail(error, "menu vertex bytes exceed the geometry bound");
    if (!view.guestSourceVertices && ((r[0x48BE] & 3) != 3 || (r[0x48BF] & 3) != 2 ||
                                      (r[0x48BE] & ~3U) != view.vertexGuestBase ||
                                      size != view.vertexBytes.size()))
        return fail(error, "menu vertex bytes do not match fetch constant 95");
    if (view.indexed && (view.indexFormat != 0 || view.indexEndian != 1 ||
                         view.indexBytes.size() != std::size_t(view.indexCount) * 2))
        return fail(error, "unsupported or incomplete menu index stream");
    if (!view.indexed && !view.indexBytes.empty())
        return fail(error, "nonindexed menu draw contains index bytes");
    recipe.indices.resize(view.indexCount);
    std::uint32_t maximum = 0;
    for (std::uint32_t i = 0; i < view.indexCount; ++i)
    {
        const auto index  = view.indexed ? (std::uint32_t(view.indexBytes[i * 2]) << 8) | view.indexBytes[i * 2 + 1] : i;
        recipe.indices[i] = index;
        maximum           = std::max(maximum, index);
    }
    const auto count = std::uint64_t(maximum) + 1;
    if (count * shader->stride * 4 > size || count * shader->attributes * 16 > kMaxGeometryBytes)
        return fail(error, "menu index references bytes outside the captured vertex range");
    recipe.vertexAttributeCount = shader->attributes;
    recipe.vertexStrideBytes    = shader->attributes * 16;
    recipe.indexCount           = view.indexCount;
    recipe.vertexData.resize(static_cast<std::size_t>(count * recipe.vertexStrideBytes));
    for (std::uint32_t vertex = 0; vertex <= maximum; ++vertex)
        for (std::uint32_t attribute = 0; attribute < shader->attributes; ++attribute)
        {
            const auto           fetch  = shader->fetches[attribute];
            const auto*          source = view.vertexBytes.data() + (std::size_t(vertex) * shader->stride + fetch.offset) * 4;
            std::array<float, 4> values{};
            const auto           first = bigWord(source);
            if (fetch.format == 25)
            {
                values[0] = float(signedHalf(first));
                values[1] = float(signedHalf(first >> 16));
            }
            else if (fetch.format == 6)
                for (unsigned c = 0; c < 4; ++c)
                    values[c] = float((first >> (c * 8)) & 255) / 255.0F;
            else
            {
                const auto components = fetch.format == 38 ? 4 : fetch.format == 57 ? 3
                                                                                    : 2;
                for (int c = 0; c < components; ++c)
                    values[c] = std::bit_cast<float>(bigWord(source + c * 4));
            }
            for (unsigned c = 0; c < 4; ++c)
                store(recipe.vertexData, std::size_t(vertex) * recipe.vertexStrideBytes + attribute * 16 + c * 4, std::bit_cast<std::uint32_t>(values[c]));
        }
    return true;
}

bool ValidateNativeMenuInitialClear(const NativeMenuDrawView& view, std::uint8_t& alpha, std::string& error)
{
    const auto geometryDigest = digest(view.vertexBytes);
    const bool knownGeometry  = geometryDigest == "266933252713ae004038e3977d90a79a9f3f49544a47245f68b2c08a645cd08e" ||
                                geometryDigest == "cb82e2af580b4b39271ad48d97a3d9e664a7a1969a96f43e23419790b3f0d744";
    if (view.registers.size() != 0x5003 || view.indexed || view.indexCount != 3 ||
        view.vertexShaderHash != 0x1E6883FCCDE1F688ULL || view.pixelShaderHash != 0xA4A965C189287B99ULL ||
        wordDigest(view.vertexMicrocode) != "2b2fe8f96319434015034dc955df9b2ab30d0e979645821cb592e667c4f58295" ||
        wordDigest(view.pixelMicrocode) != "f73f655ea80c22bde4bc93575f87664e81056b6f4714bef3a4defaa18994d18a" ||
        view.vertexBytes.size() != 84 || !knownGeometry)
        return fail(error, "unsupported menu initial clear geometry or shader");
    const auto                                    r          = view.registers;
    const std::pair<std::uint32_t, std::uint32_t> required[] = {
        { 0x2000, 0x05020140 },
        { 0x2001, 0 },
        { 0x2002, 0x530 },
        { 0x200E, 0 },
        { 0x200F, 0x20002000 },
        { 0x2080, 0 },
        { 0x2081, 0 },
        { 0x2082, 0x20002000 },
        { 0x2101, 0 },
        { 0x2102, 0 },
        { 0x2104, 0xFFFF },
        { 0x2180, 0x10010001 },
        { 0x2200, 0x8777 },
        { 0x2201, 0x10001 },
        { 0x2202, 0 },
        { 0x2203, 0x15 },
        { 0x2204, 0x10000 },
        { 0x2205, 0x10000 },
        { 0x2206, 0x300 },
        { 0x2208, 4 },
    };
    for (const auto [index, value] : required)
        if (r[index] != value)
            return fail(error, "unsupported menu initial clear register state");
    if ((r[0x4800] & 3) != 3 || (r[0x4801] & 3) != 2 ||
        (r[0x4800] & ~3U) != view.vertexGuestBase || ((r[0x4801] >> 2) & 0xFFFFFF) * 4 != 84)
        return fail(error, "initial clear bytes do not match vertex fetch zero");
    // Both pinned vertex buffers clear black at far depth. Their fourth color
    // components differ, so preserve that alpha in the native clear.
    alpha = bigWord(view.vertexBytes.data() + 24) == 0x3F800000U ? 255 : 0;
    return true;
}

bool BuildNativeMenuDrawRecipe(const NativeMenuDrawView& view,
                               const NativeMenuShaders&  shaders,
                               NativeDrawReplayRecipe&   recipe,
                               std::string&              error)
{
    recipe               = {};
    const auto* expected = contract(view);
    if (!expected || wordDigest(view.vertexMicrocode) != expected->vsUcode ||
        wordDigest(view.pixelMicrocode) != expected->psUcode)
        return fail(error, "unsupported menu guest shader identity");
    const auto found = std::find_if(shaders.begin(), shaders.end(), [&](const auto& shader)
                                    {
                                        return shader.vertexHash == expected->vs && shader.pixelHash == expected->ps;
                                    });
    if (found == shaders.end() || digest(found->vertexDxil) != expected->vsDxil ||
        digest(found->pixelDxil) != expected->psDxil)
        return fail(error, "menu native shader identity does not match its guest shader");
    if (!DecodeNativeMenuDrawGeometry(view, recipe, error) || !state(view, *expected, recipe, error))
        return false;
    recipe.vertexShaderHash = expected->vs;
    recipe.pixelShaderHash  = expected->ps;
    recipe.vertexShaderDxil = found->vertexDxil;
    recipe.pixelShaderDxil  = found->pixelDxil;
    if (expected != &kShaders[0])
    {
        if (!view.texture || !view.sampler)
            return fail(error, "textured menu shader is missing its point-of-use texture or sampler");
        recipe.textureMask = 1;
        recipe.texture     = *view.texture;
        recipe.sampler     = *view.sampler;
    }
    return ValidateNativeDrawReplayRecipe(recipe, error);
}

} // namespace rerevved::gpu
