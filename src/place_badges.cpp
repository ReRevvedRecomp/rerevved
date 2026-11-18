#include "place_badges.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>

namespace rerevved::place_badges
{

namespace
{

constexpr size_t   kHeaderSize = 128;
constexpr uint32_t kSize       = 64;
constexpr uint32_t kPfAlpha    = 0x1;
constexpr uint32_t kPfRgb      = 0x40;

struct Point
{
    float x = 0;
    float y = 0;
};

uint32_t readU32(std::span<const uint8_t> data, size_t at)
{
    return uint32_t{ data[at] } | (uint32_t{ data[at + 1] } << 8) | (uint32_t{ data[at + 2] } << 16) |
           (uint32_t{ data[at + 3] } << 24);
}

// An 8-bit channel: its shift within the little-endian pixel word.
bool channelShift(uint32_t mask, unsigned& shift)
{
    for (shift = 0; shift < 32; shift += 8)
    {
        if (mask == 0xFFu << shift)
        {
            return true;
        }
    }
    return false;
}

// Glyph stroke in a unit box, y down; the badge art leans right.
std::vector<Point> glyph(int place)
{
    if (place == 7)
    {
        return { { 0.16f, 0.08f }, { 0.86f, 0.08f }, { 0.68f, 0.34f }, { 0.52f, 0.62f }, { 0.42f, 0.92f } };
    }
    std::vector<Point> stroke{ { 0.80f, 0.14f }, { 0.64f, 0.05f }, { 0.44f, 0.06f }, { 0.28f, 0.18f }, { 0.19f, 0.38f }, { 0.17f, 0.58f } };
    constexpr int      kSteps = 40;
    constexpr float    kCx = 0.49f, kCy = 0.68f, kRx = 0.32f, kRy = 0.25f;
    for (int i = 0; i <= kSteps; ++i)
    {
        const float angle = std::numbers::pi_v<float> * (1.0f - 2.0f * static_cast<float>(i) / kSteps);
        stroke.push_back({ kCx + kRx * std::cos(angle), kCy - kRy * std::sin(angle) });
    }
    return stroke;
}

float segmentDistance(Point p, Point a, Point b)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float length = dx * dx + dy * dy;
    float       t      = length > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / length : 0;
    t                  = std::clamp(t, 0.0f, 1.0f);
    const float ex = p.x - (a.x + t * dx), ey = p.y - (a.y + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

float coverage(float distance, float edge)
{
    return std::clamp(edge - distance + 0.5f, 0.0f, 1.0f);
}

} // namespace

std::optional<int> FindBadge(std::string_view fileName)
{
    const size_t separator = fileName.find_last_of("/\\");
    if (separator != std::string_view::npos)
    {
        fileName.remove_prefix(separator + 1);
    }
    for (int place = kFirstPlace; place <= kLastPlace; ++place)
    {
        const std::string expected = "ribbon" + std::to_string(place) + ".dds";
        if (std::equal(fileName.begin(),
                       fileName.end(),
                       expected.begin(),
                       expected.end(),
                       [](char a, char b)
                       {
                           return std::tolower(static_cast<unsigned char>(a)) == b;
                       }))
        {
            return place;
        }
    }
    return std::nullopt;
}

std::vector<uint8_t> BuildBadge(std::span<const uint8_t> sourceDds, int place)
{
    if (place < kFirstPlace || place > kLastPlace || sourceDds.size() < kHeaderSize + kSize * kSize * 4 ||
        std::memcmp(sourceDds.data(), "DDS ", 4) != 0 || readU32(sourceDds, 4) != 124 ||
        readU32(sourceDds, 12) != kSize || readU32(sourceDds, 16) != kSize || readU32(sourceDds, 76) != 32 ||
        (readU32(sourceDds, 80) & (kPfRgb | kPfAlpha)) != (kPfRgb | kPfAlpha) || readU32(sourceDds, 84) != 0 ||
        readU32(sourceDds, 88) != 32)
    {
        return {};
    }
    std::array<unsigned, 4> shifts{};
    for (size_t c = 0; c < shifts.size(); ++c)
    {
        if (!channelShift(readU32(sourceDds, 92 + 4 * c), shifts[c]))
        {
            return {};
        }
    }

    std::vector<uint8_t> out(sourceDds.begin(), sourceDds.begin() + kHeaderSize + kSize * kSize * 4);
    auto                 alphaAt = [&](uint32_t x, uint32_t y)
    {
        return (readU32(out, kHeaderSize + 4 * (y * kSize + x)) >> shifts[3]) & 0xFF;
    };

    // The ring's inner edge: the first clear pixel after the ring on each axis from the edge.
    constexpr float kCenter = (kSize - 1) / 2.0f;
    float           inner   = kCenter;
    for (int axis = 0; axis < 4; ++axis)
    {
        bool  inRing = false;
        float found  = -1;
        for (uint32_t step = 0; step < kSize / 2; ++step)
        {
            const uint32_t x = axis == 0 ? step : axis == 1 ? kSize - 1 - step
                                                            : kSize / 2;
            const uint32_t y = axis == 2 ? step : axis == 3 ? kSize - 1 - step
                                                            : kSize / 2;
            const uint32_t a = alphaAt(x, y);
            if (!inRing && a > 128)
            {
                inRing = true;
            }
            else if (inRing && a < 64)
            {
                found = kCenter - static_cast<float>(step);
                break;
            }
        }
        if (found <= 0)
        {
            return {};
        }
        inner = std::min(inner, found);
    }

    constexpr float kHeight = 36.0f, kWidth = 24.0f, kLean = 0.18f;
    constexpr float kCore = 3.4f, kOutline = 6.0f;
    const auto      stroke = glyph(place);
    for (uint32_t y = 0; y < kSize; ++y)
    {
        for (uint32_t x = 0; x < kSize; ++x)
        {
            const float dx = x - kCenter, dy = y - kCenter;
            // Soft pixels just inside the ring belong to the source digit's shadow.
            const float radius = std::sqrt(dx * dx + dy * dy);
            if (radius >= inner + 2.0f || (radius >= inner - 0.5f && alphaAt(x, y) >= 160))
            {
                continue;
            }
            const float v = (static_cast<float>(y) - (kCenter + 1.0f - kHeight / 2)) / kHeight;
            const Point p{ (static_cast<float>(x) - (kCenter - kWidth / 2) - (0.5f - v) * kHeight * kLean) / kWidth, v };
            float       distance = 1e9f;
            for (size_t i = 1; i < stroke.size(); ++i)
            {
                const Point a{ stroke[i - 1].x * kWidth, stroke[i - 1].y * kHeight };
                const Point b{ stroke[i].x * kWidth, stroke[i].y * kHeight };
                distance = std::min(distance, segmentDistance({ p.x * kWidth, p.y * kHeight }, a, b));
            }
            const float outline = coverage(distance, kOutline);
            const float core    = coverage(distance, kCore);
            const float shade   = std::clamp(v, 0.0f, 1.0f);
            const float rgb[3]  = { 255.0f - 50.0f * shade, 255.0f - 45.0f * shade, 255.0f };
            uint32_t    pixel   = 0;
            for (size_t c = 0; c < 3; ++c)
            {
                pixel |= static_cast<uint32_t>(std::lround(rgb[c] * core)) << shifts[c];
            }
            pixel |= static_cast<uint32_t>(std::lround(255.0f * outline)) << shifts[3];
            const size_t at = kHeaderSize + 4 * (y * kSize + x);
            for (size_t b = 0; b < 4; ++b)
            {
                out[at + b] = static_cast<uint8_t>(pixel >> (8 * b));
            }
        }
    }
    return out;
}

} // namespace rerevved::place_badges
