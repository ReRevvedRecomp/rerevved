#pragma once

// Controller keybind defaults use the main key of SDK pairs ("Semicolon,Space") so the keycap shows;
// the title sets them before config loads.

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

inline constexpr std::array<Default, 6> kDefaults{ {
    { "keybind_a", "Semicolon,Space", "Semicolon" },
    { "keybind_b", "Quote,Backspace", "Quote" },
    { "keybind_left_trigger", "Q,I", "Q" },
    { "keybind_right_trigger", "E,O", "E" },
    { "keybind_back", "Z,Tab", "Z" },
    { "keybind_start", "X,Return", "X" },
} };

// Set each key's default to `value` before config, environment and command-line overrides; returns the count set.
size_t Apply(std::span<const Default> defaults = kDefaults);

} // namespace rerevved::keybind_defaults
