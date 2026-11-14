#pragma once

// Keyboard defaults follow PC strategy conventions for this game's button roles (Enter confirms,
// Space ends the turn, B founds a city); the title sets them before config loads.
// The first key of a pair is the keycap shown.

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace rerevved::keybind_defaults
{

struct Default
{
    std::string_view key;
    std::string_view sdkDefault;
    std::string_view value;
};

inline constexpr std::array<Default, 14> kDefaults{ {
    { "keybind_a", "Semicolon,Space", "Return" },
    { "keybind_b", "Quote,Backspace", "Space,Backspace" },
    { "keybind_x", "L", "F" },
    { "keybind_y", "P", "B" },
    { "keybind_left_shoulder", "1", "Q" },
    { "keybind_right_shoulder", "3", "E" },
    // A key press crosses the game's 0.8 trigger threshold once, which toggles the zoom.
    { "keybind_left_trigger", "Q,I", "Z,PageDown" },
    { "keybind_right_trigger", "E,O", "C" },
    { "keybind_lstick_press", "F", "T" },
    { "keybind_rstick_press", "K", "Delete" },
    { "keybind_dpad_left", "Shift+Left", "Comma,Shift+Left" },
    { "keybind_dpad_right", "Shift+Right", "Period,Shift+Right" },
    { "keybind_back", "Z,Tab", "Tab" },
    // Escape stays off B, which ends the turn on the world map.
    { "keybind_start", "X,Return", "Escape" },
} };

// Set each key's default to `value` before config, environment and command-line overrides; returns the count set.
size_t Apply(std::span<const Default> defaults = kDefaults);

} // namespace rerevved::keybind_defaults
