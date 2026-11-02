#include "button_icons.h"

#include <array>
#include <cctype>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_STRING(button_icons, "auto", "ReRevved", "Button icons the game shows: auto (keyboard when no controller is connected), xbox, playstation or keyboard")
    .allowed({ "auto", "xbox", "playstation", "keyboard" })
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

const std::string& latchedSetting()
{
    static const std::string setting = rex::cvar::Query<std::string>("button_icons");
    return setting;
}

struct PadState
{
    rex::input::GamepadFamily family    = rex::input::GamepadFamily::kOther;
    bool                      connected = true;
};

PadState padState()
{
    auto* kernel  = REX_KERNEL_STATE();
    auto* runtime = kernel ? kernel->emulator() : nullptr;
    auto* input   = runtime ? dynamic_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
    return input ? PadState{ input->ActiveGamepadFamily(), input->HasConnectedGamepad() } : PadState{};
}

const char* familyName(Family family)
{
    switch (family)
    {
        case Family::kPlayStation:
            return "playstation";
        case Family::kKeyboard:
            return "keyboard";
        default:
            return "xbox";
    }
}

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

bool MaySwap()
{
    // Assume a PlayStation pad: only then can auto still swap.
    return ParseFamily(latchedSetting(), rex::input::GamepadFamily::kPlayStation) == Family::kPlayStation;
}

Family ActiveFamily()
{
    static const Family family = []
    {
        const std::string& setting = latchedSetting();
        const PadState     pad     = setting == "auto" ? padState() : PadState{};
        const Family       parsed  = ParseFamily(setting, pad.family, pad.connected);
        REXLOG_INFO("ButtonIcons: setting={} family={}", setting, familyName(parsed));
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
