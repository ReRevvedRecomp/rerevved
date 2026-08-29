#include "app.h"

#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system.h>
#include <rex/system/game_data_selector.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "build_provenance.h"
#include "game_content.h"
#include "presence.h"

REXCVAR_DECLARE(std::string, game_data_root);

namespace
{

constexpr std::size_t kMaxReportedErrors = 10;

} // namespace

void ReRevvedApp::OnPreSetup(rex::RuntimeConfig& config)
{
    REXLOG_INFO("{}", REREVVED_BUILD_PROVENANCE);
    config.game_version = REREVVED_VERSION;
}

void ReRevvedApp::OnConfigurePaths(rex::PathConfig& paths)
{
    // Keep user state outside a potentially read-only install directory.
    paths.config_path = paths.user_data_root / "rerevved.toml";
}

bool ReRevvedApp::SetupEnvironment()
{
    // A bare launch otherwise runs without GPU emulation. The config file is
    // loaded by the base setup after these defaults are applied.
    if (rex::cvar::GetFlagByName("gpu_plugin").empty())
    {
        rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    if (!rex::ReXApp::SetupEnvironment())
    {
        return false;
    }

    // Explicit roots bypass selection, not validation.
    if (!game_data_root().empty())
    {
        return true;
    }

    rex::system::GameDataSelectorSettings settings;
    settings.default_xex_sha256  = rerevved::kBaseXexSha256;
    settings.sibling_xexp_sha256 = rerevved::kUpdateXexpSha256;
    settings.config_path         = user_data_root() / "rerevved.toml";
    return rex::system::GameDataSelector::EnsureGameData(settings);
}

bool ReRevvedApp::SetupPresentation()
{
    if (!rex::ReXApp::SetupPresentation())
    {
        return false;
    }

    // The Xenos plugin owns this cvar, so it is not registered until the base
    // presentation setup loads the plugin.
    if (rex::cvar::GetFlagByName("render_target_path_d3d12").empty())
    {
        rex::cvar::SetFlagByName("render_target_path_d3d12", "rov");
    }
    return true;
}

std::optional<rex::PathConfig> ReRevvedApp::OnFinalizePaths(const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume)
{
    (void)resume;

    auto paths = defaults;
    if (paths.game_data_root.empty())
    {
        // Selection may resolve the root after the defaults were captured.
        paths.game_data_root = std::filesystem::path(std::string(REXCVAR_GET(game_data_root)));
    }

    auto result = rerevved::VerifyContentRoot(paths.game_data_root, rerevved::ContentDepth::kQuick);
    if (!result.ok)
    {
        std::string message = fmt::format("The game content folder failed validation:\n{}\n\n", paths.game_data_root.string());
        for (std::size_t index = 0; index < result.errors.size(); ++index)
        {
            REXLOG_ERROR("Content validation: {}", result.errors[index]);
            if (index < kMaxReportedErrors)
            {
                message += result.errors[index] + "\n";
            }
        }
        if (result.errors.size() > kMaxReportedErrors)
        {
            message += fmt::format("... and {} more (see log).\n", result.errors.size() - kMaxReportedErrors);
        }
        for (const auto& error : result.errors)
        {
            // The GFX_ files ship with the title update, not the disc.
            if (error.find("GFX_") != std::string::npos)
            {
                message += "\nThe missing GFX_ files come from the version 1.3 title update; copy its Resource folder over the content folder.\n";
                break;
            }
        }
        message += "\nFix the folder, or remove game_data_root from rerevved.toml to run the selector again.";
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, message);
        app_context().RequestDeferredQuit();
        return std::nullopt;
    }

    return paths;
}

void ReRevvedApp::OnPostSetup()
{
    rex::ReXApp::OnPostSetup();

    rerevved::StartPresence();

    // GetName() remains lowercase because it also names user data paths.
    window()->SetTitle("ReRevved");
}

void ReRevvedApp::OnShutdown()
{
    rerevved::StopPresence();
}

bool ReRevvedApp::OnWindowCloseRequested()
{
    rerevved::StopPresence();
    return true;
}
