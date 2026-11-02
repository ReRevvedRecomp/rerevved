#pragma once

// Retail movies and loading tips contain both controller families.
// Map requested Xbox names to their PlayStation counterparts.

#include <cstdint>
#include <optional>
#include <string_view>

#include <rex/input/device.h>

namespace rerevved::button_icons
{

enum class Family
{
    kXbox,
    kPlayStation,
    kKeyboard,
};

// "auto" uses Keyboard without a pad, PlayStation for that pad, otherwise Xbox; unknown settings default to Xbox.
constexpr Family ParseFamily(std::string_view          setting,
                             rex::input::GamepadFamily detected         = rex::input::GamepadFamily::kOther,
                             bool                      gamepadConnected = true) noexcept
{
    if (setting == "keyboard")
    {
        return Family::kKeyboard;
    }
    if (setting == "playstation")
    {
        return Family::kPlayStation;
    }
    if (setting == "auto")
    {
        if (!gamepadConnected)
        {
            return Family::kKeyboard;
        }
        return detected == rex::input::GamepadFamily::kPlayStation ? Family::kPlayStation : Family::kXbox;
    }
    return Family::kXbox;
}

// False when PlayStation is impossible, letting hooks skip guest reads.
bool MaySwap();

// Latch after SDK window attachment so "auto" sees the launch pad; fixed until restart because tips build once.
Family ActiveFamily();

// Match .dds basenames at 0x821E3018 case-insensitively; return the native
// PlayStation stem. Only equal sizes swap: the 2:1 and 1:2 shoulder and trigger
// images are left to the label and tip swaps.
std::optional<std::string_view> ImageStem(std::string_view basename) noexcept;

// Whole-string control labels built by the narrow-string constructor at
// 0x82D3A6C8. Matching is exact and case-sensitive.
std::optional<std::string_view> Label(std::string_view label) noexcept;

// Loading-tip HTML image names passed to the tip builder at 0x82E81F58.
struct TipImage
{
    std::string_view name;
    // The Xbox shoulder and trigger boxes are 2:1 or 1:2; the PlayStation
    // images are square, so the box shrinks to its smaller side.
    bool square = false;
};

std::optional<TipImage> Tip(std::string_view name) noexcept;

constexpr int32_t SquareSide(int32_t width, int32_t height) noexcept
{
    return width < height ? width : height;
}

} // namespace rerevved::button_icons
