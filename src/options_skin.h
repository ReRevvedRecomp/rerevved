#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "button_icons.h"

namespace rerevved::options_skin
{

enum class Art
{
    kMap,
    kBar,
    kGoldArrow, // right-pointing; rotated for up and down
    kLs,        // left-stick glyph; none for the Keyboard family
};

inline constexpr int kArtCount = 4;

struct ArtFile
{
    std::string_view archive;
    std::string_view name;
};

// Nullopt if unavailable for the family (Keyboard has no right-stick art).
std::optional<ArtFile> FileFor(button_icons::Family family, Art art);

struct ScrollArrows
{
    bool up   = false;
    bool down = false;
};

ScrollArrows ArrowsFor(float scrollY, float scrollMax);

// Right stick scrolls one row up (-1) or down (+1), without wrap.
float StepScroll(float scrollY, float scrollMax, float rowHeight, int direction);

struct UvRect
{
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

// Clamped inside the texture
UvRect MapCrop(uint32_t imageWidth, uint32_t imageHeight, float screenWidth, float screenHeight);

inline constexpr float kFooterPadEm     = 0.5f;
inline constexpr float kFooterTextGap   = 0.2f;
inline constexpr float kFooterButtonGap = 0.3f;
inline constexpr float kFooterButtonEm  = 2.2f;

float FooterHeight(float em, float lineHeight, int tipLines, int noteLines);

// Chrome in pixels: banner 3.6 em, strips 2.8 em, title strip 2.2 em, margin 1 em
struct Chrome
{
    float banner     = 0.0f;
    float strip      = 0.0f;
    float footer     = 0.0f;
    float titleStrip = 0.0f;
    float margin     = 0.0f; // Both panel gaps combined
    float viewport   = 0.0f;
};

// stripRows is 0 to 2
Chrome ChromeFor(float screenHeight, float em, int stripRows, float rowHeight, float wantedRows, float border, float footerFull, float footerFloor);

// Caller uses glyph-only buttons when captions cannot fit.
float ActionWidth(int count, float available, float gap, float fullWidth);

// stripHeight minus margin aligns bumper (2:1) and trigger (1:2) prompts.
float HintHeight(float em, float stripHeight);

// Center LB/RB (2:1) at this height share so they do not dwarf triggers.
inline constexpr float kShoulderHintShare = 0.8f;
float                  ShoulderHintHeight(float em, float stripHeight);

inline constexpr float kMapZoom    = 1.25f;
inline constexpr float kMapCenterU = 0.36f;
inline constexpr float kMapCenterV = 0.55f;

} // namespace rerevved::options_skin
