#pragma once

#include "native_menu_draw.h"

namespace rerevved::gpu
{

struct NativeGuestMenuDraw
{
    // Owned by the caller for this call. State and shader words are guest BE.
    std::span<const std::uint8_t>  state, vertexMicrocode, pixelMicrocode;
    std::span<const std::uint8_t>  vertices, indices;
    const NativeDrawReplayTexture* texture   = nullptr;
    std::uint32_t                  primitive = 0, minimumVertex = 0, vertexCount = 0;
    std::uint32_t                  indexCount = 0, indexFormat = 0, stride = 0;
    bool                           indexed = true;
};

// The admitted indexed GFx and nonindexed label pairs use at most one texture. Shader
// identity, fixed state, and the full viewport must match the supported path.
bool BuildNativeGuestMenuDrawRecipe(const NativeGuestMenuDraw& draw,
                                    const NativeMenuShaders&   shaders,
                                    NativeDrawReplayRecipe&    recipe,
                                    std::string&               error);

} // namespace rerevved::gpu
