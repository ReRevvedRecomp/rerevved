#pragma once

#include "solid_draw_adapter.h"

#include <cstdint>
#include <string>

namespace rerevved::gpu::guest_d3d
{

// Consumed roots skip fetch patch 0x826ADD88; host reconstructs execution words
// while guest memory keeps previous patch.
enum class VsCodeState : std::uint8_t
{
    Fresh,      // the words in guest memory equal the expected backing patch
    Stale,      // they differ: the guest's last patch was for another state
    Unreadable, // the expected backing patch or the words as read could not be derived
};

struct VsCodeReading
{
    VsCodeState state = VsCodeState::Unreadable;
    std::string readDigest, hostDigest, error;
};

// The VS bound in the device shadow at `device`, with stream 0's stride in bytes.
VsCodeReading ReadVsCodeState(std::uint32_t device, std::uint32_t strideBytes, const ReadMemory& read);

// Both 0x826ADC48/0x826ADA38 copy G+0x30F0..0x30FF to G+0x2E38..0x2E47; only
// 0x826ADC48 also writes VS+48+416*variant. Key byte 0 is entry stride in
// dwords, set first by wrappers.
struct VsKeyReading
{
    bool        readable      = false;
    bool        cacheDiffers  = false; // G+0x2E38..0x2E47 is not the key
    bool        recordDiffers = false; // the variant record's key at VS+48+416*variant is not the key
    std::string error;
};

VsKeyReading ReadVsKeyState(std::uint32_t device, std::uint32_t strideBytes, const ReadMemory& read);

} // namespace rerevved::gpu::guest_d3d
