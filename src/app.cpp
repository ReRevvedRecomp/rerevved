#include "app.h"

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <api/gameplay_state.h>
#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/xenos_fence_trace.h>
#include <rex/logging.h>
#include <rex/system.h>
#include <rex/system/game_data_selector.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "build_info.h"
#include "game_content.h"
#include "gpu/diagnostics/native_renderer_passive_trace.h"
#include "gpu/guest_gpu_service.h"
#include "main_menu_logo_asset.h"
#include "presence.h"

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DEFINE_STRING(combat_speed, "normal", "ReRevved", "Combat presentation speed")
    .allowed({ "normal", "fast" });
REXCVAR_DEFINE_STRING(renderer, "xenos", "ReRevved", "Renderer backend: xenos or native")
    .allowed({ "xenos", "native" })
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_coverage_run, "", "ReRevved", "native-renderer observer run ID")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_coverage_transition, "", "ReRevved", "native-renderer observer transition ID")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_coverage_input_digest, "", "ReRevved", "native-renderer observer input digest")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_coverage_output, "", "ReRevved", "native-renderer observer output directory")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_passive_trace_output, "", "ReRevved", "Ignored local CSV path for the passive Resolve/VdSwap trace")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_fence_trace_output, "", "ReRevved", "Ignored local CSV path for the bounded Xenos consumer/fence trace")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace
{

constexpr std::size_t kMaxReportedErrors    = 10;
constexpr std::size_t kCoverageInputZOrder  = 1000;
constexpr uint32_t    kFinalCoverageSegment = 7;

bool isContainedPath(const std::filesystem::path& root,
                     const std::filesystem::path& path)
{
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
    {
        return false;
    }
    return *relative.begin() != "..";
}

bool containsExistingReparsePoint(const std::filesystem::path& root,
                                  const std::filesystem::path& path)
{
#if defined(_WIN32)
    if (!isContainedPath(root, path))
    {
        return true;
    }

    auto isReparsePoint = [](const std::filesystem::path& candidate)
    {
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            return error != ERROR_FILE_NOT_FOUND &&
                   error != ERROR_PATH_NOT_FOUND;
        }
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    };

    if (isReparsePoint(root))
    {
        return true;
    }
    std::filesystem::path current = root;
    for (const auto& component : path.lexically_relative(root))
    {
        current /= component;
        if (isReparsePoint(current))
        {
            return true;
        }
    }
    return false;
#else
    if (!isContainedPath(root, path))
    {
        return true;
    }

    auto isSymlink = [](const std::filesystem::path& candidate)
    {
        std::error_code error;
        const auto      status = std::filesystem::symlink_status(candidate, error);
        if (error)
        {
            return true;
        }
        return status.type() == std::filesystem::file_type::symlink;
    };

    if (isSymlink(root))
    {
        return true;
    }
    std::filesystem::path current = root;
    for (const auto& component : path.lexically_relative(root))
    {
        current /= component;
        if (isSymlink(current))
        {
            return true;
        }
    }
    return false;
#endif
}

bool resolvePassiveTracePath(std::string_view       configured,
                             std::filesystem::path& outputPath)
{
    std::error_code             error;
    const std::filesystem::path scratchRoot =
        std::filesystem::absolute("out", error).lexically_normal();
    if (error)
    {
        return false;
    }
    const std::filesystem::path candidate =
        std::filesystem::absolute(configured, error).lexically_normal();
    if (error || candidate.extension() != ".csv" ||
        !isContainedPath(scratchRoot, candidate) ||
        containsExistingReparsePoint(scratchRoot, candidate))
    {
        return false;
    }
    if (std::filesystem::exists(candidate, error) || error)
    {
        return false;
    }

    const std::filesystem::path canonicalRoot =
        std::filesystem::weakly_canonical(scratchRoot, error);
    if (error)
    {
        return false;
    }
    const std::filesystem::path canonicalParent =
        std::filesystem::weakly_canonical(candidate.parent_path(), error);
    if (error)
    {
        return false;
    }
    outputPath = (canonicalParent / candidate.filename()).lexically_normal();
    return isContainedPath(canonicalRoot, outputPath);
}

rerevved::native_renderer::SnapshotFields readCoverageSnapshot() noexcept
{
    GameplayState state{};
    state.structSize = sizeof(state);
    (void)GetGameplayState(&state, sizeof(state));

    rerevved::native_renderer::SnapshotFields fields{};
    fields.frameSequence   = state.frameSequence;
    fields.validFields     = state.validFields;
    fields.gameplayActive  = state.gameplayActive != 0;
    fields.interfaceUpdate = state.interfaceUpdate != 0;
    fields.activePlayer    = state.activePlayer;
    fields.humanPlayerMask = state.humanPlayerMask;
    fields.turnOwnerKnown  = state.turnOwnerKnown != 0;
    fields.humanTurn       = state.humanTurn != 0;
    fields.available       = state.available != 0;
    fields.civilization    = static_cast<int32_t>(state.civilization);
    fields.era             = state.era;
    fields.year            = state.year;
    fields.turn            = state.turn;
    return fields;
}

} // namespace

namespace rerevved
{

void App::OnPreSetup(rex::RuntimeConfig& config)
{
    REXLOG_INFO("{}", REREVVED_BUILD_INFO);
    config.game_version = REREVVED_VERSION;
    if (rendererBackend == rerevved::gpu::RendererBackend::Native)
    {
        config.graphics = std::make_unique<rerevved::gpu::NativeGuestGpuService>();
        config.gpu_plugin.clear();
    }
}

std::filesystem::path App::GetDefaultUserDataRoot() const
{
    const auto userFolder = rex::filesystem::GetUserFolder();
#if defined(_WIN32)
    return userFolder / "My Games" / "ReRevved";
#else
    return userFolder / "rerevved";
#endif
}

void App::OnConfigurePaths(rex::PathConfig& paths)
{
    // Keep user state outside a potentially read-only install directory.
    paths.config_path = paths.user_data_root / "rerevved.toml";
}

std::optional<rex::system::ProfileCopySpecification> App::GetProfileCopySpecification() const
{
    return rex::system::ProfileCopySpecification{
        .config_relative_path = "rerevved.toml",
        .title_id             = rerevved::kTitleId,
    };
}

bool App::SetupEnvironment()
{
    if (!rex::ReXApp::SetupEnvironment())
    {
        return false;
    }

    rendererBackend = rerevved::gpu::ParseRendererBackend(REXCVAR_GET(renderer));
    if (rendererBackend == rerevved::gpu::RendererBackend::Invalid)
    {
        REXLOG_ERROR("Invalid ReRevved renderer '{}'; expected xenos or native",
                     REXCVAR_GET(renderer));
        return false;
    }
    if (rendererBackend == rerevved::gpu::RendererBackend::Native)
    {
        rex::cvar::SetFlagByName("gpu_plugin", "");
    }
    else if (rendererBackend == rerevved::gpu::RendererBackend::Xenos)
    {
        // Apply the title default below config, environment, and CLI values.
        rex::cvar::SetFlagAsApplicationDefault("gpu_plugin", "xenos");
    }
    REXLOG_INFO("ReRevved renderer selected: {}",
                rerevved::gpu::RendererBackendName(rendererBackend));

    const std::string passiveTraceOutput =
        REXCVAR_GET(native_renderer_passive_trace_output);
    const std::string fenceTraceOutput =
        REXCVAR_GET(native_renderer_fence_trace_output);
    passiveTraceOutputPath.clear();
    fenceTraceOutputPath.clear();
    if (!passiveTraceOutput.empty() && !fenceTraceOutput.empty())
    {
        REXLOG_ERROR(
            "Passive Resolve/VdSwap tracing and consumer/fence tracing cannot run together");
        return false;
    }
    if ((!passiveTraceOutput.empty() || !fenceTraceOutput.empty()) &&
        !REXCVAR_GET(native_renderer_coverage_run).empty())
    {
        REXLOG_ERROR(
            "Native renderer diagnostic tracing cannot share a native-renderer coverage run");
        return false;
    }
    if (!passiveTraceOutput.empty() &&
        (rendererBackend != rerevved::gpu::RendererBackend::Xenos ||
         !resolvePassiveTracePath(passiveTraceOutput,
                                  passiveTraceOutputPath)))
    {
        REXLOG_ERROR(
            "Passive Resolve/VdSwap trace requires Xenos and a non-reparse CSV path under the ignored out directory");
        return false;
    }
    if (!fenceTraceOutput.empty() &&
        (rendererBackend != rerevved::gpu::RendererBackend::Xenos ||
         !resolvePassiveTracePath(fenceTraceOutput,
                                  fenceTraceOutputPath)))
    {
        REXLOG_ERROR(
            "Xenos consumer/fence trace requires Xenos and a non-reparse CSV path under the ignored out directory");
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

bool App::SetupPresentation()
{
    if (!rex::ReXApp::SetupPresentation())
    {
        return false;
    }

    if (rendererBackend == rerevved::gpu::RendererBackend::Native)
    {
        if (!window() || !nativeRenderer.Initialize(*window()))
        {
            REXLOG_ERROR("Native renderer presentation setup failed");
            return false;
        }
    }
    // The Xenos plugin owns this cvar, so it is not registered until the base
    // presentation setup loads the plugin.
    else
    {
        rex::cvar::SetFlagAsApplicationDefault("render_target_path_d3d12", "rov");
    }

    const std::string runId = REXCVAR_GET(native_renderer_coverage_run);
    if (!runId.empty())
    {
        const std::string transitionId =
            REXCVAR_GET(native_renderer_coverage_transition);
        const std::string inputDigest =
            REXCVAR_GET(native_renderer_coverage_input_digest);
        const std::string outputName =
            REXCVAR_GET(native_renderer_coverage_output);
        const std::filesystem::path runRoot = user_data_root().parent_path();
        if (user_data_root().filename() != "user-data" ||
            outputName != "observer" || runRoot.empty())
        {
            REXLOG_ERROR("native-renderer coverage requires the isolated runner path layout");
            return false;
        }

        const std::filesystem::path outputDirectory =
            runRoot / outputName;
        const std::string                       outputDirectoryText = outputDirectory.string();
        const std::string                       runRootText         = runRoot.string();
        rerevved::native_renderer::StartOptions options{};
        options.runId           = runId.c_str();
        options.transitionId    = transitionId.c_str();
        options.inputDigest     = inputDigest.c_str();
        options.outputDirectory = outputDirectoryText.c_str();
        options.outputRoot      = runRootText.c_str();
        options.xenosEnabled =
            rex::cvar::GetFlagByName("gpu_plugin") == "xenos";
        options.rovEnabled =
            rex::cvar::GetFlagByName("render_target_path_d3d12") == "rov";
        if (rerevved::native_renderer::Start(options) !=
            rerevved::native_renderer::StartStatus::Accepted)
        {
            REXLOG_ERROR("native-renderer coverage observer admission failed");
            return false;
        }

        if (rerevved::native_renderer::RecordSegment(
                0, readCoverageSnapshot()) !=
            rerevved::native_renderer::CheckpointStatus::Accepted)
        {
            REXLOG_ERROR("native-renderer coverage observer start segment failed");
            return false;
        }
        coverageStarted.store(true, std::memory_order_release);
        REXLOG_INFO("NRD-COVERAGE-BEGIN");
    }

    if (!passiveTraceOutputPath.empty())
    {
        if (!rerevved::gpu::diagnostics::GetPassiveTraceBuffer().Start(
                passiveTraceOutputPath))
        {
            REXLOG_ERROR("Passive Resolve/VdSwap trace admission failed");
            return false;
        }
        passiveTraceStarted.store(true, std::memory_order_release);
        REXLOG_INFO("NATIVE-PASSIVE-TRACE-BEGIN");
    }
    if (!fenceTraceOutputPath.empty())
    {
        if (!rex::graphics::diagnostic::GetXenosFenceTrace().Start(
                fenceTraceOutputPath))
        {
            REXLOG_ERROR("Xenos consumer/fence trace admission failed");
            return false;
        }
        fenceTraceStarted.store(true, std::memory_order_release);
        REXLOG_INFO("NATIVE-FENCE-TRACE-BEGIN");
    }
    return true;
}

std::optional<rex::PathConfig> App::OnFinalizePaths(const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume)
{
    (void)resume;

    auto paths = defaults;
    if (paths.game_data_root.empty())
    {
        // Selection may resolve the root after the defaults were captured.
        paths.game_data_root = std::filesystem::path(std::string(REXCVAR_GET(game_data_root)));
    }

    auto result = rerevved::VerifyContentRoot(paths.game_data_root, rerevved::ContentDepth::Quick);
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

void App::OnPostSetup()
{
    rex::ReXApp::OnPostSetup();

    // Resolve selected package assets after the SDK has built the loadout and
    // before the guest main thread is resumed.
    (void)rerevved::main_menu_logo::ResolveSelectedLogo(
        runtime()->active_asset_overlays());

    rerevved::StartPresence();

    // Keep the internal app name lowercase and brand the window title separately.
    window()->SetTitle(rendererBackend == rerevved::gpu::RendererBackend::Native
                           ? "ReRevved - Native D3D12"
                           : "ReRevved");

    if (coverageStarted.load(std::memory_order_acquire))
    {
        // The locked SDK dispatches higher input layers first and stops after a
        // handled event. Moving the title listener above the MNK driver keeps
        // the host-only F10 bind out of guest keyboard input.
        window()->AddInputListener(this, kCoverageInputZOrder);
        rex::ui::RegisterBind(
            "bind_native_renderer_coverage_checkpoint",
            "F10",
            "Record native-renderer coverage checkpoint",
            [this]()
            {
                const bool accepted = recordCoverageCheckpoint(false);
                REXLOG_INFO("NRD-COVERAGE-CHECKPOINT accepted={}", accepted ? "true" : "false");
            });
        coverageBindRegistered = true;
    }
}

void App::OnGuestThreadExit(rex::system::XThread* thread)
{
    (void)thread;
    finalizeFenceTrace();
    finalizePassiveTrace();
    finalizeCoverage(rerevved::native_renderer::ExitClass::GuestComplete);
}

void App::OnShutdown()
{
    finalizeFenceTrace();
    finalizePassiveTrace();
    finalizeCoverage(rerevved::native_renderer::ExitClass::Shutdown);
    if (coverageBindRegistered)
    {
        rex::ui::UnregisterBind("bind_native_renderer_coverage_checkpoint");
        coverageBindRegistered = false;
    }
    rerevved::StopPresence();
    nativeRenderer.Shutdown();
}

bool App::OnWindowCloseRequested()
{
    finalizeFenceTrace();
    finalizePassiveTrace();
    finalizeCoverage(rerevved::native_renderer::ExitClass::WindowClose);
    rerevved::StopPresence();
    nativeRenderer.Shutdown();
    return true;
}

void App::OnWindowFocusChanged(bool focused)
{
    windowFocused = focused;
}

void App::OnWindowPixelSizeChanged(uint32_t pixelWidth, uint32_t pixelHeight)
{
    if (nativeRenderer.Initialized() && pixelWidth != 0 && pixelHeight != 0 &&
        !nativeRenderer.Resize(pixelWidth, pixelHeight))
    {
        REXLOG_ERROR("Native renderer resize failed: {}x{}", pixelWidth, pixelHeight);
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                  "Native D3D12 resize failed. See the log for details.");
        app_context().RequestDeferredQuit();
    }
}

void App::OnKeyDown(rex::ui::KeyEvent& event)
{
    if (coverageBindRegistered &&
        event.virtual_key() == rex::ui::VirtualKey::kF10 &&
        event.prev_state())
    {
        event.set_handled(true);
        return;
    }
    rex::ui::ProcessKeyEvent(event);
}

bool App::recordCoverageCheckpoint(bool finalSegment)
{
    const std::lock_guard checkpointLock(coverageCheckpointMutex);
    if (!coverageStarted.load(std::memory_order_acquire))
    {
        return false;
    }
    if (!finalSegment &&
        coverageFinalizeStarted.load(std::memory_order_acquire))
    {
        return false;
    }

    const auto fields = readCoverageSnapshot();
    if (finalSegment)
    {
        return rerevved::native_renderer::RecordSegment(
                   kFinalCoverageSegment, fields) ==
               rerevved::native_renderer::CheckpointStatus::Accepted;
    }
    if (!windowFocused || coverageMarkCount >=
                              rerevved::native_renderer::kCheckpointCapacity)
    {
        return false;
    }

    const uint32_t mark    = coverageMarkCount;
    const uint32_t segment = mark + 1;
    if (rerevved::native_renderer::RecordCheckpoint(segment, mark, fields) ==
        rerevved::native_renderer::CheckpointStatus::Accepted)
    {
        ++coverageMarkCount;
        return true;
    }
    return false;
}

void App::finalizeCoverage(
    rerevved::native_renderer::ExitClass exitClass)
{
    if (!coverageStarted.load(std::memory_order_acquire) ||
        coverageFinalizeStarted.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    const bool finalSegmentRecorded = recordCoverageCheckpoint(true);
    const auto status               = rerevved::native_renderer::Finalize(exitClass);
    if (status == rerevved::native_renderer::FinalizeStatus::Accepted &&
        finalSegmentRecorded)
    {
        REXLOG_INFO("NRD-COVERAGE-END");
    }
    else
    {
        REXLOG_ERROR("native-renderer coverage observer finalization failed: {}",
                     static_cast<unsigned>(status));
    }
}

void App::finalizePassiveTrace()
{
    if (!passiveTraceStarted.load(std::memory_order_acquire))
    {
        return;
    }

    bool expected = false;
    if (!passiveTraceFinalizeStarted.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return;
    }

    auto& trace = rerevved::gpu::diagnostics::GetPassiveTraceBuffer();
    if (trace.StopAndFlush())
    {
        passiveTraceStarted.store(false, std::memory_order_release);
        const auto statistics = trace.Statistics();
        REXLOG_INFO(
            "NATIVE-PASSIVE-TRACE-END stored={} overflow={} epoch_failures={} epoch={} sequence={}",
            statistics.stored,
            statistics.overflow,
            statistics.epochTransitionFailures,
            statistics.epoch,
            statistics.lastSequence);
    }
    else
    {
        const auto statistics = trace.Statistics();
        REXLOG_ERROR(
            "Passive Resolve/VdSwap trace flush failed: stored={} overflow={} in_flight={}",
            statistics.stored,
            statistics.overflow,
            statistics.inFlightAtFlush);
        passiveTraceFinalizeStarted.store(false, std::memory_order_release);
    }
}

void App::finalizeFenceTrace()
{
    if (!fenceTraceStarted.load(std::memory_order_acquire))
    {
        return;
    }

    fenceTraceFinalization.Run(
        [this]()
        {
            auto& trace = rex::graphics::diagnostic::GetXenosFenceTrace();
            if (trace.FinishAndFlush())
            {
                fenceTraceStarted.store(false, std::memory_order_release);
                const auto statistics = trace.statistics();
                REXLOG_INFO(
                    "NATIVE-FENCE-TRACE-END stored={} overflow={} lock_waits={} max_lock_wait_ns={} dropped={} reentry={} watched={} in_flight={} unresolved={} epoch={} sequence={}",
                    statistics.stored,
                    statistics.overflow,
                    statistics.lock_waits,
                    statistics.maximum_lock_wait_nanoseconds,
                    statistics.dropped_callbacks,
                    statistics.reentry_failures,
                    statistics.watched,
                    statistics.in_flight,
                    statistics.unresolved,
                    statistics.epoch,
                    statistics.last_sequence);
                if (!statistics.valid_for_promotion())
                {
                    REXLOG_ERROR(
                        "NATIVE-FENCE-TRACE-INVALID overflow={} dropped={} reentry={} watched={} in_flight={} unresolved={}",
                        statistics.overflow,
                        statistics.dropped_callbacks,
                        statistics.reentry_failures,
                        statistics.watched,
                        statistics.in_flight,
                        statistics.unresolved);
                }
                return true;
            }

            const auto statistics = trace.statistics();
            REXLOG_ERROR(
                "Xenos consumer/fence trace flush failed: stored={} overflow={} lock_waits={} max_lock_wait_ns={} dropped={} reentry={} watched={} in_flight={} unresolved={}",
                statistics.stored,
                statistics.overflow,
                statistics.lock_waits,
                statistics.maximum_lock_wait_nanoseconds,
                statistics.dropped_callbacks,
                statistics.reentry_failures,
                statistics.watched,
                statistics.in_flight,
                statistics.unresolved);
            return false;
        });
}

} // namespace rerevved
