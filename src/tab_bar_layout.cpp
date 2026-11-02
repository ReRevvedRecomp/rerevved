#include "tab_bar_layout.h"

#include <algorithm>

namespace rerevved::tab_bar_layout
{

float RowWidth(std::span<const float> widths, const Fit& fit)
{
    float total = 0.0f;
    for (const float w : widths)
        total += w * fit.scale + 2.0f * fit.pad;
    return total;
}

Fit FitTabBar(std::span<const float> widths, float available, float minPad, float maxPad, float minScale, size_t current)
{
    Fit fit;
    if (widths.empty())
        return fit;
    float text = 0.0f;
    for (const float w : widths)
        text += w;
    const float count = static_cast<float>(widths.size());

    fit.pad = std::clamp((available - text) / (2.0f * count), minPad, maxPad);
    if (RowWidth(widths, fit) > available && text > 0.0f)
    {
        fit.pad   = minPad;
        fit.scale = std::clamp((available - 2.0f * minPad * count) / text, minScale, 1.0f);
    }
    const float total = RowWidth(widths, fit);
    if (total <= available)
    {
        fit.offset = (available - total) * 0.5f;
        return fit;
    }

    float left = 0.0f;
    for (size_t i = 0; i < std::min(current, widths.size()); ++i)
        left += widths[i] * fit.scale + 2.0f * fit.pad;
    const float right  = left + (current < widths.size() ? widths[current] * fit.scale + 2.0f * fit.pad : 0.0f);
    float       offset = 0.0f;
    if (right > available)
        offset = available - right;
    if (left + offset < 0.0f)
        offset = -left;
    fit.offset = std::clamp(offset, available - total, 0.0f);
    return fit;
}

} // namespace rerevved::tab_bar_layout
