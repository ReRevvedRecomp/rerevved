#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "gpu/d3d12/native_guest_menu_draw.h"

namespace rerevved::gpu::diagnostics
{

// Captures owned CPU inputs to the original menu scene, GFx and label draws. The
// caller creates an arm file only after reaching the intended game state.
using NativeGuestDrawConsumer  = std::function<bool(const NativeGuestMenuDraw&,
                                                    const std::filesystem::path&,
                                                    std::string&)>;
using NativeGuestFrameConsumer = std::function<bool(const std::filesystem::path&,
                                                    const std::array<std::uint32_t, 256>&,
                                                    bool,
                                                    std::string&)>;
// The converted guest table contains three 256-entry big-endian halfword
// channels, left-aligned to ten bits. Output is packed B10G10R10X2.
bool DecodeNativeGuestGammaTable(std::span<const std::uint8_t>   bytes,
                                 std::array<std::uint32_t, 256>& table,
                                 std::string&                    error);

class NativeGuestGammaEmission
{
public:
    // Captures a complete producer table while keeping it pending until the
    // original gamma emitter returns.
    bool Begin(std::uint32_t                 graphics,
               std::uint32_t                 address,
               std::span<const std::uint8_t> bytes,
               std::string&                  error);
    void Complete() noexcept;
    // A pending or mismatched emission is never usable at a frame boundary.
    bool Matches(std::uint32_t                         graphics,
                 const std::array<std::uint32_t, 256>& producerTable,
                 std::span<const std::uint8_t>         producerBytes,
                 std::string&                          error) const;

    bool                                  Pending() const noexcept;
    std::uint64_t                         Sequence() const noexcept;
    std::uint32_t                         Graphics() const noexcept;
    std::uint32_t                         Address() const noexcept;
    const std::array<std::uint32_t, 256>& Table() const noexcept;
    std::span<const std::uint8_t>         Bytes() const noexcept;

private:
    bool                           pending         = false;
    std::uint32_t                  pendingGraphics = 0, pendingAddress = 0;
    std::array<std::uint32_t, 256> pendingTable{};
    std::vector<std::uint8_t>      pendingBytes;
    std::uint64_t                  sequence = 0;
    std::uint32_t                  graphics = 0, address = 0;
    std::array<std::uint32_t, 256> table{};
    std::vector<std::uint8_t>      bytes;
};

bool StartNativeGuestDrawCapture(const std::filesystem::path& directory, std::string& error, NativeGuestDrawConsumer consumer = {}, NativeGuestFrameConsumer frameConsumer = {});
// Called before VdSwap at 0x826A4884; records a CPU submission interval, not GPU
// completion. Draws must use this device and finish before its next boundary.
void NotifyNativeGuestFrameBoundary(std::uint32_t graphics, std::uint32_t reservation, std::uint32_t descriptor);
void StopNativeGuestDrawCapture();

} // namespace rerevved::gpu::diagnostics
