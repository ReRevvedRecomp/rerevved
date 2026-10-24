#pragma once

// Selection and arithmetic for the gameplay fixes in gameplay_fixes_hooks.cpp,
// kept free of guest memory so it can be tested on the host.

#include <cstdint>

namespace rerevved::gameplay_fixes
{

// sub_82CECCD8 stores technologies at ordinals 0..N-1 and -1 at N, retaining N+1;
// starting the loop at ordinal 0 scores the first candidate, not the sentinel.
constexpr uint32_t kAiResearchFirstOrdinal = 0;

constexpr uint32_t AiResearchScanStart(uint32_t listBase) noexcept
{
    return listBase + kAiResearchFirstOrdinal * 4;
}

// Native trip count of the scoring loop; the N=0 case branches past it.
constexpr uint32_t AiResearchScanCount(int32_t retained) noexcept
{
    return retained > 1 ? static_cast<uint32_t>(retained - 1) : 0;
}

// The spaceship launch dialog returns a signed arrival year, or -1 when
// cancelled or incomplete. Year zero and BC years are valid arrivals.
constexpr bool AdmitsSpaceshipArrivalYear(int32_t dialogResult) noexcept
{
    return dialogResult != -1;
}

} // namespace rerevved::gameplay_fixes
