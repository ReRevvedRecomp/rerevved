#include "native_settings.h"

#include "overlay_settings.h"
#include "page_set.h"
#include "retail_audio.h"
#include "title_text.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>

REXCVAR_DEFINE_INT32(native_settings_test_int, 3, "Test", "Native settings test integer").range(0, 10);
REXCVAR_DEFINE_STRING(native_settings_test_enum, "low", "Test", "Native settings test enum")
    .allowed({ "low", "high" })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_settings_test_bool, false, "Test", "Native settings test bool");
REXCVAR_DEFINE_DOUBLE(native_settings_test_double, 1.5, "Test", "Native settings test real").range(0.01, 10.0);
REXCVAR_DEFINE_COMMAND(native_settings_test_command, []
                       {
                       },
                       "Test",
                       "Native settings test command");

namespace ns = rerevved::native_settings;

namespace
{

using namespace std::chrono_literals;

ns::FlagState boolState(bool value, bool restart = false)
{
    ns::FlagState state;
    state.kind            = ns::FlagKind::kBool;
    state.value           = value ? "true" : "false";
    state.requiresRestart = restart;
    return state;
}

ns::FlagState intState(std::string value, double min, double max)
{
    ns::FlagState state;
    state.kind  = ns::FlagKind::kInteger;
    state.value = std::move(value);
    state.min   = min;
    state.max   = max;
    return state;
}

ns::FlagState enumState(std::string value, std::vector<std::string> allowed)
{
    ns::FlagState state;
    state.kind    = ns::FlagKind::kString;
    state.value   = std::move(value);
    state.allowed = std::move(allowed);
    return state;
}

std::map<std::string, ns::FlagState> defaultFlags()
{
    std::map<std::string, ns::FlagState> flags;
    for (const auto key : ns::CuratedKeys())
        flags[std::string(key)] = boolState(false);
    flags["combat_speed"]         = enumState("normal", { "normal", "fast" });
    flags["resources"]            = intState("0", 0, 3);
    flags["domination_min_turns"] = enumState("10", { "10", "5" });
    flags["turn_limit"]           = enumState("2100", { "2100", "2200", "2300", "2500", "unlimited" });
    flags["starting_era"]         = enumState("ancient", { "ancient", "medieval", "industrial", "modern" });
    flags["ai_civilizations"]     = intState("4", 0, 6);
    for (const char* key : { "ai_research_bonus_percent", "ai_production_bonus_percent", "ai_gold_bonus_percent", "ai_food_bonus_percent", "ai_culture_bonus_percent" })
        flags[key] = intState("0", 0, 200);
    flags["gotw_civilization"]                        = intState("0", 0, 15);
    flags["gotw_week"]                                = enumState("1", {});
    flags["button_icons"]                             = enumState("auto", { "auto", "xbox", "playstation", "keyboard" });
    flags["mnk_sensitivity"]                          = ns::FlagState{ ns::FlagKind::kDouble, "1.000000", {}, 0.01, 10.0, false };
    flags["monitor"]                                  = intState("0", 0, 16);
    flags["window_size"]                              = enumState("", { "", "1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160", "maximized" });
    flags["display_aspect"]                           = enumState("original", { "original", "auto", "21:9", "32:9" });
    flags["resolution_scale"]                         = intState("1", 0, 8);
    flags["present_effect"]                           = enumState("bilinear", { "bilinear", "cas", "fsr", "fsr2", "fsr3" });
    flags["swap_post_effect"]                         = enumState("none", { "none", "fxaa", "fxaa_extreme" });
    flags["audio_volume"]                             = intState("100", 0, 100);
    flags["open_log_folder"]                          = ns::FlagState{};
    flags["system_link_status"]                       = enumState("Off", {});
    flags["system_link_profile_name"]                 = enumState("", {});
    flags["system_link_invite"]                       = enumState("", {});
    flags["system_link_profile_name"].requiresRestart = true;
    flags["system_link_invite"].requiresRestart       = true;
    return flags;
}

struct BindDef
{
    const char* key;
    const char* category;
    const char* defaultValue;
    const char* description;
};

constexpr const char* kControllerCategory = "Input/Keybinds/Controller";
constexpr const char* kHotkeyCategory     = "Input/Keybinds/System";

constexpr BindDef kBinds[] = {
    { "keybind_a", kControllerCategory, "Semicolon", "A button" },
    { "keybind_b", kControllerCategory, "Quote", "B button" },
    { "keybind_x", kControllerCategory, "L", "X button" },
    { "keybind_y", kControllerCategory, "P", "Y button" },
    { "keybind_left_trigger", kControllerCategory, "Q", "Left trigger" },
    { "keybind_right_trigger", kControllerCategory, "E", "Right trigger" },
    { "keybind_left_shoulder", kControllerCategory, "1", "Left shoulder" },
    { "keybind_right_shoulder", kControllerCategory, "3", "Right shoulder" },
    { "keybind_lstick_up", kControllerCategory, "W", "Left stick up" },
    { "keybind_lstick_down", kControllerCategory, "S", "Left stick down" },
    { "keybind_lstick_left", kControllerCategory, "A", "Left stick left" },
    { "keybind_lstick_right", kControllerCategory, "D", "Left stick right" },
    { "keybind_lstick_press", kControllerCategory, "F", "Left stick press" },
    { "keybind_rstick_up", kControllerCategory, "Up", "Right stick up" },
    { "keybind_rstick_down", kControllerCategory, "Down", "Right stick down" },
    { "keybind_rstick_left", kControllerCategory, "Left", "Right stick left" },
    { "keybind_rstick_right", kControllerCategory, "Right", "Right stick right" },
    { "keybind_rstick_press", kControllerCategory, "K", "Right stick press" },
    { "keybind_dpad_up", kControllerCategory, "Shift+Up", "D-pad up" },
    { "keybind_dpad_down", kControllerCategory, "Shift+Down", "D-pad down" },
    { "keybind_dpad_left", kControllerCategory, "Shift+Left", "D-pad left" },
    { "keybind_dpad_right", kControllerCategory, "Shift+Right", "D-pad right" },
    { "keybind_back", kControllerCategory, "Z", "Back button" },
    { "keybind_start", kControllerCategory, "X", "Start button" },
    { "keybind_guide", kControllerCategory, "", "Guide button" },
    { "bind_console", kHotkeyCategory, "Backtick", "Toggle console overlay" },
    { "bind_settings", kHotkeyCategory, "F1", "Toggle settings overlay" },
    { "bind_mod_manager", kHotkeyCategory, "F2", "Toggle mod manager overlay" },
    { "bind_achievements", kHotkeyCategory, "F3", "Toggle achievements overlay" },
    { "bind_fps_counter", kHotkeyCategory, "F4", "Toggle FPS counter" },
};

class FakeServices final : public ns::Services
{
public:
    FakeServices()
    : flags(defaultFlags())
    {
        rerevved::ConfigureSettingsLayout(layout);
        for (const auto& bind : kBinds)
        {
            ns::FlagState state = enumState(bind.defaultValue, {});
            state.category      = bind.category;
            state.description   = bind.description;
            state.defaultValue  = bind.defaultValue;
            flags[bind.key]     = state;
            binds.push_back(bind.key);
        }
        monitors = std::vector<ns::Choice>{ { "0", "Default" }, { "1", "1: Display (1920x1080)" }, { "2", "2: Other (2560x1440)" } };
    }

    const rex::ui::SettingsLayout& Layout() const override
    {
        return layout;
    }

    std::optional<ns::FlagState> Read(std::string_view key) override
    {
        const auto it = flags.find(std::string(key));
        if (it == flags.end())
            return std::nullopt;
        return it->second;
    }

    std::optional<std::vector<ns::Choice>> MonitorChoices() override
    {
        return monitors;
    }

    std::vector<std::string> BindKeys() override
    {
        return binds;
    }

    bool Invoke(std::string_view key) override
    {
        invoked.emplace_back(key);
        return invokeResult;
    }

    ns::ApplyStatus Apply(std::string_view key, std::string_view raw) override
    {
        applied.emplace_back(std::string(key), std::string(raw));
        if (status == ns::ApplyStatus::kRejected)
            return status;
        flags[std::string(key)].value = std::string(raw);
        return status;
    }

    ns::LiveVolumes ReadLiveVolumes() override
    {
        ns::LiveVolumes result;
        result.revision = revision;
        for (const auto& [channel, value] : live)
            result.volume[channel] = value;
        return result;
    }

    uint64_t LiveRevision() override
    {
        return revision;
    }

    void RequestLiveRefresh() override
    {
        ++refreshes;
    }

    rex::ui::SettingsLayout                          layout;
    std::map<std::string, ns::FlagState>             flags;
    std::map<size_t, ns::LiveVolume>                 live;
    uint64_t                                         revision  = 0;
    int                                              refreshes = 0;
    std::optional<std::vector<ns::Choice>>           monitors;
    std::vector<std::pair<std::string, std::string>> applied;
    std::vector<std::string>                         invoked;
    std::vector<std::string>                         binds;
    bool                                             invokeResult = true;
    ns::ApplyStatus                                  status       = ns::ApplyStatus::kSaved;
};

std::vector<std::string> captionsOf(const ns::OptionsModel& model, std::string_view title)
{
    std::vector<std::string> result;
    for (const auto& section : model.Sections())
    {
        if (section.title != title)
            continue;
        for (const auto& row : section.rows)
            result.push_back(row.label + ": " + row.Caption());
        return result;
    }
    FAIL("no section " << std::string(title));
    return result;
}

} // namespace

TEST_CASE("native_settings_rows_keep_the_f1_order_and_captions", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);

    // The game language row needs its flag; the other three General rows do not.
    // The log folder action follows the F1 General rows (F1 keeps it under Gameplay).
    CHECK(captionsOf(model, "General") == std::vector<std::string>{ "FPS Counter: Off", "Skip Intro: Off", "Open Log Folder: Open" });

    CHECK(captionsOf(model, "Gameplay") == std::vector<std::string>{ "Combat Speed: Normal",
                                                                     "Resources: Normal",
                                                                     "Raging Barbarians: Off",
                                                                     "One City Challenge: Off",
                                                                     "Earliest Domination: 10 Turns (3000 BC)",
                                                                     "Turn Limit: 2100 AD",
                                                                     "Starting Era: Ancient",
                                                                     "AI Civilizations: 4 (Default)",
                                                                     "AI Research Bonus: 0",
                                                                     "AI Production Bonus: 0",
                                                                     "AI Gold Bonus: 0",
                                                                     "AI Food Bonus: 0",
                                                                     "AI Culture Bonus: 0",
                                                                     "Standard Rules: Off",
                                                                     "Allow Saving: Off",
                                                                     "Custom Week: Off",
                                                                     "Custom Civilization: Off" });

    const auto notifications = captionsOf(model, "Notifications");
    CHECK(notifications.size() == 19);
    CHECK(notifications.front() == "Skip Rival Spaceship Scenes: Off");
    CHECK(notifications[1] == "Disable Research Advisor Prompt: Off");
    CHECK(notifications.back() == "Disable Trophy Room Offer: Off");

    CHECK(captionsOf(model, "Online") == std::vector<std::string>{ "Connection Status: Off",
                                                                   "Profile Name: Default",
                                                                   "Invite Code: Not set",
                                                                   "Direct LAN: Off",
                                                                   "Peer-to-Peer: Off",
                                                                   "Player Match Through Relay: Off",
                                                                   "Rich Presence: Off",
                                                                   "Join Button: Off" });
    CHECK(captionsOf(model, "Controls") == std::vector<std::string>{ "Gamepad in Background: Off",
                                                                     "Vibration: Off",
                                                                     "Button Icons: Auto",
                                                                     "Keyboard and Mouse: Off",
                                                                     "Mouse Aim: Off",
                                                                     "Mouse Sensitivity: 1.00",
                                                                     "A Button: ;",
                                                                     "B Button: '",
                                                                     "X Button: L",
                                                                     "Y Button: P",
                                                                     "Left Trigger: Q",
                                                                     "Right Trigger: E",
                                                                     "Left Shoulder: 1",
                                                                     "Right Shoulder: 3",
                                                                     "Left Stick Up: W",
                                                                     "Left Stick Down: S",
                                                                     "Left Stick Left: A",
                                                                     "Left Stick Right: D",
                                                                     "Left Stick Press: F",
                                                                     "Right Stick Up: Up",
                                                                     "Right Stick Down: Down",
                                                                     "Right Stick Left: Left",
                                                                     "Right Stick Right: Right",
                                                                     "Right Stick Press: K",
                                                                     "D-Pad Up: Shift+Up",
                                                                     "D-Pad Down: Shift+Down",
                                                                     "D-Pad Left: Shift+Left",
                                                                     "D-Pad Right: Shift+Right",
                                                                     "Back Button: Z",
                                                                     "Start Button: X",
                                                                     "Guide Button: (none)",
                                                                     "Toggle Console Overlay: `",
                                                                     "Toggle Settings Overlay: F1",
                                                                     "Toggle Mod Manager Overlay: F2",
                                                                     "Toggle Achievements Overlay: F3",
                                                                     "Toggle FPS Counter: F4" });
    CHECK(captionsOf(model, "Display") == std::vector<std::string>{ "Fullscreen: Off",
                                                                    "Monitor: Default",
                                                                    "Window Size: Default (1280x720)",
                                                                    "Aspect Ratio: 16:9",
                                                                    "Render Resolution: 1280x720",
                                                                    "VSync: Off",
                                                                    "Upscaling: Bilinear",
                                                                    "Anti-Aliasing: Off" });
    CHECK(captionsOf(model, "Audio") == std::vector<std::string>{ "Volume: 100", "Mute: Off", "Play Sound in Background: Off" });
}

TEST_CASE("native_settings_excludes_keybinds_and_developer_rows", "[ctest][native-settings]")
{
    const auto keys = ns::CuratedKeys();
    for (const char* excluded : { "renderer", "system_link_relay_address", "@Input/Keybinds", "@Input/Keybinds/Controller", "@Graphics" })
        CHECK(std::find(keys.begin(), keys.end(), excluded) == keys.end());
    for (const char* included : { "open_log_folder", "system_link_status", "system_link_profile_name", "system_link_invite" })
        CHECK(std::find(keys.begin(), keys.end(), included) != keys.end());

    FakeServices services;
    services.flags["renderer"] = enumState("xenos", { "xenos", "native" });
    ns::OptionsModel model(services);
    for (const auto& section : model.Sections())
    {
        for (const auto& row : section.rows)
            CHECK(row.key != "renderer");
    }
}

TEST_CASE("options_model_keeps_the_log_folder_in_general_not_gameplay", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    for (const auto& row : model.Sections()[1].rows)
    {
        CAPTURE(row.key);
        CHECK(row.key != "open_log_folder");
    }
    const auto& general = model.Sections()[0];
    REQUIRE(general.title == "General");
    CHECK(general.rows.back().key == "open_log_folder");
    CHECK(general.rows.back().group.empty());

    // F1's layout is unchanged: the action stays under Gameplay > Support there.
    rex::ui::SettingsLayout layout;
    rerevved::ConfigureSettingsLayout(layout);
    bool found = false;
    for (const auto& tab : layout.tabs)
    {
        for (const auto& section : tab.sections)
        {
            for (const auto& source : section.sources)
            {
                if (source == "open_log_folder")
                {
                    CHECK(tab.title == "Gameplay");
                    CHECK(section.title == "Support");
                    found = true;
                }
            }
        }
    }
    CHECK(found);

    // Without the command the row is left out.
    services.flags.erase("open_log_folder");
    ns::OptionsModel without(services);
    CHECK(without.Sections()[0].rows.back().key == "skip_intro_movie");
}

TEST_CASE("native_settings_skips_sections_and_rows_without_qualifying_settings", "[ctest][native-settings]")
{
    FakeServices services;
    for (const auto key : { "audio_volume", "audio_mute", "audio_play_in_background" })
        services.flags.erase(key);
    services.flags["present_effect"] = enumState("bilinear", { "bilinear" });
    services.flags["vsync"]          = enumState("on", { "on", "off" });
    {
        ns::OptionsModel model(services);
        CHECK(model.Sections().size() == 6);
        const auto display = captionsOf(model, "Display");
        CHECK(display.front() == "Fullscreen: Off");
        CHECK(std::none_of(display.begin(), display.end(), [](const std::string& row)
                           {
                               return row.starts_with("Upscaling") || row.starts_with("VSync");
                           }));
    }

    FakeServices empty;
    empty.flags.clear();
    ns::OptionsModel none(empty);
    CHECK(none.Sections().empty());
    CHECK(none.RowCount() == 0);
}

TEST_CASE("native_settings_enums_offer_only_the_allowed_values", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["present_effect"] = enumState("cas", { "bilinear", "cas", "fsr" });
    ns::OptionsModel model(services);
    size_t           index = 0;
    while (index < model.RowCount() && model.Row(index)->key != "present_effect")
        ++index;
    REQUIRE(index < model.RowCount());
    model.Select(index);
    const auto& row = *model.Row(index);
    REQUIRE(row.choices.size() == 3);
    CHECK(row.choices[0].raw == "bilinear");
    CHECK(row.choices[1].raw == "cas");
    CHECK(row.choices[2].raw == "fsr");
    CHECK(row.current == 1);

    REQUIRE(model.Step(-1));
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied[0] == std::pair<std::string, std::string>{ "present_effect", "bilinear" });
}

TEST_CASE("native_settings_sanitizes_dynamic_captions", "[ctest][native-settings]")
{
    CHECK(ns::SanitizeCaption("  Dell <U2720Q> 100%\n", 80) == "Dell U2720Q 100");
    CHECK(ns::SanitizeCaption("1: LG (3840x2160)", 80) == "1: LG (3840x2160)");
    CHECK(ns::SanitizeCaption("abcdefghij", 4) == "abcd");
    CHECK(ns::SanitizeCaption("ab   ", 80) == "ab");
    CHECK(ns::SanitizeCaption("   ", 80).empty());
    CHECK(ns::SanitizeCaption("caf\xC3\xA9", 80) == "caf");
}

TEST_CASE("native_settings_run_on_ui_returns_the_job_result", "[ctest][native-settings]")
{
    const ns::Poster immediate = [](std::function<void()> job)
    {
        job();
        return true;
    };
    const auto result = ns::RunOnUi<int>(immediate, []
                                         {
                                             return true;
                                         },
                                         1s,
                                         []
                                         {
                                             return 7;
                                         });
    REQUIRE(result);
    CHECK(*result == 7);

    std::thread      worker;
    const ns::Poster threaded = [&](std::function<void()> job)
    {
        worker = std::thread([job = std::move(job)]
                             {
                                 std::this_thread::sleep_for(30ms);
                                 job();
                             });
        return true;
    };
    const auto late = ns::RunOnUi<int>(threaded, []
                                       {
                                           return true;
                                       },
                                       5s,
                                       []
                                       {
                                           return 9;
                                       });
    worker.join();
    REQUIRE(late);
    CHECK(*late == 9);
}

TEST_CASE("native_settings_run_on_ui_gives_up_without_a_result", "[ctest][native-settings]")
{
    CHECK_FALSE(ns::RunOnUi<int>([](std::function<void()>)
                                 {
                                     return false;
                                 },
                                 []
                                 {
                                     return true;
                                 },
                                 1s,
                                 []
                                 {
                                     return 1;
                                 }));

    std::function<void()> queued;
    std::atomic<int>      runs{ 0 };
    const auto            started = std::chrono::steady_clock::now();
    CHECK_FALSE(ns::RunOnUi<int>([&](std::function<void()> job)
                                 {
                                     queued = std::move(job);
                                     return true;
                                 },
                                 []
                                 {
                                     return true;
                                 },
                                 60ms,
                                 [&]
                                 {
                                     ++runs;
                                     return 1;
                                 }));
    CHECK(std::chrono::steady_clock::now() - started < 5s);
    REQUIRE(queued);
    queued();
    CHECK(runs == 0);

    std::atomic<bool> alive{ true };
    std::thread       stopper([&]
                              {
                            std::this_thread::sleep_for(40ms);
                            alive = false;
                              });
    const auto        begun = std::chrono::steady_clock::now();
    CHECK_FALSE(ns::RunOnUi<int>([](std::function<void()>)
                                 {
                                     return true;
                                 },
                                 [&]
                                 {
                                     return alive.load();
                                 },
                                 30s,
                                 []
                                 {
                                     return 1;
                                 }));
    stopper.join();
    CHECK(std::chrono::steady_clock::now() - begun < 10s);
}

TEST_CASE("native_settings_reads_registry_state", "[ctest][native-settings]")
{
    const auto integer = ns::ReadFlag("native_settings_test_int");
    REQUIRE(integer);
    CHECK(integer->kind == ns::FlagKind::kInteger);
    CHECK(integer->min == 0.0);
    CHECK(integer->max == 10.0);
    CHECK_FALSE(integer->requiresRestart);

    const auto text = ns::ReadFlag("native_settings_test_enum");
    REQUIRE(text);
    CHECK(text->kind == ns::FlagKind::kString);
    CHECK(text->value == "low");
    CHECK(text->allowed == std::vector<std::string>{ "low", "high" });
    CHECK(text->requiresRestart);

    CHECK(ns::ReadFlag("native_settings_test_bool")->kind == ns::FlagKind::kBool);
    CHECK(ns::ReadFlag("native_settings_test_double")->kind == ns::FlagKind::kDouble);
    CHECK(ns::ReadFlag("native_settings_test_command")->kind == ns::FlagKind::kOther);
    CHECK_FALSE(ns::ReadFlag("native_settings_no_such_flag"));
}

TEST_CASE("native_settings_applies_and_saves_one_key", "[ctest][native-settings]")
{
    const auto path = std::filesystem::temp_directory_path() / "native_settings_test.toml";
    {
        std::ofstream file(path);
        file << "# keep\nnative_settings_test_enum = \"low\"\n";
    }
    const auto read = [&]
    {
        std::ifstream file(path);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };

    CHECK(ns::ApplyAndSave("native_settings_test_int", "7", path) == ns::ApplyStatus::kSaved);
    CHECK(rex::cvar::GetFlagByName("native_settings_test_int") == "7");
    CHECK(read().find("native_settings_test_int = 7") != std::string::npos);
    CHECK(read().find("# keep") != std::string::npos);

    REQUIRE(rex::cvar::SetFlagByName("native_settings_test_enum", "high"));
    CHECK(ns::ApplyAndSave("native_settings_test_bool", "true", path) == ns::ApplyStatus::kSaved);
    CHECK(read().find("native_settings_test_enum = \"low\"") != std::string::npos);
    REQUIRE(rex::cvar::SetFlagByName("native_settings_test_enum", "low"));

    const std::string before = read();
    CHECK(ns::ApplyAndSave("native_settings_test_int", "99", path) == ns::ApplyStatus::kRejected);
    CHECK(rex::cvar::GetFlagByName("native_settings_test_int") == "7");
    CHECK(read() == before);

    const auto unwritable = path / "x.toml";
    CHECK(ns::ApplyAndSave("native_settings_test_int", "8", unwritable) == ns::ApplyStatus::kUnsaved);
    CHECK(rex::cvar::GetFlagByName("native_settings_test_int") == "8");

    std::filesystem::remove(path);
    REQUIRE(rex::cvar::SetFlagByName("native_settings_test_int", "3"));
    REQUIRE(rex::cvar::SetFlagByName("native_settings_test_bool", "false"));
}

TEST_CASE("native_settings_ui_services_read_and_list_monitors", "[ctest][native-settings]")
{
    ns::HostBindings host;
    host.layout         = std::make_shared<rex::ui::SettingsLayout>();
    host.monitorChoices = []
    {
        return std::unordered_map<std::string, std::string>{ { "2", "Second <Monitor>" }, { "0", "Default" }, { "1", "First" }, { "x", "Skipped" } };
    };

    const auto services = ns::MakeUiServices(host);
    REQUIRE(services);
    CHECK(services->Read("native_settings_test_int")->kind == ns::FlagKind::kInteger);

    const auto monitors = services->MonitorChoices();
    REQUIRE(monitors);
    REQUIRE(monitors->size() == 3);
    CHECK((*monitors)[0].raw == "0");
    CHECK((*monitors)[1].caption == "First");
    CHECK((*monitors)[2].caption == "Second Monitor");

    host.monitorChoices = nullptr;
    CHECK_FALSE(ns::MakeUiServices(host)->MonitorChoices());

    host.layout.reset();
    CHECK_FALSE(ns::MakeUiServices(host));
}

TEST_CASE("native_settings_host_bindings_can_be_set_and_cleared", "[ctest][native-settings]")
{
    ns::SetHost({});
    CHECK_FALSE(ns::Host());

    ns::HostBindings host;
    host.post = [](std::function<void()> job)
    {
        job();
        return true;
    };
    host.timeout = 40ms;
    ns::SetHost(host);
    REQUIRE(ns::Host());
    CHECK(ns::Host()->timeout == 40ms);
    ns::SetHost({});
    CHECK_FALSE(ns::Host());
}

TEST_CASE("native_settings_keeps_waiting_for_a_started_job_past_the_timeout", "[ctest][native-settings]")
{
    std::thread      worker;
    const ns::Poster threaded = [&](std::function<void()> job)
    {
        worker = std::thread(std::move(job));
        return true;
    };
    const auto late = ns::RunOnUi<ns::ApplyStatus>(threaded, []
                                                   {
                                                       return true;
                                                   },
                                                   30ms,
                                                   []
                                                   {
                                                       std::this_thread::sleep_for(200ms);
                                                       return ns::ApplyStatus::kUnsaved;
                                                   });
    worker.join();
    REQUIRE(late);
    CHECK(*late == ns::ApplyStatus::kUnsaved);

    std::atomic<bool> alive{ true };
    std::atomic<bool> release{ false };
    std::thread       job;
    const auto        stopped = ns::RunOnUi<int>([&](std::function<void()> work)
                                                 {
                                              job = std::thread(std::move(work));
                                              return true;
                                                 },
                                                 [&]
                                                 {
                                              if (!alive)
                                                  return false;
                                              alive = false;
                                              return true;
                                                 },
                                                 30s,
                                                 [&]
                                                 {
                                              while (!release)
                                                  std::this_thread::sleep_for(5ms);
                                              return 1;
                                                 });
    release                   = true;
    job.join();
    CHECK_FALSE(stopped);
}

TEST_CASE("native_settings_reads_sensitivity_at_full_precision", "[ctest][native-settings]")
{
    REQUIRE(rex::cvar::SetFlagByName("mnk_sensitivity", "1.23456789"));
    const auto state = ns::ReadFlag("mnk_sensitivity");
    REQUIRE(state);
    CHECK(state->value == "1.23456789");
    REQUIRE(rex::cvar::SetFlagByName("mnk_sensitivity", state->value));
    CHECK(ns::ReadFlag("mnk_sensitivity")->value == "1.23456789");
    REQUIRE(rex::cvar::SetFlagByName("mnk_sensitivity", "1.0"));
}

namespace
{

size_t rowIndex(const ns::OptionsModel& model, std::string_view key)
{
    for (size_t i = 0; i < model.RowCount(); ++i)
    {
        if (model.Row(i)->key == key)
            return i;
    }
    FAIL("no row " << std::string(key));
    return 0;
}

const ns::OptionRow& selectRow(ns::OptionsModel& model, std::string_view key)
{
    model.Select(rowIndex(model, key));
    return *model.Row(model.Highlight());
}

} // namespace

TEST_CASE("options_model_lists_the_curated_rows_by_category", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);

    std::vector<std::string> titles;
    for (const auto& section : model.Sections())
        titles.push_back(section.title);
    CHECK(titles == std::vector<std::string>{ "General", "Gameplay", "Notifications", "Online", "Controls", "Display", "Audio" });

    const auto& gameplay = model.Sections()[1];
    REQUIRE_FALSE(gameplay.rows.empty());
    CHECK(gameplay.rows.front().key == "combat_speed");
    CHECK(gameplay.rows.front().label == "Combat Speed");
    CHECK(model.Row(model.SectionStart(1)) == &gameplay.rows.front());
    CHECK(model.Row(model.RowCount()) == nullptr);
    CHECK(gameplay.rows.front().group.empty());
    CHECK(selectRow(model, "ai_research_bonus_percent").group == "AI Handicaps");
    CHECK(selectRow(model, "gotw_standard_rules").group == "Game of the Week");
    model.Select(0);

    size_t total = 0;
    for (const auto& section : model.Sections())
    {
        total += section.rows.size();
        for (const auto& row : section.rows)
        {
            REQUIRE(row.current < row.choices.size());
            for (const auto& choice : row.choices)
                CHECK(choice.caption.find("(Current)") == std::string::npos);
        }
    }
    CHECK(total == model.RowCount());
    CHECK(model.Highlight() == 0);

    services.flags.erase("audio_volume");
    services.flags.erase("audio_mute");
    services.flags.erase("audio_play_in_background");
    ns::OptionsModel trimmed(services);
    CHECK(trimmed.Sections().back().title == "Display");
}

TEST_CASE("options_model_shows_the_layout_description_and_labels", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "audio_volume");
    CHECK(row.label == "Volume");
    CHECK(model.Description() == "Adjust how loudly the game plays.");
    CHECK(row.Caption() == "100");
}

TEST_CASE("options_model_steps_a_value_and_stops_at_the_ends", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "combat_speed");

    CHECK_FALSE(model.Step(-1));
    CHECK(services.applied.empty());

    CHECK(model.Step(1));
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied[0] == std::pair<std::string, std::string>{ "combat_speed", "fast" });
    CHECK(model.Row(model.Highlight())->Caption() == "Fast");

    CHECK_FALSE(model.Step(1));
    CHECK(services.applied.size() == 1);

    CHECK(model.Step(-1));
    CHECK(services.applied.back().second == "normal");
}

TEST_CASE("options_model_orders_a_bool_off_then_on", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "hide_research_prompt");
    REQUIRE(row.choices.size() == 2);
    CHECK(row.choices[0].caption == "Off");
    CHECK(row.choices[1].caption == "On");
    CHECK(row.current == 0);

    CHECK(model.Step(1));
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "hide_research_prompt", "true" });
    CHECK(model.Row(model.Highlight())->Caption() == "On");
    CHECK_FALSE(model.Step(1));
}

TEST_CASE("options_model_activate_toggles_a_bool_and_cycles_a_short_list", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);

    selectRow(model, "hide_research_prompt");
    CHECK(model.Activate() == ns::OptionsModel::Press::kApplied);
    CHECK(services.applied.back().second == "true");
    CHECK(model.Activate() == ns::OptionsModel::Press::kApplied);
    CHECK(services.applied.back().second == "false");

    selectRow(model, "button_icons");
    for (int i = 0; i < 3; ++i)
        CHECK(model.Step(1));
    CHECK(services.applied.back().second == "keyboard");
    CHECK(model.Activate() == ns::OptionsModel::Press::kApplied);
    CHECK(services.applied.back().second == "auto");
}

TEST_CASE("options_model_opens_a_picker_only_past_the_threshold", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);

    const auto& five = selectRow(model, "ai_research_bonus_percent");
    CHECK(five.choices.size() == 5);
    CHECK_FALSE(five.UsesPicker());

    const auto& seven = selectRow(model, "ai_civilizations");
    CHECK(seven.choices.size() == 7);
    CHECK_FALSE(seven.UsesPicker());
    CHECK(model.Activate() == ns::OptionsModel::Press::kApplied);
    CHECK_FALSE(model.PickerOpen());

    services.flags["gotw_custom_civilization"] = boolState(true);
    model.Reload();
    const auto& sixteen = selectRow(model, "gotw_civilization");
    CHECK(sixteen.choices.size() > ns::kPickerThreshold);
    CHECK(model.Activate() == ns::OptionsModel::Press::kOpenPicker);
    CHECK(model.PickerOpen());
}

TEST_CASE("options_model_steps_the_custom_week_shown_only_when_enabled", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["gotw_week"] = enumState("900", {});
    {
        ns::OptionsModel model(services);
        const auto&      gameplay = model.Sections()[1];
        CHECK(std::none_of(gameplay.rows.begin(), gameplay.rows.end(), [](const ns::OptionRow& row)
                           {
                               return row.key == "gotw_week" || row.key == "gotw_civilization";
                           }));
    }

    services.flags["gotw_custom_week"] = boolState(true);
    ns::OptionsModel model(services);
    const auto&      week = selectRow(model, "gotw_week");
    CHECK(week.label == "Week");
    CHECK(week.Caption() == "900");
    CHECK_FALSE(week.UsesPicker());
    CHECK(week.numeric);
    CHECK(model.Step(1));
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "901" });
    CHECK(model.Step(-1));
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "900" });

    services.flags["gotw_week"] = enumState("1", {});
    model.Reload();
    CHECK_FALSE(model.Step(-1));
}

TEST_CASE("options_model_picker_applies_the_cursor_row_or_closes_unchanged", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["gotw_custom_civilization"] = boolState(true);
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "gotw_civilization");
    REQUIRE(row.UsesPicker());
    REQUIRE(row.choices.size() == 16);

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenPicker);
    CHECK(model.PickerCursor() == row.current);
    model.MovePicker(-5);
    CHECK(model.PickerCursor() == 0);
    model.MovePicker(2);
    CHECK(model.PickerCursor() == 2);
    model.MovePicker(100);
    CHECK(model.PickerCursor() == 15);
    model.SetPickerCursor(2);

    model.Move(1);
    model.Select(0);
    CHECK(model.Highlight() == rowIndex(model, "gotw_civilization"));

    CHECK(model.CommitPicker());
    CHECK_FALSE(model.PickerOpen());
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_civilization", "2" });
    CHECK(model.Row(model.Highlight())->Caption() == "Greek");

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenPicker);
    model.MovePicker(3);
    model.ClosePicker();
    CHECK_FALSE(model.PickerOpen());
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenPicker);
    CHECK_FALSE(model.CommitPicker());
    CHECK(services.applied.size() == 1);
}

TEST_CASE("options_model_keeps_a_current_value_that_is_not_a_preset", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["ai_research_bonus_percent"] = intState("37", 0, 200);
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "ai_research_bonus_percent");
    REQUIRE(row.choices.size() == 6);
    CHECK(row.current == 0);
    CHECK(row.choices[0].raw == "37");
    CHECK(row.Caption() == "37");

    CHECK_FALSE(model.Step(-1));
    CHECK(model.Step(1));
    CHECK(services.applied.back().second == "0");
}

TEST_CASE("options_model_keeps_the_exact_text_of_a_preset_current_value", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["mnk_sensitivity"].value = "1.0000000001";
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "mnk_sensitivity");
    CHECK(row.Caption() == "1.00");
    CHECK(row.choices[row.current].raw == "1.0000000001");
    CHECK(row.choices.size() == 5);
}

TEST_CASE("options_model_moves_the_highlight_within_the_current_tab", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    const size_t     tabEnd = model.Sections()[0].rows.size() - 1;

    model.Move(-1);
    CHECK(model.Highlight() == 0);
    model.Move(1);
    CHECK(model.Highlight() == 1);
    model.Move(100000);
    CHECK(model.Highlight() == tabEnd);
    CHECK(model.Tab() == 0);
    model.Move(1);
    CHECK(model.Highlight() == tabEnd);

    model.Select(100000);
    CHECK(model.Highlight() == model.RowCount() - 1);
    CHECK(model.Tab() == model.Sections().size() - 1);
    model.Move(-100000);
    CHECK(model.Highlight() == model.SectionStart(model.Tab()));
}

TEST_CASE("options_model_steps_tabs_with_wrapping_and_remembers_each_tabs_row", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    const size_t     tabs = model.Sections().size();
    REQUIRE(tabs == 7);

    CHECK(model.Tab() == 0);
    CHECK(model.Highlight() == 0);
    CHECK(model.StepTab(-1));
    CHECK(model.Tab() == tabs - 1);
    CHECK(model.StepTab(1));
    CHECK(model.Tab() == 0);

    model.Move(2);
    const size_t generalRow = model.Highlight();
    CHECK(model.StepTab(1));
    CHECK(model.Tab() == 1);
    CHECK(model.Highlight() == model.SectionStart(1));
    model.Move(2);
    const size_t gameplayRow = model.Highlight();
    CHECK(model.StepTab(-1));
    CHECK(model.Highlight() == generalRow);
    CHECK(model.StepTab(1));
    CHECK(model.Highlight() == gameplayRow);

    while (model.Tab() + 1 < tabs)
        model.StepTab(1);
    CHECK(model.Tab() == tabs - 1);
    CHECK(model.StepTab(1));
    CHECK(model.Tab() == 0);

    CHECK(model.SelectTab(1));
    CHECK(model.Highlight() == gameplayRow);
    CHECK_FALSE(model.SelectTab(1));
    CHECK_FALSE(model.SelectTab(tabs));
    CHECK(model.Tab() == 1);

    model.Select(model.SectionStart(3) + 1);
    const size_t controlsRow = model.Highlight();
    model.SelectTab(0);
    model.SelectTab(3);
    CHECK(model.Highlight() == controlsRow);

    services.flags["gotw_custom_civilization"] = boolState(true);
    model.Reload();
    selectRow(model, "gotw_civilization");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenPicker);
    const size_t tab = model.Tab();
    CHECK_FALSE(model.StepTab(1));
    CHECK_FALSE(model.SelectTab(tab + 1));
    CHECK(model.Tab() == tab);
}

TEST_CASE("options_model_keeps_the_highlight_in_its_tab_when_a_toggle_hides_the_row", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["gotw_week"]        = enumState("900", {});
    services.flags["gotw_custom_week"] = boolState(true);
    ns::OptionsModel model(services);
    selectRow(model, "gotw_week");
    const size_t tab = model.Tab();
    REQUIRE(model.Row(model.Highlight())->key == "gotw_week");
    const size_t at = model.Highlight() - model.SectionStart(tab);
    REQUIRE(at + 1 < model.Sections()[tab].rows.size());
    const std::string after = model.Sections()[tab].rows[at + 1].key;

    services.flags["gotw_custom_week"] = boolState(false);
    model.Reload();
    CHECK(model.Tab() == tab);
    CHECK(model.Highlight() - model.SectionStart(tab) == at);
    CHECK(at > 0);
    const auto* row = model.Row(model.Highlight());
    REQUIRE(row != nullptr);
    CHECK(row->key == after);
    CHECK(row->key != "gotw_week");

    model.Move(1000);
    CHECK(model.Tab() == tab);

    selectRow(model, "gotw_custom_week");
    CHECK(model.Step(1));
    CHECK(model.Tab() == tab);
    CHECK(model.Row(model.Highlight())->key == "gotw_custom_week");
    const auto& rows = model.Sections()[tab].rows;
    CHECK(std::any_of(rows.begin(), rows.end(), [](const ns::OptionRow& r)
                      {
                          return r.key == "gotw_week";
                      }));
}

TEST_CASE("trigger_latch_presses_once_per_hold_despite_jitter", "[ctest][native-settings]")
{
    rerevved::TriggerLatch latch;
    int                    presses = 0;
    // Wobbling around the press threshold, never releasing below the off level.
    for (const int sample : { 0, 50, 101, 99, 101, 98, 120, 60, 101 })
        presses += latch.Update(sample) ? 1 : 0;
    CHECK(presses == 1);
    CHECK(latch.Down());

    CHECK_FALSE(latch.Update(39));
    CHECK_FALSE(latch.Down());
    CHECK_FALSE(latch.Update(100));
    CHECK(latch.Update(101));
}

TEST_CASE("stick_repeat_steps_once_then_repeats_while_held", "[ctest][native-settings]")
{
    using rerevved::StickRepeat;
    constexpr float kFrame = 1.0f / 60.0f;
    StickRepeat     stick;

    CHECK(stick.Update(0, kFrame) == 0);
    CHECK(stick.Update(StickRepeat::kOn, kFrame) == 0);
    CHECK(stick.Update(-StickRepeat::kOn, kFrame) == 0);
    CHECK_FALSE(stick.Held());

    CHECK(stick.Update(30000, kFrame) == -1);
    CHECK(stick.Held());
    int steps = 0;
    for (float t = 0.0f; t < StickRepeat::kDelay - 4 * kFrame; t += kFrame)
        steps += stick.Update(30000, kFrame) != 0 ? 1 : 0;
    CHECK(steps == 0);

    steps = 0;
    for (float t = 0.0f; t < 1.0f; t += kFrame)
        steps += stick.Update(30000, kFrame) == -1 ? 1 : 0;
    CHECK(steps >= 10);
    CHECK(steps <= 13);

    CHECK(stick.Held());
    stick.Update(StickRepeat::kOff - 1, kFrame);
    CHECK_FALSE(stick.Held());
    CHECK(stick.Update(-30000, kFrame) == 1);
}

TEST_CASE("stick_repeat_a_long_frame_gives_one_step_not_a_burst", "[ctest][native-settings]")
{
    rerevved::StickRepeat stick;
    CHECK(stick.Update(-32000, 0.016f) == 1);
    CHECK(stick.Update(-32000, 5.0f) == 1);
    CHECK(stick.Update(-32000, 0.016f) == 0);
}

TEST_CASE("stick_repeat_reverses_without_a_release_frame", "[ctest][native-settings]")
{
    rerevved::StickRepeat stick;
    CHECK(stick.Update(30000, 0.016f) == -1);
    CHECK(stick.Update(-30000, 0.016f) == 1);
    CHECK(stick.Held());
}

TEST_CASE("page_set_switches_pages_only_when_there_is_a_choice", "[ctest][native-settings]")
{
    rerevved::PageSet pages;
    CHECK(pages.Count() == 0);
    CHECK(pages.Add("Options") == 0);
    CHECK(pages.Count() == 1);
    CHECK(pages.Title(0) == "Options");

    CHECK_FALSE(pages.Switchable());
    CHECK_FALSE(pages.Step(1));
    CHECK_FALSE(pages.Step(-1));
    CHECK_FALSE(pages.Select(0));
    CHECK(pages.Current() == 0);

    pages.Add("Mods");
    pages.Add("Achievements");
    CHECK(pages.Switchable());
    CHECK(pages.Step(-1));
    CHECK(pages.Current() == 2);
    CHECK(pages.Step(1));
    CHECK(pages.Current() == 0);
    CHECK(pages.Step(1));
    CHECK(pages.Current() == 1);
    CHECK(pages.Step(1));
    CHECK(pages.Current() == 2);
    CHECK(pages.Select(0));
    CHECK_FALSE(pages.Select(3));
    CHECK(pages.Current() == 0);
}

TEST_CASE("options_model_keeps_the_highlight_on_its_row_after_an_apply", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "vsync");
    const size_t at = model.Highlight();
    CHECK(model.Step(1));
    CHECK(model.Highlight() == at);
    CHECK(model.Row(at)->key == "vsync");
}

TEST_CASE("options_model_maps_the_apply_outcome_to_a_note", "[ctest][native-settings]")
{
    CHECK(ns::OutcomeNote(ns::ApplyStatus::kSaved).empty());
    CHECK(ns::OutcomeNote(ns::ApplyStatus::kRejected) == ns::kNoteRejected);
    CHECK(ns::OutcomeNote(ns::ApplyStatus::kUnsaved) == ns::kNoteUnsaved);
    CHECK(ns::TimingNote(ns::Timing::kLive).empty());
    CHECK(ns::TimingNote(ns::Timing::kRestart) == ns::kNoteRestart);
    CHECK(ns::TimingNote(ns::Timing::kNextGame) == ns::kNoteNextGame);

    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "hide_research_prompt");
    CHECK(model.Note().empty());

    services.status = ns::ApplyStatus::kUnsaved;
    CHECK(model.Step(1));
    CHECK(model.Note() == ns::kNoteUnsaved);
    CHECK(model.Row(model.Highlight())->Caption() == "On");

    model.Move(1);
    CHECK(model.Note().empty());

    services.status = ns::ApplyStatus::kRejected;
    selectRow(model, "hide_popup_city_status");
    CHECK(model.Step(1));
    CHECK(model.Note() == ns::kNoteRejected);
    CHECK(model.Row(model.Highlight())->Caption() == "Off");
}

TEST_CASE("options_model_notes_when_a_value_takes_effect", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["vsync"] = boolState(false, true);
    ns::OptionsModel model(services);

    selectRow(model, "vsync");
    CHECK(model.Row(model.Highlight())->timing == ns::Timing::kRestart);
    CHECK(model.Note() == ns::kNoteRestart);

    selectRow(model, "resources");
    CHECK(model.Row(model.Highlight())->timing == ns::Timing::kNextGame);
    CHECK(model.Note() == ns::kNoteNextGame);

    services.status = ns::ApplyStatus::kRejected;
    CHECK(model.Step(1));
    CHECK(model.Note() == ns::kNoteRejected);
    model.Move(1);
    model.Move(-1);
    CHECK(model.Note() == ns::kNoteNextGame);

    selectRow(model, "combat_speed");
    CHECK(model.Row(model.Highlight())->timing == ns::Timing::kLive);
    CHECK(model.Note().empty());
}

TEST_CASE("options_model_offers_monitors_when_they_are_available", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "monitor");
    REQUIRE(row.choices.size() == 3);
    CHECK(row.Caption() == "Default");
    CHECK(model.Step(1));
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "monitor", "1" });
}

TEST_CASE("options_model_cannot_change_the_monitor_without_a_list", "[ctest][native-settings]")
{
    FakeServices services;
    services.monitors.reset();
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "monitor");
    REQUIRE(row.choices.size() == 1);
    CHECK_FALSE(model.Step(1));
    CHECK(model.Activate() == ns::OptionsModel::Press::kNone);
    CHECK(model.Note() == ns::kNoteNoMonitor);
    CHECK(services.applied.empty());
    model.Move(1);
    CHECK(model.Note().empty());
}

TEST_CASE("options_model_reload_reads_values_again", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "hide_research_prompt");
    services.flags["hide_research_prompt"].value = "true";
    CHECK(model.Row(model.Highlight())->Caption() == "Off");
    model.Reload();
    CHECK(model.Row(model.Highlight())->Caption() == "On");
    CHECK(model.Row(model.Highlight())->key == "hide_research_prompt");
}

namespace
{

namespace ra = rerevved::retail_audio;

void addRetailVolumes(FakeServices& services, std::string value = "-1")
{
    for (const auto& channel : ra::kChannels)
        services.flags[std::string(channel.cvar)] = intState(value, -1, 9);
}

} // namespace

TEST_CASE("options_model_lists_the_retail_volumes_after_the_audio_rows", "[ctest][native-settings]")
{
    FakeServices services;
    addRetailVolumes(services);
    ns::OptionsModel model(services);

    const auto& audio = model.Sections().back();
    REQUIRE(audio.title == "Audio");
    std::vector<std::string> keys;
    for (const auto& row : audio.rows)
        keys.push_back(row.key);
    CHECK(keys == std::vector<std::string>{ "audio_volume", "audio_mute", "audio_play_in_background", "retail_music_volume", "retail_effects_volume", "retail_advisor_volume", "retail_leader_volume" });
    CHECK(audio.rows[3].label == "Music Volume");
    CHECK(audio.rows[4].label == "Effects Volume");
    CHECK(audio.rows[5].label == "Advisor Speech Volume");
    CHECK(audio.rows[6].label == "Leader Speech Volume");
    for (size_t i = 3; i <= 6; ++i)
        CHECK_FALSE(audio.rows[i].description.empty());
    CHECK(audio.rows[3].timing == ns::Timing::kLive);
}

TEST_CASE("options_model_keeps_the_retail_volumes_out_of_f1", "[ctest][native-settings]")
{
    rex::ui::SettingsLayout layout;
    rerevved::ConfigureSettingsLayout(layout);
    for (const auto& tab : layout.tabs)
    {
        for (const auto& section : tab.sections)
        {
            for (const auto& source : section.sources)
            {
                CAPTURE(source);
                CHECK_FALSE(ra::ChannelOfCvar(source));
            }
        }
    }
}

TEST_CASE("options_model_leaves_the_retail_rows_out_without_the_cvars", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    CHECK(model.Sections().back().rows.size() == 3);

    services.flags[std::string(ra::kChannels[2].cvar)] = intState("-1", -1, 9);
    ns::OptionsModel one(services);
    CHECK(one.Sections().back().rows.size() == 4);
    CHECK(one.Sections().back().rows.back().key == ra::kChannels[2].cvar);
}

TEST_CASE("options_model_shows_the_live_game_volume_while_unset", "[ctest][native-settings]")
{
    FakeServices services;
    addRetailVolumes(services);
    services.live[0] = { 4, true };
    services.live[1] = { 9, true };
    services.live[2] = { 0, true };
    services.live[3] = { 6, false, 5, 6 };
    ns::OptionsModel model(services);

    const auto& music = selectRow(model, "retail_music_volume");
    CHECK(music.choices.size() == 10);
    CHECK(music.current == 4);
    CHECK(music.Caption() == "4");
    CHECK_FALSE(music.unavailable);
    CHECK(selectRow(model, "retail_effects_volume").Caption() == "9");
    CHECK(selectRow(model, "retail_advisor_volume").Caption() == "0");
    const auto& leader = selectRow(model, "retail_leader_volume");
    CHECK(leader.Caption() == "~6");
    CHECK(leader.current == 6);
    CHECK(leader.choices[5].caption == "5");
    CHECK_FALSE(leader.UsesPicker());
}

TEST_CASE("options_model_an_override_wins_over_the_live_volume", "[ctest][native-settings]")
{
    FakeServices services;
    addRetailVolumes(services);
    services.flags["retail_music_volume"].value = "2";
    services.live[0]                            = { 8, true };
    ns::OptionsModel model(services);
    CHECK(selectRow(model, "retail_music_volume").Caption() == "2");
}

TEST_CASE("options_model_a_volume_without_a_value_is_unavailable_and_cannot_change", "[ctest][native-settings]")
{
    FakeServices services;
    addRetailVolumes(services);
    ns::OptionsModel model(services);

    const auto& row = selectRow(model, "retail_music_volume");
    CHECK(row.unavailable);
    CHECK(row.Caption() == "Unavailable");
    CHECK(model.Note() == ns::kNoteNoAudio);
    CHECK_FALSE(model.Step(1));
    CHECK_FALSE(model.Step(-1));
    CHECK(model.Activate() == ns::OptionsModel::Press::kNone);
    CHECK(services.applied.empty());

    services.live[0] = { 5, true };
    services.revision++;
    model.Poll();
    const auto& ready = *model.Row(model.Highlight());
    CHECK(ready.key == "retail_music_volume");
    CHECK_FALSE(ready.unavailable);
    CHECK(ready.Caption() == "5");
}

TEST_CASE("options_model_steps_a_volume_one_position_with_the_override_as_text", "[ctest][native-settings]")
{
    FakeServices services;
    addRetailVolumes(services);
    services.live[1] = { 8, true };
    ns::OptionsModel model(services);
    selectRow(model, "retail_effects_volume");

    CHECK(model.Step(1));
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "retail_effects_volume", "9" });
    CHECK(model.Row(model.Highlight())->Caption() == "9");
    CHECK_FALSE(model.Step(1));
    CHECK(model.Step(-1));
    CHECK(services.applied.back().second == "8");

    services.flags["retail_effects_volume"].value = "9";
    model.Reload();
    CHECK(model.Activate() == ns::OptionsModel::Press::kApplied);
    CHECK(services.applied.back().second == "0");

    CHECK_FALSE(model.Step(-1));
}

TEST_CASE("options_model_an_off_grid_volume_moves_to_the_nearest_step_on_an_edit", "[ctest][native-settings]")
{
    FakeServices services;
    addRetailVolumes(services);
    services.live[3] = { 6, false, 5, 6 };
    ns::OptionsModel model(services);
    selectRow(model, "retail_leader_volume");
    CHECK(model.Step(-1));
    CHECK(services.applied.back().second == "5");
    CHECK(model.Row(model.Highlight())->Caption() == "5");
}

TEST_CASE("options_model_poll_asks_for_live_values_now_and_then_and_reads_changes", "[ctest][native-settings]")
{
    FakeServices services;
    addRetailVolumes(services);
    services.live[0] = { 2, true };
    ns::OptionsModel model(services);
    CHECK(services.refreshes == 0);

    model.Poll();
    CHECK(services.refreshes == 1);
    for (int i = 1; i < 30; ++i)
        model.Poll();
    CHECK(services.refreshes == 1);
    model.Poll();
    CHECK(services.refreshes == 2);

    selectRow(model, "retail_music_volume");
    services.live[0] = { 7, true };
    model.Poll();
    CHECK(model.Row(model.Highlight())->Caption() == "2");
    services.revision++;
    model.Poll();
    CHECK(model.Row(model.Highlight())->key == "retail_music_volume");
    CHECK(model.Row(model.Highlight())->Caption() == "7");
}

TEST_CASE("native_settings_applies_a_retail_volume_and_posts_the_live_request", "[ctest][native-settings]")
{
    const auto path = std::filesystem::temp_directory_path() / "native_settings_retail_volume_test.toml";
    {
        std::ofstream file(path);
        file << "# keep\n";
    }
    ra::Queue().Advance();
    ra::Live().ConsumeRefresh();

    CHECK(ns::ApplyAndSaveVolume("retail_advisor_volume", "4", path) == ns::ApplyStatus::kSaved);
    CHECK(rex::cvar::GetFlagByName("retail_advisor_volume") == "4");
    CHECK(ra::OverrideOf(2) == 4);
    CHECK(ra::Live().RefreshRequested());
    const auto posted = ra::Queue().Take();
    REQUIRE(posted.size() == 1);
    CHECK(posted[0].channel == 2);
    CHECK(posted[0].position == 4);
    CHECK(posted[0].generation == ra::Queue().Generation());

    {
        std::ifstream     file(path);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        CHECK(text.find("retail_advisor_volume = 4") != std::string::npos);
    }

    // An unsaved edit still applies live.
    CHECK(ns::ApplyAndSaveVolume("retail_advisor_volume", "5", path / "x.toml") == ns::ApplyStatus::kUnsaved);
    CHECK(ra::Queue().Take().size() == 1);

    // A rejected value posts nothing and changes nothing.
    CHECK(ns::ApplyAndSaveVolume("retail_advisor_volume", "10", path) == ns::ApplyStatus::kRejected);
    CHECK(ra::OverrideOf(2) == 5);
    CHECK_FALSE(ra::Queue().HasPending());

    // Other names behave as ApplyAndSave.
    CHECK(ns::ApplyAndSaveVolume("native_settings_test_int", "6", path) == ns::ApplyStatus::kSaved);
    CHECK_FALSE(ra::Queue().HasPending());
    REQUIRE(rex::cvar::SetFlagByName("native_settings_test_int", "3"));

    std::filesystem::remove(path);
    REQUIRE(rex::cvar::SetFlagByName("retail_advisor_volume", "-1"));
}

TEST_CASE("options_model_an_off_grid_volume_steps_to_the_next_step_in_that_direction", "[ctest][native-settings]")
{
    FakeServices services;
    addRetailVolumes(services);
    // Between 4 and 5 (a nearest of 4): Right goes to 5, Left to 4.
    services.live[0] = { 4, false, 4, 5 };
    services.live[1] = { 0, false, 0, 1 };
    services.live[2] = { 9, false, 8, 9 };
    ns::OptionsModel model(services);

    selectRow(model, "retail_music_volume");
    CHECK(model.Step(1));
    CHECK(services.applied.back().second == "5");

    selectRow(model, "retail_effects_volume");
    CHECK(model.Step(-1));
    CHECK(services.applied.back().second == "0");

    selectRow(model, "retail_advisor_volume");
    CHECK(model.Step(1));
    CHECK(services.applied.back().second == "9");
}

TEST_CASE("options_model_reads_the_live_values_and_revision_together", "[ctest][native-settings]")
{
    // Publication after the snapshot must raise the revision for the next poll.
    FakeServices services;
    addRetailVolumes(services);
    ns::OptionsModel model(services);
    selectRow(model, "retail_music_volume");
    CHECK(model.Row(model.Highlight())->unavailable);
    services.live[0] = { 3, true, 3, 3 };
    services.revision++;
    model.Poll();
    CHECK(model.Row(model.Highlight())->Caption() == "3");
}

TEST_CASE("options_model_offers_the_game_language_as_a_restart_row", "[ctest][native-settings]")
{
    FakeServices  services;
    ns::FlagState language;
    language.kind                   = ns::FlagKind::kInteger;
    language.value                  = "1";
    language.requiresRestart        = true;
    services.flags["game_language"] = language;
    ns::OptionsModel model(services);

    REQUIRE_FALSE(model.Sections().empty());
    CHECK(model.Sections().front().title == "General");
    CHECK(captionsOf(model, "General") == std::vector<std::string>{ "Language: English", "FPS Counter: Off", "Skip Intro: Off", "Open Log Folder: Open" });
    CHECK(model.Row(0)->key == "game_language");

    const auto& row = selectRow(model, "game_language");
    CHECK(row.label == "Language");
    CHECK(row.timing == ns::Timing::kRestart);
    CHECK_FALSE(row.UsesPicker());
    CHECK(row.description == "Choose the game's language.");
    REQUIRE(row.choices.size() == 5);
    std::vector<std::string> captions;
    std::vector<std::string> raws;
    for (const auto& choice : row.choices)
    {
        captions.push_back(choice.caption);
        raws.push_back(choice.raw);
    }
    CHECK(captions == std::vector<std::string>{ "English", "Deutsch", "Fran\xC3\xA7"
                                                                      "ais",
                                                "Espa\xC3\xB1"
                                                "ol",
                                                "Italiano" });
    CHECK(raws == std::vector<std::string>{ "1", "3", "4", "5", "6" });
    CHECK(model.Note() == "Takes effect after restart.");

    CHECK(model.Step(1));
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "game_language", "3" });
    CHECK(selectRow(model, "game_language").Caption() == "Deutsch");
    for (const auto& [key, value] : services.applied)
        CHECK(key == "game_language");
}

TEST_CASE("options_model_shows_an_unlisted_game_language_as_stored", "[ctest][native-settings]")
{
    FakeServices  services;
    ns::FlagState language;
    language.kind                   = ns::FlagKind::kInteger;
    language.value                  = "2";
    language.requiresRestart        = true;
    services.flags["game_language"] = language;
    ns::OptionsModel model(services);

    const auto& row = selectRow(model, "game_language");
    REQUIRE(row.choices.size() == 6);
    CHECK(row.Caption() == "2");
    CHECK(services.applied.empty());
}

namespace
{

namespace tt = rerevved::title_text;

void addEveryRow(FakeServices& services)
{
    addRetailVolumes(services);
    ns::FlagState language;
    language.kind                              = ns::FlagKind::kInteger;
    language.value                             = "1";
    language.min                               = 1;
    language.max                               = 6;
    language.requiresRestart                   = true;
    services.flags["game_language"]            = language;
    services.flags["gotw_custom_week"]         = boolState(true);
    services.flags["gotw_custom_civilization"] = boolState(true);
    services.flags["gotw_week"]                = enumState("1", {});
    services.flags["combat_speed"]             = enumState("normal", { "normal", "fast" });
}

bool needsTranslation(const std::string& caption)
{
    static const std::vector<std::string> kBrands{ "FXAA", "CAS", "FSR", "FSR 2", "FSR 3", "Xbox", "PlayStation", "Xenos" };
    if (std::find(kBrands.begin(), kBrands.end(), caption) != kBrands.end())
        return false;
    return std::any_of(caption.begin(), caption.end(), [](char c)
                       {
                           return !(std::isdigit(static_cast<unsigned char>(c)) || c == 'x' || c == ':' || c == '.' || c == ' ' || c == '~' || c == '-');
                       });
}

const tt::Row& requireRow(const std::string& key)
{
    const tt::Row* row = tt::FindRow(key);
    INFO("table has no row " << key);
    REQUIRE(row != nullptr);
    return *row;
}

constexpr std::string_view english(const tt::Row& row)
{
    return row.text[static_cast<size_t>(tt::Language::kEnglish)];
}

} // namespace

TEST_CASE("options_model_text_is_all_in_the_title_text_table", "[ctest][native-settings]")
{
    FakeServices services;
    addEveryRow(services);
    ns::OptionsModel model(services);

    // Require every curated row so unsupported fixture types fail loudly.
    std::set<std::string> rowKeys;
    for (const auto& section : model.Sections())
    {
        for (const auto& row : section.rows)
            rowKeys.insert(row.key);
    }
    for (const auto key : ns::CuratedKeys())
    {
        INFO("curated key without a row: " << key);
        CHECK(rowKeys.contains(std::string(key)));
    }

    std::set<std::string> produced;
    const auto            use = [&](const std::string& key, std::string_view text)
    {
        produced.insert(key);
        CAPTURE(key);
        CHECK(english(requireRow(key)) == text);
    };
    for (const auto& section : model.Sections())
    {
        use(tt::TabKey(section.title), section.title);
        for (const auto& row : section.rows)
        {
            if (!row.group.empty())
                use(tt::SectionKey(row.group), row.group);
            use(tt::SettingKey(row.key, "label"), row.label);
            if (row.kind == ns::RowKind::kBind)
                use(row.bindController ? "common.bind_tip_controller" : "common.bind_tip_hotkey", row.description);
            else if (!row.description.empty())
                use(tt::SettingKey(row.key, "tip"), row.description);
            for (const auto& choice : row.choices)
            {
                if (row.kind == ns::RowKind::kBind)
                {
                    if (choice.raw.empty())
                        use("common.bind_none", choice.caption);
                }
                else if (row.kind == ns::RowKind::kAction)
                    use("common.open", choice.caption);
                else if (row.kind == ns::RowKind::kStatus)
                    continue; // the registry's own words
                else if (row.kind == ns::RowKind::kText)
                {
                    if (row.secret)
                        use("common.not_set", choice.caption);
                    else if (choice.raw.empty())
                        use(tt::ChoiceKey(row.key, choice.raw), choice.caption);
                }
                else if (row.toggle)
                    use(choice.raw == "true" ? "common.on" : "common.off", choice.caption);
                else if (row.unavailable)
                    use("common.unavailable", choice.caption);
                else if (row.monitor)
                {
                    if (choice.raw == "0")
                        use(tt::ChoiceKey(row.key, choice.raw), choice.caption);
                }
                else if (!row.stepper && needsTranslation(choice.caption))
                    use(tt::ChoiceKey(row.key, choice.raw), choice.caption);
            }
        }
    }
    services.flags["system_link_invite"].value = "abc-123";
    ns::OptionsModel withInvite(services);
    use("common.set", selectRow(withInvite, "system_link_invite").Caption());
    use("note.restart", ns::kNoteRestart);
    use("note.next_game", ns::kNoteNextGame);
    use("note.unsaved", ns::kNoteUnsaved);
    use("note.rejected", ns::kNoteRejected);
    use("note.no_monitor", ns::kNoteNoMonitor);
    use("note.no_audio", ns::kNoteNoAudio);
    use("note.copied", "Copied.");
    use("note.keybind_capture", "Press a key. Esc cancels.");
    use("note.bind_refused", "That key cannot be bound. Press another key.");
    use("note.bind_icons", "Button icons update after a restart.");
    use("common.bind_or", "or");
    use("note.bind_conflict_one", "Also bound to another action.");
    use("note.bind_conflict", "Also bound to {0} other actions.");
    use("note.week_range", "Enter a week from 1 to " + std::to_string(ns::kWeekLast) + ".");

    const std::array<std::string_view, 6> kModelAreas{ "tab.", "section.", "setting.", "choice.", "common.", "note." };
    for (size_t i = tt::kIdCount; i < tt::RowCount(); ++i)
    {
        const auto key = std::string(tt::RowAt(i).key);
        CAPTURE(key);
        CHECK(std::any_of(kModelAreas.begin(), kModelAreas.end(), [&](std::string_view area)
                          {
                              return key.starts_with(area);
                          }));
        CHECK(produced.contains(key));
    }
}

TEST_CASE("options_model_follows_the_table_for_the_language", "[ctest][native-settings]")
{
    FakeServices services;
    addEveryRow(services);
    const ns::OptionsModel plain(services);
    for (const auto language : { tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian })
    {
        const ns::OptionsModel model(services, language);
        REQUIRE(model.Sections().size() == plain.Sections().size());
        for (size_t s = 0; s < plain.Sections().size(); ++s)
        {
            const auto& en = plain.Sections()[s];
            const auto& tr = model.Sections()[s];
            CHECK(tr.title == tt::Screen(tt::TabKey(en.title), en.title, language));
            REQUIRE(tr.rows.size() == en.rows.size());
            for (size_t r = 0; r < en.rows.size(); ++r)
            {
                const auto& a = en.rows[r];
                const auto& b = tr.rows[r];
                CAPTURE(a.key);
                CHECK(b.key == a.key);
                CHECK(b.label == tt::Screen(tt::SettingKey(a.key, "label"), a.label, language));
                if (a.kind != ns::RowKind::kBind)
                    CHECK(b.description == tt::Screen(tt::SettingKey(a.key, "tip"), a.description, language));
                CHECK(b.group == (a.group.empty() ? a.group : tt::Screen(tt::SectionKey(a.group), a.group, language)));
                REQUIRE(b.choices.size() == a.choices.size());
                for (size_t c = 0; c < a.choices.size(); ++c)
                {
                    CHECK(b.choices[c].raw == a.choices[c].raw);
                    if (a.kind == ns::RowKind::kBind)
                    {
                        if (a.choices[c].raw.empty())
                            CHECK(b.choices[c].caption == tt::Screen("common.bind_none", a.choices[c].caption, language));
                    }
                    else if (a.kind == ns::RowKind::kAction)
                        CHECK(b.choices[c].caption == tt::Screen("common.open", a.choices[c].caption, language));
                    else if (a.kind == ns::RowKind::kStatus)
                        CHECK(b.choices[c].caption == a.choices[c].caption);
                    else if (a.kind == ns::RowKind::kText)
                    {
                        if (a.secret)
                            CHECK(b.choices[c].caption == tt::Screen("common.not_set", a.choices[c].caption, language));
                        else
                            CHECK(b.choices[c].caption == tt::Screen(tt::ChoiceKey(a.key, a.choices[c].raw), a.choices[c].caption, language));
                    }
                    else if (a.toggle)
                        CHECK(b.choices[c].caption == tt::Screen(a.choices[c].raw == "true" ? "common.on" : "common.off", a.choices[c].caption, language));
                    else if (!a.unavailable && !a.monitor && !a.stepper && needsTranslation(a.choices[c].caption))
                        CHECK(b.choices[c].caption == tt::Screen(tt::ChoiceKey(a.key, a.choices[c].raw), a.choices[c].caption, language));
                }
            }
        }
    }
}

TEST_CASE("options_model_notes_follow_the_table_for_the_language", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services, tt::Language::kGerman);
    selectRow(model, "raging_barbarians");
    CHECK(model.Note() == tt::Screen("note.next_game", ns::kNoteNextGame, tt::Language::kGerman));
}

TEST_CASE("options_model_shows_language_names_in_their_own_language_in_every_language", "[ctest][native-settings]")
{
    FakeServices services;
    addEveryRow(services);
    for (const auto language : { tt::Language::kEnglish, tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian })
    {
        ns::OptionsModel model(services, language);
        const auto&      row = selectRow(model, "game_language");
        REQUIRE(row.choices.size() == 5);
        CHECK(row.choices[0].caption == "English");
        CHECK(row.choices[1].caption == "Deutsch");
        CHECK(row.choices[2].caption == "Fran\xC3\xA7"
                                        "ais");
        CHECK(row.choices[3].caption == "Espa\xC3\xB1"
                                        "ol");
        CHECK(row.choices[4].caption == "Italiano");
    }
}

TEST_CASE("options_model_an_action_row_runs_its_command_and_has_no_value_to_step", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "open_log_folder");
    CHECK(row.kind == ns::RowKind::kAction);
    CHECK(row.label == "Open Log Folder");
    CHECK(row.Caption() == "Open");
    CHECK(row.description == "Open the folder containing this session's log file.");
    REQUIRE(row.choices.size() == 1);

    CHECK_FALSE(model.Step(1));
    CHECK_FALSE(model.Step(-1));
    CHECK(model.Activate() == ns::OptionsModel::Press::kRan);
    CHECK(services.invoked == std::vector<std::string>{ "open_log_folder" });
    CHECK(services.applied.empty());
    CHECK(model.Note().empty());

    services.invokeResult = false;
    CHECK(model.Activate() == ns::OptionsModel::Press::kNone);
}

TEST_CASE("options_model_a_status_row_is_read_only_and_follows_the_registry", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["system_link_status"] = enumState("LAN", {});
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "system_link_status");
    CHECK(row.kind == ns::RowKind::kStatus);
    CHECK(row.label == "Connection Status");
    CHECK(row.Caption() == "LAN");
    CHECK(row.timing == ns::Timing::kLive);

    CHECK(model.Activate() == ns::OptionsModel::Press::kNone);
    CHECK_FALSE(model.Step(1));
    CHECK(services.applied.empty());
    CHECK(services.invoked.empty());

    services.flags["system_link_status"].value = "Relay: connected";
    for (int i = 0; i < 31; ++i)
        model.Poll();
    CHECK(selectRow(model, "system_link_status").Caption() == "Relay: connected");
    services.flags["system_link_status"].value = "Relay failed: <bad>" + std::string(100, 'x');
    for (int i = 0; i < 31; ++i)
        model.Poll();
    const auto& shown = selectRow(model, "system_link_status").Caption();
    CHECK(shown.starts_with("Relay failed: bad"));
    CHECK(shown.size() <= 60);
    services.flags["system_link_status"].value.clear();
    for (int i = 0; i < 31; ++i)
        model.Poll();
    CHECK(selectRow(model, "system_link_status").Caption() == "-");
}

TEST_CASE("options_model_the_online_rows_follow_the_f1_order_with_the_status_first", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    const auto&      online = model.Sections()[3];
    REQUIRE(online.title == "Online");
    std::vector<std::string> keys;
    for (const auto& row : online.rows)
        keys.push_back(row.key);
    CHECK(keys == std::vector<std::string>{ "system_link_status", "system_link_profile_name", "system_link_invite", "system_link_lan_enabled", "system_link_direct", "live_sessions", "discord_activity", "discord_invites" });
    CHECK(online.rows[0].group.empty());
    CHECK(online.rows[1].group.empty());
    CHECK(online.rows[2].group == "Connection");
    CHECK(online.rows[3].group == "Connection");
    CHECK(online.rows[1].label == "Profile Name");
    CHECK(online.rows[2].label == "Invite Code");
    // Both text rows take effect after a restart.
    CHECK(online.rows[1].timing == ns::Timing::kRestart);
    CHECK(online.rows[2].timing == ns::Timing::kRestart);
}

TEST_CASE("options_model_a_text_row_opens_an_entry_and_applies_the_text", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["system_link_profile_name"].value = "Aesh";
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "system_link_profile_name");
    CHECK(row.kind == ns::RowKind::kText);
    CHECK_FALSE(row.secret);
    CHECK(row.Caption() == "Aesh");
    CHECK(row.text == "Aesh");
    CHECK(row.rules.maxLength == 15);
    CHECK_FALSE(model.TextOpen());
    CHECK_FALSE(model.Step(1));

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    REQUIRE(model.TextOpen());
    CHECK(model.ModalOpen());
    CHECK(model.TextEditor().Text() == "Aesh");
    CHECK(model.TextRow() != nullptr);
    CHECK(services.applied.empty());

    model.TextEditor().Clear();
    for (const char c : std::string("New Name"))
        CHECK(model.TextEditor().Append(c));
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    CHECK_FALSE(model.TextOpen());
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied[0] == std::pair<std::string, std::string>{ "system_link_profile_name", "New Name" });
    CHECK(selectRow(model, "system_link_profile_name").Caption() == "New Name");
    CHECK(model.Note() == "Takes effect after restart.");
}

TEST_CASE("options_model_an_empty_profile_name_shows_the_default", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    CHECK(selectRow(model, "system_link_profile_name").Caption() == "Default");
    CHECK(selectRow(model, "system_link_profile_name").text.empty());
}

TEST_CASE("options_model_everything_else_is_inert_while_the_text_entry_is_open", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "system_link_profile_name");
    const size_t at = model.Highlight();
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);

    model.Move(1);
    model.Select(0);
    CHECK_FALSE(model.SelectTab(0));
    CHECK_FALSE(model.StepTab(1));
    CHECK_FALSE(model.Step(1));
    CHECK(model.Activate() == ns::OptionsModel::Press::kNone);
    CHECK(model.Highlight() == at);
    CHECK(services.applied.empty());

    model.CancelText();
    CHECK_FALSE(model.TextOpen());
    CHECK(services.applied.empty());
    CHECK(model.StepTab(1));
}

TEST_CASE("options_model_a_refused_text_keeps_the_entry_open_with_the_note", "[ctest][native-settings]")
{
    FakeServices services;
    services.status = ns::ApplyStatus::kRejected;
    ns::OptionsModel model(services);
    selectRow(model, "system_link_profile_name");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    model.TextEditor().Append('a');
    CHECK(model.CommitText() == ns::ApplyStatus::kRejected);
    CHECK(model.TextOpen());
    CHECK(model.Note() == "That value was not accepted.");
    CHECK(model.TextEditor().Text() == "a");

    model.SetTextRefused(false);
    CHECK(model.Note() == "Takes effect after restart.");
    services.status = ns::ApplyStatus::kUnsaved;
    CHECK(model.CommitText() == ns::ApplyStatus::kUnsaved);
    CHECK_FALSE(model.TextOpen());
    CHECK(model.Note() == "Changed for this session, but it could not be saved.");
}

TEST_CASE("options_model_the_invite_code_is_never_shown", "[ctest][native-settings]")
{
    const std::string secret = "TOPSECRET-room-secret-123";
    FakeServices      services;
    services.flags["system_link_invite"].value = secret;
    ns::OptionsModel model(services);

    const auto& row = selectRow(model, "system_link_invite");
    CHECK(row.kind == ns::RowKind::kText);
    CHECK(row.secret);
    CHECK(row.Caption() == "Set");
    CHECK(row.text.empty());
    CHECK(row.timing == ns::Timing::kRestart);
    const auto holdsSecret = [&](const ns::OptionRow& r)
    {
        const auto has = [](const std::string& text)
        {
            return text.find("SECRET") != std::string::npos;
        };
        if (has(r.label) || has(r.description) || has(r.text) || has(r.key))
            return true;
        return std::any_of(r.choices.begin(), r.choices.end(), [&](const ns::Choice& c)
                           {
                               return has(c.raw) || has(c.caption);
                           });
    };
    for (const auto& section : model.Sections())
    {
        for (const auto& r : section.rows)
            CHECK_FALSE(holdsSecret(r));
    }
    CHECK(model.Note().find("SECRET") == std::string::npos);
    CHECK(model.Description().find("SECRET") == std::string::npos);

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    CHECK(model.TextEditor().Text().empty());
    CHECK(model.TextEditor().GetRules().charset == rerevved::text_entry::Charset::kInvite);
    CHECK(model.TextEditor().Paste(" \"newcode-42\" "));
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied[0] == std::pair<std::string, std::string>{ "system_link_invite", "newcode-42" });
    CHECK(selectRow(model, "system_link_invite").Caption() == "Set");
    CHECK(selectRow(model, "system_link_invite").text.empty());

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    model.TextEditor().Clear();
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "system_link_invite", "" });
    CHECK(selectRow(model, "system_link_invite").Caption() == "Not set");
}

TEST_CASE("options_model_text_rows_follow_the_table_for_the_language", "[ctest][native-settings]")
{
    FakeServices services;
    for (const auto language : { tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian })
    {
        ns::OptionsModel model(services, language);
        CAPTURE(static_cast<int>(language));
        CHECK(selectRow(model, "open_log_folder").Caption() == tt::Screen("common.open", "Open", language));
        CHECK(selectRow(model, "system_link_invite").Caption() == tt::Screen("common.not_set", "Not set", language));
        CHECK(selectRow(model, "system_link_profile_name").Caption() == tt::Screen("choice.system_link_profile_name.default", "Default", language));
        CHECK(selectRow(model, "system_link_status").label == tt::Screen("setting.system_link_status.label", "Connection Status", language));
        CHECK(selectRow(model, "system_link_status").Caption() == "Off");
    }
}

TEST_CASE("options_model_takes_the_longest_valid_invite_code", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "system_link_invite");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    std::string code = "RXR1-";
    code.append(575, 'A');
    CHECK(model.TextEditor().Paste(code));
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied[0].second == code);
}

TEST_CASE("options_model_row_buttons_follow_the_row_kind", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["system_link_invite"].value = "RXR1-ABC";
    ns::OptionsModel model(services);
    CHECK(selectRow(model, "open_log_folder").HasOpenButton());
    CHECK_FALSE(selectRow(model, "open_log_folder").HasCopyPaste());
    const auto& invite = selectRow(model, "system_link_invite");
    CHECK(invite.HasCopyPaste());
    CHECK_FALSE(invite.HasOpenButton());
    CHECK(invite.CanCopy());
    CHECK_FALSE(selectRow(model, "system_link_profile_name").HasCopyPaste());
    CHECK_FALSE(selectRow(model, "system_link_status").HasOpenButton());

    FakeServices     empty;
    ns::OptionsModel unset(empty);
    const auto&      none = selectRow(unset, "system_link_invite");
    CHECK(none.HasCopyPaste());
    CHECK_FALSE(none.CanCopy());
    int puts = 0;
    CHECK_FALSE(unset.CopySecret([&](const char*)
                                 {
                                     ++puts;
                                 }));
    CHECK(puts == 0);
}

TEST_CASE("options_model_copy_hands_over_the_invite_and_says_so", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["system_link_invite"].value = "RXR1-ABC";
    ns::OptionsModel model(services);
    selectRow(model, "system_link_invite");
    std::string got;
    CHECK(model.CopySecret([&](const char* value)
                           {
                               got = value;
                           }));
    CHECK(got == "RXR1-ABC");
    CHECK(model.Note() == "Copied.");
    CHECK(services.applied.empty());
    CHECK(selectRow(model, "system_link_invite").text.empty());
    model.Select(0);
    CHECK(model.Note() != "Copied.");

    selectRow(model, "system_link_profile_name");
    CHECK_FALSE(model.CopySecret([&](const char*)
                                 {
                                     FAIL("copied from another row");
                                 }));
    selectRow(model, "system_link_invite");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    CHECK_FALSE(model.CopySecret([&](const char*)
                                 {
                                     FAIL("copied while the entry was open");
                                 }));
}

TEST_CASE("options_model_paste_applies_the_normalized_invite", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "system_link_invite");
    CHECK(model.PasteSecret("  \"RXR1-ABCD-EFGH\"\n") == ns::ApplyStatus::kSaved);
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied[0] == std::pair<std::string, std::string>{ "system_link_invite", "RXR1-ABCD-EFGH" });
    CHECK(selectRow(model, "system_link_invite").Caption() == "Set");
    CHECK(selectRow(model, "system_link_invite").CanCopy());
    CHECK(model.Note() == tt::Screen("note.restart", ns::kNoteRestart, tt::Language::kEnglish));

    CHECK(model.PasteSecret("not a code!") == ns::ApplyStatus::kRejected);
    CHECK(model.PasteSecret("") == ns::ApplyStatus::kRejected);
    CHECK(model.PasteSecret(std::string(selectRow(model, "system_link_invite").rules.maxLength + 1, 'A')) == ns::ApplyStatus::kRejected);
    CHECK(services.applied.size() == 1);
    CHECK(model.Note() == ns::OutcomeNote(ns::ApplyStatus::kRejected));

    selectRow(model, "system_link_profile_name");
    CHECK(model.PasteSecret("RXR1-ZZZZ") == ns::ApplyStatus::kRejected);
    selectRow(model, "system_link_invite");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    CHECK(model.PasteSecret("RXR1-ZZZZ") == ns::ApplyStatus::kRejected);
    CHECK(services.applied.size() == 1);
}

TEST_CASE("options_normalize_week_takes_digits_in_range_only", "[ctest][native-settings]")
{
    CHECK(ns::NormalizeWeek("1") == "1");
    CHECK(ns::NormalizeWeek("901") == "901");
    CHECK(ns::NormalizeWeek("0007") == "7");
    CHECK(ns::NormalizeWeek("2147483647") == "2147483647");
    CHECK(ns::NormalizeWeek("00000000000002147483647") == "2147483647");
    CHECK(ns::NormalizeWeek(std::string(27, '0') + "42") == "42");
    CHECK(ns::NormalizeWeek(std::string(200, '0') + "1") == "1");
    CHECK_FALSE(ns::NormalizeWeek(""));
    CHECK_FALSE(ns::NormalizeWeek("0"));
    CHECK_FALSE(ns::NormalizeWeek("000"));
    CHECK_FALSE(ns::NormalizeWeek("2147483648"));
    CHECK_FALSE(ns::NormalizeWeek("99999999999999999999999999"));
    CHECK_FALSE(ns::NormalizeWeek("12a"));
    CHECK_FALSE(ns::NormalizeWeek("-5"));
    CHECK_FALSE(ns::NormalizeWeek(" 5"));
}

TEST_CASE("options_model_types_the_custom_week_whole", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["gotw_week"]        = enumState("900", {});
    services.flags["gotw_custom_week"] = boolState(true);
    ns::OptionsModel model(services);
    selectRow(model, "gotw_week");

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    CHECK(model.TextEditor().Text() == "900");
    CHECK(model.TextEditor().Selected());
    CHECK(model.TextEditor().GetRules().charset == rerevved::text_entry::Charset::kDigits);
    CHECK(model.TextEditor().GetRules().maxLength == ns::kWeekEntryLength);
    CHECK(model.TextKeyboard().GetLayout() == rerevved::text_entry::Layout::kDigits);
    CHECK(model.TextRow()->key == "gotw_week");

    CHECK_FALSE(model.TextEditor().Append('x'));
    CHECK(model.TextEditor().Text() == "900");
    CHECK(model.TextEditor().Append('4'));
    CHECK(model.TextEditor().Append('2'));
    CHECK(model.TextEditor().Text() == "42");
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied[0] == std::pair<std::string, std::string>{ "gotw_week", "42" });
    CHECK_FALSE(model.TextOpen());
    CHECK(selectRow(model, "gotw_week").Caption() == "42");
    CHECK(model.Note() == ns::TimingNote(ns::Timing::kNextGame));

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    model.TextEditor().Clear();
    CHECK(model.CommitText() == ns::ApplyStatus::kRejected);
    CHECK(model.TextOpen());
    CHECK(model.Note() == "Enter a week from 1 to 2147483647.");
    CHECK(model.TextEditor().Append('0'));
    CHECK(model.CommitText() == ns::ApplyStatus::kRejected);
    model.TextEditor().Clear();
    CHECK(model.TextEditor().Paste("2147483648"));
    CHECK(model.CommitText() == ns::ApplyStatus::kRejected);
    CHECK(services.applied.size() == 1);

    model.TextEditor().Clear();
    CHECK_FALSE(model.TextEditor().Paste("12 3"));
    CHECK(model.TextEditor().Paste(" 0050 "));
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "50" });

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    model.CancelText();
    CHECK(services.applied.size() == 2);
    CHECK(model.Step(1));
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "51" });
}

TEST_CASE("options_model_week_entry_opens_on_the_plain_week_of_a_padded_value", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["gotw_week"]        = enumState("00000000042", {});
    services.flags["gotw_custom_week"] = boolState(true);
    ns::OptionsModel model(services);
    selectRow(model, "gotw_week");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    CHECK(model.TextEditor().Text() == "42");
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "42" });
}

TEST_CASE("options_model_week_entry_refuses_an_eleventh_digit_and_takes_padded_input", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["gotw_week"]        = enumState(std::string(27, '0') + "42", {});
    services.flags["gotw_custom_week"] = boolState(true);
    ns::OptionsModel model(services);
    selectRow(model, "gotw_week");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    CHECK(model.TextEditor().Text() == "42");

    // 21474836471 keeps its last digit and is refused, not saved as 2147483647.
    model.TextEditor().Clear();
    for (const char c : std::string("21474836471"))
        CHECK(model.TextEditor().Append(c));
    CHECK(model.TextEditor().Text() == "21474836471");
    CHECK(model.CommitText() == ns::ApplyStatus::kRejected);
    CHECK(model.Note() == "Enter a week from 1 to 2147483647.");
    CHECK(services.applied.empty());
    CHECK(model.TextEditor().Backspace());
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "2147483647" });

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    model.TextEditor().Clear();
    CHECK(model.TextEditor().Paste("00000000042"));
    CHECK(model.TextEditor().Text() == "42");
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "42" });
}

TEST_CASE("options_model_week_entry_types_zero_padded_weeks_as_the_plain_week", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["gotw_week"]        = enumState("900", {});
    services.flags["gotw_custom_week"] = boolState(true);
    ns::OptionsModel model(services);
    selectRow(model, "gotw_week");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    model.TextEditor().Clear();
    for (const char c : std::string("000000000042"))
        CHECK(model.TextEditor().Append(c));
    CHECK(model.TextEditor().Text() == "42");
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "42" });

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kOpenText);
    model.TextEditor().Clear();
    CHECK(model.TextEditor().Append('0'));
    CHECK(model.TextEditor().Paste("00000000042"));
    CHECK(model.CommitText() == ns::ApplyStatus::kSaved);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "gotw_week", "42" });
}

namespace
{

const ns::OptionRow* findRow(const ns::OptionsModel& model, std::string_view key)
{
    for (const auto& section : model.Sections())
    {
        for (const auto& row : section.rows)
        {
            if (row.key == key)
                return &row;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("options_model_lists_f1s_keybind_sections_after_the_controls_rows", "[ctest][native-settings]")
{
    FakeServices             services;
    ns::OptionsModel         model(services);
    const ns::OptionSection* controls = nullptr;
    for (const auto& section : model.Sections())
    {
        if (section.title == "Controls")
            controls = &section;
    }
    REQUIRE(controls);

    std::vector<std::string> keys;
    std::string              previousGroup = "-";
    std::vector<std::string> groups;
    for (const auto& row : controls->rows)
    {
        keys.push_back(row.key);
        if (row.group != previousGroup)
            groups.push_back(row.group);
        previousGroup = row.group;
    }
    CHECK(groups == std::vector<std::string>{ "", "Keybinds", "Overlay Hotkeys" });
    const size_t firstBind = std::find(keys.begin(), keys.end(), "keybind_a") - keys.begin();
    REQUIRE(firstBind < keys.size());
    CHECK(keys[firstBind - 1] == "mnk_sensitivity");
    CHECK(std::find(keys.begin(), keys.end(), "bind_debug_overlay") == keys.end());

    const auto* a = findRow(model, "keybind_a");
    REQUIRE(a);
    CHECK(a->kind == ns::RowKind::kBind);
    CHECK(a->label == "A Button");
    // The title keeps one main key where the SDK ships two.
    CHECK(a->Caption() == ";");
    CHECK(a->bindController);
    // The SDK reads a bind live; only the keyboard icons follow a restart.
    CHECK(a->timing == ns::Timing::kLive);
    CHECK(findRow(model, "keybind_dpad_up")->label == "D-Pad Up");
    CHECK(findRow(model, "keybind_dpad_up")->Caption() == "Shift+Up");
    CHECK(findRow(model, "keybind_guide")->Caption() == "(none)");
    const auto* console = findRow(model, "bind_console");
    REQUIRE(console);
    CHECK(console->label == "Toggle Console Overlay");
    CHECK(console->Caption() == "`");
    CHECK_FALSE(console->bindController);
    CHECK(console->timing == ns::Timing::kLive);
    CHECK(findRow(model, "bind_fps_counter")->label == "Toggle FPS Counter");
}

TEST_CASE("options_model_binds_the_captured_key_and_ends_the_capture", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "keybind_x");

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kCapture);
    CHECK(model.Capturing());
    CHECK(model.IsCapturing("keybind_x"));
    CHECK(model.ModalOpen());
    CHECK(model.Note() == "Press a key. Esc cancels.");
    CHECK_FALSE(model.Step(1));
    model.Move(1);
    CHECK(model.Row(model.Highlight())->key == "keybind_x");
    CHECK_FALSE(model.StepTab(1));
    CHECK_FALSE(model.ClearBind());
    CHECK_FALSE(model.ResetBind());
    CHECK(model.Activate() == ns::OptionsModel::Press::kNone);

    CHECK(model.CommitBinding("Shift+G") == ns::ApplyStatus::kSaved);
    CHECK_FALSE(model.Capturing());
    REQUIRE(services.applied.size() == 1);
    CHECK(services.applied[0] == std::pair<std::string, std::string>{ "keybind_x", "Shift+G" });
    CHECK(findRow(model, "keybind_x")->Caption() == "Shift+G");
    CHECK(model.Note() == "Button icons update after a restart.");
    CHECK(model.CommitBinding("H") == ns::ApplyStatus::kRejected);
    CHECK(services.applied.size() == 1);

    selectRow(model, "bind_console");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kCapture);
    CHECK(model.CommitBinding("F9") == ns::ApplyStatus::kSaved);
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "bind_console", "F9" });
    CHECK(model.Note().empty());
}

TEST_CASE("options_model_cancels_a_capture_without_changing_the_key", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    selectRow(model, "keybind_y");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kCapture);
    model.CancelCapture();
    CHECK_FALSE(model.Capturing());
    CHECK(services.applied.empty());
    CHECK(findRow(model, "keybind_y")->Caption() == "P");
    CHECK(model.Activate() == ns::OptionsModel::Press::kCapture);
    model.Reload();
    CHECK_FALSE(model.Capturing());
}

TEST_CASE("options_model_clears_and_resets_a_bind", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    const auto&      row = selectRow(model, "keybind_a");
    CHECK(row.CanClearBind());
    CHECK_FALSE(row.CanResetBind());
    CHECK_FALSE(model.ResetBind());
    CHECK(services.applied.empty());

    CHECK(model.ClearBind());
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "keybind_a", "" });
    CHECK(selectRow(model, "keybind_a").Caption() == "(none)");
    CHECK_FALSE(selectRow(model, "keybind_a").CanClearBind());
    CHECK(selectRow(model, "keybind_a").CanResetBind());
    CHECK_FALSE(model.ClearBind());
    CHECK(services.applied.size() == 1);

    CHECK(model.ResetBind());
    CHECK(services.applied.back() == std::pair<std::string, std::string>{ "keybind_a", "Semicolon" });
    CHECK(selectRow(model, "keybind_a").Caption() == ";");

    selectRow(model, "keybind_guide");
    CHECK_FALSE(model.ClearBind());
    CHECK_FALSE(model.ResetBind());

    selectRow(model, "mnk_mode");
    CHECK_FALSE(model.ClearBind());
    CHECK_FALSE(model.ResetBind());
}

TEST_CASE("options_model_flags_a_key_other_binds_also_use_as_f1_does", "[ctest][native-settings]")
{
    FakeServices services;
    services.flags["keybind_x"].conflicts = 1;
    services.flags["keybind_y"].conflicts = 3;
    ns::OptionsModel model(services);
    // F1 only warns; the bind is kept and the note says how many share it.
    selectRow(model, "keybind_x");
    CHECK(model.Note() == "Also bound to another action. Button icons update after a restart.");
    selectRow(model, "keybind_y");
    CHECK(model.Note() == "Also bound to 3 other actions. Button icons update after a restart.");
    selectRow(model, "bind_console");
    CHECK(model.Note().empty());
    services.flags["bind_console"].conflicts = 2;
    model.Reload();
    selectRow(model, "bind_console");
    CHECK(model.Note() == "Also bound to 2 other actions.");

    services.status = ns::ApplyStatus::kRejected;
    selectRow(model, "keybind_a");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kCapture);
    CHECK(model.CommitBinding("Q") == ns::ApplyStatus::kRejected);
    CHECK(model.Note() == ns::OutcomeNote(ns::ApplyStatus::kRejected));
    CHECK(findRow(model, "keybind_a")->Caption() == ";");
}

TEST_CASE("options_model_bind_text_follows_the_table_for_the_language", "[ctest][native-settings]")
{
    FakeServices services;
    // A player's binding can hold two keys even though title defaults use one.
    services.flags["keybind_a"].value = "Semicolon,Space";
    for (const auto language : { tt::Language::kGerman, tt::Language::kFrench, tt::Language::kSpanish, tt::Language::kItalian })
    {
        ns::OptionsModel model(services, language);
        CAPTURE(static_cast<int>(language));
        const auto* a = findRow(model, "keybind_a");
        REQUIRE(a);
        CHECK(a->label == tt::Screen("setting.keybind_a.label", "A Button", language));
        CHECK(a->label != "A Button");
        CHECK(a->group == tt::Screen("section.keybinds", "Keybinds", language));
        CHECK(findRow(model, "bind_console")->group == tt::Screen("section.overlay_hotkeys", "Overlay Hotkeys", language));
        CHECK(findRow(model, "keybind_guide")->Caption() == tt::Screen("common.bind_none", "(none)", language));
        CHECK(a->description == tt::Screen("common.bind_tip_controller", "x", language));
        CHECK(a->Caption() == "; " + tt::Screen("common.bind_or", "or", language) + " Space");
        CHECK(a->Caption() != "; or Space");
    }
}

TEST_CASE("native_settings_host_lists_f1s_binds_and_counts_shared_keys", "[ctest][native-settings]")
{
    rex::ui::RegisterBind("bind_native_test_a", "F9", "Native test one", []
                          {
                          });
    rex::ui::RegisterBind("bind_native_test_b", "F9", "Native test two", []
                          {
                          });
    rex::ui::RegisterBind("bind_debug_overlay", "F9", "Toggle debug overlay", []
                          {
                          });

    ns::HostBindings host;
    auto             layout = std::make_shared<rex::ui::SettingsLayout>();
    rerevved::ConfigureSettingsLayout(*layout);
    host.layout         = layout;
    const auto services = ns::MakeUiServices(host);
    REQUIRE(services);
    const auto keys = services->BindKeys();
    const auto has  = [&](std::string_view key)
    {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    CHECK(has("bind_native_test_a"));
    CHECK(has("bind_native_test_b"));
    CHECK_FALSE(has("bind_debug_overlay"));

    const auto a = services->Read("bind_native_test_a");
    REQUIRE(a);
    CHECK(a->category == "Input/Keybinds/System");
    CHECK(a->description == "Native test one");
    CHECK(a->defaultValue == "F9");
    CHECK(a->conflicts == 2);
    rex::cvar::SetFlagByName("bind_native_test_b", "F10");
    CHECK(services->Read("bind_native_test_a")->conflicts == 1);
    CHECK(services->Read("bind_native_test_b")->conflicts == 0);

    for (const char* name : { "bind_native_test_a", "bind_native_test_b", "bind_debug_overlay" })
    {
        rex::ui::UnregisterBind(name);
        rex::cvar::UnregisterFlag(name);
    }
}

TEST_CASE("options_model_refuses_a_key_with_no_name_and_keeps_waiting", "[ctest][native-settings]")
{
    FakeServices     services;
    ns::OptionsModel model(services);
    model.RefuseCapture();
    selectRow(model, "keybind_a");
    CHECK(model.Note() != "That key cannot be bound. Press another key.");

    REQUIRE(model.Activate() == ns::OptionsModel::Press::kCapture);
    model.RefuseCapture();
    CHECK(model.Capturing());
    CHECK(model.Note() == "That key cannot be bound. Press another key.");
    CHECK(services.applied.empty());
    CHECK(model.CommitBinding("Z") == ns::ApplyStatus::kSaved);
    CHECK(model.Note() != "That key cannot be bound. Press another key.");
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kCapture);
    CHECK(model.Note() == "Press a key. Esc cancels.");
    model.RefuseCapture();
    model.CancelCapture();
    REQUIRE(model.Activate() == ns::OptionsModel::Press::kCapture);
    CHECK(model.Note() == "Press a key. Esc cancels.");
}
