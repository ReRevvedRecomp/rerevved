#pragma once

// Sampled from retail Options tiles; shared by the overlay skin and Options.

#include <imgui.h>

namespace rerevved::palette
{

constexpr ImVec4 rgba(int r, int g, int b, float a = 1.0f)
{
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

constexpr ImVec4 kNavy      = rgba(16, 28, 74);
constexpr ImVec4 kNavyDeep  = rgba(8, 13, 35);
constexpr ImVec4 kNavyMid   = rgba(25, 57, 148);
constexpr ImVec4 kEdge      = rgba(78, 122, 217);
constexpr ImVec4 kHighlight = rgba(0, 95, 169);
constexpr ImVec4 kGlow      = rgba(117, 205, 255);
constexpr ImVec4 kBand      = rgba(38, 83, 219);
// The Civilopedia description panel's translucent navy (gfx_civilopedia.gfx), framed in black.
constexpr ImVec4 kPanelFill = rgba(9, 26, 56, 192.0f / 255.0f);
constexpr ImVec4 kPanelEdge = rgba(0, 0, 0);

} // namespace rerevved::palette
