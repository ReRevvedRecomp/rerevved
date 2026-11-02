#pragma once

#include <cstdint>
#include <string_view>

#include "button_icons.h"

namespace rerevved::options_glyphs
{

enum class Button
{
    kA,
    kB,
    kLB,
    kRB,
    kLT,
    kRT,
    kX,
    kY,
};

inline constexpr int kButtonCount = 8;

enum class Source
{
    kRetailFpk,
    kKeyboardArt,
};

struct GlyphFile
{
    Source           source;
    std::string_view name;
};

// Keyboard names are SDK key names matching shipped art, the keys the page binds.
GlyphFile FileFor(button_icons::Family family, Button button);

// Aspect-preserving width at height; a zero dimension gives 0.
float WidthForHeight(uint32_t imageWidth, uint32_t imageHeight, float height);

float PromptWidth(uint32_t imageWidth, uint32_t imageHeight, float height, float fallbackWidth);

} // namespace rerevved::options_glyphs
