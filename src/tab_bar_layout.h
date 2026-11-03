#pragma once

#include <cstddef>
#include <span>

namespace rerevved::tab_bar_layout
{

struct Fit
{
    float pad   = 0.0f;
    float scale = 1.0f;
    // Offset from span left edge; negative only to scroll the current tab into view.
    float offset = 0.0f;
};

// Widths use scale 1: shrink maxPad to minPad, then text to minScale, then scroll.
Fit FitTabBar(std::span<const float> widths, float available, float minPad, float maxPad, float minScale, size_t current);

float RowWidth(std::span<const float> widths, const Fit& fit);

} // namespace rerevved::tab_bar_layout
