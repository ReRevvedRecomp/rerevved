#include "gpu/d3d12/native_draw_replay.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{

using namespace rerevved::gpu;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "native_draw_replay_test: " << message << '\n';
        std::exit(1);
    }
}

NativeDrawReplayRecipe makeRecipe()
{
    NativeDrawReplayRecipe recipe;
    recipe.schemaVersion = 2;
    recipe.width = recipe.height = 4;
    recipe.sampleCount           = 4;
    recipe.sampleMask            = 9;
    recipe.vertexShaderHash      = 1;
    recipe.pixelShaderHash       = 2;
    // These tests exercise recipe admission only. D3D12 validates executable
    // shader containers when the replay creates its pipeline state.
    recipe.vertexShaderDxil.resize(4);
    recipe.pixelShaderDxil.resize(4);
    recipe.vertexAttributeCount = 3;
    recipe.vertexStrideBytes    = 48;
    recipe.vertexData.resize(3 * recipe.vertexStrideBytes);
    recipe.indices    = { 0, 1, 2 };
    recipe.indexCount = 3;
    recipe.vertexConstants.resize(256 * 16);
    recipe.pixelConstants.resize(224 * 16);
    recipe.sharedConstants.resize(336);
    recipe.initialSample0.resize(64);
    recipe.initialSample1.resize(64);
    recipe.viewport          = { 0.0F, 0.0F, 4.0F, 4.0F, 0.0F, 1.0F };
    recipe.scissor           = { 0, 0, 4, 4 };
    recipe.textureMask       = 1;
    recipe.textures[0].width = recipe.textures[0].height = 4;
    recipe.textures[0].bytes.resize(64);
    return recipe;
}

} // namespace

int main()
{
    std::string error;
    auto        recipe = makeRecipe();
    require(ValidateNativeDrawReplayRecipe(recipe, error), "textured float4 input admission");
    recipe.initialSample0.clear();
    recipe.initialSample1.clear();
    require(ValidateNativeDrawReplayRecipe(recipe, error), "schema two native clear admission");
    recipe.initialSample0.resize(64);
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "partial captured color initialization rejected");
    recipe            = makeRecipe();
    recipe.sampleMask = 15;
    require(ValidateNativeDrawReplayRecipe(recipe, error), "schema two full raster coverage admission");
    recipe.sampleMask = 3;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "unsupported sample layout rejection");
    recipe              = makeRecipe();
    recipe.targetFormat = NativeDrawReplayTargetFormat::Rgb10a2;
    require(ValidateNativeDrawReplayRecipe(recipe, error), "RGB10 output admission");
    recipe.depth.enabled = true;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "mixed packed depth and RGB10 output rejection");
    recipe               = makeRecipe();
    recipe.depth.enabled = recipe.depth.writeEnabled = true;
    recipe.rasterizer.cull                           = 2;
    recipe.depth.initialSamples.resize(128);
    require(ValidateNativeDrawReplayRecipe(recipe, error), "packed D24 depth input admission");
    recipe.depth.initialSamples[0] = 1;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "uncaptured stencil semantics rejection");
    recipe.depth.initialSamples[0] = 0;
    recipe.depth.initialSamples.pop_back();
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "incomplete depth plane rejection");
    recipe.depth.initialSamples.clear();
    recipe.depth.initialClear = std::numeric_limits<float>::quiet_NaN();
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "non-finite depth clear rejection");
    recipe.depth.initialClear = 1.0F;
    recipe.depth.compare      = 9;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "invalid depth comparison rejection");
    recipe.depth.compare = 4;
    recipe.depth.enabled = false;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "depth writes without testing rejection");
    for (auto format : { NativeDrawReplayTextureFormat::Rgba8,
                         NativeDrawReplayTextureFormat::R8,
                         NativeDrawReplayTextureFormat::Bc1,
                         NativeDrawReplayTextureFormat::Bc2 })
    {
        recipe                    = makeRecipe();
        recipe.textures[0].format = format;
        recipe.textures[0].bytes.resize(format == NativeDrawReplayTextureFormat::Rgba8 ? 64 : format == NativeDrawReplayTextureFormat::Bc1 ? 8
                                                                                                                                           : 16);
        require(ValidateNativeDrawReplayRecipe(recipe, error), "texture format payload admission");
        recipe.textures[0].bytes.pop_back();
        require(!ValidateNativeDrawReplayRecipe(recipe, error), "truncated texture rejection");
    }
    recipe                     = makeRecipe();
    recipe.textures[0].swizzle = { 5, 5, 5, 0 };
    require(ValidateNativeDrawReplayRecipe(recipe, error), "constant component mapping admission");
    recipe.textures[0].swizzle[0] = 6;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "invalid component selector rejection");
    recipe                    = makeRecipe();
    recipe.sampler.address[0] = 0;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "foreign sampler enum rejection");
    recipe                   = makeRecipe();
    recipe.sampler.border[0] = std::numeric_limits<float>::quiet_NaN();
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "non-finite sampler rejection");
    recipe                   = makeRecipe();
    recipe.vertexStrideBytes = 32;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "attribute stride mismatch rejection");
    recipe             = makeRecipe();
    recipe.textureMask = 2;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "unbound texture slot rejection");
    recipe             = makeRecipe();
    recipe.textureMask = 7;
    recipe.textures[1] = recipe.textures[0];
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "missing third movie plane rejected");
    recipe.textures[2] = recipe.textures[0];
    require(ValidateNativeDrawReplayRecipe(recipe, error), "three complete texture planes admitted");
    recipe.textures[2].bytes.pop_back();
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "truncated third plane rejected");
    recipe.textureMask = 1;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "unused plane payload rejected");
    recipe               = makeRecipe();
    recipe.schemaVersion = 1;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "schema one subset remains restricted");
    recipe.textureMask = 0;
    recipe.textures[0].bytes.clear();
    recipe.vertexShaderHash     = 0x11213E38D7154104ULL;
    recipe.pixelShaderHash      = 0x3A92D78FE55C7B83ULL;
    recipe.vertexAttributeCount = 2;
    recipe.vertexStrideBytes    = 32;
    recipe.vertexData.resize(96);
    require(ValidateNativeDrawReplayRecipe(recipe, error), "schema one panel remains admitted");
    recipe.sampleMask = 15;
    require(!ValidateNativeDrawReplayRecipe(recipe, error), "schema one sample mask remains restricted");
    std::cout << "native_draw_replay_test: PASS\n";
}
