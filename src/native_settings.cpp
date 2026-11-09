#include "native_settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "retail_audio.h"

namespace rerevved::native_settings
{

enum class Kind
{
    kBool,
    kEnum,
    kLabeledInt,
    kPresetInt,
    kPresetDouble,
    kMonitor,
    kWeek,
    kAction,
    kStatus,
    kText,
    kSecret,
};

enum class Applies
{
    kLive,
    kNextGame,
};

struct Spec
{
    std::string_view                  key;
    Kind                              kind;
    Applies                           applies;
    std::string_view                  label;
    std::span<const std::string_view> presets;
    text_entry::Rules                 rules;
};

namespace
{

constexpr std::array<std::string_view, 5> kBonusPresets{ "0", "25", "50", "100", "200" };
constexpr std::array<std::string_view, 5> kVolumePresets{ "0", "25", "50", "75", "100" };
constexpr std::array<std::string_view, 5> kSensitivityPresets{ "0.5", "1.0", "1.5", "2.0", "4.0" };

// SDK invite: 359 bytes (RXR1- plus 575 base-32 characters); extra room for ignored spaces and dashes.
constexpr size_t kInviteMaxLength = 1024;

constexpr std::array<std::string_view, 7> kCategoryNames{ "General", "Gameplay", "Notifications", "Online", "Controls", "Display", "Audio" };

constexpr auto kSpecs = std::to_array<Spec>({
    { "combat_speed", Kind::kEnum, Applies::kLive, {}, {} },
    { "unit_movement", Kind::kEnum, Applies::kLive, {}, {} },
    { "spaceship_scenes", Kind::kBool, Applies::kLive, {}, {} },
    { "resources", Kind::kLabeledInt, Applies::kNextGame, {}, {} },
    { "raging_barbarians", Kind::kBool, Applies::kNextGame, {}, {} },
    { "one_city_challenge", Kind::kBool, Applies::kNextGame, {}, {} },
    { "domination_min_turns", Kind::kEnum, Applies::kNextGame, {}, {} },
    { "turn_limit", Kind::kEnum, Applies::kNextGame, {}, {} },
    { "starting_era", Kind::kEnum, Applies::kNextGame, {}, {} },
    { "ai_civilizations", Kind::kLabeledInt, Applies::kNextGame, {}, {} },
    { "always_at_war", Kind::kBool, Applies::kNextGame, {}, {} },
    { "multiplayer_ai_difficulty", Kind::kEnum, Applies::kNextGame, {}, {} },
    { "ai_research_bonus_percent", Kind::kPresetInt, Applies::kNextGame, {}, kBonusPresets },
    { "ai_production_bonus_percent", Kind::kPresetInt, Applies::kNextGame, {}, kBonusPresets },
    { "ai_gold_bonus_percent", Kind::kPresetInt, Applies::kNextGame, {}, kBonusPresets },
    { "ai_food_bonus_percent", Kind::kPresetInt, Applies::kNextGame, {}, kBonusPresets },
    { "ai_culture_bonus_percent", Kind::kPresetInt, Applies::kNextGame, {}, kBonusPresets },
    { "gotw_standard_rules", Kind::kBool, Applies::kNextGame, {}, {} },
    { "gotw_allow_saving", Kind::kBool, Applies::kNextGame, {}, {} },
    { "gotw_custom_week", Kind::kBool, Applies::kNextGame, "Custom Week", {} },
    { "gotw_week", Kind::kWeek, Applies::kNextGame, {}, {} },
    { "gotw_custom_civilization", Kind::kBool, Applies::kNextGame, "Custom Civilization", {} },
    { "gotw_civilization", Kind::kLabeledInt, Applies::kNextGame, {}, {} },

    { "show_research_prompt", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_city_status", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_discoveries", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_combat_reports", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_rival_great_people", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_rankings", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_rival_war_peace", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_save_reminder", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_villages", Kind::kBool, Applies::kLive, {}, {} },
    { "show_village_barbarian_messages", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_city_captures", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_rival_government", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_artifacts", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_spy_reports", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_victory_warnings", Kind::kBool, Applies::kLive, {}, {} },
    { "show_barbarian_messages", Kind::kBool, Applies::kLive, {}, {} },
    { "show_popup_trophy_room", Kind::kBool, Applies::kLive, {}, {} },

    { "system_link_status", Kind::kStatus, Applies::kLive, {}, {} },
    { "system_link_profile_name", Kind::kText, Applies::kLive, {}, {}, { 15, text_entry::Charset::kName } },
    { "system_link_invite", Kind::kSecret, Applies::kLive, {}, {}, { kInviteMaxLength, text_entry::Charset::kInvite } },
    { "system_link_lan_enabled", Kind::kBool, Applies::kLive, {}, {} },
    { "system_link_direct", Kind::kBool, Applies::kLive, {}, {} },
    { "live_sessions", Kind::kBool, Applies::kLive, {}, {} },
    { "discord_activity", Kind::kBool, Applies::kLive, {}, {} },
    { "discord_invites", Kind::kBool, Applies::kLive, {}, {} },

    { "input_gamepad_in_background", Kind::kBool, Applies::kLive, {}, {} },
    { "input_vibration", Kind::kBool, Applies::kLive, {}, {} },
    { "button_icons", Kind::kEnum, Applies::kLive, {}, {} },
    { "mnk_mode", Kind::kBool, Applies::kLive, {}, {} },
    { "mnk_mouse", Kind::kBool, Applies::kLive, {}, {} },
    { "mnk_sensitivity", Kind::kPresetDouble, Applies::kLive, {}, kSensitivityPresets },

    { "fullscreen", Kind::kBool, Applies::kLive, {}, {} },
    { "monitor", Kind::kMonitor, Applies::kLive, {}, {} },
    { "window_size", Kind::kEnum, Applies::kLive, {}, {} },
    { "display_aspect", Kind::kEnum, Applies::kLive, {}, {} },
    { "resolution_scale", Kind::kLabeledInt, Applies::kLive, {}, {} },
    { "vsync", Kind::kBool, Applies::kLive, {}, {} },
    { "present_effect", Kind::kEnum, Applies::kLive, {}, {} },
    { "swap_post_effect", Kind::kEnum, Applies::kLive, {}, {} },
    { "show_fps_counter", Kind::kBool, Applies::kLive, {}, {} },
    { "skip_intro_movie", Kind::kBool, Applies::kLive, {}, {} },
    { "game_language", Kind::kLabeledInt, Applies::kLive, {}, {} },
    { "open_log_folder", Kind::kAction, Applies::kLive, {}, {} },

    { "audio_volume", Kind::kPresetInt, Applies::kLive, {}, kVolumePresets },
    { "audio_mute", Kind::kBool, Applies::kLive, {}, {} },
    { "audio_play_in_background", Kind::kBool, Applies::kLive, {}, {} },
});

const Spec* findSpec(std::string_view key)
{
    const auto it = std::find_if(kSpecs.begin(), kSpecs.end(), [&](const Spec& spec)
                                 {
                                     return spec.key == key;
                                 });
    return it == kSpecs.end() ? nullptr : &*it;
}

bool isSafeChar(char c)
{
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) != 0)
        return true;
    switch (c)
    {
        case ' ':
        case '.':
        case ',':
        case ':':
        case ';':
        case '(':
        case ')':
        case '\'':
        case '-':
        case '/':
        case '+':
            return true;
        default:
            return false;
    }
}

std::optional<int64_t> parseInteger(std::string_view text)
{
    int64_t     value = 0;
    const char* end   = text.data() + text.size();
    const auto  r     = std::from_chars(text.data(), end, value);
    if (text.empty() || r.ec != std::errc{} || r.ptr != end)
        return std::nullopt;
    return value;
}

std::optional<double> parseReal(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    const std::string copy(text);
    char*             end   = nullptr;
    const double      value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

bool sameValue(Kind kind, std::string_view a, std::string_view b)
{
    switch (kind)
    {
        case Kind::kLabeledInt:
        case Kind::kPresetInt:
        case Kind::kMonitor:
        case Kind::kWeek:
        {
            const auto x = parseInteger(a);
            const auto y = parseInteger(b);
            return x && y ? *x == *y : a == b;
        }
        case Kind::kPresetDouble:
        {
            const auto x = parseReal(a);
            const auto y = parseReal(b);
            return x && y ? std::fabs(*x - *y) < 1e-9 : a == b;
        }
        default:
            return a == b;
    }
}

using title_text::Language;

std::string tr(Language language, const std::string& key, std::string_view english)
{
    return title_text::Screen(key, english, language);
}

std::string labelOf(const rex::ui::SettingsLayout& layout, const Spec& spec, Language language)
{
    std::string english(spec.key);
    if (!spec.label.empty())
        english = std::string(spec.label);
    else if (const auto it = layout.labels.find(std::string(spec.key)); it != layout.labels.end())
        english = it->second;
    return tr(language, title_text::SettingKey(spec.key, "label"), english);
}

std::string choiceLabel(const rex::ui::SettingsLayout& layout, const Spec& spec, const std::string& raw, Language language)
{
    std::string english;
    if (const auto row = layout.choice_labels.find(std::string(spec.key)); row != layout.choice_labels.end())
    {
        if (const auto it = row->second.find(raw); it != row->second.end())
            english = it->second;
    }
    if (english.empty())
    {
        english = layout.format_label ? layout.format_label(raw) : raw;
        if (english.empty())
            english = "Default";
    }
    return tr(language, title_text::ChoiceKey(spec.key, raw), english);
}

std::string onOff(bool on, Language language)
{
    return on ? tr(language, "common.on", "On") : tr(language, "common.off", "Off");
}

std::string valueCaption(const rex::ui::SettingsLayout& layout, const Spec& spec, const std::string& raw, Language language)
{
    switch (spec.kind)
    {
        case Kind::kBool:
            return onOff(raw == "true", language);
        case Kind::kPresetInt:
        case Kind::kWeek:
            return raw;
        case Kind::kPresetDouble:
        {
            const auto value = parseReal(raw);
            if (!value)
                return raw;
            char text[32];
            std::snprintf(text, sizeof text, "%.2f", *value);
            return text;
        }
        case Kind::kMonitor:
            return raw == "0" ? tr(language, title_text::ChoiceKey(spec.key, raw), "Default") : raw;
        case Kind::kAction:
        case Kind::kStatus:
        case Kind::kText:
        case Kind::kSecret:
            return raw;
        default:
            return choiceLabel(layout, spec, raw, language);
    }
}

std::vector<Choice> candidates(const rex::ui::SettingsLayout& layout,
                               const Spec&                    spec,
                               const FlagState&               state,
                               const std::vector<Choice>&     monitors,
                               Language                       language)
{
    std::vector<Choice> result;
    switch (spec.kind)
    {
        case Kind::kBool:
            result = { { "true", onOff(true, language) }, { "false", onOff(false, language) } };
            break;
        case Kind::kEnum:
            for (const auto& raw : state.allowed)
                result.push_back({ raw, choiceLabel(layout, spec, raw, language) });
            break;
        case Kind::kLabeledInt:
        {
            std::vector<std::pair<int64_t, std::string>> values;
            if (const auto row = layout.choice_labels.find(std::string(spec.key)); row != layout.choice_labels.end())
            {
                for (const auto& entry : row->second)
                {
                    const auto number = parseInteger(entry.first);
                    if (!number)
                        continue;
                    if ((state.min && static_cast<double>(*number) < *state.min) ||
                        (state.max && static_cast<double>(*number) > *state.max))
                        continue;
                    values.emplace_back(*number, entry.first);
                }
            }
            std::sort(values.begin(), values.end());
            for (const auto& value : values)
                result.push_back({ value.second, choiceLabel(layout, spec, value.second, language) });
            break;
        }
        case Kind::kPresetInt:
        case Kind::kPresetDouble:
            for (const auto raw : spec.presets)
                result.push_back({ std::string(raw), valueCaption(layout, spec, std::string(raw), language) });
            break;
        case Kind::kMonitor:
            result = monitors;
            // Only the default monitor's caption is title-owned; others are system text.
            for (auto& choice : result)
            {
                if (choice.raw == "0")
                    choice.caption = tr(language, title_text::ChoiceKey(spec.key, choice.raw), choice.caption);
            }
            break;
        case Kind::kWeek:
        {
            constexpr int64_t kSpan  = 10;
            const auto        parsed = parseInteger(state.value);
            const int64_t     week   = parsed && *parsed >= 1 && *parsed <= kWeekLast ? *parsed : 1;
            for (int64_t value = std::max<int64_t>(1, week - kSpan); value <= std::min(kWeekLast, week + kSpan); ++value)
                result.push_back({ std::to_string(value), std::to_string(value) });
            break;
        }
        case Kind::kAction:
        case Kind::kStatus:
        case Kind::kText:
        case Kind::kSecret:
            break;
    }
    return result;
}

// Preserve exact registry text so non-preset values round-trip; put them first.
struct OrderedChoices
{
    std::vector<Choice> list;
    size_t              current = 0;
};

OrderedChoices orderedChoices(const rex::ui::SettingsLayout& layout,
                              const Spec&                    spec,
                              const FlagState&               state,
                              const std::vector<Choice>&     monitors,
                              Language                       language)
{
    OrderedChoices result;
    result.list   = candidates(layout, spec, state, monitors, language);
    const auto it = std::find_if(result.list.begin(), result.list.end(), [&](const Choice& choice)
                                 {
                                     return sameValue(spec.kind, choice.raw, state.value);
                                 });
    if (it != result.list.end())
    {
        result.current = static_cast<size_t>(it - result.list.begin());
        it->raw        = state.value;
    }
    else
    {
        result.list.insert(result.list.begin(), Choice{ state.value, valueCaption(layout, spec, state.value, language) });
        result.current = 0;
    }
    return result;
}

std::string statusCaption(const FlagState& state)
{
    const std::string text = SanitizeCaption(state.value, 60);
    return text.empty() ? "-" : text;
}

// Printable ASCII prevents damaged config from displaying unreadable glyphs.
std::string printable(std::string_view text)
{
    std::string result(text);
    for (char& c : result)
    {
        if (c < 0x20 || c > 0x7e)
            c = '?';
    }
    return result;
}

// Never copy a secret into the row; expose only whether it is set.
void fillSingle(OptionRow& row, const Spec& spec, const FlagState& state, Language language)
{
    row.current = 0;
    switch (spec.kind)
    {
        case Kind::kAction:
            row.kind    = RowKind::kAction;
            row.choices = { { "", tr(language, "common.open", "Open") } };
            break;
        case Kind::kStatus:
            row.kind    = RowKind::kStatus;
            row.choices = { { state.value, statusCaption(state) } };
            break;
        case Kind::kText:
            row.kind    = RowKind::kText;
            row.rules   = spec.rules;
            row.text    = state.value;
            row.choices = { { state.value, state.value.empty() ? tr(language, title_text::ChoiceKey(spec.key, ""), "Default") : printable(state.value) } };
            break;
        case Kind::kSecret:
            row.kind      = RowKind::kText;
            row.secret    = true;
            row.secretSet = !state.value.empty();
            row.rules     = spec.rules;
            row.choices   = { { "", state.value.empty() ? tr(language, "common.not_set", "Not set") : tr(language, "common.set", "Set") } };
            break;
        default:
            break;
    }
}

bool isSingle(const Spec& spec)
{
    return spec.kind == Kind::kAction || spec.kind == Kind::kStatus || spec.kind == Kind::kText || spec.kind == Kind::kSecret;
}

bool qualifies(const rex::ui::SettingsLayout& layout, const Spec& spec, const FlagState& state)
{
    switch (spec.kind)
    {
        case Kind::kBool:
            return state.kind == FlagKind::kBool;
        case Kind::kEnum:
            return state.kind == FlagKind::kString && state.allowed.size() >= 2;
        case Kind::kLabeledInt:
        {
            const auto row = layout.choice_labels.find(std::string(spec.key));
            return state.kind == FlagKind::kInteger && row != layout.choice_labels.end() && row->second.size() >= 2;
        }
        case Kind::kPresetInt:
        case Kind::kMonitor:
            return state.kind == FlagKind::kInteger;
        case Kind::kPresetDouble:
            return state.kind == FlagKind::kDouble;
        case Kind::kWeek:
        case Kind::kStatus:
        case Kind::kText:
        case Kind::kSecret:
            return state.kind == FlagKind::kString;
        case Kind::kAction:
            return state.kind == FlagKind::kOther;
    }
    return false;
}

Timing TimingOf(const Spec& spec, const FlagState& state)
{
    if (state.requiresRestart)
        return Timing::kRestart;
    return spec.applies == Applies::kNextGame ? Timing::kNextGame : Timing::kLive;
}

} // namespace

std::optional<std::string> NormalizeWeek(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    if (!std::all_of(text.begin(), text.end(), [](char c)
                     {
                         return c >= '0' && c <= '9';
                     }))
        return std::nullopt;
    while (text.size() > 1 && text.front() == '0')
        text.remove_prefix(1);
    if (text.size() > kWeekMaxDigits)
        return std::nullopt;
    int64_t value = 0;
    for (const char c : text)
        value = value * 10 + (c - '0');
    if (value < 1 || value > kWeekLast)
        return std::nullopt;
    return std::to_string(value);
}

std::string_view TimingNote(Timing timing)
{
    switch (timing)
    {
        case Timing::kRestart:
            return kNoteRestart;
        case Timing::kNextGame:
            return kNoteNextGame;
        case Timing::kLive:
            break;
    }
    return {};
}

std::string_view OutcomeNote(ApplyStatus status)
{
    switch (status)
    {
        case ApplyStatus::kRejected:
            return kNoteRejected;
        case ApplyStatus::kUnsaved:
            return kNoteUnsaved;
        case ApplyStatus::kSaved:
            break;
    }
    return {};
}

std::vector<std::string_view> CuratedKeys()
{
    std::vector<std::string_view> keys;
    for (const auto& spec : kSpecs)
        keys.push_back(spec.key);
    return keys;
}

std::string SanitizeCaption(std::string_view text, size_t maxLength)
{
    std::string result;
    for (const char c : text.substr(0, maxLength))
    {
        const char out = isSafeChar(c) ? c : ' ';
        if (out == ' ' && (result.empty() || result.back() == ' '))
            continue;
        result.push_back(out);
    }
    if (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result;
}

namespace
{

// Hide values with disabled inline toggles (Week while Custom Week is off).
bool lockedByToggle(const rex::ui::SettingsLayout&                    layout,
                    const std::unordered_map<std::string, FlagState>& flags,
                    std::string_view                                  key)
{
    for (const auto& pair : layout.inline_toggles)
    {
        if (pair.value != key)
            continue;
        const auto toggle = flags.find(pair.toggle);
        if (toggle == flags.end())
            continue;
        return (toggle->second.value == "true") != pair.unlock_when_checked;
    }
    return false;
}

std::string layoutText(const std::unordered_map<std::string, std::string>& map, std::string_view key)
{
    const auto it = map.find(std::string(key));
    return it == map.end() ? std::string() : it->second;
}

std::string layoutTextIn(const std::unordered_map<std::string, std::string>& map, std::string_view cvar, std::string_view field, Language language)
{
    const std::string english = layoutText(map, cvar);
    return english.empty() ? english : tr(language, title_text::SettingKey(cvar, field), english);
}

// F1's key text for a binding: "Semicolon,Space" reads "; or Space".
std::string formatBinding(std::string_view value, Language language)
{
    static constexpr std::pair<std::string_view, std::string_view> kSymbols[] = {
        { "Backtick", "`" },
        { "Minus", "-" },
        { "Plus", "=" },
        { "Comma", "," },
        { "Period", "." },
        { "Semicolon", ";" },
        { "Slash", "/" },
        { "Backslash", "\\" },
        { "LBracket", "[" },
        { "RBracket", "]" },
        { "Quote", "'" },
    };
    std::string result;
    while (!value.empty())
    {
        const size_t     comma = value.find(',');
        std::string_view key   = value.substr(0, comma);
        for (const auto& [name, symbol] : kSymbols)
        {
            if (key == name)
            {
                key = symbol;
                break;
            }
        }
        if (!result.empty())
            result += " " + tr(language, "common.bind_or", "or") + " ";
        result += key;
        value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
    }
    return result;
}

// F1 label fallback: short first description sentence, else name, then format.
std::string bindLabel(const rex::ui::SettingsLayout& layout, std::string_view key, std::string_view description)
{
    if (const auto it = layout.labels.find(std::string(key)); it != layout.labels.end())
        return it->second;
    std::string_view lead = description;
    if (const size_t stop = lead.find(". "); stop != std::string_view::npos)
        lead = lead.substr(0, stop);
    while (!lead.empty() && (lead.back() == '.' || lead.back() == ' '))
        lead.remove_suffix(1);
    std::string label;
    if (!lead.empty() && lead.size() <= 48)
    {
        label = std::string(lead);
    }
    else
    {
        label = std::string(key);
        std::replace(label.begin(), label.end(), '_', ' ');
        if (!label.empty())
            label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    }
    return layout.format_label ? layout.format_label(label) : label;
}

// Match F1: Controls lists Keybinds then Overlay Hotkeys.
void appendBinds(Services& services, OptionSection& section, Language language)
{
    const auto& layout = services.Layout();
    for (const auto& key : services.BindKeys())
    {
        const auto state = services.Read(key);
        if (!state || state->kind != FlagKind::kString)
            continue;
        const bool        controller = state->category == "Input/Keybinds/Controller" || state->category.starts_with("Input/Keybinds/Controller/");
        const std::string group      = controller ? "Keybinds" : "Overlay Hotkeys";
        OptionRow         row;
        row.key                 = key;
        row.label               = tr(language, title_text::SettingKey(key, "label"), bindLabel(layout, key, state->description));
        row.group               = tr(language, title_text::SectionKey(group), group);
        row.kind                = RowKind::kBind;
        row.bindValue           = state->value;
        row.bindDefault         = state->defaultValue;
        row.bindController      = controller;
        row.conflicts           = state->conflicts;
        row.timing              = state->requiresRestart ? Timing::kRestart : Timing::kLive;
        row.description         = controller ? tr(language, "common.bind_tip_controller", "Used when Keyboard and Mouse is on. Select, then press a key; hold Shift, Ctrl or Alt for a combination.")
                                             : tr(language, "common.bind_tip_hotkey", "Select, then press the new key.");
        const std::string shown = state->value.empty() ? tr(language, "common.bind_none", "(none)") : formatBinding(state->value, language);
        row.choices             = { { state->value, shown } };
        row.current             = 0;
        section.rows.push_back(std::move(row));
    }
}

// Each volume: override, else live value, else unavailable; missing cvars are omitted.
void appendRetailAudio(Services& services, const LiveVolumes& live, OptionSection& section, Language language)
{
    const auto& layout = services.Layout();
    for (size_t channel = 0; channel < retail_audio::kChannelCount; ++channel)
    {
        const std::string_view cvar  = retail_audio::kChannels[channel].cvar;
        const auto             state = services.Read(cvar);
        if (!state || state->kind != FlagKind::kInteger)
            continue;
        OptionRow row;
        row.key         = std::string(cvar);
        row.label       = layoutTextIn(layout.labels, cvar, "label", language);
        row.description = layoutTextIn(layout.tooltips, cvar, "tip", language);
        if (row.label.empty())
            row.label = row.key;

        const auto                stored     = parseInteger(state->value);
        const bool                overridden = stored && retail_audio::IsPosition(static_cast<int>(*stored));
        std::optional<LiveVolume> shown;
        if (overridden)
            shown = LiveVolume{ static_cast<int>(*stored), true };
        else
            shown = live.volume[channel];

        if (!shown)
        {
            row.unavailable = true;
            row.choices     = { { "", tr(language, "common.unavailable", "Unavailable") } };
            row.current     = 0;
        }
        else
        {
            for (int position = retail_audio::kMinPosition; position <= retail_audio::kMaxPosition; ++position)
                row.choices.push_back({ std::to_string(position), std::to_string(position) });
            row.current = static_cast<size_t>(shown->position);
            if (!shown->exact)
            {
                row.choices[row.current].caption.insert(0, "~");
                row.offGrid = true;
                row.below   = static_cast<size_t>(std::clamp(shown->below, retail_audio::kMinPosition, retail_audio::kMaxPosition));
                row.above   = static_cast<size_t>(std::clamp(shown->above, retail_audio::kMinPosition, retail_audio::kMaxPosition));
            }
        }
        section.rows.push_back(std::move(row));
    }
}

} // namespace

OptionsModel::OptionsModel(Services& services, title_text::Language language)
: services_(services)
, language_(language)
{
    if (auto available = services_.MonitorChoices())
        monitors_ = std::move(*available);
    rebuild();
}

void OptionsModel::rebuild()
{
    const auto&                                layout = services_.Layout();
    std::unordered_map<std::string, FlagState> flags;
    for (const auto& spec : kSpecs)
    {
        if (auto state = services_.Read(spec.key))
            flags.emplace(std::string(spec.key), std::move(*state));
    }
    // Read values and revision together to avoid marking stale rows current.
    const LiveVolumes live = services_.ReadLiveVolumes();
    std::string       highlighted;
    std::string       highlightedTab;
    size_t            highlightedAt = 0;
    if (const OptionRow* row = Row(highlight_))
    {
        highlighted    = row->key;
        highlightedTab = sections_[SectionOf(highlight_)].title;
        highlightedAt  = highlight_ - SectionStart(SectionOf(highlight_));
    }

    sections_.clear();
    for (const auto name : kCategoryNames)
    {
        OptionSection section;
        section.title     = tr(language_, title_text::TabKey(name), name);
        const auto addRow = [&](const std::string& source, const std::string& groupTitle)
        {
            const Spec* spec = findSpec(source);
            if (!spec)
                return;
            const auto it = flags.find(source);
            if (it == flags.end() || !qualifies(layout, *spec, it->second) || lockedByToggle(layout, flags, source))
                return;
            OptionRow row;
            row.key     = source;
            row.label   = labelOf(layout, *spec, language_);
            row.group   = groupTitle.empty() ? groupTitle : tr(language_, title_text::SectionKey(groupTitle), groupTitle);
            row.timing  = TimingOf(*spec, it->second);
            row.toggle  = spec->kind == Kind::kBool;
            row.monitor = spec->kind == Kind::kMonitor;
            row.stepper = spec->kind == Kind::kWeek;
            if (spec->kind == Kind::kWeek)
            {
                row.numeric = true;
                row.rules   = { kWeekEntryLength, text_entry::Charset::kDigits };
            }
            if (const auto tip = layout.tooltips.find(source); tip != layout.tooltips.end())
                row.description = tr(language_, title_text::SettingKey(source, "tip"), tip->second);
            if (isSingle(*spec))
            {
                fillSingle(row, *spec, it->second, language_);
                section.rows.push_back(std::move(row));
                return;
            }
            auto ordered = orderedChoices(layout, *spec, it->second, monitors_, language_);
            if (row.toggle)
            {
                // Off then On, so Right turns it on.
                std::reverse(ordered.list.begin(), ordered.list.end());
                ordered.current = ordered.list.size() - 1 - ordered.current;
            }
            row.choices = std::move(ordered.list);
            row.current = ordered.current;
            section.rows.push_back(std::move(row));
        };
        for (const auto& tab : layout.tabs)
        {
            if (tab.title != name)
                continue;
            for (const auto& group : tab.sections)
            {
                for (const auto& source : group.sources)
                {
                    addRow(source, group.title);
                }
            }
        }
        if (name == "Controls")
            appendBinds(services_, section, language_);
        if (name == "Audio")
            appendRetailAudio(services_, live, section, language_);
        if (!section.rows.empty())
            sections_.push_back(std::move(section));
    }
    liveRevision_ = live.revision;
    for (auto& [key, state] : flags)
    {
        const Spec* spec = findSpec(key);
        if (spec && spec->kind == Kind::kSecret)
            text_entry::Wipe(state.value);
    }

    rowCount_ = 0;
    for (const auto& section : sections_)
        rowCount_ += section.rows.size();
    highlight_ = 0;
    bool found = false;
    for (size_t i = 0; i < rowCount_ && !highlighted.empty(); ++i)
    {
        if (Row(i)->key == highlighted)
        {
            highlight_ = i;
            found      = true;
            break;
        }
    }
    if (!found && !highlighted.empty())
    {
        for (size_t t = 0; t < sections_.size(); ++t)
        {
            if (sections_[t].title == highlightedTab)
            {
                highlight_ = SectionStart(t) + std::min(highlightedAt, sections_[t].rows.size() - 1);
                break;
            }
        }
    }
    remember();
}

void OptionsModel::remember()
{
    const OptionRow* row = Row(highlight_);
    if (row)
        remembered_[sections_[SectionOf(highlight_)].title] = row->key;
}

size_t OptionsModel::SectionStart(size_t section) const
{
    size_t start = 0;
    for (size_t i = 0; i < section && i < sections_.size(); ++i)
        start += sections_[i].rows.size();
    return start;
}

const OptionRow* OptionsModel::Row(size_t index) const
{
    for (const auto& section : sections_)
    {
        if (index < section.rows.size())
            return &section.rows[index];
        index -= section.rows.size();
    }
    return nullptr;
}

size_t OptionsModel::SectionOf(size_t index) const
{
    size_t section = 0;
    for (; section < sections_.size(); ++section)
    {
        if (index < sections_[section].rows.size())
            break;
        index -= sections_[section].rows.size();
    }
    return section;
}

void OptionsModel::Reload()
{
    if (auto available = services_.MonitorChoices())
        monitors_ = std::move(*available);
    pickerOpen_ = false;
    editor_.reset();
    captureKey_.clear();
    captureRefused_ = false;
    rebuild();
}

void OptionsModel::refreshStatus()
{
    for (auto& section : sections_)
    {
        for (auto& row : section.rows)
        {
            if (row.kind != RowKind::kStatus)
                continue;
            if (const auto state = services_.Read(row.key))
                row.choices[0] = { state->value, statusCaption(*state) };
        }
    }
}

void OptionsModel::Poll()
{
    // About twice a second at 60 frames; read the guest only on request.
    constexpr uint32_t kRefreshInterval = 30;
    if (polls_++ % kRefreshInterval == 0)
    {
        services_.RequestLiveRefresh();
        refreshStatus();
    }
    if (services_.LiveRevision() != liveRevision_)
        rebuild();
}

void OptionsModel::Select(size_t index)
{
    if (ModalOpen() || rowCount_ == 0)
        return;
    index = std::min(index, rowCount_ - 1);
    if (index != highlight_)
    {
        outcome_   = ApplyStatus::kSaved;
        noMonitor_ = false;
        copied_    = false;
    }
    highlight_ = index;
    remember();
}

void OptionsModel::Move(int delta)
{
    if (ModalOpen() || rowCount_ == 0)
        return;
    const size_t tab    = Tab();
    const size_t first  = SectionStart(tab);
    const size_t last   = first + sections_[tab].rows.size() - 1;
    const auto   target = std::clamp<int64_t>(static_cast<int64_t>(highlight_) + delta, static_cast<int64_t>(first), static_cast<int64_t>(last));
    Select(static_cast<size_t>(target));
}

bool OptionsModel::SelectTab(size_t tab)
{
    if (ModalOpen() || tab >= sections_.size() || tab == Tab())
        return false;
    size_t target = SectionStart(tab);
    if (const auto it = remembered_.find(sections_[tab].title); it != remembered_.end())
    {
        for (size_t r = 0; r < sections_[tab].rows.size(); ++r)
        {
            if (sections_[tab].rows[r].key == it->second)
            {
                target += r;
                break;
            }
        }
    }
    Select(target);
    return true;
}

bool OptionsModel::StepTab(int direction)
{
    if (ModalOpen() || rowCount_ == 0 || direction == 0)
        return false;
    const size_t count = sections_.size();
    const size_t tab   = Tab();
    return SelectTab(direction < 0 ? (tab + count - 1) % count : (tab + 1) % count);
}

bool OptionsModel::applyChoice(size_t index)
{
    const OptionRow* row = Row(highlight_);
    if (!row || index >= row->choices.size() || (index == row->current && !row->offGrid))
        return false;
    const std::string key = row->key;
    const std::string raw = row->choices[index].raw;
    copied_               = false;
    outcome_              = services_.Apply(key, raw);
    noMonitor_            = false;
    rebuild();
    return true;
}

bool OptionsModel::Step(int direction)
{
    const OptionRow* row = Row(highlight_);
    if (ModalOpen() || !row || direction == 0)
        return false;
    if (row->offGrid)
        return applyChoice(direction > 0 ? row->above : row->below);
    const int64_t target = static_cast<int64_t>(row->current) + (direction > 0 ? 1 : -1);
    if (target < 0 || target >= static_cast<int64_t>(row->choices.size()))
        return false;
    return applyChoice(static_cast<size_t>(target));
}

OptionsModel::Press OptionsModel::Activate()
{
    const OptionRow* row = Row(highlight_);
    if (ModalOpen() || !row)
        return Press::kNone;
    switch (row->kind)
    {
        case RowKind::kAction:
            return services_.Invoke(row->key) ? Press::kRan : Press::kNone;
        case RowKind::kStatus:
            return Press::kNone;
        case RowKind::kBind:
            captureKey_     = row->key;
            captureRefused_ = false;
            copied_         = false;
            outcome_        = ApplyStatus::kSaved;
            noMonitor_      = false;
            return Press::kCapture;
        case RowKind::kText:
            editor_.emplace(row->rules, row->text);
            keyboard_  = text_entry::Keyboard{};
            textKey_   = row->key;
            copied_    = false;
            outcome_   = ApplyStatus::kSaved;
            noMonitor_ = false;
            return Press::kOpenText;
        case RowKind::kChoice:
            break;
    }
    if (row->numeric && row->current < row->choices.size())
    {
        // Start from the plain week: a configured "00000000042" would be cut to ten characters as week 4.
        editor_.emplace(row->rules, NormalizeWeek(row->choices[row->current].raw).value_or(std::string()));
        editor_->SelectAll();
        keyboard_  = text_entry::Keyboard(text_entry::LayoutFor(row->rules.charset));
        textKey_   = row->key;
        copied_    = false;
        outcome_   = ApplyStatus::kSaved;
        noMonitor_ = false;
        return Press::kOpenText;
    }
    if (row->choices.size() < 2)
    {
        noMonitor_ = row->monitor;
        return Press::kNone;
    }
    if (row->stepper)
        return Press::kNone;
    if (row->UsesPicker())
    {
        pickerOpen_   = true;
        pickerCursor_ = row->current;
        return Press::kOpenPicker;
    }
    const size_t next = row->offGrid ? row->above : (row->current + 1) % row->choices.size();
    return applyChoice(next) ? Press::kApplied : Press::kNone;
}

ApplyStatus OptionsModel::PasteSecret(std::string_view text)
{
    const OptionRow* row = Row(highlight_);
    if (ModalOpen() || !row || !row->HasCopyPaste())
        return ApplyStatus::kRejected;
    text_entry::Editor editor(row->rules);
    const std::string  key = row->key;
    copied_                = false;
    noMonitor_             = false;
    if (!editor.Paste(text))
    {
        outcome_ = ApplyStatus::kRejected;
        return outcome_;
    }
    outcome_                 = services_.Apply(key, editor.Text());
    const ApplyStatus status = outcome_;
    rebuild();
    return status;
}

bool OptionsModel::CopySecret(const std::function<void(const char*)>& put)
{
    const OptionRow* row = Row(highlight_);
    if (ModalOpen() || !row || !row->CanCopy() || !put)
        return false;
    auto state = services_.Read(row->key);
    if (!state || state->value.empty())
        return false;
    put(state->value.c_str());
    text_entry::Wipe(state->value);
    copied_    = true;
    outcome_   = ApplyStatus::kSaved;
    noMonitor_ = false;
    return true;
}

ApplyStatus OptionsModel::CommitBinding(std::string_view value)
{
    if (captureKey_.empty())
        return ApplyStatus::kRejected;
    const std::string key = captureKey_;
    captureKey_.clear();
    captureRefused_          = false;
    copied_                  = false;
    noMonitor_               = false;
    outcome_                 = services_.Apply(key, value);
    const ApplyStatus status = outcome_;
    rebuild();
    return status;
}

void OptionsModel::CancelCapture()
{
    captureKey_.clear();
    captureRefused_ = false;
}

void OptionsModel::RefuseCapture()
{
    if (!captureKey_.empty())
        captureRefused_ = true;
}

bool OptionsModel::ClearBind()
{
    const OptionRow* row = Row(highlight_);
    if (ModalOpen() || !row || !row->CanClearBind())
        return false;
    const std::string key = row->key;
    copied_               = false;
    noMonitor_            = false;
    outcome_              = services_.Apply(key, "");
    const bool done       = outcome_ != ApplyStatus::kRejected;
    rebuild();
    return done;
}

bool OptionsModel::ResetBind()
{
    const OptionRow* row = Row(highlight_);
    if (ModalOpen() || !row || !row->CanResetBind())
        return false;
    const std::string key   = row->key;
    const std::string value = row->bindDefault;
    copied_                 = false;
    noMonitor_              = false;
    outcome_                = services_.Apply(key, value);
    const bool done         = outcome_ != ApplyStatus::kRejected;
    rebuild();
    return done;
}

const OptionRow* OptionsModel::TextRow() const
{
    if (!editor_)
        return nullptr;
    for (const auto& section : sections_)
    {
        for (const auto& row : section.rows)
        {
            if (row.key == textKey_)
                return &row;
        }
    }
    return nullptr;
}

ApplyStatus OptionsModel::CommitText()
{
    if (!editor_)
        return ApplyStatus::kRejected;
    const std::string key = textKey_;
    copied_               = false;
    if (editor_->GetRules().charset == text_entry::Charset::kDigits)
    {
        const auto week = NormalizeWeek(editor_->Text());
        if (!week)
        {
            outcome_   = ApplyStatus::kRejected;
            noMonitor_ = false;
            return outcome_;
        }
        outcome_ = services_.Apply(key, *week);
    }
    else
    {
        outcome_ = services_.Apply(key, editor_->Text());
    }
    noMonitor_               = false;
    const ApplyStatus status = outcome_;
    if (status != ApplyStatus::kRejected)
        editor_.reset();
    rebuild();
    return status;
}

void OptionsModel::CancelText()
{
    editor_.reset();
}

void OptionsModel::MovePicker(int delta)
{
    const OptionRow* row = Row(highlight_);
    if (!pickerOpen_ || !row)
        return;
    const auto target = static_cast<int64_t>(pickerCursor_) + delta;
    pickerCursor_     = static_cast<size_t>(std::clamp<int64_t>(target, 0, static_cast<int64_t>(row->choices.size()) - 1));
}

void OptionsModel::SetPickerCursor(size_t index)
{
    const OptionRow* row = Row(highlight_);
    if (!pickerOpen_ || !row)
        return;
    pickerCursor_ = std::min(index, row->choices.size() - 1);
}

bool OptionsModel::CommitPicker()
{
    if (!pickerOpen_)
        return false;
    pickerOpen_ = false;
    return applyChoice(pickerCursor_);
}

void OptionsModel::ClosePicker()
{
    pickerOpen_ = false;
}

const std::string& OptionsModel::Description() const
{
    static const std::string kEmpty;
    const OptionRow*         row = Row(highlight_);
    return row ? row->description : kEmpty;
}

std::string OptionsModel::Note() const
{
    if (noMonitor_)
        return tr(language_, "note.no_monitor", kNoteNoMonitor);
    if (copied_)
        return tr(language_, "note.copied", "Copied.");
    if (!captureKey_.empty())
    {
        return captureRefused_ ? tr(language_, "note.bind_refused", "That key cannot be bound. Press another key.")
                               : tr(language_, "note.keybind_capture", "Press a key. Esc cancels.");
    }
    if (outcome_ == ApplyStatus::kRejected && editor_ && editor_->GetRules().charset == text_entry::Charset::kDigits)
        return tr(language_, "note.week_range", "Enter a week from 1 to 2147483647.");
    switch (outcome_)
    {
        case ApplyStatus::kRejected:
            return tr(language_, "note.rejected", kNoteRejected);
        case ApplyStatus::kUnsaved:
            return tr(language_, "note.unsaved", kNoteUnsaved);
        case ApplyStatus::kSaved:
            break;
    }
    const OptionRow* row = Row(highlight_);
    if (!row)
        return {};
    if (row->unavailable)
        return tr(language_, "note.no_audio", kNoteNoAudio);
    std::string timing;
    switch (row->timing)
    {
        case Timing::kRestart:
            timing = tr(language_, "note.restart", kNoteRestart);
            break;
        case Timing::kNextGame:
            timing = tr(language_, "note.next_game", kNoteNextGame);
            break;
        case Timing::kLive:
            break;
    }
    std::string note;
    const auto  add = [&](const std::string& text)
    {
        if (text.empty())
            return;
        note += note.empty() ? "" : " ";
        note += text;
    };
    if (row->kind == RowKind::kBind && row->conflicts > 0)
    {
        std::string conflict = row->conflicts == 1 ? tr(language_, "note.bind_conflict_one", "Also bound to another action.") : tr(language_, "note.bind_conflict", "Also bound to {0} other actions.");
        if (const size_t at = conflict.find("{0}"); at != std::string::npos)
            conflict.replace(at, 3, std::to_string(row->conflicts));
        add(conflict);
    }
    add(timing);
    if (row->kind == RowKind::kBind && row->bindController)
        add(tr(language_, "note.bind_icons", "Button icons update after a restart."));
    return note;
}

} // namespace rerevved::native_settings
