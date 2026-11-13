// F1 settings layout for ReRevved.

#include "overlay_settings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>

#include <rex/ui/overlay/settings_layout.h>

#include "display_aspect.h"
#include "gameplay_fixes.h"
#include "gameplay_session_settings.h"

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
                if (next && std::isalpha(c) && (i == start || !std::isdigit(static_cast<unsigned char>(result[i - 1]))))
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

// -1 keeps the volume the game loaded; 0 to 9 are the retail Options steps.
std::unordered_map<std::string, std::string> retailVolumeChoices()
{
    std::unordered_map<std::string, std::string> choices{ { "-1", "Game Default" } };
    for (int position = 0; position <= 9; ++position)
        choices.emplace(std::to_string(position), std::to_string(position));
    return choices;
}

using rerevved::host_settings::HostSettings;

std::string boolText(bool value)
{
    return value ? "true" : "false";
}

struct HostRow
{
    std::string_view name;
    std::string (*text)(const HostSettings&);
};

// Includes the local-only settings the host forces off and the presentation speeds that stay native.
constexpr HostRow kHostRows[] = {
    { "ai_civilizations", [](const HostSettings& host)
      {
          return std::to_string(host.aiCivilizations);
      } },
    { "raging_barbarians", [](const HostSettings& host)
      {
          return boolText(host.ragingBarbarians);
      } },
    { "one_city_challenge", [](const HostSettings& host)
      {
          return boolText(host.oneCityChallenge);
      } },
    { "resources", [](const HostSettings& host)
      {
          return std::to_string(host.resources);
      } },
    { "ai_research_bonus_percent", [](const HostSettings& host)
      {
          return std::to_string(host.aiResearchBonusPercent);
      } },
    { "ai_production_bonus_percent", [](const HostSettings& host)
      {
          return std::to_string(host.aiProductionBonusPercent);
      } },
    { "ai_gold_bonus_percent", [](const HostSettings& host)
      {
          return std::to_string(host.aiGoldBonusPercent);
      } },
    { "ai_food_bonus_percent", [](const HostSettings& host)
      {
          return std::to_string(host.aiFoodBonusPercent);
      } },
    { "ai_culture_bonus_percent", [](const HostSettings& host)
      {
          return std::to_string(host.aiCultureBonusPercent);
      } },
    { "domination_min_turns", [](const HostSettings& host)
      {
          return std::to_string(host.dominationMinimumTurns);
      } },
    { "combat_speed", [](const HostSettings&)
      {
          return std::string("normal");
      } },
    { "turn_limit", [](const HostSettings& host)
      {
          return std::string(rerevved::gameplay_fixes::TurnLimitName(host.turnLimit));
      } },
    { "starting_era", [](const HostSettings& host)
      {
          return std::string(rerevved::gameplay_fixes::StartingEraName(host.startingEra));
      } },
    { "technology_cost_multiplier", [](const HostSettings& host)
      {
          return std::to_string(host.technologyCostMultiplier);
      } },
    { "multiplayer_ai_difficulty", [](const HostSettings& host)
      {
          return std::string(rerevved::gameplay_fixes::MultiplayerAiDifficultyName(host.multiplayerAiDifficulty));
      } },
    { "spaceship_scenes", [](const HostSettings&)
      {
          return boolText(true);
      } },
    { "unit_movement", [](const HostSettings&)
      {
          return std::string("normal");
      } },
    { "always_at_war", [](const HostSettings&)
      {
          return boolText(false);
      } },
    { "gotw_custom_week", [](const HostSettings& host)
      {
          return boolText(host.gotwCustomWeek);
      } },
    { "gotw_week", [](const HostSettings& host)
      {
          return host.gotwCustomWeek ? std::to_string(host.gotwWeek) : std::string();
      } },
    { "gotw_standard_rules", [](const HostSettings& host)
      {
          return boolText(host.gotwStandardRules);
      } },
    { "gotw_allow_saving", [](const HostSettings&)
      {
          return boolText(false);
      } },
    { "gotw_custom_civilization", [](const HostSettings&)
      {
          return boolText(false);
      } },
    { "gotw_civilization", [](const HostSettings&)
      {
          return std::string("0");
      } },
};

} // namespace

std::optional<rex::ui::SettingsRowLock> rerevved::HostRowLock(const host_settings::HostSettings* host, std::string_view name)
{
    static const HostSettings native;
    for (const HostRow& row : kHostRows)
    {
        if (row.name == name)
        {
            return rex::ui::SettingsRowLock{ row.text(host ? *host : native), std::string(gameplay_session::kHostSettingLockedReason) };
        }
    }
    return std::nullopt;
}

std::optional<rex::ui::SettingsRowLock> rerevved::LobbyRowLock(const rex::session_data::Status& status, std::string_view name)
{
    if (status.state != rex::session_data::State::kReceived)
    {
        return std::nullopt;
    }
    const auto host = host_settings::Decode(status.bytes);
    if (!host)
    {
        return std::nullopt;
    }
    auto lock = HostRowLock(&*host, name);
    if (lock)
    {
        lock->reason = std::string(gameplay_session::kHostSettingLobbyReason);
    }
    return lock;
}

void rerevved::ConfigureSettingsLayout(rex::ui::SettingsLayout& layout)
{
    layout.row_lock = [](std::string_view name) -> std::optional<rex::ui::SettingsRowLock>
    {
        if (!gameplay_session::NetworkGameActive())
            return LobbyRowLock(rex::session_data::Current(), name);
        return HostRowLock(gameplay_session::ForNetworkGame().get(), name);
    };
    layout.tabs = {
        { "General", { { "", { "game_language", "system_link_profile_name", "show_fps_counter", "skip_intro_movie", "open_log_folder" } } } },
        { "Gameplay",
          {
              { "", { "combat_speed", "unit_movement", "resources", "raging_barbarians", "one_city_challenge", "domination_min_turns", "turn_limit", "starting_era", "technology_cost_multiplier", "ai_civilizations", "always_at_war" } },
              { "AI Handicaps", { "ai_research_bonus_percent", "ai_production_bonus_percent", "ai_gold_bonus_percent", "ai_food_bonus_percent", "ai_culture_bonus_percent" } },
              { "Game of the Week", { "gotw_standard_rules", "gotw_allow_saving", "gotw_custom_week", "gotw_week", "gotw_custom_civilization", "gotw_civilization" } },
          } },
        { "Notifications",
          {
              { "", { "spaceship_scenes", "show_research_prompt", "show_popup_city_status", "show_popup_discoveries", "show_popup_combat_reports", "show_popup_rival_great_people", "show_popup_rankings", "show_popup_rival_war_peace", "show_popup_save_reminder", "show_popup_villages", "show_village_barbarian_messages", "show_popup_city_captures", "show_popup_rival_government", "show_popup_artifacts", "show_popup_spy_reports", "show_popup_victory_warnings", "show_barbarian_messages", "show_popup_trophy_room" } },
          } },
        { "Online",
          {
              { "", { "multiplayer_ai_difficulty", "system_link_status" } },
              { "Connection", { "system_link_invite", "system_link_lan_enabled", "system_link_direct" } },
              { "Matchmaking", { "live_sessions" } },
              { "Discord", { "discord_activity", "discord_invites" } },
          } },
        { "Controls",
          {
              { "", { "input_gamepad_in_background", "input_vibration", "button_icons", "mnk_mode", "mnk_mouse", "mnk_sensitivity" } },
              { "Keybinds", { "@Input/Keybinds/Controller" }, true },
              { "Overlay Hotkeys", { "@Input/Keybinds" }, true },
          } },
        { "Display",
          {
              { "", { "fullscreen", "monitor", "window_size", "display_aspect", "resolution_scale", "vsync", "present_effect", "swap_post_effect" } },
          } },
        { "Audio", { { "", { "audio_volume", "audio_mute", "audio_play_in_background", "retail_music_volume", "retail_effects_volume", "retail_advisor_volume", "retail_leader_volume" } } } },
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
        { "display_aspect", Audience::kPlayer },
        { "resolution_scale", Audience::kPlayer },
        { "vsync", Audience::kPlayer },
        { "present_effect", Audience::kPlayer },
        { "swap_post_effect", Audience::kPlayer },
        { "show_fps_counter", Audience::kPlayer },
        { "audio_volume", Audience::kPlayer },
        { "audio_mute", Audience::kPlayer },
        { "audio_play_in_background", Audience::kPlayer },
        { "retail_music_volume", Audience::kPlayer },
        { "retail_effects_volume", Audience::kPlayer },
        { "retail_advisor_volume", Audience::kPlayer },
        { "retail_leader_volume", Audience::kPlayer },
        { "mnk_mode", Audience::kPlayer },
        { "mnk_mouse", Audience::kPlayer },
        { "mnk_sensitivity", Audience::kPlayer },
        { "input_gamepad_in_background", Audience::kPlayer },
        { "input_vibration", Audience::kPlayer },
        { "button_icons", Audience::kPlayer },
        { "skip_intro_movie", Audience::kPlayer },
        { "game_language", Audience::kPlayer },
        { "@Input/Keybinds", Audience::kPlayer },
        { "combat_speed", Audience::kPlayer },
        { "unit_movement", Audience::kPlayer },
        { "spaceship_scenes", Audience::kPlayer },
        { "ai_civilizations", Audience::kPlayer },
        { "multiplayer_ai_difficulty", Audience::kPlayer },
        { "ai_research_bonus_percent", Audience::kPlayer },
        { "ai_production_bonus_percent", Audience::kPlayer },
        { "ai_gold_bonus_percent", Audience::kPlayer },
        { "ai_food_bonus_percent", Audience::kPlayer },
        { "ai_culture_bonus_percent", Audience::kPlayer },
        { "resources", Audience::kPlayer },
        { "raging_barbarians", Audience::kPlayer },
        { "one_city_challenge", Audience::kPlayer },
        { "domination_min_turns", Audience::kPlayer },
        { "turn_limit", Audience::kPlayer },
        { "starting_era", Audience::kPlayer },
        { "technology_cost_multiplier", Audience::kPlayer },
        { "always_at_war", Audience::kPlayer },
        { "gotw_custom_week", Audience::kPlayer },
        { "gotw_week", Audience::kPlayer },
        { "gotw_custom_civilization", Audience::kPlayer },
        { "gotw_civilization", Audience::kPlayer },
        { "gotw_standard_rules", Audience::kPlayer },
        { "gotw_allow_saving", Audience::kPlayer },
        { "show_research_prompt", Audience::kPlayer },
        { "show_popup_*", Audience::kPlayer },
        { "show_barbarian_messages", Audience::kPlayer },
        { "show_village_barbarian_messages", Audience::kPlayer },
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
    layout.inline_toggles    = { { "gotw_custom_week", "gotw_week", true }, { "gotw_custom_civilization", "gotw_civilization", true } };
    // Restart and startup-only notices are appended by the SDK.
    layout.tooltips = {
        // Gameplay.
        { "combat_speed", "Choose how quickly battle animations play." },
        { "unit_movement", "Choose how quickly units move." },
        { "spaceship_scenes", "Show the rocket scene when a spaceship part is completed." },
        { "ai_civilizations", "Set the number of AI opponents." },
        { "multiplayer_ai_difficulty", "Set the difficulty of AI in multiplayer games you host." },
        { "ai_research_bonus_percent", "Increase AI research speed by this percentage." },
        { "ai_production_bonus_percent", "Increase AI production speed by this percentage." },
        { "ai_gold_bonus_percent", "Increase gold from AI cities by this percentage." },
        { "ai_food_bonus_percent", "Increase food growth in AI cities by this percentage. Starvation is unaffected." },
        { "ai_culture_bonus_percent", "Increase culture from AI cities by this percentage." },
        { "resources", "Choose how resources are placed around each starting location." },
        { "raging_barbarians", "Enable for more aggressive Barbarians. Barbarians cannot capture a civilization's capital before 3000 BC." },
        { "one_city_challenge", "Enable to block Settler production." },
        { "domination_min_turns", "Set the minimum turns before a Domination victory in new games." },
        { "turn_limit", "Set the year a game ends by turn limit." },
        { "starting_era", "Set the era a new game begins in." },
        { "technology_cost_multiplier", "Multiply the cost of every technology by this amount." },
        { "always_at_war", "Enable to keep every civilization at war." },
        { "gotw_custom_week", "Enable to play a different week's game instead of this week's." },
        { "gotw_custom_civilization", "Enable to play Game of the Week as the civilization you choose." },
        { "gotw_standard_rules", "Enable to play Game of the Week without mods or gameplay changes other than Earliest Domination." },
        { "gotw_allow_saving", "Enable to save and load Game of the Week games." },
        { "open_log_folder", "Open the folder containing this session's log file." },
        // Notifications.
        { "show_research_prompt", "Show the research advisor prompt. Turn off to choose research yourself." },
        { "show_popup_rival_war_peace", "Show notices when other civilizations declare war or make peace." },
        { "show_popup_rival_government", "Show notices when other civilizations change government." },
        { "show_popup_city_captures", "Show notices when cities are taken or recaptured." },
        { "show_popup_victory_warnings", "Show warnings about rivals nearing victory." },
        { "show_popup_rival_great_people", "Show notices about rival Great People." },
        { "show_popup_rankings", "Show rankings, greatest city notices and culture flips." },
        { "show_popup_artifacts", "Show artifact rumors and ancient site discoveries." },
        { "show_popup_combat_reports", "Show notices about Great Generals, captures and promotions." },
        { "show_popup_spy_reports", "Show notices about stolen gold and kidnapped Great People." },
        { "show_popup_city_status", "Show notices about disorder, celebrations, culture and blockades." },
        { "show_popup_discoveries", "Show notices about technologies, eras, milestones, trade and roads." },
        { "show_popup_villages", "Show advisor notices after entering villages." },
        { "show_popup_save_reminder", "Show save reminders. Turning this off does not save your game." },
        { "show_popup_trophy_room", "Show invitations to visit the Trophy Room." },
        { "show_barbarian_messages", "Show the Barbarian leader's greeting when you first meet a tribe. Online games always show it." },
        { "show_village_barbarian_messages", "Show Barbarian reward messages. The reward is granted either way." },
        // Online.
        { "system_link_status", "Your current connection to the local network or relay." },
        { "system_link_profile_name", "The name other players see." },
        { "system_link_invite", "Paste your host's invite code to join their relay room." },
        { "system_link_lan_enabled", "Enable to play over your local network without a relay invite." },
        { "system_link_direct", "Enable to connect directly to relay players when possible." },
        { "live_sessions", "Enable to host and find Player Match games in your relay room." },
        { "discord_activity", "Enable to show what you're doing in ReRevved on Discord." },
        { "discord_invites", "Enable to let Discord friends join your hosted Player Match." },
        // Controls.
        { "input_gamepad_in_background", "Enable to keep the gamepad active while the window is inactive." },
        { "input_vibration", "Enable controller rumble." },
        { "button_icons", "Choose which button icons the game shows." },
        { "mnk_mode", "Enable to play with keyboard and mouse." },
        { "mnk_mouse", "Enable to move the right stick with your mouse." },
        { "mnk_sensitivity", "How strongly mouse movement moves the right stick." },
        // Display.
        { "fullscreen", "Enable to fill your selected monitor with the game." },
        { "monitor", "Choose which monitor the game opens on." },
        { "window_size", "Choose the window size outside fullscreen." },
        { "display_aspect", "Ultrawide shows more of the world on wide screens and keeps the interface centered. 4:3 takes effect after restart." },
        { "resolution_scale", "Choose how much detail the game renders. Auto matches your display." },
        { "vsync", "Enable to keep the game at its normal frame pacing." },
        { "present_effect", "Choose how the image is scaled and sharpened for your display." },
        { "swap_post_effect", "Choose how jagged edges are smoothed." },
        { "show_fps_counter", "Enable to show frames per second in the corner." },
        { "skip_intro_movie", "Enable to skip the opening scenes at startup." },
        { "game_language", "Choose the game's language." },
        // Audio.
        { "audio_volume", "Adjust how loudly the game plays." },
        { "audio_mute", "Enable to silence all game audio." },
        { "audio_play_in_background", "Enable to keep hearing the game while another window is active." },
        { "retail_music_volume", "Set the music volume from 0 to 9. The game applies your choice each time a profile loads." },
        { "retail_effects_volume", "Set the sound effects volume from 0 to 9. The game applies your choice each time a profile loads." },
        { "retail_advisor_volume", "Set the advisor speech volume from 0 to 9. The game applies your choice each time a profile loads." },
        { "retail_leader_volume", "Set the leader speech volume from 0 to 9. The game applies your choice each time a profile loads." },
        // Developer.
        { "renderer", "Native is experimental." },
        { "system_link_relay_address", "Your relay server's address and port." },
    };
    layout.choice_labels = {
        // The guest renders 1280x720; the scale multiplies both axes.
        { "resolution_scale", { { "0", "Auto" }, { "1", "1280x720" }, { "2", "2560x1440" }, { "3", "3840x2160" } } },
        { "renderer", { { "xenos", "Xenos" }, { "native", "Native (Experimental)" } } },
        { "window_size", { { "", "Default (1280x720)" }, { "maximized", "Maximized" } } },
        { "button_icons", { { "auto", "Auto" }, { "xbox", "Xbox" }, { "playstation", "PlayStation" }, { "keyboard", "Keyboard" } } },
        { "display_aspect", { { "original", "16:9" }, { "16:10", "16:10" }, { "4:3", "4:3" }, { "auto", "Ultrawide" }, { "21:9", "21:9" }, { "32:9", "32:9" } } },
        { "ai_civilizations", { { "0", "None (Multiplayer)" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4 (Default)" }, { "5", "5" }, { "6", "6" } } },
        { "multiplayer_ai_difficulty", { { "king", "King" }, { "emperor", "Emperor" }, { "deity", "Deity" } } },
        { "gotw_civilization", { { "0", "Roman" }, { "1", "Egyptian" }, { "2", "Greek" }, { "3", "Spanish" }, { "4", "German" }, { "5", "Russian" }, { "6", "Chinese" }, { "7", "American" }, { "8", "Japanese" }, { "9", "French" }, { "10", "Indian" }, { "11", "Arabian" }, { "12", "Aztec" }, { "13", "Zulu" }, { "14", "Mongolian" }, { "15", "English" } } },
        { "domination_min_turns", { { "10", "10 Turns (3000 BC)" }, { "5", "5 Turns (3500 BC)" } } },
        { "turn_limit", { { "2100", "2100 AD" }, { "2200", "2200 AD" }, { "2300", "2300 AD" }, { "2500", "2500 AD" }, { "unlimited", "Unlimited" } } },
        { "starting_era", { { "ancient", "Ancient" }, { "medieval", "Medieval" }, { "industrial", "Industrial" }, { "modern", "Modern" }, { "future", "Future" } } },
        { "technology_cost_multiplier", { { "1", "1x" }, { "2", "2x" }, { "3", "3x" }, { "5", "5x" }, { "10", "10x" }, { "20", "20x" }, { "50", "50x" }, { "100", "100x" } } },
        { "resources", { { "0", "Normal" }, { "1", "Balanced" }, { "3", "Legendary" } } },
        { "retail_music_volume", retailVolumeChoices() },
        { "retail_effects_volume", retailVolumeChoices() },
        { "retail_advisor_volume", retailVolumeChoices() },
        { "retail_leader_volume", retailVolumeChoices() },
        { "swap_post_effect", { { "none", "Off" }, { "fxaa", "FXAA" }, { "fxaa_extreme", "FXAA (Extreme)" } } },
        { "game_language", { { "1", "English" }, { "3", "Deutsch" }, { "4", "Fran\u00e7ais" }, { "5", "Espa\u00f1ol" }, { "6", "Italiano" } } },
        { "present_effect", { { "cas", "CAS" }, { "fsr", "FSR" }, { "fsr2", "FSR 2" }, { "fsr3", "FSR 3" } } },
    };
    // The 4:3 preset renders from 640x480.
    if (rerevved::display_aspect::NarrowLatched())
        layout.choice_labels["resolution_scale"] = { { "0", "Auto" }, { "1", "640x480" }, { "2", "1280x960" }, { "3", "1920x1440" } };
    layout.labels = {
        { "fullscreen", "Fullscreen" },
        { "resolution_scale", "Render Resolution" },
        { "vsync", "VSync" },
        { "present_effect", "Upscaling" },
        { "swap_post_effect", "Anti-Aliasing" },
        { "show_fps_counter", "FPS Counter" },
        { "window_size", "Window Size" },
        { "display_aspect", "Aspect Ratio" },
        { "audio_volume", "Volume" },
        { "audio_mute", "Mute" },
        { "audio_play_in_background", "Play Sound in Background" },
        { "retail_music_volume", "Music Volume" },
        { "retail_effects_volume", "Effects Volume" },
        { "retail_advisor_volume", "Advisor Speech Volume" },
        { "retail_leader_volume", "Leader Speech Volume" },
        { "mnk_sensitivity", "Mouse Sensitivity" },
        { "mnk_mode", "Keyboard and Mouse" },
        { "mnk_mouse", "Mouse Aim" },
        { "input_gamepad_in_background", "Gamepad in Background" },
        { "input_vibration", "Vibration" },
        { "button_icons", "Button Icons" },
        { "skip_intro_movie", "Skip Intro" },
        { "game_language", "Language" },
        { "combat_speed", "Combat Speed" },
        { "unit_movement", "Unit Movement" },
        { "spaceship_scenes", "Spaceship Scenes" },
        { "show_research_prompt", "Research Advisor Prompt" },
        { "show_popup_rival_war_peace", "Rival War and Peace News" },
        { "show_popup_rival_government", "Rival Government Changes" },
        { "show_popup_city_captures", "City Capture Reports" },
        { "show_popup_victory_warnings", "Victory Race Warnings" },
        { "show_popup_rival_great_people", "Rival Great People" },
        { "show_popup_rankings", "Rankings and Culture News" },
        { "show_popup_artifacts", "Artifact and Ancient Site News" },
        { "show_popup_combat_reports", "Combat Reports" },
        { "show_popup_spy_reports", "Spy Reports" },
        { "show_popup_city_status", "City Status Reports" },
        { "show_popup_discoveries", "Discovery and Progress News" },
        { "show_popup_villages", "Village Outcomes" },
        { "show_popup_save_reminder", "Save Reminder" },
        { "show_popup_trophy_room", "Trophy Room Offer" },
        { "show_barbarian_messages", "Barbarian Leader Messages" },
        { "show_village_barbarian_messages", "Village Barbarian Messages" },
        { "ai_civilizations", "AI Civilizations" },
        { "multiplayer_ai_difficulty", "AI Difficulty (Multiplayer)" },
        { "ai_research_bonus_percent", "AI Research Bonus" },
        { "ai_production_bonus_percent", "AI Production Bonus" },
        { "ai_gold_bonus_percent", "AI Gold Bonus" },
        { "ai_food_bonus_percent", "AI Food Bonus" },
        { "ai_culture_bonus_percent", "AI Culture Bonus" },
        { "resources", "Resources" },
        { "raging_barbarians", "Raging Barbarians" },
        { "one_city_challenge", "One City Challenge" },
        { "domination_min_turns", "Earliest Domination" },
        { "turn_limit", "Turn Limit" },
        { "starting_era", "Starting Era" },
        { "technology_cost_multiplier", "Technology Cost" },
        { "always_at_war", "Always At War" },
        { "gotw_custom_week", "Week" },
        { "gotw_week", "Week" },
        { "gotw_custom_civilization", "Civilization" },
        { "gotw_civilization", "Civilization" },
        { "gotw_standard_rules", "Standard Rules" },
        { "gotw_allow_saving", "Allow Saving" },
        { "open_log_folder", "Open Log Folder" },
        { "system_link_status", "Connection Status" },
        { "system_link_profile_name", "Profile Name" },
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
