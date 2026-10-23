#pragma once

#include "native_gameplay_draw.h"

namespace rerevved::gpu
{

// The compiled artifact a load reads from each catalog entry. Schema 1
// catalogs hold DXIL only; schema 2 entries may hold both formats.
enum class NativeShaderFormat
{
    Dxil,
    Spirv,
};

// Loads a directory's shaders.toml (schema 1 or 2). Only the requested
// format's bytes and digests are filled; an entry without that format's
// artifact refuses the whole catalog as "shader_format_missing: <entry>".
bool LoadNativeGameplayShaders(const std::filesystem::path&                directory,
                               std::vector<NativeGameplayShaderInterface>& shaders,
                               std::string&                                error,
                               NativeShaderFormat                          format = NativeShaderFormat::Dxil);

// depth-only.toml schema 1/2 supplies vertex-only entries, absent file means
// none. Pin VS/hash/word/artifact/SQ/context/interpolator identity,
// mode_control5, color_mask0; attributes come from each draw's vertex fetches.
bool LoadNativeDepthOnlyShaders(const std::filesystem::path&                directory,
                                std::vector<NativeGameplayShaderInterface>& shaders,
                                std::string&                                error,
                                NativeShaderFormat                          format = NativeShaderFormat::Dxil);

} // namespace rerevved::gpu
