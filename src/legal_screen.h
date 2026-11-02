#pragma once

// Skip Intro (skip_intro_movie) completes the legal-screen dwell via update 0x82DEE860.

#include <cstdint>

namespace rerevved::legal_screen
{

// Constructor 0x82DEE920 stores this table for the GFX_LegalScreen.gfx owner.
constexpr uint32_t kLegalScreenVtable = 0x82126EF4;

// Byte the update sets after it dispatches main-menu preload 0x82E7EA60.
constexpr uint32_t kPreloadStartedOffset = 0x434;

constexpr uint32_t kLegalScreenDwell = 5000;

} // namespace rerevved::legal_screen
