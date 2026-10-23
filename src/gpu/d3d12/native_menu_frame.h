#pragma once

#include "native_frame_replay.h"
#include "native_menu_draw.h"

namespace rerevved::gpu
{

struct NativeMenuFrameEventView
{
    enum class Kind
    {
        Draw,
        Copy,
        Swap
    };
    Kind               kind                = Kind::Draw;
    std::uint64_t      id                  = 0;
    bool               success             = false;
    bool               hostIssued          = false;
    bool               resolve             = false;
    std::uint32_t      primitiveType       = 0;
    std::uint64_t      hostPixelShaderHash = 0;
    std::uint64_t      frontbuffer         = 0;
    std::uint32_t      frontbufferWidth    = 0;
    std::uint32_t      frontbufferHeight   = 0;
    NativeMenuDrawView draw;
};

// Image ordering only; the original consumer retains guest query, memory and
// presentation side effects while the native frame is compared.
bool BuildNativeMenuFrameRecipe(std::span<const NativeMenuFrameEventView> events,
                                const NativeMenuShaders&                  shaders,
                                std::span<const std::uint32_t>            gamma,
                                NativeFrameReplayRecipe&                  recipe,
                                std::string&                              error);

} // namespace rerevved::gpu
