#pragma once

#include "solid_draw_adapter.h"

#include <cstdint>
#include <string>

namespace rerevved::gpu::guest_d3d
{

// Digest VS after PS link/G+0x2E2C stream 0 fetch patch, independent of stale
// guest words. No declaration/records leaves code as read. Adapter links
// solid_draw_adapter.cpp fold/patch.
bool HostPatchedVertexDigest(std::uint32_t device, std::uint32_t stride, const ReadMemory& read, std::string& digest, std::string& error);

} // namespace rerevved::gpu::guest_d3d
