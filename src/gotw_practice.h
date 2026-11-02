#pragma once

#include "gotw_settings.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rerevved::gotw
{

// Gameplay sidecar block (schema 3); nonstandard rules record only the rules bit.
struct PracticeBlock
{
    bool     standardRules      = false;
    bool     customWeek         = false;
    bool     customCivilization = false;
    uint32_t week               = 0;
    uint32_t seed               = 0;
    uint32_t civilization       = 0;
    int32_t  dominationMinimum  = 10; // Earliest Domination: 5 or 10.

    bool operator==(const PracticeBlock&) const = default;
};

// Flags, week and seed words (big endian), civilization and domination bytes.
inline constexpr size_t kPracticeBlockSize = 1 + 4 + 4 + 1 + 1;

bool IsValid(const PracticeBlock& block) noexcept;
void AppendPracticeBlock(std::vector<uint8_t>& out, const PracticeBlock& block);
// Reads one block at offset and advances it; empty for a short, noncanonical or invalid block.
std::optional<PracticeBlock> ReadPracticeBlock(std::span<const uint8_t> bytes, size_t& offset) noexcept;

// Empty without valid practice policy and nonzero seed; the caller supplies civilization and domination.
std::optional<PracticeBlock> CurrentPracticeBlock(uint32_t effectiveCivilization, int32_t dominationMinimum) noexcept;
// Isolation-only marker so a resaved practice or isolated game stays isolated on later loads.
bool NeedsIsolationMarker(bool hasBlock) noexcept;
// Latch saved practice policy; call ResetForLoad first.
void      RestorePractice(const PracticeBlock& block) noexcept;
Selection SelectionFor(const PracticeBlock& block) noexcept;

enum class LoadKind
{
    Ordinary, // No practice block, claimed or present, in the sidecar.
    Practice,
    Isolated, // A practice block is present, claimed or rejected, but cannot resume.
};

// Setup modes: 1 and 2 are local, 3 is a load; mode 0, multiplayer 4 to 6 and others never resume.
constexpr bool IsSinglePlayerSetupMode(int32_t mode) noexcept
{
    return mode >= 1 && mode <= 3;
}

// Resume only on the single-player owner writer path with a matching week; any other block or schema 3 claim isolates the load.
LoadKind ClassifyLoad(uint32_t nativeWeek, const std::optional<PracticeBlock>& saved, bool sidecarClaimsPractice, bool resumable) noexcept;

} // namespace rerevved::gotw
