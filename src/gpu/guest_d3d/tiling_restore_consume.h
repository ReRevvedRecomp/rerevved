#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

struct TilingRestoreInputs
{
    std::array<std::uint32_t, 6> saved{};
    std::uint8_t                 valid = 0;
};

struct TilingRestoreWrite
{
    std::uint32_t address = 0, value = 0;
    bool          operator==(const TilingRestoreWrite&) const = default;
};

struct TilingRestorePlan
{
    std::array<TilingRestoreWrite, 7> writes{};
    std::array<std::uint64_t, 7>      registers{}; // r5 through r11
};

TilingRestorePlan DeriveTilingRestore(std::uint32_t device, const TilingRestoreInputs& inputs) noexcept;

enum class TilingRestoreFallback : std::uint8_t
{
    Stubs,
    Device,
    Diagnostics,
    ExitCheck,
    Unreadable,
    Unwritable,
    Mapping,
    Alias
};
inline constexpr std::array<std::string_view, 8> kTilingRestoreFallbackNames{
    "stubs", "device", "diagnostics", "exit_check", "unreadable", "unwritable", "mapping", "alias"
};

struct TilingRestoreMemory
{
    std::function<bool(std::uint32_t, std::uint32_t&)>                        read;
    std::function<bool(std::uint32_t, std::uint32_t)>                         writable;
    std::function<std::optional<std::uint64_t>(std::uint32_t, std::uint32_t)> identity;
    std::function<bool(const TilingRestoreWrite&)>                            write;
};

struct TilingRestoreState
{
    bool requested = false, checking = false, stubs = false, device = false, diagnostics = false;
};

bool                                 TilingRestoreConsumeRequested() noexcept;
bool                                 TilingRestoreExitCheckRequested() noexcept;
bool                                 TilingRestoreReportingRequested() noexcept;
TilingRestoreInputs                  ReadTilingRestoreInputs(const TilingRestoreMemory& memory, std::uint32_t device);
std::optional<TilingRestoreFallback> PreflightTilingRestore(const TilingRestoreState& state, const TilingRestoreMemory& memory, std::uint32_t device, TilingRestorePlan& plan);
bool                                 ConsumeTilingRestoreWith(const TilingRestoreState& state, const TilingRestoreMemory& memory, PPCContext& context) noexcept;
bool                                 ConsumeTilingRestore(PPCContext& context, bool diagnostics) noexcept;

struct TilingRestoreCounts
{
    std::uint64_t                enter = 0, consumed = 0, original = 0;
    std::array<std::uint64_t, 8> fallback{};
    std::uint64_t                exitChecked = 0, exitMismatch = 0, exitAbiMismatch = 0, exitUnreadable = 0;
    std::uint64_t                exitIndeterminate = 0, exitUnmatched = 0, eventDrop = 0, replayFault = 0;
};

void NoteTilingRestoreEnter() noexcept;
void NoteTilingRestoreOriginal() noexcept;
// Drains interval counts, never lifetime totals or active checker scopes.
TilingRestoreCounts TakeTilingRestoreCounts(bool shutdown = false) noexcept;
TilingRestoreCounts TilingRestoreLifetimeCounts() noexcept;
std::string         FormatTilingRestoreCounts(const TilingRestoreCounts& counts);
std::string         ReportTilingRestore(bool shutdown = false) noexcept;

class TilingRestoreExitScope
{
public:
    explicit TilingRestoreExitScope(const PPCContext& context) noexcept;
    TilingRestoreExitScope(const PPCContext& context, const TilingRestoreMemory& memory, bool enabled) noexcept;
    ~TilingRestoreExitScope();
    TilingRestoreExitScope(const TilingRestoreExitScope&)            = delete;
    TilingRestoreExitScope& operator=(const TilingRestoreExitScope&) = delete;

private:
    struct Entry;
    std::unique_ptr<Entry> entry;
};

} // namespace rerevved::gpu::guest_d3d
