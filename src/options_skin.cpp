#include "options_skin.h"

#include <algorithm>

namespace rerevved::options_skin
{

std::optional<ArtFile> FileFor(button_icons::Family family, Art art)
{
    switch (art)
    {
        case Art::kMap:
            return ArtFile{ "Common0.FPK", "gfx_mapbackground_512x256.dds" };
        case Art::kBar:
            return ArtFile{ "Pregame.FPK", "bardarkblue.dds" };
        case Art::kGoldArrow:
            return ArtFile{ "Pregame.FPK", "goldarrow.dds" };
        case Art::kLs:
            if (family == button_icons::Family::kXbox)
                return ArtFile{ "Pregame.FPK", "ls.dds" };
            if (family == button_icons::Family::kPlayStation)
                return ArtFile{ "Pregame.FPK", "ps3ls.dds" };
            return std::nullopt;
    }
    return std::nullopt;
}

ScrollArrows ArrowsFor(float scrollY, float scrollMax)
{
    // A pixel of slack keeps a fractional scroll from flickering an arrow.
    constexpr float kSlack = 1.0f;
    return { scrollY > kSlack, scrollY < scrollMax - kSlack };
}

float StepScroll(float scrollY, float scrollMax, float rowHeight, int direction)
{
    if (scrollMax <= 0.0f)
        return 0.0f;
    const float step = direction < 0 ? -rowHeight : direction > 0 ? rowHeight
                                                                  : 0.0f;
    return std::clamp(scrollY + step, 0.0f, scrollMax);
}

float FooterHeight(float em, float lineHeight, int tipLines, int noteLines)
{
    const int tip  = std::max(0, tipLines);
    const int note = std::max(0, noteLines);
    float     h    = 2.0f * kFooterPadEm * em + kFooterButtonEm * em;
    if (tip > 0 || note > 0)
        h += kFooterButtonGap * em;
    h += lineHeight * static_cast<float>(tip + note);
    if (tip > 0 && note > 0)
        h += kFooterTextGap * em;
    return h;
}

float ActionWidth(int count, float available, float gap, float fullWidth)
{
    if (count <= 0)
        return fullWidth;
    const float share = (available - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
    return std::clamp(share, 0.0f, fullWidth);
}

float HintHeight(float em, float stripHeight)
{
    return std::max(0.0f, stripHeight - 0.4f * em);
}

float ShoulderHintHeight(float em, float stripHeight)
{
    return HintHeight(em, stripHeight) * kShoulderHintShare;
}

Chrome ChromeFor(float screenHeight, float em, int stripRows, float rowHeight, float wantedRows, float border, float footerFull, float footerFloor)
{
    Chrome c;
    c.banner            = 3.6f * em;
    c.strip             = 2.8f * em;
    c.footer            = footerFull;
    c.titleStrip        = 2.2f * em;
    c.margin            = 1.0f * em;
    const float rows    = static_cast<float>(stripRows);
    const auto  measure = [&]
    {
        // Banner, 2 px rule, strips, margin, title strip, 1 px rule, bottom border, footer.
        return screenHeight - c.banner - 2.0f - rows * c.strip - c.margin - c.titleStrip - 1.0f - border - c.footer;
    };
    const float wanted = wantedRows * rowHeight;
    const auto  shrink = [&](float& value, float floor, float count)
    {
        const float deficit = wanted - measure();
        if (deficit > 0.0f)
            value -= std::min(deficit / count, std::max(0.0f, value - floor));
    };
    shrink(c.titleStrip, 0.0f, 1.0f);
    shrink(c.margin, 0.0f, 1.0f);
    shrink(c.banner, 2.4f * em, 1.0f);
    shrink(c.strip, 1.8f * em, rows);
    shrink(c.footer, std::min(footerFloor, footerFull), 1.0f);
    c.viewport = std::max(0.0f, measure());
    return c;
}

UvRect MapCrop(uint32_t imageWidth, uint32_t imageHeight, float screenWidth, float screenHeight)
{
    if (imageWidth == 0 || imageHeight == 0 || screenWidth <= 0.0f || screenHeight <= 0.0f)
        return {};
    const float iw    = static_cast<float>(imageWidth);
    const float ih    = static_cast<float>(imageHeight);
    const float scale = std::max(screenWidth / iw, screenHeight / ih) * kMapZoom;
    const float fu    = std::min(1.0f, screenWidth / (iw * scale));
    const float fv    = std::min(1.0f, screenHeight / (ih * scale));
    const float u0    = std::clamp(kMapCenterU - fu * 0.5f, 0.0f, 1.0f - fu);
    const float v0    = std::clamp(kMapCenterV - fv * 0.5f, 0.0f, 1.0f - fv);
    return { u0, v0, u0 + fu, v0 + fv };
}

} // namespace rerevved::options_skin
