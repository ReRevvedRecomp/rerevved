#pragma once

// Guest layout and gate for the local Resume repair in network_pause_hooks.cpp,
// kept free of guest memory so it can be tested on the host.

#include <cstdint>

namespace rerevved::network_pause
{

// Frontend root pointer. Its word at +4 holds the pause state in bit 0x800.
constexpr uint32_t kFrontendRootGlobal = 0x82FFD624;
constexpr uint32_t kRootFlagsOffset    = 4;
constexpr uint32_t kPausedFlag         = 0x800;

// Message field the native producer initializes before sub_82E2A998 runs. A
// resume (r4 = 0) never reads it.
constexpr uint32_t kResumeLocalArgument = 0xFFFFFFFF;

// Apply any participant's resume locally when this is a network game and the root
// still reports the pause. A zero request byte means resume, nonzero means pause.
constexpr bool ShouldApplyLocalResume(uint8_t networkByte, uint8_t requestByte, uint32_t root, uint32_t rootFlags) noexcept
{
    return networkByte != 0 && requestByte == 0 && root != 0 && (rootFlags & kPausedFlag) != 0;
}

} // namespace rerevved::network_pause
