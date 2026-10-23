#pragma once

#include "indexed_buffer_consume.h"
#include "solid_draw_adapter.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace rerevved::gpu::guest_d3d
{

// Skipped fetch patch reads only(vs_code_probe.h). Write watch lacks read-fault
// providers:loads never fault. Counts bound exposure, not readers. Key-read
// failure preserves word outcome, key_unreadable only.
inline constexpr std::size_t kVsWatchLogLimit = 8; // stale shaders logged

struct VsCodeWatchCounts
{
    std::uint64_t consumed     = 0; // dirty vertex fetch consumes examined
    std::uint64_t stale        = 0; // VS backing differs from the expected fetch patch, before inline linkage
    std::uint64_t fresh        = 0; // equal
    std::uint64_t unreadable   = 0; // not derivable
    std::uint64_t staleShaders = 0; // distinct VS objects seen stale since the probe started, up to kVsWatchShaderCap
    // The patch's key cache (vs_code_probe.h), of the consumes whose words were readable:
    std::uint64_t keyCacheStale  = 0; // G+0x2E38..0x2E47 is not the key the skipped patch would have stored
    std::uint64_t keyRecordStale = 0; // the VS variant record's key is not that key
    std::uint64_t keyUnreadable  = 0; // the key cache could not be read
    std::uint64_t bytes          = 0; // guest payload copied by diagnostic reads
};

inline constexpr std::size_t kVsWatchShaderCap = 256;

// The cvar native_vs_code_watch.
bool VsCodeWatchRequested() noexcept;

// Installs the observer of consumed dirty vertex fetch draws, reading guest memory
// through `read`, when the cvar is set. The reader must be safe to call from a
// guest thread at a draw entry.
void InstallVsCodeWatch(ReadMemory read);
// Removes it.
void RemoveVsCodeWatch() noexcept;

// Returns the counts since the previous take and resets them (the shader count
// is cumulative).
VsCodeWatchCounts TakeVsCodeWatchCounts() noexcept;
// " vs_watch_consumed=N ... vs_watch_stale_shaders=N vs_watch_key_cache_stale=N vs_watch_key_record_stale=N vs_watch_key_unreadable=N vs_watch_bytes=N".
std::string FormatVsCodeWatchCounts(const VsCodeWatchCounts& counts);

} // namespace rerevved::gpu::guest_d3d
