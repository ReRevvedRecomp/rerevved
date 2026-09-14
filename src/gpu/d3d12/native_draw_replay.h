#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rerevved::gpu
{

enum class NativeDrawReplayBlendFactor : std::uint8_t
{
    Zero,
    One,
    SourceColor,
    InverseSourceColor,
    SourceAlpha,
    InverseSourceAlpha,
    DestinationAlpha,
    InverseDestinationAlpha,
    DestinationColor,
    InverseDestinationColor,
    SourceAlphaSaturated,
    BlendFactor,
    InverseBlendFactor,
};

enum class NativeDrawReplayBlendOp : std::uint8_t
{
    Add,
    Subtract,
    ReverseSubtract,
    Minimum,
    Maximum,
};

struct NativeDrawReplayViewport
{
    float x        = 0.0F;
    float y        = 0.0F;
    float width    = 0.0F;
    float height   = 0.0F;
    float minDepth = 0.0F;
    float maxDepth = 1.0F;
};

struct NativeDrawReplayScissor
{
    std::int32_t left   = 0;
    std::int32_t top    = 0;
    std::int32_t right  = 0;
    std::int32_t bottom = 0;
};

struct NativeDrawReplayBlendState
{
    bool                        enabled          = false;
    bool                        alphaToCoverage  = false;
    bool                        logicOpEnabled   = false;
    NativeDrawReplayBlendFactor sourceColor      = NativeDrawReplayBlendFactor::One;
    NativeDrawReplayBlendFactor destinationColor = NativeDrawReplayBlendFactor::Zero;
    NativeDrawReplayBlendOp     colorOp          = NativeDrawReplayBlendOp::Add;
    NativeDrawReplayBlendFactor sourceAlpha      = NativeDrawReplayBlendFactor::One;
    NativeDrawReplayBlendFactor destinationAlpha = NativeDrawReplayBlendFactor::Zero;
    NativeDrawReplayBlendOp     alphaOp          = NativeDrawReplayBlendOp::Add;
    std::uint8_t                writeMask        = 0x0F;
};

enum class NativeDrawReplayTextureFormat : std::uint8_t
{
    Rgba8,
    R8,
    Bc1,
    Bc2,
};

struct NativeDrawReplayTexture
{
    std::vector<std::uint8_t>     bytes;
    std::uint32_t                 width  = 0;
    std::uint32_t                 height = 0;
    NativeDrawReplayTextureFormat format = NativeDrawReplayTextureFormat::Rgba8;
    // Component selectors: RGBA=0..3, constant zero=4, constant one=5.
    std::array<std::uint8_t, 4> swizzle = { 0, 1, 2, 3 };
};

struct NativeDrawReplaySampler
{
    bool minLinear = false;
    bool magLinear = false;
    bool mipLinear = false;
    // D3D texture address modes: wrap, mirror, clamp, border, mirror-once.
    std::array<std::uint32_t, 3> address = { 3, 3, 3 };
    std::array<float, 4>         border  = {};
    float                        minLod  = 0.0F;
    float                        maxLod  = 0.0F;
    float                        mipBias = 0.0F;
};

struct NativeDrawReplayDepthState
{
    bool enabled      = false;
    bool writeEnabled = false;
    // D3D comparison values: never=1 through always=8.
    std::uint32_t compare      = 4;
    float         initialClear = 1.0F;
    // Optional two planes of little-endian (depth24 << 8), with zero stencil.
    std::vector<std::uint8_t> initialSamples;
};

struct NativeDrawReplayRasterizer
{
    // none=0, front=1, back=2.
    std::uint32_t cull                  = 0;
    bool          frontCounterClockwise = false;
};

enum class NativeDrawReplayTargetFormat : std::uint8_t
{
    Rgba8,
    Rgb10a2,
};

// The recipe is deliberately made of host-independent bytes and scalar state.
// All byte files are little-endian and are bounded by LoadNativeDrawReplayRecipe.
struct NativeDrawReplayRecipe
{
    std::uint32_t         schemaVersion = 1;
    std::filesystem::path sourcePath;
    // The caller supplies a fresh output path before replay.
    std::filesystem::path outputPath;

    std::vector<std::uint8_t>  vertexShaderDxil;
    std::vector<std::uint8_t>  pixelShaderDxil;
    std::vector<std::uint8_t>  vertexData;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint8_t>  vertexConstants;
    std::vector<std::uint8_t>  pixelConstants;
    std::vector<std::uint8_t>  sharedConstants;
    std::vector<std::uint8_t>  initialSample0;
    std::vector<std::uint8_t>  initialSample1;

    std::array<std::uint8_t, 4>  clearColor = { 0, 0, 0, 255 };
    NativeDrawReplayViewport     viewport;
    NativeDrawReplayScissor      scissor;
    NativeDrawReplayBlendState   blend;
    NativeDrawReplayTexture      texture;
    NativeDrawReplaySampler      sampler;
    NativeDrawReplayDepthState   depth;
    NativeDrawReplayRasterizer   rasterizer;
    NativeDrawReplayTargetFormat targetFormat = NativeDrawReplayTargetFormat::Rgba8;

    std::uint32_t width                = 0;
    std::uint32_t height               = 0;
    std::uint32_t sampleCount          = 0;
    std::uint32_t sampleMask           = 0;
    std::uint32_t vertexStrideBytes    = 0;
    std::uint32_t vertexAttributeCount = 0;
    std::uint32_t indexCount           = 0;
    std::uint32_t textureMask          = 0;
    std::uint64_t vertexShaderHash     = 0;
    std::uint64_t pixelShaderHash      = 0;
};

struct NativeDrawReplayResult
{
    bool                  success = false;
    std::filesystem::path outputPath;
    std::string           error;
};

bool LoadNativeDrawReplayRecipe(const std::filesystem::path& recipePath,
                                NativeDrawReplayRecipe&      recipe,
                                std::string&                 error);

bool ValidateNativeDrawReplayRecipe(const NativeDrawReplayRecipe& recipe,
                                    std::string&                  error);

} // namespace rerevved::gpu
