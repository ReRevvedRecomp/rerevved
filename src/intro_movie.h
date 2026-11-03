#pragma once

// skip_intro_movie takes the guest's no-movie branch in the shared frontend entry, for the intro state only.

#include <cstdint>

namespace rerevved::intro_movie
{

// Object table factory 0x821CABC8 stores for IntroMovie.bik; AttractMovie shares the entry with table 0x82121320.
constexpr uint32_t kIntroStateVtable = 0x82131A18;

} // namespace rerevved::intro_movie
