#pragma once

#include <filesystem>
#include <string>

#include "gpu/d3d12/native_render_frame.h"

namespace rerevved::gpu
{

// Package is the shader catalog directory for the admitted rgba8-32-tiled form.
bool LoadPresentationFixture(const std::filesystem::path& package, NativeRenderFrame& frame, std::string& error);

} // namespace rerevved::gpu
