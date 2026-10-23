#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rerevved::gpu::diagnostics
{

// What opens a frame reference window. Any trigger may open window 1; the first to fire wins.
struct XenosFrameReferenceOptions
{
    // The VdSwap boundary count that opens window 1; 0 leaves it to the other triggers.
    std::uint32_t armFrame = 0;
    // An 8-64 hex-digit pixel-word-digest prefix selects the first matching GPU draw; window 1
    // binds armDelay+1 swaps after its frame regardless of queued guest work. Owns the exclusive
    // SDK frame observer.
    std::string   pixelDigest;
    std::uint32_t armDelay = 0;
    // 1..16 sequential windows. Window 1 writes the directory and also opens when <directory>/arm
    // exists. Window k >= 2 opens when <directory>/arm-<k> exists and writes <directory>/window-<k>.
    std::uint32_t windows = 1;
};

// Trigger thread reserves SDK mailbox: guest VdSwap for file/arm-frame triggers, command processor
// for digest. Worker only waits/saves, so delay cannot move frame. Save input/ snapshot then
// reference.toml last with boundary/status/span.
bool StartXenosFrameReference(const std::filesystem::path& directory, const XenosFrameReferenceOptions& options, std::string& error);
// Window 1 at a nonzero arm frame only.
bool StartXenosFrameReference(const std::filesystem::path& directory, std::uint32_t armFrame, std::string& error);
void NotifyXenosFrameReferenceBoundary() noexcept;
void StopXenosFrameReference();

} // namespace rerevved::gpu::diagnostics
