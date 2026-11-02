#include "options_glyphs.h"

#include <array>

namespace rerevved::options_glyphs
{

namespace
{

using Family = button_icons::Family;

constexpr GlyphFile retail(std::string_view name)
{
    return { Source::kRetailFpk, name };
}

constexpr GlyphFile keyboard(std::string_view name)
{
    return { Source::kKeyboardArt, name };
}

constexpr std::array<std::array<GlyphFile, kButtonCount>, 3> kFiles{ {
    { { retail("a.dds"), retail("b.dds"), retail("lb.dds"), retail("rb.dds"), retail("lt.dds"), retail("rt.dds"), retail("x.dds"), retail("y.dds") } },
    { { retail("ps3x.dds"), retail("ps3circle.dds"), retail("ps3l1.dds"), retail("ps3r1.dds"), retail("ps3l2.dds"), retail("ps3r2.dds"), retail("ps3square.dds"), retail("ps3triangle.dds") } },
    { { keyboard("Return.dds"), keyboard("Escape.dds"), keyboard("PageUp.dds"), keyboard("PageDown.dds"), keyboard("Q.dds"), keyboard("E.dds"), keyboard("X.dds"), keyboard("Y.dds") } },
} };

} // namespace

GlyphFile FileFor(Family family, Button button)
{
    return kFiles[static_cast<size_t>(family)][static_cast<size_t>(button)];
}

float WidthForHeight(uint32_t imageWidth, uint32_t imageHeight, float height)
{
    if (imageWidth == 0 || imageHeight == 0)
        return 0.0f;
    return height * static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
}

float PromptWidth(uint32_t imageWidth, uint32_t imageHeight, float height, float fallbackWidth)
{
    const float width = WidthForHeight(imageWidth, imageHeight, height);
    return width > 0.0f ? width : fallbackWidth;
}

} // namespace rerevved::options_glyphs
