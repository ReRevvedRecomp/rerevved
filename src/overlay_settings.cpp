// F1 settings layout for ReRevved.

#include "app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

#include <rex/ui/overlay/settings_layout.h>

namespace
{

using rex::cvar::Audience;

// "Skip research prompt at turn start" -> "Skip Research Prompt at Turn Start".
// Minor words stay lowercase unless first or last; capitals already present stay.
std::string titleCase(std::string_view text)
{
    static constexpr std::array<std::string_view, 17> minor = {
        "a", "an", "and", "as", "at", "but", "by", "for", "in", "nor", "of", "on", "or", "per", "the", "to", "with"
    };
    std::string result(text);
    size_t      start = 0;
    while (start < result.size())
    {
        size_t end = result.find(' ', start);
        if (end == std::string::npos)
            end = result.size();
        const std::string_view word(result.data() + start, end - start);
        const bool             edge = start == 0 || end == result.size();
        if (edge || std::find(minor.begin(), minor.end(), word) == minor.end())
        {
            // Capitalize the word and each part after a hyphen or slash.
            bool next = true;
            for (size_t i = start; i < end; ++i)
            {
                unsigned char c = static_cast<unsigned char>(result[i]);
                if (next && std::isalpha(c))
                {
                    result[i] = static_cast<char>(std::toupper(c));
                    next      = false;
                }
                else if (c == '-' || c == '/')
                    next = true;
            }
        }
        start = end + 1;
    }
    return result;
}

} // namespace

void rerevved::App::OnConfigureSettings(rex::ui::SettingsLayout& layout)
{
    layout.tabs = {
        { "Gameplay",
          {
              { "", { "combat_speed", "ai_civilizations" } },
              { "", { "resources" } },
              { "", { "raging_barbarians" } },
              { "", { "one_city_challenge" } },
              { "Game of the Week", { "gotw_custom_week", "gotw_week", "gotw_standard_rules" } },
              { "Support", { "open_log_folder" } },
          } },
        { "Notifications",
          {
              { "", { "hide_research_prompt", "hide_popup_rival_war_peace", "hide_popup_rival_government", "hide_popup_city_captures", "hide_popup_victory_warnings", "hide_popup_rival_great_people", "hide_popup_rankings", "hide_popup_artifacts", "hide_popup_combat_reports", "hide_popup_spy_reports", "hide_popup_city_status", "hide_popup_discoveries", "hide_popup_villages", "hide_popup_save_reminder", "hide_popup_trophy_room", "hide_popup_government_available", "hide_barbarian_messages", "hide_village_barbarian_messages" } },
          } },
        { "Online",
          {
              { "", { "system_link_status", "system_link_profile_name" } },
              { "Connection", { "system_link_invite", "system_link_lan_enabled", "system_link_direct" } },
              { "Matchmaking", { "live_sessions" } },
              { "Discord", { "discord_activity", "discord_invites" } },
          } },
        { "Controls",
          {
              { "", { "input_gamepad_in_background", "input_vibration", "mnk_mode", "mnk_mouse", "mnk_sensitivity" } },
              { "Keybinds", { "@Input/Keybinds/Controller" }, true },
              { "Overlay Hotkeys", { "@Input/Keybinds" }, true },
          } },
        { "Display",
          {
              { "", { "fullscreen", "monitor", "window_size", "resolution_scale", "vsync", "present_effect", "swap_post_effect", "show_fps_counter" } },
          } },
        { "Audio", { { "", { "audio_volume", "audio_mute", "audio_play_in_background" } } } },
        { "Developer",
          {
              { "Renderer", { "renderer", "@Graphics" } },
              { "Online", { "system_link_relay_address" } },
          } },
    };

    // F1 shows only the rows listed here. Everything else stays in the config
    // file, command line and console; settings_show_developer adds the
    // Developer tab.
    layout.audience = {
        { "*", Audience::kHidden },
        // Player rows.
        { "fullscreen", Audience::kPlayer },
        { "monitor", Audience::kPlayer },
        { "window_size", Audience::kPlayer },
        { "resolution_scale", Audience::kPlayer },
        { "vsync", Audience::kPlayer },
        { "present_effect", Audience::kPlayer },
        { "swap_post_effect", Audience::kPlayer },
        { "show_fps_counter", Audience::kPlayer },
        { "audio_volume", Audience::kPlayer },
        { "audio_mute", Audience::kPlayer },
        { "audio_play_in_background", Audience::kPlayer },
        { "mnk_mode", Audience::kPlayer },
        { "mnk_mouse", Audience::kPlayer },
        { "mnk_sensitivity", Audience::kPlayer },
        { "input_gamepad_in_background", Audience::kPlayer },
        { "input_vibration", Audience::kPlayer },
        { "@Input/Keybinds", Audience::kPlayer },
        { "combat_speed", Audience::kPlayer },
        { "ai_civilizations", Audience::kPlayer },
        { "resources", Audience::kPlayer },
        { "raging_barbarians", Audience::kPlayer },
        { "one_city_challenge", Audience::kPlayer },
        { "gotw_custom_week", Audience::kPlayer },
        { "gotw_week", Audience::kPlayer },
        { "gotw_standard_rules", Audience::kPlayer },
        { "hide_research_prompt", Audience::kPlayer },
        { "hide_popup_*", Audience::kPlayer },
        { "hide_barbarian_messages", Audience::kPlayer },
        { "hide_village_barbarian_messages", Audience::kPlayer },
        { "open_log_folder", Audience::kPlayer },
        { "system_link_status", Audience::kPlayer },
        { "system_link_profile_name", Audience::kPlayer },
        { "system_link_invite", Audience::kPlayer },
        { "system_link_lan_enabled", Audience::kPlayer },
        { "system_link_direct", Audience::kPlayer },
        { "live_sessions", Audience::kPlayer },
        { "discord_activity", Audience::kPlayer },
        { "discord_invites", Audience::kPlayer },
        // The debug overlay has no key and no row.
        { "bind_debug_overlay", Audience::kHidden },
        // Developer rows. Native draws nothing without developer-only shader
        // catalogs, so players cannot pick it; the @Graphics controls tune
        // either backend and are for testing.
        { "renderer", Audience::kDeveloper },
        { "@Graphics", Audience::kDeveloper },
        { "system_link_relay_address", Audience::kDeveloper },
    };

    layout.format_label      = titleCase;
    layout.show_descriptions = false;
    layout.float_decimals    = { { "mnk_sensitivity", 2 } };
    layout.inline_toggles    = { { "gotw_custom_week", "gotw_week", true } };
    layout.tooltips          = { { "renderer", "Native is experimental. Applies after restart." },
                                 { "gotw_custom_week", "Enable to play a different week's game instead of this week's." },
                                 { "gotw_standard_rules", "Enable to play Game of the Week without gameplay changes." } };
    layout.choice_labels     = {
        // The guest renders 1280x720; the scale multiplies both axes.
        { "resolution_scale", { { "1", "1280x720" }, { "2", "2560x1440" }, { "3", "3840x2160" } } },
        { "renderer", { { "xenos", "Xenos" }, { "native", "Native (Experimental)" } } },
        { "window_size", { { "", "Default (1280x720)" }, { "maximized", "Maximized" } } },
        { "ai_civilizations", { { "0", "None (Multiplayer)" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4 (Default)" } } },
        { "resources", { { "0", "Normal" }, { "1", "Balanced" }, { "3", "Legendary" } } },
        { "swap_post_effect", { { "none", "Off" }, { "fxaa", "FXAA" }, { "fxaa_extreme", "FXAA (Extreme)" } } },
        { "present_effect", { { "cas", "CAS" }, { "fsr", "FSR" }, { "fsr2", "FSR 2" }, { "fsr3", "FSR 3" } } },
    };
    layout.labels = {
        { "fullscreen", "Fullscreen" },
        { "resolution_scale", "Render Resolution" },
        { "vsync", "VSync" },
        { "present_effect", "Upscaling" },
        { "swap_post_effect", "Anti-Aliasing" },
        { "show_fps_counter", "FPS Counter" },
        { "window_size", "Window Size" },
        { "audio_volume", "Volume" },
        { "audio_mute", "Mute" },
        { "audio_play_in_background", "Play Sound in Background" },
        { "mnk_sensitivity", "Mouse Sensitivity" },
        { "input_gamepad_in_background", "Gamepad in Background" },
        { "input_vibration", "Vibration" },
        { "combat_speed", "Combat Speed" },
        { "hide_research_prompt", "Disable Research Advisor Prompt" },
        { "hide_popup_rival_war_peace", "Disable Rival War and Peace News" },
        { "hide_popup_rival_government", "Disable Rival Government Changes" },
        { "hide_popup_city_captures", "Disable City Capture Reports" },
        { "hide_popup_victory_warnings", "Disable Victory Race Warnings" },
        { "hide_popup_rival_great_people", "Disable Rival Great People and Wonders" },
        { "hide_popup_rankings", "Disable Rankings and Culture News" },
        { "hide_popup_artifacts", "Disable Artifact and Ancient Site News" },
        { "hide_popup_combat_reports", "Disable Combat Reports" },
        { "hide_popup_spy_reports", "Disable Spy Reports" },
        { "hide_popup_city_status", "Disable City Status Reports" },
        { "hide_popup_discoveries", "Disable Discovery and Progress News" },
        { "hide_popup_villages", "Disable Village Outcomes" },
        { "hide_popup_save_reminder", "Disable Save Reminder" },
        { "hide_popup_trophy_room", "Disable Trophy Room Offer" },
        { "hide_popup_government_available", "Disable New Government Prompt" },
        { "hide_barbarian_messages", "Disable Barbarian Leader Messages" },
        { "hide_village_barbarian_messages", "Disable Village Barbarian Messages" },
        { "ai_civilizations", "AI Civilizations" },
        { "resources", "Resources" },
        { "raging_barbarians", "Raging Barbarians" },
        { "one_city_challenge", "One City Challenge" },
        { "gotw_custom_week", "Week" },
        { "gotw_week", "Week" },
        { "gotw_standard_rules", "Standard Rules" },
        { "open_log_folder", "Open Log Folder" },
        { "system_link_status", "Status" },
        { "system_link_profile_name", "Player Name" },
        { "system_link_invite", "Invite Code" },
        { "system_link_lan_enabled", "Direct LAN" },
        { "system_link_direct", "Peer-to-Peer" },
        { "live_sessions", "Player Match Through Relay" },
        { "discord_activity", "Rich Presence" },
        { "discord_invites", "Join Button" },
        { "system_link_relay_address", "Relay Address" },
        { "renderer", "Renderer" },
        { "monitor", "Monitor" },
    };
}
