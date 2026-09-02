#include "rerevved_app.h"

#include <Windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <api/gameplay_state.h>
#include <fmt/format.h>
#include <rex/cvar.h>
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
#include "presence.h"

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DEFINE_STRING(renderer, "xenos", "ReRevved/Video", "Renderer backend: xenos or native")
    .allowed({ "xenos", "native" })
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_coverage_run, "", "ReRevved/Diagnostics", "native-renderer observer run ID")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_coverage_transition, "", "ReRevved/Diagnostics", "native-renderer observer transition ID")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_coverage_input_digest, "", "ReRevved/Diagnostics", "native-renderer observer input digest")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_coverage_output, "", "ReRevved/Diagnostics", "native-renderer observer output directory")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_passive_trace_output, "", "ReRevved/Diagnostics", "Ignored local CSV path for the passive Resolve/VdSwap trace")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_renderer_fence_trace_output, "", "ReRevved/Diagnostics", "Ignored local CSV path for the bounded Xenos consumer/fence trace")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace
{

constexpr std::size_t kMaxReportedErrors    = 10;
constexpr std::size_t kCoverageInputZOrder  = 1000;
constexpr uint32_t    kFinalCoverageSegment = 7;

bool IsContainedPath(const std::filesystem::path& root,
                     const std::filesystem::path& path)
{
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
    {
        return false;
    }
    return *relative.begin() != "..";
}

bool ContainsExistingReparsePoint(const std::filesystem::path& root,
                                  const std::filesystem::path& path)
{
    if (!IsContainedPath(root, path))
    {
        return true;
    }

    auto is_reparse_point = [](const std::filesystem::path& candidate)
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

    if (is_reparse_point(root))
    {
        return true;
    }
    std::filesystem::path current = root;
    for (const auto& component : path.lexically_relative(root))
    {
        current /= component;
        if (is_reparse_point(current))
        {
            return true;
        }
    }
    return false;
}

bool ResolvePassiveTracePath(std::string_view       configured,
                             std::filesystem::path& output_path)
{
    std::error_code             error;
    const std::filesystem::path scratch_root =
        std::filesystem::absolute("out", error).lexically_normal();
    if (error)
    {
        return false;
    }
    const std::filesystem::path candidate =
        std::filesystem::absolute(configured, error).lexically_normal();
    if (error || candidate.extension() != ".csv" ||
        !IsContainedPath(scratch_root, candidate) ||
        ContainsExistingReparsePoint(scratch_root, candidate))
    {
        return false;
    }
    if (std::filesystem::exists(candidate, error) || error)
    {
        return false;
    }

    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(scratch_root, error);
    if (error)
    {
        return false;
    }
    const std::filesystem::path canonical_parent =
        std::filesystem::weakly_canonical(candidate.parent_path(), error);
    if (error)
    {
        return false;
    }
    output_path = (canonical_parent / candidate.filename()).lexically_normal();
    return IsContainedPath(canonical_root, output_path);
}

rerevved::native_renderer::SnapshotFields ReadCoverageSnapshot() noexcept
{
    ReRevvedGameplayState state{};
    state.struct_size = sizeof(state);
    (void)ReRevvedGetGameplayState(&state, sizeof(state));

    rerevved::native_renderer::SnapshotFields fields{};
    fields.frame_sequence    = state.frame_sequence;
    fields.valid_fields      = state.valid_fields;
    fields.gameplay_active   = state.gameplay_active != 0;
    fields.interface_update  = state.interface_update != 0;
    fields.active_player     = state.active_player;
    fields.human_player_mask = state.human_player_mask;
    fields.turn_owner_known  = state.turn_owner_known != 0;
    fields.human_turn        = state.human_turn != 0;
    fields.available         = state.available != 0;
    fields.civilization      = static_cast<int32_t>(state.civilization);
    fields.era               = state.era;
    fields.year              = state.year;
    fields.turn              = state.turn;
    return fields;
}

} // namespace

void ReRevvedApp::OnPreSetup(rex::RuntimeConfig& config)
{
    REXLOG_INFO("{}", REREVVED_BUILD_INFO);
    config.game_version = REREVVED_VERSION;
    if (renderer_backend_ == rerevved::gpu::RendererBackend::Native)
    {
        config.graphics = std::make_unique<rerevved::gpu::NativeGuestGpuService>();
        config.gpu_plugin.clear();
    }
}

void ReRevvedApp::OnConfigurePaths(rex::PathConfig& paths)
{
    // Keep user state outside a potentially read-only install directory.
    paths.config_path = paths.user_data_root / "rerevved.toml";
}

bool ReRevvedApp::SetupEnvironment()
{
    if (!rex::ReXApp::SetupEnvironment())
    {
        return false;
    }

    renderer_backend_ = rerevved::gpu::ParseRendererBackend(REXCVAR_GET(renderer));
    if (renderer_backend_ == rerevved::gpu::RendererBackend::Invalid)
    {
        REXLOG_ERROR("Invalid ReRevved renderer '{}'; expected xenos or native",
                     REXCVAR_GET(renderer));
        return false;
    }
    if (renderer_backend_ == rerevved::gpu::RendererBackend::Native)
    {
        rex::cvar::SetFlagByName("gpu_plugin", "");
    }
    else if (rex::cvar::GetFlagByName("gpu_plugin").empty())
    {
        // Preserve the established bare-launch default after config and CLI
        // values have had the opportunity to select an explicit backend.
        rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    REXLOG_INFO("ReRevved renderer selected: {}",
                rerevved::gpu::RendererBackendName(renderer_backend_));

    const std::string passive_trace_output =
        REXCVAR_GET(native_renderer_passive_trace_output);
    const std::string fence_trace_output =
        REXCVAR_GET(native_renderer_fence_trace_output);
    passive_trace_output_path_.clear();
    fence_trace_output_path_.clear();
    if (!passive_trace_output.empty() && !fence_trace_output.empty())
    {
        REXLOG_ERROR(
            "Passive Resolve/VdSwap tracing and consumer/fence tracing cannot run together");
        return false;
    }
    if ((!passive_trace_output.empty() || !fence_trace_output.empty()) &&
        !REXCVAR_GET(native_renderer_coverage_run).empty())
    {
        REXLOG_ERROR(
            "Native renderer diagnostic tracing cannot share a native-renderer coverage run");
        return false;
    }
    if (!passive_trace_output.empty() &&
        (renderer_backend_ != rerevved::gpu::RendererBackend::Xenos ||
         !ResolvePassiveTracePath(passive_trace_output,
                                  passive_trace_output_path_)))
    {
        REXLOG_ERROR(
            "Passive Resolve/VdSwap trace requires Xenos and a non-reparse CSV path under the ignored out directory");
        return false;
    }
    if (!fence_trace_output.empty() &&
        (renderer_backend_ != rerevved::gpu::RendererBackend::Xenos ||
         !ResolvePassiveTracePath(fence_trace_output,
                                  fence_trace_output_path_)))
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

bool ReRevvedApp::SetupPresentation()
{
    if (!rex::ReXApp::SetupPresentation())
    {
        return false;
    }

    if (renderer_backend_ == rerevved::gpu::RendererBackend::Native)
    {
        if (!window() || !native_renderer_.Initialize(*window()))
        {
            REXLOG_ERROR("Native renderer presentation setup failed");
            return false;
        }
    }
    // The Xenos plugin owns this cvar, so it is not registered until the base
    // presentation setup loads the plugin.
    else if (rex::cvar::GetFlagByName("render_target_path_d3d12").empty())
    {
        rex::cvar::SetFlagByName("render_target_path_d3d12", "rov");
    }

    const std::string run_id = REXCVAR_GET(native_renderer_coverage_run);
    if (!run_id.empty())
    {
        const std::string transition_id =
            REXCVAR_GET(native_renderer_coverage_transition);
        const std::string input_digest =
            REXCVAR_GET(native_renderer_coverage_input_digest);
        const std::string output_name =
            REXCVAR_GET(native_renderer_coverage_output);
        const std::filesystem::path run_root = user_data_root().parent_path();
        if (user_data_root().filename() != "user-data" ||
            output_name != "observer" || run_root.empty())
        {
            REXLOG_ERROR("native-renderer coverage requires the isolated runner path layout");
            return false;
        }

        const std::filesystem::path output_directory =
            run_root / output_name;
        const std::string                       output_directory_text = output_directory.string();
        const std::string                       run_root_text         = run_root.string();
        rerevved::native_renderer::StartOptions options{};
        options.run_id           = run_id.c_str();
        options.transition_id    = transition_id.c_str();
        options.input_digest     = input_digest.c_str();
        options.output_directory = output_directory_text.c_str();
        options.output_root      = run_root_text.c_str();
        options.xenos_enabled =
            rex::cvar::GetFlagByName("gpu_plugin") == "xenos";
        options.rov_enabled =
            rex::cvar::GetFlagByName("render_target_path_d3d12") == "rov";
        if (rerevved::native_renderer::Start(options) !=
            rerevved::native_renderer::StartStatus::Accepted)
        {
            REXLOG_ERROR("native-renderer coverage observer admission failed");
            return false;
        }

        if (rerevved::native_renderer::RecordSegment(
                0, ReadCoverageSnapshot()) !=
            rerevved::native_renderer::CheckpointStatus::Accepted)
        {
            REXLOG_ERROR("native-renderer coverage observer start segment failed");
            return false;
        }
        coverage_started_.store(true, std::memory_order_release);
        REXLOG_INFO("NRD-COVERAGE-BEGIN");
    }

    if (!passive_trace_output_path_.empty())
    {
        if (!rerevved::gpu::diagnostics::GetPassiveTraceBuffer().Start(
                passive_trace_output_path_))
        {
            REXLOG_ERROR("Passive Resolve/VdSwap trace admission failed");
            return false;
        }
        passive_trace_started_.store(true, std::memory_order_release);
        REXLOG_INFO("NATIVE-PASSIVE-TRACE-BEGIN");
    }
    if (!fence_trace_output_path_.empty())
    {
        if (!rex::graphics::diagnostic::GetXenosFenceTrace().Start(
                fence_trace_output_path_))
        {
            REXLOG_ERROR("Xenos consumer/fence trace admission failed");
            return false;
        }
        fence_trace_started_.store(true, std::memory_order_release);
        REXLOG_INFO("NATIVE-FENCE-TRACE-BEGIN");
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
    window()->SetTitle(renderer_backend_ == rerevved::gpu::RendererBackend::Native
                           ? "ReRevved - Native D3D12"
                           : "ReRevved");

    if (coverage_started_.load(std::memory_order_acquire))
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
                const bool accepted = RecordCoverageCheckpoint(false);
                REXLOG_INFO("NRD-COVERAGE-CHECKPOINT accepted={}", accepted ? "true" : "false");
            });
        coverage_bind_registered_ = true;
    }
}

void ReRevvedApp::OnGuestThreadExit(rex::system::XThread* thread)
{
    (void)thread;
    FinalizeFenceTrace();
    FinalizePassiveTrace();
    FinalizeCoverage(rerevved::native_renderer::ExitClass::GuestComplete);
}

void ReRevvedApp::OnShutdown()
{
    FinalizeFenceTrace();
    FinalizePassiveTrace();
    FinalizeCoverage(rerevved::native_renderer::ExitClass::Shutdown);
    if (coverage_bind_registered_)
    {
        rex::ui::UnregisterBind("bind_native_renderer_coverage_checkpoint");
        coverage_bind_registered_ = false;
    }
    rerevved::StopPresence();
    native_renderer_.Shutdown();
}

bool ReRevvedApp::OnWindowCloseRequested()
{
    FinalizeFenceTrace();
    FinalizePassiveTrace();
    FinalizeCoverage(rerevved::native_renderer::ExitClass::WindowClose);
    rerevved::StopPresence();
    native_renderer_.Shutdown();
    return true;
}

void ReRevvedApp::OnWindowFocusChanged(bool focused)
{
    window_focused_ = focused;
}

void ReRevvedApp::OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height)
{
    if (native_renderer_.initialized() && pixel_width != 0 && pixel_height != 0 &&
        !native_renderer_.Resize(pixel_width, pixel_height))
    {
        REXLOG_ERROR("Native renderer resize failed: {}x{}", pixel_width, pixel_height);
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                  "Native D3D12 resize failed. See the log for details.");
        app_context().RequestDeferredQuit();
    }
}

void ReRevvedApp::OnKeyDown(rex::ui::KeyEvent& event)
{
    if (coverage_bind_registered_ &&
        event.virtual_key() == rex::ui::VirtualKey::kF10 &&
        event.prev_state())
    {
        event.set_handled(true);
        return;
    }
    rex::ui::ProcessKeyEvent(event);
}

bool ReRevvedApp::RecordCoverageCheckpoint(bool final_segment)
{
    const std::lock_guard checkpoint_lock(coverage_checkpoint_mutex_);
    if (!coverage_started_.load(std::memory_order_acquire))
    {
        return false;
    }
    if (!final_segment &&
        coverage_finalize_started_.load(std::memory_order_acquire))
    {
        return false;
    }

    const auto fields = ReadCoverageSnapshot();
    if (final_segment)
    {
        return rerevved::native_renderer::RecordSegment(
                   kFinalCoverageSegment, fields) ==
               rerevved::native_renderer::CheckpointStatus::Accepted;
    }
    if (!window_focused_ || coverage_mark_count_ >=
                                rerevved::native_renderer::kCheckpointCapacity)
    {
        return false;
    }

    const uint32_t mark    = coverage_mark_count_;
    const uint32_t segment = mark + 1;
    if (rerevved::native_renderer::RecordCheckpoint(segment, mark, fields) ==
        rerevved::native_renderer::CheckpointStatus::Accepted)
    {
        ++coverage_mark_count_;
        return true;
    }
    return false;
}

void ReRevvedApp::FinalizeCoverage(
    rerevved::native_renderer::ExitClass exit_class)
{
    if (!coverage_started_.load(std::memory_order_acquire) ||
        coverage_finalize_started_.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    const bool final_segment_recorded = RecordCoverageCheckpoint(true);
    const auto status                 = rerevved::native_renderer::Finalize(exit_class);
    if (status == rerevved::native_renderer::FinalizeStatus::Accepted &&
        final_segment_recorded)
    {
        REXLOG_INFO("NRD-COVERAGE-END");
    }
    else
    {
        REXLOG_ERROR("native-renderer coverage observer finalization failed: {}",
                     static_cast<unsigned>(status));
    }
}

void ReRevvedApp::FinalizePassiveTrace()
{
    if (!passive_trace_started_.load(std::memory_order_acquire))
    {
        return;
    }

    bool expected = false;
    if (!passive_trace_finalize_started_.compare_exchange_strong(
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
        passive_trace_started_.store(false, std::memory_order_release);
        const auto statistics = trace.statistics();
        REXLOG_INFO(
            "NATIVE-PASSIVE-TRACE-END stored={} overflow={} epoch_failures={} epoch={} sequence={}",
            statistics.stored,
            statistics.overflow,
            statistics.epoch_transition_failures,
            statistics.epoch,
            statistics.last_sequence);
    }
    else
    {
        const auto statistics = trace.statistics();
        REXLOG_ERROR(
            "Passive Resolve/VdSwap trace flush failed: stored={} overflow={} in_flight={}",
            statistics.stored,
            statistics.overflow,
            statistics.in_flight_at_flush);
        passive_trace_finalize_started_.store(false, std::memory_order_release);
    }
}

void ReRevvedApp::FinalizeFenceTrace()
{
    if (!fence_trace_started_.load(std::memory_order_acquire))
    {
        return;
    }

    fence_trace_finalization_.Run(
        [this]()
        {
            auto& trace = rex::graphics::diagnostic::GetXenosFenceTrace();
            if (trace.FinishAndFlush())
            {
                fence_trace_started_.store(false, std::memory_order_release);
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
