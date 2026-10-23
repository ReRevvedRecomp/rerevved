#pragma once

#include "gpu/guest_d3d/resolve_surface_registry.h"

#include <cstdint>
#include <functional>
#include <span>

namespace rerevved::gpu::guest_d3d
{

// Reads guest virtual memory at an address, false when it is not readable.
using ResolveSurfaceGuestRead = std::function<bool(std::uint32_t address, std::span<std::uint8_t> bytes)>;

// Lock 0x82697CA0 reads flags at stack+84(recomp.137:12775); unlock 0x82697F38
// keys texture/buffer(level+0x18). Without 0x10, writable lock ends ranges at
// final unlock; 0x10 address view(RVA-F-0254). Outside writes end at once.
void ObserveResolveSurfaceLock(ResolveSurfaceRegistry& surfaces, std::uint32_t object, std::uint32_t stack, const ResolveSurfaceGuestRead& read);

// resource_mirror_hooks.h entries include EndTiling 0x8269D320 and free
// 0x8280DEC8, plus lock/unlock cores.
void ObserveResolveSurfaceLock(std::uint32_t object, std::uint32_t stack) noexcept;
void ObserveResolveSurfaceUnlock(std::uint32_t object) noexcept;
void ObserveResolveSurfaceTiling() noexcept;
void ObserveResolveSurfaceFree(std::uint32_t address, std::uint32_t extent) noexcept;

} // namespace rerevved::gpu::guest_d3d
