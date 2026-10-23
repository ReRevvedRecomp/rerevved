#include "gpu/d3d12/native_gameplay_draw.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <rex/crypto/sha256.h>

using namespace rerevved::gpu;

namespace
{

std::string bytesDigest(const std::vector<std::uint8_t>& bytes)
{
    return rex::crypto::sha256(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string wordDigest(const std::vector<std::uint32_t>& words)
{
    std::vector<std::uint8_t> bytes(words.size() * 4);
    for (std::size_t i = 0; i < words.size(); ++i)
        for (std::size_t byte = 0; byte < 4; ++byte)
            bytes[i * 4 + byte] = static_cast<std::uint8_t>(words[i] >> (byte * 8));
    return bytesDigest(bytes);
}

void storeBE32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset]     = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void commonState(std::vector<std::uint32_t>& registers, bool right)
{
    registers[0x2000] = 0x0A010280;
    registers[0x2001] = 0;
    registers[0x2002] = 0x530;
    registers[0x200E] = 0;
    registers[0x200F] = 0x20002000;
    registers[0x2080] = right ? 0x7D80 : 0;
    registers[0x2081] = right ? 640 : 0;
    registers[0x2082] = right ? 0x02D00500 : 0x02D00280;
    registers[0x2100] = 0xFFFFFF;
    registers[0x2101] = 0;
    registers[0x2102] = 0;
    registers[0x2104] = 0xF;
    registers[0x210F] = 0x44200000;
    registers[0x2110] = 0x44200000;
    registers[0x2111] = 0xC3B40000;
    registers[0x2112] = 0x43B40000;
    registers[0x2113] = 0x3F800000;
    registers[0x2114] = 0;
    registers[0x2204] = 0x80000;
    registers[0x2205] = 0x18002;
    registers[0x2206] = 0x43F;
    registers[0x2208] = 4;
    registers[0x2302] = 4;
    registers[0x2307] = 0xFF000;
    registers[0x2308] = 0xFF100;
}

NativeGameplayShaderInterface shaderFor(const std::vector<std::uint32_t>&          vsWords,
                                        const std::vector<std::uint32_t>&          psWords,
                                        std::vector<NativeGameplayVertexAttribute> attributes,
                                        std::vector<std::uint32_t>                 textureSlots = {})
{
    NativeGameplayShaderInterface shader;
    shader.vertexShaderHash      = 0x1111222233334444ULL;
    shader.pixelShaderHash       = 0x5555666677778888ULL;
    shader.vertexShaderDxil      = { 'D', 'X', 'B', 'C' };
    shader.pixelShaderDxil       = { 'P', 'S', 'D', 'X' };
    shader.vertexGuestWordDigest = wordDigest(vsWords);
    shader.pixelGuestWordDigest  = wordDigest(psWords);
    shader.vertexDxilDigest      = bytesDigest(shader.vertexShaderDxil);
    shader.pixelDxilDigest       = bytesDigest(shader.pixelShaderDxil);
    shader.vertexAttributes      = std::move(attributes);
    shader.rasterizedDepth       = true;
    shader.independentSamplers   = true;
    shader.textureFetchSlots     = std::move(textureSlots);
    shader.textureDimensions.assign(shader.textureFetchSlots.size(),
                                    NativeDrawReplayTextureDimension::Texture2D);
    return shader;
}

// Depth-only needs RB_MODECONTROL=5, RB_COLOR_MASK=0 and no pixel stage.
void depthOnlyDraw()
{
    std::vector<std::uint32_t> registers(0x5003);
    commonState(registers, false);
    registers[0x2104] = 0;
    registers[0x2208] = 5;
    registers[0x2200] = 0x24F00776;
    registers[0x2201] = 0x00010001;
    registers[0x2202] = 0x87000007;
    registers[0x4800] = 0x2003;
    registers[0x4801] = 0x26;

    std::vector<std::uint8_t> vertices(3 * 12);
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
        for (std::size_t component = 0; component < 3; ++component)
            storeBE32(vertices, vertex * 12 + component * 4, std::bit_cast<std::uint32_t>(float(vertex + component)));
    std::vector<std::uint8_t>  indices = { 0, 0, 0, 1, 0, 2 };
    std::vector<std::uint32_t> vsWords = { 1, 2, 3, 4 };
    std::vector<std::uint32_t> psWords = { 5, 6, 7, 8 };
    auto                       shader  = shaderFor(vsWords, psWords, { { 0, 12, 0, 57, true, false } });
    shader.depthOnly                   = true;
    shader.pixelShaderHash             = 0;
    shader.pixelShaderDxil.clear();
    shader.pixelGuestWordDigest.clear();
    shader.pixelDxilDigest.clear();
    NativeGameplayVertexFetchSpan fetch{ 0, 0x2000, vertices };
    NativeGameplayDrawView        view;
    view.registers       = registers;
    view.vertexMicrocode = vsWords;
    view.vertexFetches   = std::span<const NativeGameplayVertexFetchSpan>(&fetch, 1);
    view.indexBytes      = indices;
    view.primitive       = 4;
    view.indexCount      = 3;
    view.indexFormat     = 0;
    view.indexEndian     = 1;
    NativeDrawReplayRecipe recipe;
    std::string            error;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("a depth-only draw builds a pixel-less recipe that writes depth only");
    REQUIRE((recipe.depthOnly && recipe.pixelShaderHash == 0 && recipe.pixelShaderBytes().empty() && recipe.blend.writeMask == 0 &&
             recipe.depth.enabled && recipe.depth.writeEnabled && recipe.depth.compare == 8 && recipe.textureMask == 0));
    UNSCOPED_INFO(error.c_str());
    REQUIRE(ValidateNativeDrawReplayRecipe(recipe, error));
    std::shared_ptr<const NativeDrawReplayAdmission> admission;
    UNSCOPED_INFO("the depth-only recipe's pipeline key marks the null pixel shader");
    REQUIRE((AdmitNativeDrawReplayRecipe(recipe, admission, error) && admission->PipelineStateKey().flags.back() == 1));

    // Validated bytecode shares an empty pixel payload with the version.
    NativeGameplayShaderValidationCache cache;
    NativeGameplayDrawTarget            target;
    target.viewport = recipe.viewport;
    target.scissor  = recipe.scissor;
    for (unsigned pass = 0; pass < 2; ++pass)
    {
        NativeDrawReplayRecipe cached;
        UNSCOPED_INFO("a cached depth-only draw owns an empty pixel payload through its shader version");
        REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, target, cached, error, &cache) && cached.shaderVersion && cached.sharedPixelShaderDxil &&
                 cached.sharedPixelShaderDxil->empty() && cached.shaderVersion->PixelBytecode() == cached.sharedPixelShaderDxil &&
                 ValidateNativeDrawReplayRecipe(cached, error)));
    }

    const auto refused = [&](const char* name, const std::string& expected)
    {
        NativeDrawReplayRecipe rejected;
        std::string            reason;
        const bool             built = BuildNativeGameplayDrawRecipe(view, shader, rejected, reason);
        if (built || reason != expected)
        {
            std::cerr << name << ": " << (built ? "accepted" : reason) << '\n';
            UNSCOPED_INFO(name);
            REQUIRE(false);
        }
    };
    view.pixelMicrocode = psWords;
    refused("a pixel shader with a depth-only interface", "depth_only_pixel_shader");
    view.pixelMicrocode = {};
    registers[0x2104]   = 0xF;
    refused("depth-only mode with a color mask", "depth_only_color_writes");
    registers[0x2104] = 0;
    registers[0x2208] = 4;
    refused("color and depth mode with the color mask clear", "depth_only_color_writes");
    registers[0x2208]      = 5;
    shader.pixelShaderHash = 0x5555666677778888ULL;
    refused("a depth-only interface with a pixel identity", "gameplay shader descriptor is missing bounded pinned DXIL");
    shader.pixelShaderHash = 0;
    shader.depthOnly       = false;
    refused("a pixel-less interface without the depth-only kind", "gameplay shader descriptor is missing bounded pinned DXIL");
    shader.depthOnly = true;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    {
        // Key A7E9CB62's alpha test (Greater against 0x3F169697) is a no-op
        // without a pixel shader: the recipe equals the untested one, with no
        // threshold bound. Alpha to mask stays refused.
        registers[0x210E] = 0x3F169697;
        for (const auto control : { 0x8700000CU, 0x8700000EU, 0x8700000BU })
        {
            registers[0x2202] = control;
            NativeDrawReplayRecipe tested;
            UNSCOPED_INFO(std::format("a pixel-less draw ignores RB_COLORCONTROL {:#x}", control).c_str());
            REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, tested, error) && tested.depthOnly && tested.sharedConstants == recipe.sharedConstants &&
                     tested.vertexData == recipe.vertexData && tested.indices == recipe.indices && tested.depth.compare == recipe.depth.compare &&
                     tested.depth.writeEnabled == recipe.depth.writeEnabled && tested.blend.writeMask == 0));
        }
        // Nor does SQ_INTERPOLATOR_CNTL matter without a pixel shader.
        registers[0x2182] = 0x3F003F;
        NativeDrawReplayRecipe stale;
        UNSCOPED_INFO("a pixel-less draw ignores SQ_INTERPOLATOR_CNTL");
        REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, stale, error) && stale.sharedConstants == recipe.sharedConstants));
        registers[0x2182] = shader.shaderInterpolators;
        registers[0x2202] = 0x8700001C;
        refused("a pixel-less draw with alpha to mask", "alpha_to_mask_unsupported");
        registers[0x2202] = 0x87000007;
        registers[0x210E] = 0;
    }

    const auto invalid = [&](const char* name, auto mutate, const std::string& expected)
    {
        auto value = recipe;
        mutate(value);
        std::string reason;
        if (ValidateNativeDrawReplayRecipe(value, reason) || reason != expected)
        {
            std::cerr << name << ": " << reason << '\n';
            UNSCOPED_INFO(name);
            REQUIRE(false);
        }
    };
    invalid("a depth-only recipe with color writes", [](NativeDrawReplayRecipe& value)
            {
                value.blend.writeMask = 0xF;
            },
            "depth-only replay writes color, samples textures or has no depth or stencil state");
    invalid("a depth-only recipe without depth or stencil", [](NativeDrawReplayRecipe& value)
            {
                value.depth.enabled = value.depth.writeEnabled = false;
            },
            "depth-only replay writes color, samples textures or has no depth or stencil state");
    invalid("a depth-only recipe with pixel DXIL", [](NativeDrawReplayRecipe& value)
            {
                value.pixelShaderDxil = { 1, 2, 3, 4 };
            },
            "DXIL input is empty or exceeds the shader byte bound");
    invalid("a depth-only recipe with a pixel hash", [](NativeDrawReplayRecipe& value)
            {
                value.pixelShaderHash = 2;
            },
            "recipe shader hashes do not match the supported captured draw");
    invalid("a pixel-less recipe without the depth-only kind", [](NativeDrawReplayRecipe& value)
            {
                value.depthOnly = false;
            },
            "recipe shader hashes do not match the supported captured draw");
    std::cout << "Depth-only draws build pixel-less recipes; pixel shaders, color masks, color mode and pixel identities are refused\n";
}

void terrainLikeDraw()
{
    std::vector<std::uint32_t> registers(0x5003);
    commonState(registers, false);
    registers[0x2200] = 0x24F00732;
    registers[0x2201] = 0x00010706;
    registers[0x2202] = 0x8700000C;
    registers[0x210E] = 0x210E;
    registers[0x4800] = 0x2003;
    registers[0x4801] = 0x26;
    registers[0x4000] = 0x01020304U;
    registers[0x43FF] = 0x7FC12345U;
    registers[0x4400] = 0x80000000U;
    registers[0x47FF] = 0x87654321U;

    std::vector<std::uint8_t> vertices(3 * 12);
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        storeBE32(vertices, vertex * 12, std::bit_cast<std::uint32_t>(float(vertex + 1)));
        storeBE32(vertices, vertex * 12 + 4, std::bit_cast<std::uint32_t>(float(2 * vertex)));
        vertices[vertex * 12 + 8]  = 0x00;
        vertices[vertex * 12 + 9]  = 0x40;
        vertices[vertex * 12 + 10] = 0x80;
        vertices[vertex * 12 + 11] = 0xFF;
    }
    std::vector<std::uint8_t>  indices = { 0, 0, 0, 1, 0, 2 };
    std::vector<std::uint32_t> vsWords = { 1, 2, 3, 4 };
    std::vector<std::uint32_t> psWords = { 5, 6, 7, 8 };
    auto                       shader  = shaderFor(vsWords,
                                                   psWords,
                                                   { { 0, 12, 0, 37, false, false }, { 0, 12, 8, 6, false, true } });
    shader.alphaTestEnabled            = true;
    shader.alphaTestCompare            = 4;
    NativeGameplayVertexFetchSpan fetch{ 0, 0x2000, vertices };
    NativeGameplayDrawView        view;
    view.registers       = registers;
    view.vertexMicrocode = vsWords;
    view.pixelMicrocode  = psWords;
    view.vertexFetches   = std::span<const NativeGameplayVertexFetchSpan>(&fetch, 1);
    view.indexBytes      = indices;
    view.primitive       = 4;
    view.indexCount      = 3;
    view.indexFormat     = 0;
    view.indexEndian     = 1;
    NativeDrawReplayRecipe recipe;
    std::string            error;
    registers[0x2180] = shader.shaderProgram = 1U << 18;
    UNSCOPED_INFO("an unspecialized pixel shader cannot accept generated pixel coordinates");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("generated pixel parameter") != std::string::npos));
    shader.generatedPixelParameters = true;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    registers[0x2180] = shader.shaderProgram = 0;
    shader.generatedPixelParameters          = false;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    // Pretransformed pixel positions (clipping off, viewport transform off)
    // need the screen-space VS, and that VS needs exactly that state.
    registers[0x2204] = 0x90000;
    registers[0x2206] = 0x400;
    UNSCOPED_INFO("screen-space state without the specialized VS is refused by name");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error == "screen_space_position_unpinned"));
    shader.screenSpacePosition = true;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    registers[0x2206] = 0x43F;
    UNSCOPED_INFO("the screen-space VS refuses an enabled viewport transform");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error == "viewport_transform_unexpected"));
    registers[0x2204] = 0x80000;
    UNSCOPED_INFO("the screen-space VS refuses the ordinary clipped state");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error == "viewport_transform_unexpected"));
    shader.screenSpacePosition = false;
    registers[0x2206]          = 0x400;
    UNSCOPED_INFO("a disabled viewport transform with guest clipping on stays unsupported");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error.find("0x2206") != std::string::npos));
    registers[0x2206] = 0x43F;
    UNSCOPED_INFO(error.c_str());
    REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && recipe.sharedConstants.size() == 336));
    {
        // UCP_ENA_0 (PA_CL_CLIP_CNTL 0x80001) needs the VS that clips by user
        // plane 0, which binds PA_CL_UCP_0 at shared bytes 464..479.
        const std::array<std::uint32_t, 4> plane{ 0xAF55A600U, 0x3BB5CC6CU, 0xBF7FFEB1U, 0x3EF9D4B4U };
        std::ranges::copy(plane, registers.begin() + 0x2388);
        registers[0x238C] = 0x3F800000U;
        registers[0x2204] = 0x80001;
        UNSCOPED_INFO("user clip plane 0 without the clipping VS is refused by name");
        REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error == "user_clip_plane_unpinned"));
        shader.userClipPlane = true;
        UNSCOPED_INFO(error.c_str());
        REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && recipe.sharedConstants.size() == 480));
        for (std::size_t index = 0; index < plane.size(); ++index)
        {
            std::uint32_t value;
            std::memcpy(&value, recipe.sharedConstants.data() + 464 + index * 4, 4);
            UNSCOPED_INFO("the recipe binds PA_CL_UCP_0 as c29");
            REQUIRE(value == plane[index]);
        }
        for (const auto clip : { 0x80000U, 0x80003U, 0x80002U, 0xA0001U, 0x90001U })
        {
            registers[0x2204] = clip;
            UNSCOPED_INFO(std::format("the clipping VS refuses PA_CL_CLIP_CNTL {:#x}", clip).c_str());
            REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error == "user_clip_plane_unexpected"));
        }
        shader.userClipPlane = false;
        for (const auto clip : { 0x80003U, 0x80002U, 0xA0001U })
        {
            registers[0x2204] = clip;
            UNSCOPED_INFO(std::format("PA_CL_CLIP_CNTL {:#x} stays unsupported", clip).c_str());
            REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error.find("0x2204") != std::string::npos));
        }
        registers[0x2204] = 0x80000;
        std::ranges::fill_n(registers.begin() + 0x2388, 5, 0U);
    }
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("terrain attributes packed as float4");
    REQUIRE((recipe.vertexAttributeCount == 2 && recipe.vertexStrideBytes == 32));
    {
        NativeGameplayDrawTarget full;
        full.width        = 1280;
        registers[0x2082] = 0x02D00500;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(DecodeNativeGameplayViewportAndScissor(registers, full.viewport, full.scissor, error));
        NativeDrawReplayRecipe logical;
        UNSCOPED_INFO("tile target admission cannot silently expand the guest surface pitch");
        REQUIRE(!BuildNativeGameplayDrawRecipe(view, shader, full, logical, error));
        full.fullViewportTarget = true;
        UNSCOPED_INFO("explicit guest shadow target retains the full logical viewport");
        REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, full, logical, error) && logical.width == 1280 &&
                 logical.viewport.width == 1280 && logical.scissor.right == 1280));
        registers[0x2000] ^= 1;
        UNSCOPED_INFO("full viewport admission still rejects a different surface pitch");
        REQUIRE(!BuildNativeGameplayDrawRecipe(view, shader, full, logical, error));
        registers[0x2000] ^= 1;
        // A window scissor inside the logical surface is kept and applied;
        // one past its bottom edge is refused.
        registers[0x2082] = 0x015F0500;
        UNSCOPED_INFO("a 1280x351 window scissor on the full logical viewport is admitted");
        REQUIRE((DecodeNativeGameplayViewportAndScissor(registers, full.viewport, full.scissor, error) &&
                 BuildNativeGameplayDrawRecipe(view, shader, full, logical, error) && logical.scissor.right == 1280 &&
                 logical.scissor.bottom == 351));
        registers[0x2082]   = 0x02D10500;
        full.scissor.bottom = 721;
        UNSCOPED_INFO("a window scissor below the logical surface is refused");
        REQUIRE(!BuildNativeGameplayDrawRecipe(view, shader, full, logical, error));
        // An inset top left corner, as a GFx movie's display rectangle has.
        registers[0x2081] = 0x001801A9;
        registers[0x2082] = 0x020B0401;
        UNSCOPED_INFO("an inset window scissor on the full logical viewport is admitted and applied");
        REQUIRE((DecodeNativeGameplayViewportAndScissor(registers, full.viewport, full.scissor, error) &&
                 BuildNativeGameplayDrawRecipe(view, shader, full, logical, error) && logical.scissor.left == 425 &&
                 logical.scissor.top == 24 && logical.scissor.right == 1025 && logical.scissor.bottom == 523));
        registers[0x2081] |= 0x80000000U;
        UNSCOPED_INFO("the window offset disable bit is refused on the full logical viewport");
        REQUIRE((DecodeNativeGameplayViewportAndScissor(registers, full.viewport, full.scissor, error) &&
                 !BuildNativeGameplayDrawRecipe(view, shader, full, logical, error)));
        registers[0x2081] = 0;
        registers[0x2082] = 0x02D00280;
    }
    UNSCOPED_INFO("terrain indices decoded");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>({ 0, 1, 2 }));
    UNSCOPED_INFO("terrain float vertex attribute decoded");
    REQUIRE(std::bit_cast<float>(std::uint32_t(recipe.vertexData[0]) |
                                 (std::uint32_t(recipe.vertexData[1]) << 8) |
                                 (std::uint32_t(recipe.vertexData[2]) << 16) |
                                 (std::uint32_t(recipe.vertexData[3]) << 24)) == 1.0F);
    UNSCOPED_INFO("alpha reference copied to shared constants");
    REQUIRE((recipe.sharedConstants[300] == 0x0E && recipe.sharedConstants[301] == 0x21));
    UNSCOPED_INFO("both complete shader constant banks are retained");
    REQUIRE((recipe.vertexConstants.size() == 4096 && recipe.pixelConstants.size() == 4096));
    const auto constantWord = [](const std::vector<std::uint8_t>& bytes, std::size_t offset)
    {
        return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
               (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
    };
    const auto halfPixelX = std::bit_cast<float>(constantWord(recipe.sharedConstants, 292));
    const auto halfPixelY = std::bit_cast<float>(constantWord(recipe.sharedConstants, 296));
    UNSCOPED_INFO("shared constants shift geometry +0.5 pixels using the logical viewport, not the 640-pixel target half");
    REQUIRE((recipe.viewport.width == 1280.0F && recipe.viewport.height == 720.0F &&
             halfPixelX == 1.0F / recipe.viewport.width && halfPixelY == -1.0F / recipe.viewport.height &&
             halfPixelX * recipe.viewport.width * 0.5F == 0.5F &&
             -halfPixelY * recipe.viewport.height * 0.5F == 0.5F));
    UNSCOPED_INFO("stage constants preserve bank boundaries, byte order and floating-point payload bits");
    REQUIRE((constantWord(recipe.vertexConstants, 0) == 0x01020304U &&
             constantWord(recipe.vertexConstants, 4092) == 0x7FC12345U &&
             constantWord(recipe.pixelConstants, 0) == 0x80000000U &&
             constantWord(recipe.pixelConstants, 4092) == 0x87654321U));
    storeBE32(vertices, 4, 0xFFFFFFFFU);
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("referenced guest float NaN attribute retains its payload bits");
    REQUIRE((recipe.vertexData[4] == 0xFF && recipe.vertexData[5] == 0xFF &&
             recipe.vertexData[6] == 0xFF && recipe.vertexData[7] == 0xFF));
    NativeGameplayShaderValidationCache validation;
    NativeGameplayDrawTarget            target;
    const auto                          cachedDraw = [&]()
    {
        return BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error, &validation);
    };
    const auto expectedVertices = recipe.vertexData;
    UNSCOPED_INFO("repeated shader validation preserves converted geometry");
    REQUIRE((cachedDraw() && cachedDraw() && recipe.vertexData == expectedVertices));
    UNSCOPED_INFO("the producer binds a genuine immutable version to both shader owners");
    REQUIRE((recipe.shaderVersion == validation.Version() && recipe.shaderVersion &&
             recipe.shaderVersion->VertexBytecode() == recipe.sharedVertexShaderDxil &&
             recipe.shaderVersion->PixelBytecode() == recipe.sharedPixelShaderDxil));
    {
        std::shared_ptr<const NativeDrawReplayAdmission> admission;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(BuildNativeGameplayDrawAdmission(view, shader, target, admission, error, &validation));
        UNSCOPED_INFO("admitted gameplay conversion preserves raw bytes and exact shader ownership");
        REQUIRE((admission->Recipe().vertexData == recipe.vertexData && admission->Recipe().indices == recipe.indices &&
                 admission->Recipe().vertexConstants == recipe.vertexConstants &&
                 admission->Recipe().sharedVertexShaderDxil == recipe.sharedVertexShaderDxil));
        auto invalid      = view;
        invalid.registers = {};
        UNSCOPED_INFO("admitted gameplay construction rejects malformed input and clears the owner");
        REQUIRE((!BuildNativeGameplayDrawAdmission(invalid, shader, target, admission, error, &validation) && !admission));
    }
    {
        for (unsigned stage = 0; stage < 2; ++stage)
        {
            auto       mismatched = recipe;
            auto&      payload    = stage ? mismatched.sharedPixelShaderDxil : mismatched.sharedVertexShaderDxil;
            const auto original   = payload;
            payload               = std::make_shared<const std::vector<std::uint8_t>>(*original);
            std::shared_ptr<const NativeDrawReplayAdmission> admission;
            UNSCOPED_INFO("equal bytecode from a different owner cannot claim the producer's version");
            REQUIRE((!AdmitNativeDrawReplayRecipe(mismatched, admission, error) && !admission &&
                     error.find("shader version") != std::string::npos));
            payload = std::shared_ptr<const std::vector<std::uint8_t>>(original.get(), [](const auto*)
                                                                       {
                                                                       });
            UNSCOPED_INFO("the same bytecode address with unrelated ownership cannot bind a version");
            REQUIRE((!AdmitNativeDrawReplayRecipe(mismatched, admission, error) && !admission));
            payload.reset();
            (stage ? mismatched.pixelShaderDxil : mismatched.vertexShaderDxil) = *original;
            UNSCOPED_INFO("raw bytecode cannot borrow an unrelated version's identity");
            REQUIRE((!AdmitNativeDrawReplayRecipe(std::move(mismatched), admission, error) && !admission));
        }
    }
    std::weak_ptr<const NativeDrawReplayShaderVersion> retiredVersion;
    {
        const auto retained = recipe;
        retiredVersion      = retained.shaderVersion;
        UNSCOPED_INFO("cached draws own the exact validated shader bytes");
        REQUIRE((retained.sharedVertexShaderDxil && retained.sharedPixelShaderDxil &&
                 retained.vertexShaderDxil.empty() && retained.pixelShaderDxil.empty() &&
                 std::ranges::equal(retained.vertexShaderBytes(), shader.vertexShaderDxil) &&
                 std::ranges::equal(retained.pixelShaderBytes(), shader.pixelShaderDxil)));
        UNSCOPED_INFO("unchanged validated bytecode must share immutable storage");
        REQUIRE((cachedDraw() && recipe.sharedVertexShaderDxil == retained.sharedVertexShaderDxil &&
                 recipe.sharedPixelShaderDxil == retained.sharedPixelShaderDxil && recipe.shaderVersion == retained.shaderVersion));
        const auto expected = shader.vertexShaderDxil;
        shader.vertexShaderDxil[0] ^= 1;
        shader.vertexDxilDigest = bytesDigest(shader.vertexShaderDxil);
        UNSCOPED_INFO("a valid bytecode update cannot mutate an already owned recipe");
        REQUIRE((cachedDraw() && recipe.sharedVertexShaderDxil != retained.sharedVertexShaderDxil &&
                 recipe.shaderVersion != retained.shaderVersion &&
                 std::ranges::equal(*retained.shaderVersion->VertexBytecode(), expected)));
        shader.vertexShaderDxil = expected;
        shader.vertexDxilDigest = bytesDigest(expected);
        validation              = {};
        UNSCOPED_INFO("shader cache retirement must preserve retained recipe ownership");
        REQUIRE((!retiredVersion.expired() && std::ranges::equal(retained.vertexShaderBytes(), expected) && cachedDraw()));
    }
    UNSCOPED_INFO("a replaced shader version retires after its final recipe owner releases");
    REQUIRE(retiredVersion.expired());
    {
        NativeGameplayGeometryCache geometry(512U * 1024U * 1024U);
        const auto                  reuseDraw = [&]()
        {
            return BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error, &validation, &geometry);
        };
        UNSCOPED_INFO("repeated validated geometry must share an immutable decoded buffer");
        REQUIRE((reuseDraw() && !recipe.sharedVertexData && reuseDraw() && recipe.sharedVertexData));
        auto                       retained = recipe;
        std::vector<std::uint32_t> cachedIndices;
        std::uint32_t              maximum = 0;
        UNSCOPED_INFO("a validated warm entry reuses its exact index conversion");
        REQUIRE((geometry.LookupIndices(view, cachedIndices, maximum) && cachedIndices == recipe.indices && maximum == 2));
        std::swap(indices[1], indices[3]);
        UNSCOPED_INFO("same-maximum index mutations change topology while retaining valid vertices");
        REQUIRE((reuseDraw() && recipe.indices == std::vector<std::uint32_t>({ 1, 0, 2 }) &&
                 recipe.sharedVertexData == retained.sharedVertexData));
        UNSCOPED_INFO("retained recipes own their final index bytes independently");
        REQUIRE(retained.indices == std::vector<std::uint32_t>({ 0, 1, 2 }));
        std::swap(indices[1], indices[3]);
        UNSCOPED_INFO(error.c_str());
        REQUIRE(reuseDraw());
        registers[0x2101] = 1;
        UNSCOPED_INFO("index bounds decide conversion identity even when the maximum is unchanged");
        REQUIRE((reuseDraw() && recipe.indices == std::vector<std::uint32_t>({ 1, 1, 2 })));
        registers[0x2101] = 0;
        view.primitive    = 6;
        view.indexCount   = 4;
        indices.insert(indices.end(), { 0, 2 });
        view.indexBytes = indices;
        UNSCOPED_INFO("strip topology and count changes invalidate list conversion reuse");
        REQUIRE((reuseDraw() && recipe.indices == std::vector<std::uint32_t>({ 0, 1, 2, 2, 1, 2 })));
        view.primitive  = 4;
        view.indexCount = 3;
        indices.resize(6);
        view.indexBytes = indices;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(reuseDraw());
        const auto restart = registers[0x2205];
        registers[0x2205] |= 1U << 21;
        registers[0x2103] = 1;
        fetch.bytes       = std::span<const std::uint8_t>(vertices).first(vertices.size() - 1);
        UNSCOPED_INFO("restart rejection still precedes malformed fetch validation after a warm conversion");
        REQUIRE((!reuseDraw() && error == "primitive_reset_in_list"));
        registers[0x2205] = restart;
        registers[0x2103] = 0;
        fetch.bytes       = vertices;
        view.indexEndian  = 0;
        UNSCOPED_INFO("warm conversion cannot bypass endian admission");
        REQUIRE((!reuseDraw() && error == "gameplay draw has an unsupported or incomplete indexed stream"));
        view.indexEndian = 1;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(reuseDraw());
        recipe.indices[0] = 99;
        UNSCOPED_INFO("caller recipe mutation cannot change the owned conversion result");
        REQUIRE((geometry.LookupIndices(view, cachedIndices, maximum) && cachedIndices == retained.indices));
        UNSCOPED_INFO(error.c_str());
        REQUIRE(reuseDraw());
        UNSCOPED_INFO("shared vertices retain every payload bit");
        REQUIRE(std::ranges::equal(recipe.vertexBytes(), expectedVertices));
        vertices[0] ^= 1;
        UNSCOPED_INFO("used source mutation must decode a new buffer");
        REQUIRE((reuseDraw() && !recipe.sharedVertexData));
        NativeDrawReplayRecipe uncached;
        UNSCOPED_INFO("mutation fallback matches independent conversion");
        REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, target, uncached, error) &&
                 std::ranges::equal(recipe.vertexBytes(), uncached.vertexBytes())));
        vertices[0] ^= 1;
        UNSCOPED_INFO("restored exact source can reuse the retained immutable version");
        REQUIRE((reuseDraw() && recipe.sharedVertexData == retained.sharedVertexData));
        recipe.vertexData = { 1 };
        UNSCOPED_INFO("caller-owned mutation cannot alter cached bytes");
        REQUIRE(std::ranges::equal(retained.vertexBytes(), expectedVertices));
        shader.vertexAttributes[1].normalized = false;
        UNSCOPED_INFO("attribute semantics must decide cache identity");
        REQUIRE((reuseDraw() && !recipe.sharedVertexData));
        shader.vertexAttributes[1].normalized = true;
        vsWords[0] ^= 1;
        UNSCOPED_INFO("geometry hits must still validate shader bytes");
        REQUIRE(!reuseDraw());
        vsWords[0] ^= 1;
        fetch.bytes = std::span<const std::uint8_t>(vertices).first(vertices.size() - 1);
        UNSCOPED_INFO("geometry hits must still validate source extents");
        REQUIRE(!reuseDraw());
        fetch.bytes = vertices;
        geometry.SetBudget(0);
        UNSCOPED_INFO("budget retirement removes cached index conversions");
        REQUIRE(!geometry.LookupIndices(view, cachedIndices, maximum));
        UNSCOPED_INFO("cache budget exhaustion falls back without changing geometry");
        REQUIRE((reuseDraw() && !recipe.sharedVertexData &&
                 std::ranges::equal(recipe.vertexBytes(), expectedVertices)));
        UNSCOPED_INFO("cache retirement cannot invalidate a retained recipe");
        REQUIRE(std::ranges::equal(retained.vertexBytes(), expectedVertices));
        geometry.SetBudget(512U * 1024U * 1024U);
        UNSCOPED_INFO("the cache can resume after budget fallback");
        REQUIRE((reuseDraw() && reuseDraw() && recipe.sharedVertexData));
        shader.independentSamplers = false;
        UNSCOPED_INFO("geometry hits must still validate per-draw state");
        REQUIRE(!reuseDraw());
        shader.independentSamplers = true;
        indices[5]                 = 1;
        UNSCOPED_INFO("a changed indexed prefix must decode its own vertex extent");
        REQUIRE((reuseDraw() && !recipe.sharedVertexData && recipe.vertexBytes().size() == 64));
        UNSCOPED_INFO("the smaller decoded prefix can be reused");
        REQUIRE((reuseDraw() && recipe.sharedVertexData));
        const auto prefix = recipe.sharedVertexData;
        vertices[35] ^= 1;
        UNSCOPED_INFO("unused source tails cannot affect the validated decoded prefix");
        REQUIRE((reuseDraw() && recipe.sharedVertexData == prefix));
        vertices[35] ^= 1;
        indices[5] = 2;
        UNSCOPED_INFO("restoring indices preserves the original complete decoded buffer");
        REQUIRE((reuseDraw() && std::ranges::equal(recipe.vertexBytes(), expectedVertices)));
        const auto previousFrame = recipe.sharedVertexData;
        geometry.BeginFrame();
        UNSCOPED_INFO("the next frame can reuse exact preceding source bytes");
        REQUIRE((reuseDraw() && recipe.sharedVertexData == previousFrame));
        geometry.BeginFrame();
        vertices[0] ^= 1;
        UNSCOPED_INFO("a new frame's source rewrite cannot reuse stale geometry");
        REQUIRE((reuseDraw() && !recipe.sharedVertexData));
        UNSCOPED_INFO("cross-frame source rewrite matches independent decoding");
        REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, target, uncached, error) &&
                 std::ranges::equal(recipe.vertexBytes(), uncached.vertexBytes())));
        geometry.BeginFrame();
        vertices[0] ^= 1;
        UNSCOPED_INFO("unused preceding cache versions are retired before a later frame");
        REQUIRE((reuseDraw() && !recipe.sharedVertexData && std::ranges::equal(recipe.vertexBytes(), expectedVertices)));
        UNSCOPED_INFO("retiring an unused cache entry preserves retained frame ownership");
        REQUIRE(std::ranges::equal(*previousFrame, expectedVertices));
    }
    for (auto* words : { &vsWords, &psWords })
    {
        (*words)[0] ^= 1;
        UNSCOPED_INFO("cached shader validation rejects in-place guest word changes");
        REQUIRE((!cachedDraw() && error.find("pinned interface") != std::string::npos));
        (*words)[0] ^= 1;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(cachedDraw());
    }
    for (auto* dxil : { &shader.vertexShaderDxil, &shader.pixelShaderDxil })
    {
        (*dxil)[0] ^= 1;
        UNSCOPED_INFO("cached shader validation rejects in-place DXIL changes");
        REQUIRE((!cachedDraw() && error.find("pinned interface") != std::string::npos));
        (*dxil)[0] ^= 1;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(cachedDraw());
    }
    for (auto* pin : { &shader.vertexGuestWordDigest, &shader.pixelGuestWordDigest, &shader.vertexDxilDigest, &shader.pixelDxilDigest })
    {
        const auto original = *pin;
        *pin                = "changed";
        UNSCOPED_INFO("cached shader validation rejects changed digest pins");
        REQUIRE((!cachedDraw() && error.find("pinned interface") != std::string::npos));
        *pin = original;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(cachedDraw());
    }
    const auto originalHash = shader.vertexShaderHash;
    shader.vertexShaderHash = 0;
    UNSCOPED_INFO("cached shader validation still requires a shader identity");
    REQUIRE((!cachedDraw() && error.find("bounded pinned DXIL") != std::string::npos));
    shader.vertexShaderHash                            = originalHash;
    const std::vector<std::uint32_t> memoryExportWords = { 0, 0xC600, 0 };
    view.vertexMicrocode                               = memoryExportWords;
    shader.vertexGuestWordDigest                       = wordDigest(memoryExportWords);
    UNSCOPED_INFO("new correctly pinned shader bytes still undergo memory-export validation");
    REQUIRE((!cachedDraw() && error.find("memory export") != std::string::npos));
    view.vertexMicrocode         = vsWords;
    shader.vertexGuestWordDigest = wordDigest(vsWords);
    UNSCOPED_INFO(error.c_str());
    REQUIRE(cachedDraw());
    shader.independentSamplers = false;
    UNSCOPED_INFO("cached shader bytes do not bypass draw interface validation");
    REQUIRE((!cachedDraw() && error.find("independent samplers") != std::string::npos));
    UNSCOPED_INFO("shared-sampler shader cannot consume per-texture sampler bindings");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error.find("independent samplers") != std::string::npos));
}

void multiFetchStripDraw()
{
    std::vector<std::uint32_t> registers(0x5003);
    commonState(registers, true);
    registers[0x2200] = 0x24F00776;
    registers[0x2201] = 0x00010001;
    registers[0x2202] = 0x87000004;
    registers[0x4800] = 0x3003;
    registers[0x4801] = 0x1A;
    registers[0x4802] = 0x4003;
    registers[0x4803] = 0x26;
    std::vector<std::uint8_t> positions(3 * 8);
    std::vector<std::uint8_t> normals(3 * 12);
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        storeBE32(positions, vertex * 8, std::bit_cast<std::uint32_t>(float(vertex)));
        storeBE32(positions, vertex * 8 + 4, std::bit_cast<std::uint32_t>(float(vertex + 1)));
        storeBE32(normals, vertex * 12, std::bit_cast<std::uint32_t>(1.0F));
        storeBE32(normals, vertex * 12 + 4, std::bit_cast<std::uint32_t>(0.0F));
        storeBE32(normals, vertex * 12 + 8, std::bit_cast<std::uint32_t>(0.0F));
    }
    std::vector<std::uint8_t>     indices = { 0, 0, 0, 1, 0, 2, 0, 2, 0, 1, 0, 0 };
    std::vector<std::uint32_t>    vsWords = { 9, 10, 11, 12 };
    std::vector<std::uint32_t>    psWords = { 13, 14, 15, 16 };
    auto                          shader  = shaderFor(vsWords,
                                                      psWords,
                                                      { { 0, 8, 0, 37, false, false }, { 1, 12, 0, 57, false, false } },
                                                      { 0 });
    NativeGameplayVertexFetchSpan positionFetch{ 0, 0x3000, positions };
    NativeGameplayVertexFetchSpan normalFetch{ 1, 0x4000, normals };
    NativeGameplayVertexFetchSpan fetches[] = { positionFetch, normalFetch };
    NativeDrawReplayTexture       texture;
    texture.width = texture.height = 1;
    texture.bytes                  = { 255, 128, 64, 255 };
    NativeDrawReplaySampler      sampler;
    NativeGameplayTextureBinding textureBinding{ 0, &texture, &sampler };
    NativeGameplayDrawView       view;
    view.registers       = registers;
    view.vertexMicrocode = vsWords;
    view.pixelMicrocode  = psWords;
    view.vertexFetches   = fetches;
    view.indexBytes      = indices;
    view.textureBindings = std::span<const NativeGameplayTextureBinding>(&textureBinding, 1);
    view.primitive       = 6;
    view.indexCount      = 6;
    view.indexFormat     = 0;
    view.indexEndian     = 1;
    NativeDrawReplayRecipe recipe;
    std::string            error;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("triangle strip expanded with alternating winding");
    REQUIRE((recipe.indices.size() == 12 && recipe.indices[0] == 0 && recipe.indices[3] == 2));
    UNSCOPED_INFO("right gameplay half, always depth compare and depth write preserved");
    REQUIRE((recipe.viewport.x == -640.0F && recipe.depth.writeEnabled && recipe.depth.compare == 8 &&
             !recipe.blend.alphaToCoverage));
    registers[0x2202] = 0x87000014;
    UNSCOPED_INFO("dithered guest alpha to mask is refused instead of host alpha to coverage");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error == "alpha_to_mask_unsupported"));
    registers[0x2202] = 0x87000004;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("ordered texture and per-texture sampler preserved");
    REQUIRE((recipe.textureMask == 1 && recipe.samplers.size() == 1));
    {
        // RB_BLENDCONTROL factors 4, 5, 8 and 9 name colors; in an alpha slot
        // they are the matching alpha factors, which D3D12 requires there.
        using Factor  = NativeDrawReplayBlendFactor;
        const auto at = [&](std::uint32_t blend)
        {
            registers[0x2201] = blend;
            NativeDrawReplayRecipe blended;
            std::string            reason;
            UNSCOPED_INFO(reason.c_str());
            REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, blended, reason));
            return blended.blend;
        };
        const auto multiply = at(0x00080008);
        UNSCOPED_INFO("the Multiply word keeps destination color and blends alpha by destination alpha");
        REQUIRE((multiply.sourceColor == Factor::DestinationColor && multiply.destinationColor == Factor::Zero &&
                 multiply.sourceAlpha == Factor::DestinationAlpha && multiply.destinationAlpha == Factor::Zero));
        const auto colors = at(0x09080504);
        UNSCOPED_INFO("color factors stay colors in the color slots and become alphas in the alpha slots");
        REQUIRE((colors.sourceColor == Factor::SourceColor && colors.destinationColor == Factor::InverseSourceColor &&
                 colors.sourceAlpha == Factor::DestinationAlpha && colors.destinationAlpha == Factor::InverseDestinationAlpha));
        const auto swapped = at(0x05040908);
        UNSCOPED_INFO("source color factors in the alpha slots become source alpha factors");
        REQUIRE((swapped.sourceColor == Factor::DestinationColor && swapped.destinationColor == Factor::InverseDestinationColor &&
                 swapped.sourceAlpha == Factor::SourceAlpha && swapped.destinationAlpha == Factor::InverseSourceAlpha));
        registers[0x2201] = 0x00010001;
    }
    {
        const bool copied = CopyNativeDrawReplayTextureVersion(texture, error);
        UNSCOPED_INFO(error.c_str());
        REQUIRE(copied);
        const auto                                       version = texture.textureVersion;
        const NativeGameplayDrawTarget                   target{ recipe.width, recipe.height, recipe.sampleCount, recipe.sampleMask, recipe.targetFormat, recipe.viewport, recipe.scissor };
        std::shared_ptr<const NativeDrawReplayAdmission> first, second;
        const bool                                       admitted = BuildNativeGameplayDrawAdmission(view, shader, target, first, error);
        UNSCOPED_INFO(error.c_str());
        REQUIRE(admitted);
        texture.swizzle   = { 2, 1, 0, 3 };
        sampler.magLinear = true;
        UNSCOPED_INFO("gameplay conversion preserves shared immutable resource identity and distinct admitted binding state");
        REQUIRE((BuildNativeGameplayDrawAdmission(view, shader, target, second, error) &&
                 first->Recipe().textures[0].textureVersion == version && second->Recipe().textures[0].textureVersion == version &&
                 first->TextureBindings()[0].swizzle == std::array<std::uint8_t, 4>{ 0, 1, 2, 3 } &&
                 second->TextureBindings()[0].swizzle == texture.swizzle &&
                 !first->TextureBindings()[0].sampler.magLinear && second->TextureBindings()[0].sampler.magLinear));
    }
    shader.alphaTestEnabled = true;
    shader.alphaTestCompare = 4;
    UNSCOPED_INFO("alpha specialization mismatch rejected");
    REQUIRE(!BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    shader.alphaTestEnabled                  = false;
    shader.vertexAttributes[0].fetchConstant = 96;
    UNSCOPED_INFO("out of range vertex fetch rejected before lookup");
    REQUIRE(!BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    shader.vertexAttributes[0].fetchConstant = 0;
    const auto validAttribute                = shader.vertexAttributes[0];
    for (const auto& invalid : std::array<NativeGameplayVertexAttribute, 4>{
             NativeGameplayVertexAttribute{ 0, 0, 0, 37, false, false },
             NativeGameplayVertexAttribute{ 0, 8, -1, 37, false, false },
             NativeGameplayVertexAttribute{ 0, 8, 4, 37, false, false },
             NativeGameplayVertexAttribute{ 2, 8, 0, 37, false, false } })
    {
        shader.vertexAttributes[0] = invalid;
        UNSCOPED_INFO("attribute preflight must reject zero strides, invalid offsets and absent fetches");
        REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
                 error.find("unsupported fetch layout") != std::string::npos));
    }
    shader.vertexAttributes[0] = validAttribute;
    fetches[1].bytes           = std::span<const std::uint8_t>(normals).first(normals.size() - 4);
    registers[0x4803]          = (static_cast<std::uint32_t>(fetches[1].bytes.size()) / 4U) << 2U | 2U;
    UNSCOPED_INFO("the last indexed attribute must fit even when preceding vertices are complete");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("shorter than its indexed attributes") != std::string::npos));
}

void topologyAndIndexState()
{
    std::vector<std::uint32_t> registers(0x5003);
    commonState(registers, false);
    registers[0x2200] = 0x24F00732;
    registers[0x2201] = 0x00010706;
    registers[0x2202] = 0x87000000;
    registers[0x4800] = 0x2003;
    registers[0x4801] = 0x4A;

    std::vector<std::uint8_t> vertices(6 * 12);
    for (std::size_t vertex = 0; vertex < 6; ++vertex)
    {
        storeBE32(vertices, vertex * 12, std::bit_cast<std::uint32_t>(float(vertex)));
        storeBE32(vertices, vertex * 12 + 4, std::bit_cast<std::uint32_t>(float(vertex + 1)));
        vertices[vertex * 12 + 8]  = static_cast<std::uint8_t>(vertex);
        vertices[vertex * 12 + 9]  = 0x40;
        vertices[vertex * 12 + 10] = 0x80;
        vertices[vertex * 12 + 11] = 0xFF;
    }
    std::vector<std::uint32_t>    vsWords = { 17, 18, 19, 20 };
    std::vector<std::uint32_t>    psWords = { 21, 22, 23, 24 };
    auto                          shader  = shaderFor(vsWords,
                                                      psWords,
                                                      { { 0, 12, 0, 37, false, false },
                                                        { 0, 12, 8, 6, false, true } });
    NativeGameplayVertexFetchSpan fetch{ 0, 0x2000, vertices };
    NativeGameplayDrawView        view;
    view.registers       = registers;
    view.vertexMicrocode = vsWords;
    view.pixelMicrocode  = psWords;
    view.vertexFetches   = std::span<const NativeGameplayVertexFetchSpan>(&fetch, 1);
    view.primitive       = 4;
    view.indexCount      = 3;
    view.indexFormat     = 1;
    view.indexEndian     = 2;
    NativeDrawReplayRecipe recipe;
    std::string            error;

    std::vector<std::uint8_t> indices32(12);
    storeBE32(indices32, 0, 0);
    storeBE32(indices32, 4, 1);
    storeBE32(indices32, 8, 2);
    view.indexBytes = indices32;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("observed 32-bit big-endian index list decoded");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>({ 0, 1, 2 }));

    view.indexed     = false;
    view.indexBytes  = {};
    view.indexFormat = 0;
    view.indexEndian = 1;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("nonindexed triangle list generated sequential indices");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>({ 0, 1, 2 }));

    view.primitive  = 6;
    view.indexCount = 4;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("nonindexed triangle strip expanded with alternating winding");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>({ 0, 1, 2, 2, 1, 3 }));
    // Primitive reset compares DMA indices only, so generated indices ignore it.
    registers[0x2205] |= 1U << 21;
    registers[0x2103] = 1;
    UNSCOPED_INFO("nonindexed strips ignore the primitive reset index");
    REQUIRE((BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             recipe.indices == std::vector<std::uint32_t>({ 0, 1, 2, 2, 1, 3 })));
    registers[0x2205] &= ~(1U << 21);
    registers[0x2103] = 0;

    view.indexed     = true;
    view.indexFormat = 0;
    view.indexEndian = 1;
    view.primitive   = 6;
    view.indexCount  = 7;
    registers[0x2205] |= 1U << 21;
    registers[0x2103] = 0xFFFF;
    registers[0x2100] = 5;
    std::vector<std::uint8_t> restartIndices(14);
    for (std::size_t index = 0; index < 7; ++index)
    {
        const auto value              = static_cast<std::uint16_t>(index == 3 ? 0xFFFF : index == 4 ? 3
                                                                                     : index == 5   ? 4
                                                                                     : index == 6   ? 5
                                                                                                    : index);
        restartIndices[index * 2]     = static_cast<std::uint8_t>(value >> 8);
        restartIndices[index * 2 + 1] = static_cast<std::uint8_t>(value);
    }
    view.indexBytes = restartIndices;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("triangle strip primitive restart splits observed 16-bit stream");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>({ 0, 1, 2, 3, 4, 5 }));

    // Restart must reset strip parity, including after a degenerate triangle
    // and an incomplete segment. Unused large indices cannot extend vertices.
    std::vector<std::uint8_t> boundaryIndices;
    const auto                setIndices = [&](std::initializer_list<std::uint16_t> values)
    {
        boundaryIndices.clear();
        for (const auto value : values)
        {
            boundaryIndices.push_back(static_cast<std::uint8_t>(value >> 8));
            boundaryIndices.push_back(static_cast<std::uint8_t>(value));
        }
        view.indexBytes = boundaryIndices;
        view.indexCount = static_cast<std::uint32_t>(values.size());
    };
    registers[0x2100] = 0xFFFFFF;
    setIndices({ 0xFFFF, 0xFFFE, 0xFFFF, 0, 1, 1, 2, 0xFFFF, 0xFFFF, 3, 4, 5, 0xFFFF, 0xFFFE });
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("strip restarts reset winding and ignore incomplete segment vertex ranges");
    REQUIRE((recipe.indices == std::vector<std::uint32_t>({ 0, 1, 1, 1, 1, 2, 3, 4, 5 }) &&
             recipe.vertexData.size() == 6 * recipe.vertexStrideBytes));
    // The SDK ignores primitive reset for lists; a list that reaches the reset
    // index is refused rather than dropping its triangle.
    view.primitive = 4;
    setIndices({ 0xFFFE, 0xFFFF, 0, 0, 1, 2, 0xFFFE, 0xFFFE });
    UNSCOPED_INFO("a list reaching the primitive reset index is refused");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) && error == "primitive_reset_in_list"));
    setIndices({ 0, 1, 2, 0xFFFE });
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("a list without the reset index ignores incomplete trailing indices");
    REQUIRE((recipe.indices == std::vector<std::uint32_t>({ 0, 1, 2 }) &&
             recipe.vertexData.size() == 3 * recipe.vertexStrideBytes));
    view.primitive    = 6;
    registers[0x2102] = 0xFFFFFF;
    registers[0x2101] = 1;
    registers[0x2100] = 4;
    setIndices({ 0, 1, 2, 0xFFFF, 3, 4 });
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("restart precedes 24-bit offset wrapping and min/max clamping");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>({ 4, 1, 1 }));
    registers[0x2101] = 0;

    registers[0x2205] &= ~(1U << 21);
    registers[0x2103]                       = 0;
    registers[0x2100]                       = 3;
    view.primitive                          = 4;
    view.indexCount                         = 3;
    std::vector<std::uint8_t> offsetIndices = { 0, 0, 0, 1, 0, 2 };
    view.indexBytes                         = offsetIndices;
    registers[0x2102]                       = 1;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("VGT index offset is applied before vertex bounds");
    REQUIRE(recipe.indices == std::vector<std::uint32_t>({ 1, 2, 3 }));

    registers[0x2000]        = 0x14000500;
    registers[0x2001]        = 0x00040000;
    registers[0x2002]        = 0;
    registers[0x2200]        = 0x24F00730;
    registers[0x2202]        = 0x87000007;
    registers[0x2205]        = 0x18002;
    registers[0x2082]        = 0x02D00500;
    registers[0x2100]        = 0xFFFFFF;
    registers[0x2102]        = 0;
    view.indexBytes          = indices32;
    view.indexCount          = 3;
    view.indexFormat         = 1;
    view.indexEndian         = 2;
    shader.colorTargetFormat = 4;
    shader.colorExpBias      = 0;
    shader.sampleCount       = 1;
    shader.sampleMask        = 1;
    NativeGameplayDrawTarget auxiliaryTarget;
    auxiliaryTarget.width       = 1280;
    auxiliaryTarget.height      = 720;
    auxiliaryTarget.sampleCount = 1;
    auxiliaryTarget.sampleMask  = 1;
    auxiliaryTarget.format      = NativeDrawReplayTargetFormat::Rg16Snorm;
    auxiliaryTarget.viewport    = { 0, 0, 1280, 720, 0, 1 };
    auxiliaryTarget.scissor     = { 0, 0, 1280, 720 };
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, auxiliaryTarget, recipe, error));
    UNSCOPED_INFO("full-pass auxiliary target preserves single-sample Rg16Snorm state");
    REQUIRE((recipe.width == 1280 && recipe.height == 720 && recipe.sampleCount == 1 &&
             recipe.sampleMask == 1 && recipe.targetFormat == NativeDrawReplayTargetFormat::Rg16Snorm &&
             recipe.viewport.x == 0.0F && recipe.scissor.right == 1280 && !recipe.depth.enabled));

    registers[0x2000]        = 0x0A010280;
    registers[0x2001]        = 0;
    registers[0x2002]        = 0x530;
    registers[0x2080]        = 0x7D80;
    registers[0x2081]        = 0x0270033E;
    registers[0x2082]        = 0x02D004A6;
    registers[0x210F]        = 0x43340000;
    registers[0x2110]        = 0x447C8000;
    registers[0x2111]        = 0xC2400000;
    registers[0x2112]        = 0x44280000;
    registers[0x2113]        = 0;
    registers[0x2114]        = 0;
    registers[0x2200]        = 0x24F00270;
    registers[0x2202]        = 0x87000004;
    registers[0x2205]        = 0x18000;
    view.indexed             = false;
    view.indexBytes          = {};
    view.indexCount          = 6;
    view.indexFormat         = 0;
    view.indexEndian         = 1;
    shader.colorTargetFormat = 0;
    shader.sampleCount       = 4;
    shader.sampleMask        = 9;
    NativeGameplayDrawTarget hudTarget;
    hudTarget.width       = 640;
    hudTarget.height      = 720;
    hudTarget.sampleCount = 4;
    hudTarget.sampleMask  = 9;
    hudTarget.format      = NativeDrawReplayTargetFormat::Rgba8;
    hudTarget.viewport    = { 190, 624, 360, 96, 0, 0 };
    hudTarget.scissor     = { 190, 624, 550, 720 };
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, hudTarget, recipe, error));
    UNSCOPED_INFO("full-pass HUD preserves translated viewport, zero depth range, and nonindexed geometry");
    REQUIRE((recipe.viewport.x == 190.0F && recipe.viewport.minDepth == 0.0F &&
             recipe.viewport.maxDepth == 0.0F && recipe.scissor.left == 190 &&
             recipe.scissor.right == 550 && !recipe.depth.enabled && recipe.indices.size() == 6));

    registers[0x2081] = registers[0x2082];
    hudTarget.scissor = {};
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, hudTarget, recipe, error));
    UNSCOPED_INFO("an empty HUD scissor remains empty without discarding the draw inputs");
    REQUIRE((recipe.scissor.left == recipe.scissor.right && recipe.scissor.top == recipe.scissor.bottom));

    registers[0x2081] = registers[0x2082] = 0;
    registers[0x210F] = registers[0x2110] = 0x438DC000;
    registers[0x2111]                     = 0xC3728000;
    registers[0x2112]                     = 0x43728000;
    hudTarget.viewport                    = { -640, 0, 567, 485, 0, 0 };
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, hudTarget, recipe, error));
    UNSCOPED_INFO("an offscreen empty HUD draw preserves its viewport and geometry with no rasterized pixels");
    REQUIRE((recipe.viewport.x == -640 && recipe.scissor.left == 0 && recipe.scissor.top == 0 &&
             recipe.scissor.right == 0 && recipe.scissor.bottom == 0 && recipe.indices.size() == 6));
    registers[0x2082] = 1U | (1U << 16);
    UNSCOPED_INFO("nonempty offscreen HUD windows still require supported scissor bounds");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, hudTarget, recipe, error) &&
             error.find("exceeds the screen scissor") != std::string::npos));

    registers[0x2000]        = 0x0A010280;
    registers[0x2001]        = 0;
    registers[0x2002]        = 0x530;
    registers[0x2200]        = 0x24F00732;
    registers[0x2202]        = 0x87000000;
    registers[0x2080]        = 0;
    registers[0x2081]        = 0;
    registers[0x2082]        = 0x02D00280;
    registers[0x210F]        = 0x44200000;
    registers[0x2110]        = 0x44200000;
    registers[0x2111]        = 0xC3B40000;
    registers[0x2112]        = 0x43B40000;
    registers[0x2113]        = 0x3F800000;
    registers[0x2114]        = 0;
    registers[0x2102]        = 1;
    view.indexed             = true;
    view.indexBytes          = offsetIndices;
    view.indexCount          = 3;
    view.indexFormat         = 0;
    view.indexEndian         = 1;
    shader.colorTargetFormat = 0;
    shader.sampleCount       = 4;
    shader.sampleMask        = 9;

    view.indexEndian = 2;
    UNSCOPED_INFO("mismatched 16-bit index endian is rejected");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("indexed stream") != std::string::npos));
    view.indexEndian = 1;
    view.primitive   = 8;
    UNSCOPED_INFO("clear and trigger primitive types are rejected as draws");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("primitive topology") != std::string::npos));

    view.primitive    = 4;
    registers[0x2200] = 0x24F18271;
    registers[0x210D] = 0x00FFFF01;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("HUD stencil equality and increment-wrap preserve the guest operations");
    REQUIRE((recipe.stencil.enabled && recipe.stencil.reference == 1 &&
             recipe.stencil.readMask == 255 && recipe.stencil.writeMask == 255 &&
             recipe.stencil.front.compare == 3 && recipe.stencil.front.pass == 7 &&
             recipe.stencil.front.fail == 1 && !recipe.depth.enabled));
    registers[0x2200] |= 0x80U;
    registers[0x210C] = 0x00FFFF02;
    UNSCOPED_INFO("unequal per-face stencil references are rejected");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("stencil") != std::string::npos));
    registers[0x2200] = 0x24F00776;
    registers[0x2208] = 5;
    registers[0x2104] = 0xFFFF;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, recipe, error));
    UNSCOPED_INFO("depth-only geometry suppresses color writes without discarding its draw");
    REQUIRE((recipe.blend.writeMask == 0 && recipe.depth.enabled && recipe.depth.writeEnabled));
    registers[0x2208] = 4;
    registers[0x2104] = 0xF;
    registers[0x2001] = 3U << 16;
    UNSCOPED_INFO("unknown color target format is rejected");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("color target") != std::string::npos));

    registers[0x2001] = 0;
    registers[0x2100] = 0xFFFFFF;
    registers[0x2102] = 0;
    view.indexed      = true;
    view.indexFormat  = 1;
    view.indexEndian  = 2;
    view.indexCount   = 3;
    std::vector<std::uint8_t> oversizedIndices(12);
    storeBE32(oversizedIndices, 0, 0xFFFFFF);
    storeBE32(oversizedIndices, 4, 0xFFFFFF);
    storeBE32(oversizedIndices, 8, 0xFFFFFF);
    view.indexBytes = oversizedIndices;
    UNSCOPED_INFO("large indexed vertex range is rejected before allocation");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("decoded gameplay vertices") != std::string::npos));

    const std::vector<std::uint32_t> clauseWords = { 0x1001, 0x2000, 0, 0, 0xC600, 0 };
    view.vertexMicrocode                         = clauseWords;
    shader.vertexGuestWordDigest                 = wordDigest(clauseWords);
    UNSCOPED_INFO("ALU clause data is not interpreted as control-flow memory export");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("decoded gameplay vertices") != std::string::npos));

    const std::vector<std::uint32_t> memoryExportWords = { 0, 0xC600, 0 };
    view.vertexMicrocode                               = memoryExportWords;
    shader.vertexGuestWordDigest                       = wordDigest(memoryExportWords);
    UNSCOPED_INFO("SDK control-flow memory export is rejected");
    REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, recipe, error) &&
             error.find("memory export") != std::string::npos));
}

// Observable fields refuse by name; SDK-ignored fields keep the recipe intact.
void strictStateMutations()
{
    std::vector<std::uint32_t> registers(0x5003);
    commonState(registers, false);
    registers[0x2200] = 0x24F00732;
    registers[0x2201] = 0x00010706;
    registers[0x2202] = 0x87000000;
    registers[0x4800] = 0x2003;
    registers[0x4801] = 0x26;
    std::vector<std::uint8_t> vertices(3 * 12);
    for (std::size_t vertex = 0; vertex < 3; ++vertex)
    {
        storeBE32(vertices, vertex * 12, std::bit_cast<std::uint32_t>(float(vertex)));
        storeBE32(vertices, vertex * 12 + 4, std::bit_cast<std::uint32_t>(float(vertex + 2)));
        storeBE32(vertices, vertex * 12 + 8, 0x10204080U);
    }
    std::vector<std::uint8_t>     indices = { 0, 0, 0, 1, 0, 2 };
    std::vector<std::uint32_t>    vsWords = { 25, 26, 27, 28 };
    std::vector<std::uint32_t>    psWords = { 29, 30, 31, 32 };
    auto                          shader  = shaderFor(vsWords, psWords, { { 0, 12, 0, 37, false, false }, { 0, 12, 8, 6, false, true } });
    NativeGameplayVertexFetchSpan fetch{ 0, 0x2000, vertices };
    NativeGameplayDrawView        view;
    view.registers       = registers;
    view.vertexMicrocode = vsWords;
    view.pixelMicrocode  = psWords;
    view.vertexFetches   = std::span<const NativeGameplayVertexFetchSpan>(&fetch, 1);
    view.indexBytes      = indices;
    view.primitive       = 4;
    view.indexCount      = 3;
    NativeDrawReplayRecipe baseline, recipe;
    std::string            error;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, baseline, error));
    const auto build = [&]()
    {
        return BuildNativeGameplayDrawRecipe(view, shader, recipe, error);
    };
    const auto unchanged = [&]()
    {
        const auto& a = recipe;
        const auto& b = baseline;
        return build() && a.indices == b.indices && a.vertexData == b.vertexData && a.vertexConstants == b.vertexConstants &&
               a.pixelConstants == b.pixelConstants && a.sharedConstants == b.sharedConstants && a.blend == b.blend &&
               a.depth.enabled == b.depth.enabled && a.depth.writeEnabled == b.depth.writeEnabled && a.depth.compare == b.depth.compare &&
               a.stencil.enabled == b.stencil.enabled && a.rasterizer.cull == b.rasterizer.cull &&
               a.rasterizer.frontCounterClockwise == b.rasterizer.frontCounterClockwise &&
               a.rasterizer.depthClip == b.rasterizer.depthClip && a.sampleCount == b.sampleCount &&
               a.sampleMask == b.sampleMask && a.targetFormat == b.targetFormat && a.viewport.x == b.viewport.x &&
               a.viewport.width == b.viewport.width && a.scissor.right == b.scissor.right && a.scissor.bottom == b.scissor.bottom;
    };

    struct Mutation
    {
        std::uint32_t    index, value;
        std::string_view refusal;
    };

    // Fields refused by name or by an existing state contract message.
    for (const auto& mutation : std::initializer_list<Mutation>{
             { 0x2202, 0x87000010, "alpha_to_mask_unsupported" },
             { 0x2293, 0x81, "viz_query_kills_pixels" },
             { 0x2201, 0x000C0706, "blend factor" },
             { 0x2201, 0x0001070C, "blend factor" },
             { 0x2201, 0x00012706, "color-mask" },
             { 0x2104, 0xFF, "color-mask" },
             { 0x2202, 0x87000020, "alpha-to-coverage offsets" },
             { 0x2202, 0x87000100, "alpha-to-coverage offsets" },
             { 0x2202, 0x86000000, "alpha-to-coverage offsets" },
             { 0x2205, 0x18802, "polygon or rasterizer" },
             { 0x2205, 0x19002, "polygon or rasterizer" },
             { 0x2205, 0x1A002, "polygon or rasterizer" },
             { 0x2205, 0x1800A, "polygon or rasterizer" },
             { 0x2205, 0x18022, "polygon or rasterizer" },
             { 0x2205, 0x98002, "polygon or rasterizer" },
             { 0x2205, 0x118002, "polygon or rasterizer" },
             { 0x2205, 0x818002, "polygon or rasterizer" },
             { 0x2205, 0x1018002, "polygon or rasterizer" },
             { 0x2205, 0x10002, "polygon or rasterizer" },
             { 0x2205, 0x08002, "polygon or rasterizer" },
             { 0x2205, 0x18003, "simultaneous front and back" },
             { 0x2200, 0x24F0073A, "early depth" },
             { 0x2002, 0x10530, "depth target format" },
             { 0x2204, 0x80001, "user_clip_plane_unpinned" },
             { 0x2204, 0x80003, "0x2204" },
             { 0x2204, 0xC0000, "0x2204" },
             { 0x2204, 0x84000, "0x2204" },
             { 0x2204, 0x00000, "0x2204" },
             { 0x2206, 0x03F, "0x2206" },
             { 0x2206, 0x13F, "0x2206" },
             { 0x2302, 5, "0x2302" },
             { 0x2302, 0, "0x2302" },
             { 0x2307, 0xFE000, "0x2307" },
             { 0x2308, 0xFF000, "0x2308" },
             { 0x2208, 0, "color/depth mode" },
             { 0x2208, 6, "color/depth mode" },
             { 0x2000, 0x0A020280, "sample encoding" },
             { 0x2001, 0x00030000, "color target format" },
             { 0x2001, 0x00100000, "color target format" },
         })
    {
        const auto original       = registers[mutation.index];
        registers[mutation.index] = mutation.value;
        UNSCOPED_INFO(std::string(mutation.refusal).c_str());
        REQUIRE((!build() && error.find(mutation.refusal) != std::string::npos));
        registers[mutation.index] = original;
    }

    // Fields the SDK draw path never reads, or that only matter for
    // primitives, modes or targets already refused above.
    for (const auto& [index, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
             { 0x2312, 0x1 },        // PA_SC_AA_MASK
             { 0x2301, 0x7F },       // PA_SC_AA_CONFIG
             { 0x2203, 0xFFFFFFFF }, // RB_HIZCONTROL
             { 0x2183, 0xFFFF },     // SQ_WRAPPING_0
             { 0x2184, 0xFFFF },     // SQ_WRAPPING_1
             { 0x2209, 0x0C0C0C0C }, // RB_BLENDCONTROL1..3 with RT1..3 unwritten
             { 0x220B, 0x0C0C0C0C },
             { 0x2003, 0x000F0000 }, // RB_COLOR1..3_INFO
             { 0x2005, 0x000F0000 },
             { 0x2105, 0x3F800000 }, // RB_BLEND_RED..ALPHA with no constant factors
             { 0x2108, 0x3F800000 },
             { 0x2109, 0x3F800000 }, // RB_FOG_COLOR
             { 0x210C, 0x00FFFF01 }, // RB_STENCILREFMASK_BF and RB_STENCILREFMASK with stencil off
             { 0x210D, 0x00FFFF01 },
             { 0x210E, 0x3F000000 }, // RB_ALPHA_REF with alpha test off
             { 0x2280, 0x00100010 }, // PA_SU_POINT_SIZE
             { 0x2281, 0xFFFF0000 }, // PA_SU_POINT_MINMAX
             { 0x2282, 0x8 },        // PA_SU_LINE_CNTL
             { 0x2283, 0xFFFF },     // PA_SC_LINE_STIPPLE
             { 0x2300, 0x1 },        // PA_SC_LINE_CNTL
             { 0x2384, 0x3F800000 }, // PA_CL_POINT_*
             { 0x2380, 0x3F800000 }, // PA_SU_POLY_OFFSET_* with offsets disabled
             { 0x2383, 0x3F800000 },
             { 0x2388, 0x3F800000 }, // PA_CL_UCP_0 with user clip planes disabled
             { 0x2303, 0x3F800000 }, // PA_CL_GB_* guard band
             { 0x2306, 0x3F800000 },
             { 0x2292, 0x80000001 }, // PA_SC_MPASS_PS_CNTL
             { 0x2293, 0x01 },       // PA_SC_VIZ_QUERY counting only
             { 0x2284, 1 },          // VGT_OUTPUT_PATH_CNTL in the implicit major mode
             { 0x21FC, 0x100 },      // explicit major mode without tessellation
             { 0x2285, 2 },          // VGT_HOS_CNTL
             { 0x2316, 0xE },        // VGT_VERTEX_REUSE_BLOCK_CNTL
             { 0x2317, 0x10 },       // VGT_OUT_DEALLOC_CNTL
             { 0x2318, 0x100 },      // RB_COPY_* and clear values are resolve state
             { 0x231E, 0xFFFFFFFF },
             { 0x2324, 1 },          // RB_SAMPLE_COUNT_CTL
             { 0x2207, 0xFF },       // VGT_CURRENT_BIN_ID_MIN
             { 0x2006, 0x1000 },     // COHER_DEST_BASE_0
             { 0x2000, 0x00010280 }, // RB_SURFACE_INFO.hiz_pitch
             { 0x4908, 0x00FF0000 }, // loop constants; the raw translator refuses loops and calls
         })
    {
        const auto original = registers[index];
        registers[index]    = value;
        UNSCOPED_INFO("an SDK-ignored field leaves the recipe unchanged");
        REQUIRE(unchanged());
        registers[index] = original;
    }

    // Stencil compare functions and operations map one-to-one to D3D12.
    registers[0x2200] = 0x24F00731;
    for (std::uint32_t value = 0; value < 8; ++value)
    {
        registers[0x2200] = (0x24F00731U & ~(0xFFFU << 8)) | (value << 8) | (value << 11) | (value << 14) | (((value + 1) & 7U) << 17);
        UNSCOPED_INFO("Xenos stencil compare and operation encodings map to D3D12 by adding one");
        REQUIRE((build() && recipe.stencil.front.compare == value + 1 && recipe.stencil.front.fail == value + 1 &&
                 recipe.stencil.front.pass == value + 1 && recipe.stencil.front.depthFail == ((value + 1) & 7U) + 1 &&
                 recipe.stencil.back.compare == recipe.stencil.front.compare && recipe.stencil.back.pass == recipe.stencil.front.pass));
    }
    registers[0x2200] = 0x24F00732;

    // Tessellation applies only in the explicit major mode.
    registers[0x2284] = 1;
    registers[0x21FC] = 0x100;
    UNSCOPED_INFO("explicit-mode tessellation is refused");
    REQUIRE((!build() && error == "tessellation_unsupported"));
    registers[0x2284] = registers[0x21FC] = 0;

    // Only the single position export is translated.
    for (std::uint32_t mode = 1; mode < 8; ++mode)
    {
        registers[0x2180] = shader.shaderProgram = mode << 24;
        UNSCOPED_INFO("non-position vertex export modes are refused");
        REQUIRE((!build() && error == "vertex_export_mode_unsupported"));
    }
    registers[0x2180] = shader.shaderProgram = 0;
    UNSCOPED_INFO("the position-only export remains admitted");
    REQUIRE(unchanged());

    // PA_CL_CLIP_CNTL.clip_disable turns off host depth clipping, as the SDK
    // pipeline cache does; guest clipping keeps it on.
    UNSCOPED_INFO("guest clipping keeps host depth clipping");
    REQUIRE((build() && recipe.rasterizer.depthClip));
    registers[0x2204]          = 0x90000;
    registers[0x2206]          = 0x400;
    shader.screenSpacePosition = true;
    UNSCOPED_INFO("clip_disable turns off host depth clipping");
    REQUIRE((build() && !recipe.rasterizer.depthClip));
    // Without the viewport transform the SDK ignores PA_CL_VPORT_*, so the
    // screen-space VS needs the 1280x720 viewport and 0..1 depth range.
    for (const auto& [index, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
             { 0x2113, 0x3F000000 }, // ZSCALE 0.5
             { 0x2110, 0x43A00000 }, // XOFFSET 320
         })
    {
        const auto original = registers[index];
        registers[index]    = value;
        NativeGameplayDrawTarget target;
        NativeDrawReplayScissor  scissor;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(DecodeNativeGameplayViewportAndScissor(registers, target.viewport, scissor, error));
        UNSCOPED_INFO("a screen-space draw refuses a viewport the SDK would ignore");
        REQUIRE((!BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error) && error == "screen_space_viewport"));
        shader.screenSpacePosition = false;
        registers[0x2204]          = 0x80000;
        registers[0x2206]          = 0x43F;
        UNSCOPED_INFO("the same viewport stays admitted with the viewport transform");
        REQUIRE(BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error));
        shader.screenSpacePosition = true;
        registers[0x2204]          = 0x90000;
        registers[0x2206]          = 0x400;
        registers[index]           = original;
    }
    // A one-pixel window offset cancelled by XOFFSET keeps the decoded
    // viewport canonical, but the SDK still shifts the vertices by it.
    registers[0x2081] = 0x80000000;
    for (const auto [window, xOffset] : { std::pair<std::uint32_t, float>{ 0, 640.0F }, { 1, 639.0F } })
    {
        registers[0x2080] = window;
        registers[0x2110] = std::bit_cast<std::uint32_t>(xOffset);
        NativeGameplayDrawTarget target;
        UNSCOPED_INFO(error.c_str());
        REQUIRE(DecodeNativeGameplayViewportAndScissor(registers, target.viewport, target.scissor, error));
        const bool admitted = BuildNativeGameplayDrawRecipe(view, shader, target, recipe, error);
        UNSCOPED_INFO("a screen-space draw refuses exactly a nonzero window offset");
        REQUIRE((window ? !admitted && error == "screen_space_window_offset" : admitted));
    }
    registers[0x2080]          = 0;
    registers[0x2081]          = 0;
    registers[0x2110]          = std::bit_cast<std::uint32_t>(640.0F);
    registers[0x2204]          = 0x80000;
    registers[0x2206]          = 0x43F;
    shader.screenSpacePosition = false;
    UNSCOPED_INFO("the clipped state remains admitted");
    REQUIRE(unchanged());

    // Depth-only mode runs no pixel shader in the SDK.
    registers[0x2208]       = 5;
    registers[0x2202]       = 0x8700000C;
    shader.alphaTestEnabled = true;
    shader.alphaTestCompare = 4;
    UNSCOPED_INFO("depth-only alpha testing is refused");
    REQUIRE((!build() && error == "depth_only_alpha_test"));
    registers[0x2202]       = 0x8700000F;
    shader.alphaTestCompare = 7;
    UNSCOPED_INFO(error.c_str());
    REQUIRE(build());
    registers[0x2202]       = 0x87000000;
    shader.alphaTestEnabled = false;
    shader.alphaTestCompare = 0;
    for (const auto killWord : { std::pair<std::uint32_t, std::uint32_t>{ 0, 0x18000000 }, { 0x8C000000, 0 } })
    {
        const std::vector<std::uint32_t> kill = { 0x1001, 0x2000, 0, killWord.first, 0, killWord.second };
        view.pixelMicrocode                   = kill;
        shader.pixelGuestWordDigest           = wordDigest(kill);
        UNSCOPED_INFO("a depth-only pixel shader with a kill is refused");
        REQUIRE((!build() && error == "depth_only_pixel_kill"));
        registers[0x2208] = 4;
        UNSCOPED_INFO("a color draw keeps its pixel kill");
        REQUIRE(build());
        registers[0x2208] = 5;
    }
    const std::vector<std::uint32_t> noKill = { 0x1001, 0x2000, 0, 0, 0, 0 };
    view.pixelMicrocode                     = noKill;
    shader.pixelGuestWordDigest             = wordDigest(noKill);
    UNSCOPED_INFO("a depth-only pixel shader without kills stays admitted");
    REQUIRE(build());
}

} // namespace

TEST_CASE("native_gameplay_draw", "[ctest][native-gameplay-draw]")
{
    static_assert(!std::is_default_constructible_v<NativeDrawReplayShaderVersion>);
    static_assert(!std::is_copy_constructible_v<NativeDrawReplayShaderVersion>);
    static_assert(!std::is_constructible_v<NativeDrawReplayShaderVersion,
                                           std::shared_ptr<const std::vector<std::uint8_t>>,
                                           std::shared_ptr<const std::vector<std::uint8_t>>>);
    NativeGameplayShaderInterface interpolation;
    interpolation.shaderInterpolators = 0x00010003;
    UNSCOPED_INFO("centroids-only draws ignore the sampling pattern");
    REQUIRE(NativeGameplayInterpolatorsMatch(interpolation, 0xFFFF0003));
    UNSCOPED_INFO("unproved shaders retain all parameter shade bits");
    REQUIRE(!NativeGameplayInterpolatorsMatch(interpolation, 1));
    interpolation.pixelShaderHash      = 0xA4A965C189287B99ULL;
    interpolation.pixelGuestWordDigest = "f73f655ea80c22bde4bc93575f87664e81056b6f4714bef3a4defaa18994d18a";
    interpolation.sampleCount          = 1;
    UNSCOPED_INFO("the captured one-sample color shader ignores unused interpolators");
    REQUIRE(NativeGameplayInterpolatorsMatch(interpolation, 1));
    UNSCOPED_INFO("the consumed parameter shade bit remains significant");
    REQUIRE(!NativeGameplayInterpolatorsMatch(interpolation, 0));
    interpolation.sampleCount   = 4;
    interpolation.shaderContext = 8;
    UNSCOPED_INFO("mixed multisampling preserves the consumed sampling position");
    REQUIRE(!NativeGameplayInterpolatorsMatch(interpolation, 1));
    UNSCOPED_INFO("unused mixed sampling positions do not create new specializations");
    REQUIRE(NativeGameplayInterpolatorsMatch(interpolation, 0xFFFF0001));
    interpolation.generatedPixelParameters = true;
    UNSCOPED_INFO("generated parameters cannot use the ordinary color shader proof");
    REQUIRE(!NativeGameplayInterpolatorsMatch(interpolation, 0x00010001));
    interpolation.generatedPixelParameters = false;
    interpolation.pixelGuestWordDigest.clear();
    UNSCOPED_INFO("a hash without matching bytes cannot relax the input interface");
    REQUIRE(!NativeGameplayInterpolatorsMatch(interpolation, 0x00010001));
    {
        // A pixel-less entry reads no interpolators, but SQ_CONTEXT_MISC
        // control 3 stays refused.
        NativeGameplayShaderInterface pixelLess;
        pixelLess.depthOnly           = true;
        pixelLess.shaderInterpolators = 0x00010003;
        UNSCOPED_INFO("a pixel-less entry ignores SQ_INTERPOLATOR_CNTL");
        REQUIRE((NativeGameplayInterpolatorsMatch(pixelLess, 0x003F003F) && NativeGameplayInterpolatorsMatch(pixelLess, 0)));
        pixelLess.shaderContext = 12;
        UNSCOPED_INFO("a pixel-less entry keeps the control 3 refusal");
        REQUIRE(!NativeGameplayInterpolatorsMatch(pixelLess, 0x00010003));
    }
    depthOnlyDraw();
    terrainLikeDraw();
    multiFetchStripDraw();
    topologyAndIndexState();
    strictStateMutations();
}
