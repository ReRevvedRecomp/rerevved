#include "button_icons.h"

#include <array>
#include <cctype>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(button_icons, "xbox", "ReRevved", "Controller button icons the game shows: xbox or playstation")
    .allowed({ "xbox", "playstation" })
    .audience(rex::cvar::Audience::kPlayer)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rerevved::button_icons
{
namespace
{

using Pair = std::pair<std::string_view, std::string_view>;

constexpr std::array<Pair, 17> kImages{ {
    { "a", "PS3X" },
    { "b", "PS3CIRCLE" },
    { "x", "PS3SQUARE" },
    { "y", "PS3TRIANGLE" },
    { "ls", "PS3LS" },
    { "rs", "PS3RS" },
    { "l3", "PS3L3" },
    { "r3", "PS3R3" },
    { "dpad", "PS3DPAD" },
    { "dup", "PS3DUP" },
    { "ddown", "PS3DDOWN" },
    { "dleft", "PS3DLEFT" },
    { "dright", "PS3DRIGHT" },
    { "startbutton", "PS3START" },
    { "backbutton", "PS3SELECT" },
    { "xboxvertical", "PS3VERTICAL" },
    { "xboxhorizontal", "PS3HORIZONTAL" },
} };

constexpr std::array<Pair, 17> kLabels{ {
    { "LB", "PS3L1" },
    { "RB", "PS3R1" },
    { "LT", "PS3L2" },
    { "RT", "PS3R2" },
    { "LS", "PS3LS" },
    { "RS", "PS3RS" },
    { "L3", "PS3L3" },
    { "R3", "PS3R3" },
    { "DUP", "PS3DUP" },
    { "DDOWN", "PS3DDOWN" },
    { "DLEFT", "PS3DLEFT" },
    { "DRIGHT", "PS3DRIGHT" },
    { "DPAD", "PS3DPAD" },
    { "DHORIZONTAL", "PS3DHORIZONTAL" },
    { "DVERTICAL", "PS3DVERTICAL" },
    { "STARTBUTTON", "PS3START" },
    { "BACKBUTTON", "PS3SELECT" },
} };

// The eight images the tip table builds; no face letter reaches the builder.
constexpr std::array<std::pair<std::string_view, TipImage>, 8> kTips{ {
    { "LT", { "PS3L2", true } },
    { "RT", { "PS3R2", true } },
    { "LB", { "PS3L1", true } },
    { "RB", { "PS3R1", true } },
    { "L3", { "PS3L3", false } },
    { "R3", { "PS3R3", false } },
    { "XBOXVERTICAL", { "PS3VERTICAL", false } },
    { "XBOXHORIZONTAL", { "PS3HORIZONTAL", false } },
} };

bool equalsLower(std::string_view text, std::string_view lower) noexcept
{
    if (text.size() != lower.size())
    {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(text[i])) != lower[i])
        {
            return false;
        }
    }
    return true;
}

} // namespace

Family ActiveFamily()
{
    static const Family family = []
    {
        const Family parsed = ParseFamily(rex::cvar::Query<std::string>("button_icons"));
        REXLOG_INFO("ButtonIcons: family={}", parsed == Family::kPlayStation ? "playstation" : "xbox");
        return parsed;
    }();
    return family;
}

std::optional<std::string_view> ImageStem(std::string_view basename) noexcept
{
    constexpr std::string_view kExtension = ".dds";
    if (basename.size() <= kExtension.size() || !equalsLower(basename.substr(basename.size() - kExtension.size()), kExtension))
    {
        return std::nullopt;
    }
    const std::string_view stem = basename.substr(0, basename.size() - kExtension.size());
    for (const auto& [from, to] : kImages)
    {
        if (equalsLower(stem, from))
        {
            return to;
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> Label(std::string_view label) noexcept
{
    for (const auto& [from, to] : kLabels)
    {
        if (label == from)
        {
            return to;
        }
    }
    return std::nullopt;
}

std::optional<TipImage> Tip(std::string_view name) noexcept
{
    for (const auto& [from, to] : kTips)
    {
        if (name == from)
        {
            return to;
        }
    }
    return std::nullopt;
}

} // namespace rerevved::button_icons
