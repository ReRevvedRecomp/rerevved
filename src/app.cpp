#include "app.h"
#include "gotw_settings.h"
#include "gpu/completion/guest_progress_hooks.h"

#if defined(_WIN32)
#include <Windows.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <api/gameplay_state.h>
#include <fmt/format.h>
#include <rex/crypto/sha256.h>
#include <rex/cvar.h>
#include <rex/discord_rpc.h>
#include <rex/filesystem.h>
#include <rex/graphics/xenos_fence_trace.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/session_data.h>
#include <rex/system.h>
#include <rex/system/game_data_selector.h>
#include <rex/system/xmemory.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "build_info.h"
#include "discord_presence.h"
#include "game_content.h"
#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/guest_d3d/clear_consume.h"
#include "gpu/guest_d3d/clear_derive.h"
#if REREVVED_DEVELOPER
#include "gpu/guest_d3d/draw_effect_probe.h"
#endif
#include "gpu/guest_d3d/gamma_observer.h"
#include "gpu/guest_d3d/guest_memory.h"
#include "gpu/guest_d3d/indexed_buffer_consume.h"
#include "gpu/guest_d3d/indexed_buffer_draw_inputs.h"
#include "gpu/guest_d3d/lowerer_consume.h"
#include "gpu/guest_d3d/native_draw_families.h"
#include "gpu/guest_d3d/resolve_consume.h"
#include "gpu/guest_d3d/resolve_derive.h"
#include "gpu/guest_d3d/resolve_surface_registry.h"
#include "gpu/guest_d3d/resource_mirror_registry.h"
#include "gpu/guest_d3d/ring_stubs.h"
#include "gpu/guest_d3d/scene_draw_coverage.h"
#include "gpu/guest_d3d/set_predication_consume.h"
#include "gpu/guest_d3d/user_pointer_consume.h"
#include "gpu/guest_d3d/window_writer_consume.h"
#include "gpu/guest_gpu_service.h"
#include "gpu/guest_memory_watch.h"
#if REREVVED_DEVELOPER
#include "gpu/present/native_cadence_trace.h"
#endif
#if defined(_WIN32)
#include "gpu/present/native_app_session.h"
#include "gpu/present/native_immediate_drawer.h"
#include "tools/presentation_fixture.h"
#endif
#include "host_settings.h"
#include "main_menu_logo_asset.h"
#include "mod_script_rules.h"
#include "xenos_frame_counter.h"
#if REREVVED_DEVELOPER
#include "gpu/diagnostics/label_draw_capture.h"
#include "gpu/diagnostics/route_trace.h"
#include "gpu/diagnostics/scene_resource_capture.h"
#include "gpu/diagnostics/shader_harvest.h"
#include "gpu/diagnostics/xenos_frame_reference.h"
#include "gpu/guest_d3d/completion_probe.h"
#include "gpu/guest_d3d/guest_api_census.h"
#include "gpu/guest_d3d/original_residue_probe.h"
#include "gpu/guest_d3d/tiling_probe.h"
#include "gpu/guest_d3d/tiling_restore_consume.h"
#include "gpu/guest_d3d/video_display_mode_stub.h"
#include "gpu/guest_d3d/video_import_stub_table.h"
#include "gpu/guest_d3d/video_scaler_stub.h"
#include "gpu/guest_d3d/vs_code_watch.h"
#include "gpu/guest_d3d/vs_readback_probe.h"
#endif

namespace
{

void openLogFolder()
{
    const std::filesystem::path logFile = rex::GetLogFilePath();
    if (logFile.empty())
    {
        REXLOG_WARN("Open Log Folder: no log file is active");
        return;
    }
    rex::LaunchFileExplorer(logFile.parent_path());
}

#if REREVVED_DEVELOPER
// Tracks the current thread's draw-family build for native_frame_timing:
// the guest caller, whether textures read guest memory, and whether any
// texture read overlaps a resolve range.
struct NativeBuildReads
{
    bool          building = false, textureRead = false, textureResolve = false;
    std::uint32_t caller = 0;
};

NativeBuildReads& CurrentNativeBuildReads() noexcept
{
    thread_local NativeBuildReads reads;
    return reads;
}
#endif

} // namespace

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(bool, window_hidden);
REXCVAR_DEFINE_STRING(native_present_fixture, "", "ReRevved", "Admitted rgba8-32-tiled shader catalog; no guest launch")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if REREVVED_DEVELOPER
REXCVAR_DEFINE_STRING(native_present_trace, "", "ReRevved", "New TOML presentation event log")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_cadence_trace, "", "ReRevved", "New TOML file of timestamped swap, publication and present events, with swap-attempt, publication and distinct-shown-frame rates and shown-frame interval percentiles written at close; empty is off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_effect_probe, "", "ReRevved", "New TOML file of the guest device bytes the IndexedBuffer draw entry changes on untiled draws, sampled one root in 16 and written at close; empty is off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_present_script, "", "ReRevved", "Hidden lifecycle harness: lifecycle or empty")
    .allowed({ "", "lifecycle" })
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

namespace
{

// The hidden lifecycle harness reports through its event log instead of modal
// dialogs; a shipping build has no harness.
bool NativeScriptActive()
{
#if REREVVED_DEVELOPER
    return !REXCVAR_GET(native_present_script).empty();
#else
    return false;
#endif
}

} // namespace

REXCVAR_DEFINE_STRING(combat_speed, "normal", "ReRevved", "Combat presentation speed")
    .allowed({ "normal", "fast" });
REXCVAR_DEFINE_BOOL(hide_research_prompt, false, "ReRevved", "Do not open the technology prompt at turn start or after a village; choose research from the technology window");
REXCVAR_DEFINE_BOOL(hide_popup_rival_war_peace, false, "ReRevved", "Do not show reports of wars declared and peace signed between other civilizations");
REXCVAR_DEFINE_BOOL(hide_popup_rival_government, false, "ReRevved", "Do not show reports of other civilizations changing government");
REXCVAR_DEFINE_BOOL(hide_popup_city_captures, false, "ReRevved", "Do not show reports of cities taken or recaptured");
REXCVAR_DEFINE_BOOL(hide_popup_victory_warnings, false, "ReRevved", "Do not show warnings that a rival is close to a victory or that the game is ending");
REXCVAR_DEFINE_BOOL(hide_popup_rival_great_people, false, "ReRevved", "Do not show reports of rival Great People and completed Wonders");
REXCVAR_DEFINE_BOOL(hide_popup_rankings, false, "ReRevved", "Do not show strongest economy, greatest city and culture conversion reports");
REXCVAR_DEFINE_BOOL(hide_popup_artifacts, false, "ReRevved", "Do not show artifact rumors and ancient site discoveries");
REXCVAR_DEFINE_BOOL(hide_popup_combat_reports, false, "ReRevved", "Do not show Great General, capture, promotion and SDI reports");
REXCVAR_DEFINE_BOOL(hide_popup_spy_reports, false, "ReRevved", "Do not show reports of stolen gold and kidnapped Great People");
REXCVAR_DEFINE_BOOL(hide_popup_city_status, false, "ReRevved", "Do not show disorder, celebration, culture and blockade reports");
REXCVAR_DEFINE_BOOL(hide_popup_discoveries, false, "ReRevved", "Do not show new technology, era, milestone, trade and road reports");
REXCVAR_DEFINE_BOOL(hide_popup_villages, false, "ReRevved", "Do not show the result of entering a village");
REXCVAR_DEFINE_BOOL(hide_popup_save_reminder, false, "ReRevved", "Hide the save reminder. This does not save the game.");
REXCVAR_DEFINE_BOOL(hide_popup_trophy_room, false, "ReRevved", "Do not offer to visit the Trophy Room");
REXCVAR_DEFINE_BOOL(hide_popup_government_available, false, "ReRevved", "Do not ask to change government when a new one becomes available; keep the current one");
REXCVAR_DEFINE_BOOL(hide_barbarian_messages, false, "ReRevved", "Do not show the Barbarian leader message on first contact with a Barbarian tribe");
REXCVAR_DEFINE_BOOL(hide_village_barbarian_messages, false, "ReRevved", "Do not show the Barbarian leader message that names the reward for capturing a Barbarian village; the reward is still granted");
REXCVAR_DEFINE_INT32(ai_civilizations, 4, "ReRevved", "Maximum AI civilizations in a new game. A game with one human keeps at least one AI civilization. Multiplayer uses the host's value for all players")
    .range(0, 4);
REXCVAR_DEFINE_COMMAND(open_log_folder, openLogFolder, "ReRevved", "Open the folder that holds the log file and crash dumps");
REXCVAR_DEFINE_BOOL(xenos_frame_counter, false, "ReRevved", "Log one-second VdSwap return counts and decoded swap intervals")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(renderer, "xenos", "ReRevved", "Renderer backend: xenos or native")
    .allowed({ "xenos", "native" })
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(native_draw_skip, false, "ReRevved", "Bring-up only for renderer=native: skip guest draws without a native route and label their completion partial")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if REREVVED_DEVELOPER
REXCVAR_DEFINE_BOOL(native_frame_timing, false, "ReRevved", "Bring-up only for renderer=native: log once a second where guest frame time goes: swaps, resolved reads, encodes, producer recording, fences and pipelines, and guest D3D calls by role")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif
REXCVAR_DEFINE_BOOL(native_resolve_graph, true, "ReRevved", "Bring-up only for renderer=native: record off-screen resolves inside the frame's command stream with host readbacks; false replays each resolve in a frame of its own")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_resolve_alias, true, "ReRevved", "Bring-up only for renderer=native with native_resolve_graph: bind an off-screen resolve a presented draw samples as its frame texture instead of its read-back bytes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_surface_registry, true, "ReRevved", "On by default for renderer=native: keep each off-screen resolve as a host surface by destination range across frames and bind it at draws in place of the alias table, ended by write locks, unlocked guest stores and frees")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_stream_queue, true, "ReRevved", "Bring-up only for renderer=native: queue the next draw stream request while one records, admitted by the stream when it takes it; false makes each request wait for the stream to drain")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_direct_stream, true, "ReRevved", "On by default for renderer=native: the draw stream charges each request's own bytes to a running total instead of walking every owner, holds up to two terminal frames instead of two requests, and destroys retired frames on a deleter thread")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_eager_graph_segments, true, "ReRevved", "Bring-up only for renderer=native with native_resolve_graph: send recorded graph passes as segments during the frame; false leaves the open resolves to the swap, which sends them as one segment")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_read_mirror_min_kib, 64, "ReRevved", "Bring-up only for renderer=native: retain, compare and watch guest memory reads of at least this many KiB; smaller reads are copied")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_resource_mirrors, true, "ReRevved", "On by default for renderer=native: keep host copies of the textures and buffers guest draws bind, refreshed after locks, unlocks and guest writes, and bind them at draws instead of reading guest memory")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_census, false, "ReRevved", "Bring-up only for renderer=native: log each distinct unsupported draw, clear, resolve or completion failure once (census:), skip it and keep completing; overrides native_draw_skip")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_draw_sequencer, false, "ReRevved", "Bring-up only for renderer=native: build guest draws on one sequencer thread from snapshots the guest copies at each draw, in draw order, draining before each clear, resolve and swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_panel_capture, "", "ReRevved", "Bring-up only for renderer=native guest boot: solid adapter capture whose panel shaders admit the menu panel draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_label_shaders, "", "ReRevved", "Bring-up only for renderer=native guest boot: shader catalog directory whose label pair admits the boot label batches")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_gfx_shaders, "", "ReRevved", "Bring-up only for renderer=native guest boot: shader catalog directory whose GFx pairs admit the Scaleform quads at 0x82303E90")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_mesh_shaders, "", "ReRevved", "Bring-up only for renderer=native guest boot: shader catalog directory whose MRT prepass pair admits the post-ESRB scene mesh draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_menu_shaders, "", "ReRevved", "Bring-up only for renderer=native guest boot: shader catalog directory whose menu backdrop pair admits the title and menu scene draws")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_leader_shaders, "", "ReRevved", "Bring-up only for renderer=native guest boot: shader catalog directory whose leader pairs admit the leader model draws of civ select and the world")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_movie_shaders, "", "ReRevved", "Bring-up only for renderer=native guest boot: shader catalog directory whose movie pair admits the intro movie quads")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_draw_scene_shaders, "", "ReRevved", "Bring-up only for renderer=native guest boot: shader catalog directory whose scene pairs admit IndexedBuffer draws from 0x8250C768, 0x8250CB98 and 0x8230686C")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if REREVVED_DEVELOPER
REXCVAR_DEFINE_STRING(native_guest_adapter_output, "", "ReRevved", "Fresh directory under out for the label adapter capture, armed at start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_route_trace, "", "ReRevved", "Fresh directory under out/diagnostics for cold-boot guest entry and caller counts")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_scene_capture, "", "ReRevved", "Fresh directory under out/diagnostics for scene resource provenance")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_scene_capture_frames, 2, "ReRevved", "Complete VdSwap intervals captured after scene arm")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_scene_capture_arm_frame, 0, "ReRevved", "Arm the scene capture at this VdSwap boundary count; 0 keeps file arming")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_scene_capture_arm_ps_digest, "", "ReRevved", "Arm the scene capture at the first draw whose pixel shader word digest starts with these 8 to 64 hex digits; the window opens native_scene_capture_arm_delay + 1 VdSwap boundaries after that draw's frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_scene_capture_arm_delay, 0, "ReRevved", "Extra VdSwap boundaries between the pixel digest draw and the scene capture window")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_scene_capture_windows, 1, "ReRevved", "Scene capture windows, 1..16; window k >= 2 opens when <capture>/arm-<k> exists and writes <capture>/window-<k>")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_scene_capture_payloads, false, "ReRevved", "Retain scene CPU input bytes in an identity-bound payload manifest")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_scene_capture_payload_bytes, 268435456, "ReRevved", "Hard byte budget for unique scene payloads including pre-arm history")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_scene_capture_effects, false, "ReRevved", "Record bounded guest effects in the armed scene window")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_scene_capture_targets, false, "ReRevved", "Request lossless Xenos target seeds and frame references; unavailable SDK readbacks remain gaps")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_scene_capture_target_bytes, 67108864, "ReRevved", "Hard byte budget for lossless target and reference receipts")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_xenos_frame_reference, "", "ReRevved", "Fresh directory under out/diagnostics for one owned Xenos frame with its RGB10 output")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_xenos_frame_reference_arm_frame, 0, "ReRevved", "Request the Xenos frame reference at this VdSwap boundary count; 0 leaves the arm file and digest triggers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_xenos_frame_reference_arm_ps_digest, "", "ReRevved", "Request the Xenos frame reference at the first draw whose pixel shader word digest starts with these 8 to 64 hex digits, native_xenos_frame_reference_arm_delay + 1 VdSwap boundaries after that draw's frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_xenos_frame_reference_arm_delay, 0, "ReRevved", "Extra VdSwap boundaries between the pixel digest draw and the Xenos frame reference request")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_xenos_frame_reference_windows, 1, "ReRevved", "Xenos frame reference windows, 1..16; window k >= 2 opens when <reference>/arm-<k> exists and writes <reference>/window-<k>")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_shader_harvest, "", "ReRevved", "Fresh directory under out/diagnostics for the first draw of each distinct caller and shader pair")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_shader_harvest_caller, "", "ReRevved", "Harvest only draws from this 0x-prefixed 8-digit caller address; empty keeps every caller")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_shader_harvest_frame_forms, 0, "ReRevved", "Shader harvest frames written per new frame signature, 0..16; 0 records no frame forms")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_UINT32(native_shader_harvest_frame_mib, 1024, "ReRevved", "MiB budget for harvest frame forms, 1..65536; frames whose first draw precedes every clear have a separate budget of the same size")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_shader_harvest_known, "", "ReRevved", "Prior shader harvest directories, separated by ';': frames they wrote or whose content they cover are only counted, and schema 3 key records are reused")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_shader_harvest_arm, "", "ReRevved", "Write harvest frames only once this file exists, relative to the harvest directory; keys and signatures are counted from the start")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

REXCVAR_DEFINE_BOOL(system_link_trace, false, "ReRevved", "Log System Link datagram kinds, sizes and ports, never payloads")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::xenos_frames
{
namespace
{

int64_t steadyNow(void*)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void logBins(const std::vector<Bin>& bins)
{
    for (const auto& bin : bins)
    {
        REXLOG_INFO("xenos_frames,{},{},{},{},{},{:.6f},{:.6f},{:.6f},{:.6f},{},{},{},{},{}",
                    bin.startNs,
                    bin.endNs,
                    bin.attempts,
                    bin.returns,
                    bin.totalReturns,
                    bin.returnsPerSecond,
                    bin.dtMinMs,
                    bin.dtMeanMs,
                    bin.dtMaxMs,
                    bin.intervals[0],
                    bin.intervals[1],
                    bin.intervals[2],
                    bin.intervals[3],
                    bin.partial ? 1 : 0);
    }
}

struct Reporter
{
    std::mutex              mutex;
    std::condition_variable wake;
    Counter                 counter{ true, steadyNow, nullptr };
    bool                    stopping{};
    std::once_flag          stopOnce;
    std::thread             thread;

    Reporter()
    {
        REXLOG_INFO("xenos_frames metric=VdSwap returns/s; timestamps=steady_clock nanoseconds; decoded/presented frames are not measured");
        REXLOG_INFO("xenos_frames,start_ns,end_ns,attempts,returns,total_returns,returns_per_s,dt_min_ms,dt_mean_ms,dt_max_ms,interval0,interval1,interval2,interval_other,partial");
        thread = std::thread([this]
                             {
                                 run();
                             });
    }

    ~Reporter()
    {
        Stop();
    }

    void Stop()
    {
        std::call_once(stopOnce, [this]
                       {
                           {
                               std::lock_guard lock(mutex);
                               counter.Finish();
                               stopping = true;
                           }
                           wake.notify_one();
                           thread.join();
                       });
    }

    void run()
    {
        for (;;)
        {
            std::unique_lock lock(mutex);
            wake.wait_for(lock, std::chrono::seconds(1), [this]
                          {
                              return stopping;
                          });
            counter.Poll();
            const auto bins     = counter.TakeBins();
            const bool finished = stopping;
            const auto summary  = counter.GetSummary();
            // Neither logging nor thread joining holds the title counter lock.
            lock.unlock();
            logBins(bins);
            if (finished)
            {
                REXLOG_INFO("xenos_frames_summary metric=VdSwap returns/s start_ns={} end_ns={} total_returns={} segment_mean={:.6f} one_second_min={:.6f} one_second_max={:.6f} full_bins={}",
                            summary.startNs,
                            summary.endNs,
                            summary.totalReturns,
                            summary.meanReturnsPerSecond,
                            summary.minReturnsPerSecond,
                            summary.maxReturnsPerSecond,
                            summary.fullBins);
                return;
            }
        }
    }
};

std::atomic<Reporter*> activeReporter{};

} // namespace

void Start()
{
    // Constructed only for an enabled Xenos launch, before guest execution. Never
    // destroyed: a guest exit() leaves swap threads running past static teardown,
    // so atexit flushes the final bin while the reporter stays valid.
    static Reporter* reporter = []
    {
        auto* created = new Reporter;
        std::atexit(Finalize);
        return created;
    }();
    activeReporter.store(reporter, std::memory_order_release);
}

void Finalize()
{
    if (auto* reporter = activeReporter.load(std::memory_order_acquire))
    {
        reporter->Stop();
    }
}

void RecordAttempt()
{
    if (auto* reporter = activeReporter.load(std::memory_order_acquire))
    {
        std::lock_guard lock(reporter->mutex);
        reporter->counter.Attempt();
    }
}

void RecordReturn(uint32_t decodedInterval)
{
    if (auto* reporter = activeReporter.load(std::memory_order_acquire))
    {
        std::lock_guard lock(reporter->mutex);
        reporter->counter.Return(decodedInterval);
    }
}

} // namespace rerevved::xenos_frames

namespace
{

constexpr std::size_t kMaxReportedErrors = 10;
constexpr const char* kNativeGuestTitle  = "ReRevved - Native D3D12";
constexpr auto        kDiscordJoinWait   = std::chrono::seconds{ 10 };
// Visible diagnostics for a held ledger span and for a failed bring-up session.
constexpr const char* kCompletionDebtTitleSuffix    = " - waiting for GPU completion: ";
constexpr const char* kCompletionFailureTitleSuffix = " - native bring-up stopped: ";
// Bounded native bring-up logs: rejected submissions per ring generation, and swaps.
constexpr std::uint32_t kLoggedRejections  = 8;
constexpr std::uint64_t kLoggedNativeSwaps = 8;

// No guest gamma writer is hooked, so native frames use an identity 8-to-10-bit
// ramp and are labeled with the assumption.
std::array<std::uint32_t, 256> identityGammaTable()
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i)
    {
        const auto value = (i * 1023U + 127U) / 255U;
        table[i]         = rerevved::gpu::PackGammaEntry(value, value, value);
    }
    return table;
}

#if REREVVED_DEVELOPER
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

bool resolveShadowDirectory(std::string_view       configured,
                            bool                   fresh,
                            std::filesystem::path& output)
{
    std::error_code error;
    const auto      root = std::filesystem::absolute("out", error).lexically_normal();
    if (error || configured.empty())
    {
        return false;
    }
    const auto path = std::filesystem::absolute(configured, error).lexically_normal();
    if (error || !isContainedPath(root, path) || containsExistingReparsePoint(root, path))
    {
        return false;
    }
    const bool exists = std::filesystem::exists(path, error);
    if (error || (fresh ? exists : !std::filesystem::is_directory(path, error)) || error)
    {
        return false;
    }
    output = path;
    return true;
}
#endif

rerevved::gpu::guest_d3d::NativeDrawMode NativeDrawModeFromFlags()
{
    using rerevved::gpu::guest_d3d::NativeDrawMode;
    if (REXCVAR_GET(native_census))
        return NativeDrawMode::Census;
    return REXCVAR_GET(native_draw_skip) ? NativeDrawMode::Skip : NativeDrawMode::Strict;
}

// The scripted native overlay check opens the input-capturing settings
// overlay through whatever key bind_settings holds.
rex::ui::VirtualKey SettingsOverlayKey()
{
    return rex::ui::ParseVirtualKey(rex::cvar::GetFlagByName("bind_settings"));
}

} // namespace

namespace rerevved::host_settings
{

HostSettings LocalSettings()
{
    HostSettings settings;
    settings.aiCivilizations = REXCVAR_GET(ai_civilizations);
    settings.mods            = LocalMods();
    return settings;
}

} // namespace rerevved::host_settings

namespace rerevved
{

void App::OnPreSetup(rex::RuntimeConfig& config)
{
    REXLOG_INFO("{}", REREVVED_BUILD_INFO);
    config.game_version = REREVVED_VERSION;

    // Sent to every peer that joins a game this machine hosts.
    rex::session_data::SetHostDataProvider([]
                                           {
                                               return host_settings::Encode(host_settings::LocalSettings());
                                           });

    config.mod_script_host = CreateModScriptRulesHost();

    // Player overlays on F1-F4; overlay setup registers the binds after this.
    // The SDK debug overlay starts unbound; the Developer tab can bind it.
    rex::ui::SetBindDefault("bind_settings", "F1");
    rex::ui::SetBindDefault("bind_mod_manager", "F2");
    rex::ui::SetBindDefault("bind_achievements", "F3");
    rex::ui::SetBindDefault("bind_fps_counter", "F4");
    rex::ui::SetBindDefault("bind_debug_overlay", "");

    if (rendererBackend == rerevved::gpu::RendererBackend::Native)
    {
        config.graphics.reset();
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
    // Apply before config loading and the System Link startup snapshot.
    rex::cvar::SetFlagAsApplicationDefault("live_sessions", "true");

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

    // Game of the Week and Leaderboards require a Live sign-in. The SDK
    // reports one without contacting a service; config and CLI still win.
    rex::cvar::SetFlagAsApplicationDefault("live_signin", "true");

    const std::string configuredRenderer = REXCVAR_GET(renderer);
#if REREVVED_DEVELOPER
    // Settings can change renderer in memory. The latched value remains the
    // active backend until restart.
    rendererBackend = rerevved::gpu::LatchRendererBackend(configuredRenderer);
    if (rendererBackend == rerevved::gpu::RendererBackend::Invalid)
    {
        REXLOG_ERROR("Invalid ReRevved renderer '{}'; expected xenos or native", configuredRenderer);
        return false;
    }
    const bool native    = rendererBackend == gpu::RendererBackend::Native;
    auto       admission = gpu::AdmitNativeAppRoute(native, []<typename T>(std::string_view name, T)
                                                    {
                                                  return rex::cvar::Query<T>(name);
                                                    });
    // AdmitNativeAppRoute already refuses the panel capture in these cases.
    for (const auto& family : gpu::guest_d3d::NativeDrawFamilyRegistry())
    {
        if (!admission.empty() || rex::cvar::Query<std::string>(family.option).empty())
            continue;
        if (!native)
            admission = fmt::format("{} requires renderer=native.", family.option);
        else if (!std::string(REXCVAR_GET(native_present_fixture)).empty())
            admission = fmt::format("{} requires a guest boot without native_present_fixture.", family.option);
    }
    if (!admission.empty())
    {
        REXLOG_ERROR("{}", admission);
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, admission);
        return false;
    }
#else
    // Player builds render with Xenos. Native test builds saved renderer and
    // native_* settings to rerevved.toml; they must not stop startup here.
    if (configuredRenderer != "xenos")
        REXLOG_WARN("Ignoring renderer '{}'; this build renders with Xenos", configuredRenderer);
    for (const auto& family : gpu::guest_d3d::NativeDrawFamilyRegistry())
        if (!rex::cvar::Query<std::string>(family.option).empty())
            REXLOG_WARN("Ignoring {}; this build renders with Xenos", family.option);
    if (!std::string(REXCVAR_GET(native_present_fixture)).empty())
        REXLOG_WARN("Ignoring native_present_fixture; this build renders with Xenos");
    // The native renderer saved gpu_plugin = "", which outranks the Xenos
    // default below and leaves a black window.
    if (const auto plugin = rex::cvar::Query<std::string>("gpu_plugin"); plugin != "xenos")
    {
        REXLOG_WARN("Ignoring gpu_plugin '{}'; this build renders with Xenos", plugin);
        rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    rendererBackend   = rerevved::gpu::LatchRendererBackend("xenos");
    const bool native = false;
#endif
    // Before any guest hook runs: native_gamma_apply is on by default for the native renderer only.
    gpu::guest_d3d::SetGammaNativeRenderer(native);
    if (native)
    {
        rex::cvar::SetFlagByName("gpu_plugin", "");
        nativeFixture = !std::string(REXCVAR_GET(native_present_fixture)).empty();
        REXLOG_INFO("ReRevved renderer selected: native ({})", nativeFixture ? "fixture" : "guest boot");
        // The fixture route never constructs a Runtime, so it needs no content.
        return nativeFixture || selectGameData();
    }
    else if (rendererBackend == rerevved::gpu::RendererBackend::Xenos)
    {
        // Apply the title default below config, environment, and CLI values.
        rex::cvar::SetFlagAsApplicationDefault("gpu_plugin", "xenos");
    }
    REXLOG_INFO("ReRevved renderer selected: {}",
                rerevved::gpu::RendererBackendName(rendererBackend));

#if REREVVED_DEVELOPER
    const std::string adapterOutput = REXCVAR_GET(native_guest_adapter_output);
    if (!adapterOutput.empty())
    {
        std::filesystem::path resolved;
        if (rendererBackend != rerevved::gpu::RendererBackend::Xenos || !resolveShadowDirectory(adapterOutput, true, resolved))
        {
            REXLOG_ERROR("Label adapter capture requires Xenos and a fresh output directory under out");
            return false;
        }
    }

    const std::string sceneOutput = REXCVAR_GET(native_scene_capture);
    if (REXCVAR_GET(native_scene_capture_targets) &&
        (sceneOutput.empty() || !REXCVAR_GET(native_scene_capture_payloads) ||
         !REXCVAR_GET(native_scene_capture_target_bytes) || rendererBackend != rerevved::gpu::RendererBackend::Xenos))
    {
        REXLOG_ERROR("Scene targets require Xenos, scene payloads, a scene directory and a nonzero target byte budget");
        return false;
    }
    if (REXCVAR_GET(native_scene_capture_effects) &&
        (sceneOutput.empty() || !REXCVAR_GET(native_scene_capture_payloads) || std::string(REXCVAR_GET(native_route_trace)).empty()))
    {
        REXLOG_ERROR("Scene effects require native_scene_capture, native_scene_capture_payloads and native_route_trace");
        return false;
    }
    if (!sceneOutput.empty())
    {
        std::filesystem::path output;
        std::string           error;
        const auto            diagnostics = std::filesystem::absolute("out/diagnostics").lexically_normal();
        bool                  separate    = true;
        for (const auto& other : { adapterOutput, std::string(REXCVAR_GET(native_route_trace)) })
        {
            if (other.empty())
                continue;
            std::filesystem::path otherPath;
            if (!resolveShadowDirectory(other, true, otherPath) ||
                !resolveShadowDirectory(sceneOutput, true, output) || output == otherPath ||
                isContainedPath(output, otherPath) || isContainedPath(otherPath, output))
                separate = false;
        }
        if (rendererBackend != rerevved::gpu::RendererBackend::Xenos || !separate ||
            !resolveShadowDirectory(sceneOutput, true, output) || !isContainedPath(diagnostics, output) ||
            !rerevved::gpu::diagnostics::StartSceneResourceCapture(output, REXCVAR_GET(native_scene_capture_frames), REREVVED_GIT_COMMIT " " REREVVED_GIT_DIRTY, REXGLUE_GIT_REVISION " " REXGLUE_GIT_DIRTY, error, REXCVAR_GET(native_scene_capture_payloads), REXCVAR_GET(native_scene_capture_payload_bytes), REXCVAR_GET(native_scene_capture_effects), REXCVAR_GET(native_scene_capture_targets), REXCVAR_GET(native_scene_capture_target_bytes), REXCVAR_GET(native_scene_capture_arm_frame), { REXCVAR_GET(native_scene_capture_arm_ps_digest), REXCVAR_GET(native_scene_capture_arm_delay), REXCVAR_GET(native_scene_capture_windows) }))
        {
            REXLOG_ERROR("Scene capture requires Xenos and a fresh separate directory under out/diagnostics: {}", error);
            return false;
        }
    }

    const std::string referenceOutput = REXCVAR_GET(native_xenos_frame_reference);
    if (!referenceOutput.empty() || REXCVAR_GET(native_xenos_frame_reference_arm_frame) || !REXCVAR_GET(native_xenos_frame_reference_arm_ps_digest).empty())
    {
        std::filesystem::path                                        output;
        std::string                                                  error;
        const auto                                                   diagnostics = std::filesystem::absolute("out/diagnostics").lexically_normal();
        const rerevved::gpu::diagnostics::XenosFrameReferenceOptions options{ REXCVAR_GET(native_xenos_frame_reference_arm_frame), REXCVAR_GET(native_xenos_frame_reference_arm_ps_digest), REXCVAR_GET(native_xenos_frame_reference_arm_delay), REXCVAR_GET(native_xenos_frame_reference_windows) };
        if (rendererBackend != rerevved::gpu::RendererBackend::Xenos ||
            !resolveShadowDirectory(referenceOutput, true, output) || !isContainedPath(diagnostics, output) ||
            !rerevved::gpu::diagnostics::StartXenosFrameReference(output, options, error))
        {
            REXLOG_ERROR("Xenos frame reference requires Xenos and a fresh directory under out/diagnostics: {}", error);
            return false;
        }
    }

    const std::string harvestOutput = REXCVAR_GET(native_shader_harvest);
    const std::string harvestCaller = REXCVAR_GET(native_shader_harvest_caller);
    if (!harvestOutput.empty() || !harvestCaller.empty())
    {
        std::filesystem::path        output;
        std::string                  error;
        std::optional<std::uint32_t> caller;
        std::uint32_t                parsed      = 0;
        const auto                   diagnostics = std::filesystem::absolute("out/diagnostics").lexically_normal();
        // The scene capture and frame reference have created their directories already.
        bool separate = resolveShadowDirectory(harvestOutput, true, output);
        for (const auto& other : { adapterOutput, sceneOutput, referenceOutput, std::string(REXCVAR_GET(native_route_trace)) })
        {
            std::error_code pathError;
            const auto      otherPath = std::filesystem::absolute(other, pathError).lexically_normal();
            if (!other.empty() && (pathError || output == otherPath || isContainedPath(output, otherPath) || isContainedPath(otherPath, output)))
                separate = false;
        }
        if (!harvestCaller.empty())
        {
            if (!rerevved::gpu::diagnostics::ParseShaderHarvestCaller(harvestCaller, parsed))
                separate = false;
            caller = parsed;
        }
        const auto                                       frameForms = REXCVAR_GET(native_shader_harvest_frame_forms);
        const auto                                       frameMiB   = REXCVAR_GET(native_shader_harvest_frame_mib);
        rerevved::gpu::diagnostics::ShaderHarvestOptions options{ caller, frameForms, std::uint64_t(frameMiB) << 20 };
        const std::string                                knownList = REXCVAR_GET(native_shader_harvest_known);
        for (std::size_t begin = 0; begin <= knownList.size();)
        {
            const auto end  = std::min(knownList.find(';', begin), knownList.size());
            const auto item = knownList.substr(begin, end - begin);
            if (!item.empty())
                options.known.push_back(std::filesystem::absolute(item).lexically_normal());
            begin = end + 1;
        }
        if (const std::string arm = REXCVAR_GET(native_shader_harvest_arm); !arm.empty())
            options.armFile = std::filesystem::path(arm).is_absolute() ? std::filesystem::path(arm) : output / arm;
        // The prior harvest is read-only. Store the new harvest in a separate directory.
        for (const auto& known : options.known)
            if (known == output || isContainedPath(known, output))
                separate = false;
        if (rendererBackend != rerevved::gpu::RendererBackend::Xenos || !separate || !isContainedPath(diagnostics, output) || frameForms > 16 || frameMiB < 1 ||
            frameMiB > 65536 || (!options.armFile.empty() && !frameForms) ||
            !rerevved::gpu::diagnostics::StartShaderHarvest(output, options, rerevved::gpu::guest_d3d::ReadGuestMemory, error))
        {
            REXLOG_ERROR("Shader harvest requires Xenos, a fresh separate directory under out/diagnostics, an optional 0x-prefixed 8-digit caller, 0..16 frame forms "
                         "(at least 1 with an arm file), 1..65536 MiB and readable known harvests: {}",
                         error);
            return false;
        }
    }

    const std::string routeOutput = REXCVAR_GET(native_route_trace);
    if (!routeOutput.empty())
    {
        std::filesystem::path output;
        std::filesystem::path adapterDirectory;
        std::string           error;
        const auto            diagnostics = std::filesystem::absolute("out/diagnostics").lexically_normal();
        if (!resolveShadowDirectory(routeOutput, true, output) ||
            !isContainedPath(diagnostics, output) ||
            (!adapterOutput.empty() &&
             (!resolveShadowDirectory(adapterOutput, true, adapterDirectory) ||
              output == adapterDirectory || isContainedPath(output, adapterDirectory) ||
              isContainedPath(adapterDirectory, output))) ||
            !rerevved::gpu::diagnostics::StartRouteTrace(output,
                                                         REREVVED_GIT_COMMIT " " REREVVED_GIT_DIRTY,
                                                         REXGLUE_GIT_REVISION " " REXGLUE_GIT_DIRTY,
                                                         error))
        {
            REXLOG_ERROR("Route trace requires a fresh directory under out/diagnostics: {}", error);
            return false;
        }
    }
#endif

    return selectGameData();
}

bool App::selectGameData()
{
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

bool App::ConstructRuntime(const rex::PathConfig& paths)
{
    // Weekly GOTW seeds reach the settings field on the UI thread only.
    rerevved::gotw::SetSeedDispatcher([this](std::function<void()> update)
                                      {
                                          app_context().CallInUIThreadDeferred(std::move(update));
                                      },
                                      gameplayUiAlive);
    if (rendererBackend == gpu::RendererBackend::Native)
    {
        if (nativeFixture)
            return false;
        // Register title video services before LaunchModule resumes the guest.
        return rex::ReXApp::ConstructRuntime(paths) && startGuestVideoServices();
    }
    return rex::ReXApp::ConstructRuntime(paths);
}

std::unique_ptr<rex::ui::ImmediateDrawer> App::OnCreateImmediateDrawer()
{
#if defined(_WIN32)
    if (rendererBackend == gpu::RendererBackend::Native)
    {
        nativeOverlays = std::make_shared<gpu::NativeOverlayMailbox>();
        return std::make_unique<gpu::NativeImmediateDrawer>(nativeOverlays);
    }
#endif
    return nullptr;
}

bool App::SetupPresentation()
{
    if (!rex::ReXApp::SetupPresentation())
    {
#if REREVVED_DEVELOPER
        if (rendererBackend == gpu::RendererBackend::Native && !window() &&
            !REXCVAR_GET(native_present_trace).empty() && !std::filesystem::exists(REXCVAR_GET(native_present_trace)))
        {
            std::ofstream trace(REXCVAR_GET(native_present_trace));
            trace << "[[event]]\nname = 'unavailable'\nframe = 0\nvalue = 0\n";
        }
#endif
        return false;
    }

    if (rendererBackend == rerevved::gpu::RendererBackend::Native)
    {
        return setupNativePresentation();
    }
    // The Xenos plugin owns this cvar, so it is not registered until the base
    // presentation setup loads the plugin.
    else
    {
        rex::cvar::SetFlagAsApplicationDefault("render_target_path_d3d12", "rov");
    }

#if REREVVED_DEVELOPER
    if (const std::string adapterOutput = REXCVAR_GET(native_guest_adapter_output); !adapterOutput.empty())
    {
        std::filesystem::path output;
        std::string           error;
        if (!resolveShadowDirectory(adapterOutput, true, output) ||
            !rerevved::gpu::diagnostics::StartLabelAdapterCapture(output, { rerevved::gpu::guest_d3d::ReadGuestMemory }, error))
        {
            REXLOG_ERROR("Label adapter setup failed: {}", error);
            return false;
        }
    }
#endif
    if (REXCVAR_GET(xenos_frame_counter) && rendererBackend == rerevved::gpu::RendererBackend::Xenos)
    {
        rerevved::xenos_frames::Start();
    }
    return true;
}

std::optional<rex::PathConfig> App::OnFinalizePaths(const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume)
{
    (void)resume;
    // The fixture keeps the event loop without ever resuming the runtime path.
    if (rendererBackend == gpu::RendererBackend::Native && nativeFixture)
        return std::nullopt;

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

void App::OnPreSystemLinkInit()
{
    if (!rerevved::DiscordInvitesEnabled() || !REXCVAR_GET(discord_join_launch))
    {
        return;
    }

    // Discord sends the join event only after this launch connects, so wait
    // for it before the System Link settings freeze.
    const std::string secret =
        rex::discord_rpc::WaitForJoin(rerevved::kDiscordClientId, kDiscordJoinWait);
    if (secret.empty())
    {
        REXLOG_INFO("Discord join launch: no join arrived; starting normally");
        return;
    }
    const bool applied = rex::cvar::SetFlagByName("live_invite", secret);
    REXLOG_INFO("Discord join launch: invite {}", applied ? "applied" : "rejected");
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
                           ? kNativeGuestTitle
                           : "ReRevved");
}

// Snapshot builds follow plugin and script activation before the guest thread;
// F2 rescans replace the catalog only.
void App::OnPostLaunchModule(rex::system::XThread* thread)
{
    rex::ReXApp::OnPostLaunchModule(thread);

    std::vector<host_settings::ModRef> mods;
    for (const auto& active : runtime()->active_mod_states())
    {
        const auto& packages = runtime()->active_mods_info();
        const auto  package  = std::ranges::find(packages, active.id, &rex::system::ModPackage::id);
        mods.push_back({ active.id,
                         package != packages.end() ? package->version : std::string{},
                         package != packages.end() ? package->display_name : std::string{} });
    }
    host_settings::PublishLocalMods(std::move(mods));
}

void App::OnGuestThreadExit(rex::system::XThread* thread)
{
    (void)thread;
    rerevved::xenos_frames::Finalize();
}

void App::OnShutdown()
{
    rerevved::gotw::SetSeedDispatcher({});
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ReportCompletionProbe(true);
    rerevved::gpu::guest_d3d::ReportTilingRestore(true);
    rerevved::gpu::guest_d3d::ReportVsReadbackProbe(true);
    rerevved::gpu::guest_d3d::ReportVideoScalerStub(true);
    rerevved::gpu::guest_d3d::ReportVideoDisplayModeStub(true);
    rerevved::gpu::guest_d3d::ReportVideoImportStubs(true);
#endif
    // The guest has exited; release title video services before the Runtime:
    // ledger and producers stop, then the draw dispatcher, then MMIO.
    bool guestVideoStopped = true;
    if (guestVideo)
    {
        // No new host work, and no completion advances, after the source closes.
        closeNativeCompletion();
        guestVideoStopped    = guestVideo->StopProducers(gpu::kVblankJoinTimeout);
        const bool withdrawn = removeNativeDraws();
        if (runtime())
            runtime()->SetDetachedVideoDelegate(nullptr);
        if (guestVideoStopped)
        {
            guestVideo->Shutdown(std::chrono::milliseconds::zero());
            guestVideo.reset();
        }
        releaseNativeFrames(withdrawn && guestVideoStopped);
    }
    if (guestVideoStopped)
    {
#if REREVVED_DEVELOPER
        rerevved::gpu::guest_d3d::CloseOriginalResidueProbe();
        rerevved::gpu::guest_d3d::ReportOriginalResidueProbe(true);
#endif
#if REREVVED_DEVELOPER
        if (rerevved::gpu::guest_d3d::RingConsumeCvar())
            rerevved::gpu::guest_d3d::ReportRingConsume();
#endif
    }
#if defined(_WIN32)
    if (nativePresentation)
    {
        {
            // Retained workers must not post through a destroyed app context.
            std::lock_guard lock(*nativeDispatchMutex);
            gameplayUiAlive->store(false);
        }
        if (!nativeDrain)
            nativeDrain = nativePresentation->Close();
        nativeDrain->result.wait_until(nativeDrain->deadline.Expiry());
        if (!guestVideoStopped)
            hardExitAfterMissedVblankJoin();
        ShutdownOverlays();
        nativePresentation->FlushTrace();
#if REREVVED_DEVELOPER
        {
            std::string cadenceError;
            if (!gpu::CloseNativeCadenceTrace(cadenceError))
                REXLOG_ERROR("Native cadence trace close failed: {}", cadenceError);
        }
        {
            std::string effectError;
            if (!gpu::guest_d3d::CloseDrawEffectProbe(effectError))
                REXLOG_ERROR("Native draw effect probe close failed: {}", effectError);
        }
#endif
        nativePresentation.reset();
        return;
    }
#endif
    if (!guestVideoStopped)
        hardExitAfterMissedVblankJoin();
    rerevved::xenos_frames::Finalize();
    gameplayUiAlive->store(false);
    SetGuestFrameStats({});
#if REREVVED_DEVELOPER
    rerevved::gpu::diagnostics::StopRouteTrace();
    rerevved::gpu::diagnostics::StopSceneResourceCapture();
    rerevved::gpu::diagnostics::StopShaderHarvest();
    rerevved::gpu::diagnostics::StopXenosFrameReference();
    rerevved::gpu::diagnostics::StopLabelAdapterCapture();
#endif
    rerevved::StopPresence();
    nativeRenderer.Shutdown();
}

bool App::OnWindowCloseRequested()
{
#if defined(_WIN32)
    if (nativePresentation)
    {
        // The source refuses new work before the producer drains.
        closeNativeCompletion();
        const auto decision = nativeClose.Request();
        if (decision == gpu::NativeAppClose::Decision::Accept)
        {
            // The SDK terminates the title after this returns; stop vblank first.
            stopGuestVideoProducers();
            nativePresentation->Event("close_accept");
            return true;
        }
        nativePresentation->Event("close_veto");
        if (decision == gpu::NativeAppClose::Decision::Veto)
            return false;
        nativeClosing            = true;
        const auto alive         = gameplayUiAlive;
        const auto dispatchMutex = nativeDispatchMutex;
        auto*      context       = &app_context();
        nativeDrain              = nativePresentation->Close([this, alive, context, dispatchMutex](bool drained)
                                                             {
                                                    std::lock_guard lock(*dispatchMutex);
                                                    if (!alive->load())
                                                        return;
                                                    context->CallInUIThreadDeferred([this, alive, drained]
                                                                                    {
                                                                                        if (!alive->load())
                                                                                            return;
                                                                                        nativeClose.Complete([this]
                                                                                                             {
                                                                                                                 ShutdownOverlays();
                                                                                                                 nativePresentation->Event("overlays_shutdown");
                                                                                                             });
                                                                                        if (!drained)
                                                                                            REXLOG_ERROR("Native shutdown deadline expired or drain failed; owners retained");
                                                                                        if (OnWindowCloseRequested())
                                                                                            window()->RequestClose();
                                                                                    });
                                                             });
        return false;
    }
#endif
    rerevved::xenos_frames::Finalize();
    gameplayUiAlive->store(false);
    SetGuestFrameStats({});
#if REREVVED_DEVELOPER
    rerevved::gpu::diagnostics::StopRouteTrace();
    rerevved::gpu::diagnostics::StopSceneResourceCapture();
    rerevved::gpu::diagnostics::StopShaderHarvest();
    rerevved::gpu::diagnostics::StopXenosFrameReference();
    rerevved::gpu::diagnostics::StopLabelAdapterCapture();
#endif
    rerevved::StopPresence();
    nativeRenderer.Shutdown();
    return true;
}

void App::OnWindowFocusChanged(bool focused)
{
#if defined(_WIN32)
    if (nativePresentation && !nativeClosing)
        nativePresentation->RequestRepaint();
#endif
}

void App::OnWindowPixelSizeChanged(uint32_t pixelWidth, uint32_t pixelHeight)
{
#if defined(_WIN32)
    if (nativePresentation && !nativeClosing)
        nativePresentation->RequestExtent({ pixelWidth, pixelHeight });
#endif
    if (nativeRenderer.Initialized() && pixelWidth != 0 && pixelHeight != 0 &&
        !nativeRenderer.Resize(pixelWidth, pixelHeight))
    {
        REXLOG_ERROR("Native renderer resize failed: {}x{}", pixelWidth, pixelHeight);
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                  "Native D3D12 resize failed. See the log for details.");
        app_context().RequestDeferredQuit();
    }
}

bool App::setupNativePresentation()
{
    std::string error;
#if defined(_WIN32)
    gpu::NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(REXCVAR_GET(native_stream_queue) ? gpu::NativeRendererD3D12::kDrawStreamQueueDepth : 1);
    gpu::NativeRendererD3D12::SetDefaultDirectStream(REXCVAR_GET(native_direct_stream));
    gpu::NativeRenderFrame                   frame;
    std::shared_ptr<gpu::NativeAppEventSink> events;
#if REREVVED_DEVELOPER
    // Opened before the session so its first boundaries are recorded; the session closes it.
    const auto cadencePath   = REXCVAR_GET(native_cadence_trace);
    const bool cadenceFailed = !cadencePath.empty() && !gpu::OpenNativeCadenceTrace(cadencePath, error);
    const bool cadenceOpen   = !cadencePath.empty() && !cadenceFailed;
    // Like the cadence trace, opened before the draw hooks can see a root.
    const auto effectPath   = REXCVAR_GET(native_draw_effect_probe);
    const bool effectFailed = !cadenceFailed && !effectPath.empty() && !gpu::guest_d3d::OpenDrawEffectProbe(effectPath, error);
    const bool effectOpen   = !cadenceFailed && !effectPath.empty() && !effectFailed;
    const auto tracePath    = REXCVAR_GET(native_present_trace);
    const bool traceFailed  = !cadenceFailed && !effectFailed && !tracePath.empty() && !(events = gpu::OpenNativeAppTraceFile(tracePath, error));
    if (cadenceFailed)
        error += ": " + cadencePath;
    else if (effectFailed)
        error += ": " + effectPath;
    const bool recordersOpen = !cadenceFailed && !effectFailed && !traceFailed;
#else
    const bool recordersOpen = true;
#endif
    if (!recordersOpen)
    {
        // The failed recorder set the error.
    }
    else if (runtime())
        error = "Native presentation must start before the Runtime.";
    else if (!nativeFixture)
        // Guest frames arrive through the completion source; until one is published the canvas stays black.
        nativePresentation = gpu::NativeAppSession::CreateGuest(window()->GetNativeWindowHandle(), events, error);
    else if (gpu::LoadPresentationFixture(REXCVAR_GET(native_present_fixture), frame, error))
        nativePresentation = gpu::NativeAppSession::Create(window()->GetNativeWindowHandle(), std::move(frame), events, error);
    if (nativePresentation)
    {
        nativePresentation->Event("detached", 0, 1);
        nativePresentation->SetOverlayMailbox(nativeOverlays);
        if (nativeFixture)
            window()->SetTitle("ReRevved - Native fixture");
        const auto alive         = gameplayUiAlive;
        const auto dispatchMutex = nativeDispatchMutex;
        const auto frameRate     = nativeFrameRate;
        auto*      context       = &app_context();
        // The SDK counter reads presenter refreshes, which native mode never produces.
        SetGuestFrameStats([frameRate]
                           {
                               return frameRate->Snapshot(rex::ui::FrameRateMeter::Clock::now());
                           });
        nativePresentation->Start([this, alive, context, dispatchMutex, frameRate](gpu::PresentResult result)
                                  {
                                      if (gpu::ShowsNewFrame(result))
                                          frameRate->Record(rex::ui::FrameRateMeter::Clock::now());
                                      std::lock_guard lock(*dispatchMutex);
                                      if (!alive->load())
                                          return;
                                      context->CallInUIThreadDeferred([this, alive, result]
                                                                      {
                                                                          if (alive->load())
                                                                              nativePresentationTick(result);
                                                                      });
                                  });
        return true;
    }
#if REREVVED_DEVELOPER
    if (cadenceOpen)
    {
        std::string closeError;
        gpu::CloseNativeCadenceTrace(closeError);
    }
    if (effectOpen)
    {
        std::string closeError;
        gpu::guest_d3d::CloseDrawEffectProbe(closeError);
    }
#endif
#else
    error = "Native presentation currently requires Windows D3D12.";
#endif
    const std::string failure = std::string(nativeFixture ? "Native fixture startup failed: " : "Native presentation startup failed: ") + error;
    REXLOG_ERROR("{}", failure);
    // The hidden harness consumes the event log without opening a modal dialog.
    if (!NativeScriptActive())
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, failure);
    return false;
}

bool App::startGuestVideoServices()
{
    using rex::X_STATUS;
    if (!createNativeFrames())
        return false;
    guestVideo = std::make_shared<gpu::GuestVideoServices>();
    gpu::GuestVideoServices::Options options;
    const auto                       alive         = gameplayUiAlive;
    const auto                       dispatchMutex = nativeDispatchMutex;
    auto*                            context       = &app_context();
    options.drawMode                               = NativeDrawModeFromFlags();
    options.draws                                  = nativeFrames.get();
#if defined(_WIN32)
    if (nativeCompletion && options.drawMode == gpu::guest_d3d::NativeDrawMode::Census)
        // Each skipped host-work failure, then the source failure once: it
        // stopped the source, so it ends the census session.
        options.pollDrawFailure = [source = nativeCompletion, reported = std::make_shared<bool>(false)]() -> std::optional<gpu::GuestVideoServices::DrawFailure>
        {
            source->Completed();
            source->ObserveCompletedOutputs();
            if (auto skipped = source->TakeSkippedFailure())
                return gpu::GuestVideoServices::DrawFailure{ std::move(*skipped), false };
            auto failure = source->Failure();
            if (!failure || std::exchange(*reported, true))
                return std::nullopt;
            return gpu::GuestVideoServices::DrawFailure{ std::move(*failure), true };
        };
    else if (nativeCompletion)
        options.pollDrawFailure = [source = nativeCompletion]() -> std::optional<gpu::GuestVideoServices::DrawFailure>
        {
            source->Completed();
            source->ObserveCompletedOutputs();
            if (auto failure = source->Failure())
                return gpu::GuestVideoServices::DrawFailure{ std::move(*failure), true };
            return std::nullopt;
        };
#endif
    options.assembleSwap = [this](const rex::system::DetachedVideoSwap& swap)
    {
        return assembleNativeSwap(swap);
    };
    options.reportDebt = [this, alive, context, dispatchMutex](const gpu::CompletionDebt& debt)
    {
        std::lock_guard lock(*dispatchMutex);
        if (!alive->load())
            return;
        context->CallInUIThreadDeferred([this, alive, debt]
                                        {
                                            if (!alive->load())
                                                return;
#if defined(_WIN32)
                                            if (nativePresentation)
                                                nativePresentation->Event("completion_debt", debt.swapCount, debt.writePointer);
#endif
                                            // A failed session keeps its more specific title.
                                            if (!nativeCompletionFailed)
                                                window()->SetTitle(std::string(kNativeGuestTitle) + kCompletionDebtTitleSuffix +
                                                                   (debt.ledger.unowned.empty() ? debt.ledger.oldestRecord : debt.ledger.unowned));
                                        });
    };
    // The first unsupported draw in strict mode lands here with its hook and caller.
    options.reportFailure = [this, alive, context, dispatchMutex](const gpu::completion::LedgerFailure& failure)
    {
        std::lock_guard lock(*dispatchMutex);
        if (!alive->load())
            return;
        context->CallInUIThreadDeferred([this, alive, failure]
                                        {
                                            if (!alive->load())
                                                return;
                                            nativeCompletionFailed = true;
#if defined(_WIN32)
                                            if (nativePresentation)
                                                nativePresentation->Event("completion_failed", static_cast<std::uint64_t>(failure.kind), failure.address);
#endif
                                            window()->SetTitle(std::string(kNativeGuestTitle) + kCompletionFailureTitleSuffix + failure.detail);
                                        });
    };
    rex::X_STATUS status = guestVideo->Start(*runtime(), std::move(options));
    bool          freed  = true;
    if (XSUCCEEDED(status) && !installNativeDraws())
    {
        status = X_STATUS_UNSUCCESSFUL;
        freed  = guestVideo->Shutdown();
    }
    if (XSUCCEEDED(status))
    {
        status = runtime()->SetDetachedVideoDelegate(guestVideo);
        if (XFAILED(status))
        {
            closeNativeCompletion();
            const bool stopped = guestVideo->StopProducers(gpu::kVblankJoinTimeout);
            freed              = removeNativeDraws() && stopped;
            guestVideo->Shutdown();
        }
    }
    if (XFAILED(status))
    {
        REXLOG_ERROR("Native guest video services failed to start: {:08X}", status);
        guestVideo.reset();
        closeNativeCompletion();
        releaseNativeFrames(freed);
        return false;
    }
#if defined(_WIN32)
    if (nativePresentation)
        nativePresentation->Event("guest_video_started");
#endif
    return true;
}

#if REREVVED_DEVELOPER
// native_frame_timing counters. Drawing threads add resolved reads; the
// window and its baselines change only under the mutex, on the swap thread.
struct App::NativeFrameTiming
{
    struct Totals
    {
        gpu::NativeDrawReplayTimingStats                      timing{};
        gpu::NativeDrawReplayCacheStats                       cache{};
        std::pair<std::uint64_t, std::uint64_t>               encodes{}, graphPeeks{};
        std::uint64_t                                         graphPeekRecording = 0;
        gpu::guest_d3d::NativeResolveGraphStats               graph{};
        std::uint64_t                                         reads = 0, readNanoseconds = 0, builds = 0, buildNanoseconds = 0;
        gpu::guest_d3d::NativeFrameAssembler::ReadMirrorStats mirror{};
    };

    std::atomic<std::uint64_t>            reads{ 0 }, readNanoseconds{ 0 }, builds{ 0 }, buildNanoseconds{ 0 };
    std::mutex                            mutex;
    std::chrono::steady_clock::time_point windowStart{}, lastSwap{};
    std::chrono::steady_clock::duration   assembly{}, longestSwap{};
    std::uint64_t                         swaps = 0;
    gpu::guest_d3d::GuestApiCounts        apiCalls{};   // API-call counts for this timing window.
    gpu::guest_d3d::NativeDrawCensus      drawCensus{}; // Draw counts for this timing window.
    Totals                                totals;

    // GuestReadSite counters include bytes, timing, lock/validation waits,
    // image overlaps, and registry overlaps.
    struct ReadSite
    {
        std::atomic<std::uint64_t> reads{ 0 }, nanoseconds{ 0 }, lockNanoseconds{ 0 }, viewNanoseconds{ 0 }, bytes{ 0 }, images{ 0 }, flushes{ 0 }, surfaces{ 0 };
        std::atomic<bool>          logged{ false };
    };

    static constexpr std::size_t                                  kOutsideBuild = gpu::guest_d3d::kGuestReadSiteCount;
    std::array<ReadSite, gpu::guest_d3d::kGuestReadSiteCount + 1> readSites;
    // Count newly seen texture versions and their payload bytes by read source:
    // any resolve-range read, other guest-memory reads, or mirror hits only.
    std::mutex                                                                                versionsMutex;
    std::unordered_map<const void*, std::weak_ptr<const gpu::NativeDrawReplayTextureVersion>> versions;
    std::array<std::atomic<std::uint64_t>, 3>                                                 freshVersions{}, freshBytes{};
#if defined(_WIN32)
    std::weak_ptr<gpu::NativeAppSession>      producer;
    std::weak_ptr<gpu::D3D12CompletionSource> completion;
#endif
};
#endif

// The assembler and its completion source. Without the option of any family
// in NativeDrawFamilyRegistry the assembler admits nothing, so every draw has
// no native route: strict mode fails at the first one, skip mode skips it.
bool App::createNativeFrames()
{
    namespace guest = gpu::guest_d3d;
#if REREVVED_DEVELOPER
    if (REXCVAR_GET(native_frame_timing))
    {
        nativeFrameTiming = std::make_shared<NativeFrameTiming>();
        guest::EnableGuestApiCensus(true);
        guest::EnableNativeDrawCensus(true);
    }
#endif
    guest::NativeFrameAssemblerConfig config;
    // A deferred draw's target words come from its snapshot.
    config.readTarget = [](std::uint32_t address, std::uint32_t& word)
    {
        const auto held  = guest::GuestReadSnapshotScope::View(address, 4);
        const auto bytes = held ? *held : gpu::guest_d3d::ViewGuestMemory(address, 4, false);
        if (bytes.size() != 4)
            return false;
        word = (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
        return true;
    };
    config.viewTarget = [](std::uint32_t address, std::uint32_t size) -> std::span<const std::uint8_t>
    {
        try
        {
            if (const auto held = guest::GuestReadSnapshotScope::View(address, size))
                return *held;
            return gpu::guest_d3d::ViewGuestMemory(address, size, false);
        }
        catch (const std::exception&)
        {
            return {};
        }
    };
    guest::ReadMemory shaderReads = gpu::guest_d3d::ReadGuestMemory;
    auto              shaderCache = std::make_shared<guest::GuestShaderReadCache>();
    shaderReads.shaderCache       = shaderCache;
    shaderReads.view              = [](std::uint64_t address, std::uint64_t size, bool physical)
    {
        return gpu::guest_d3d::ViewGuestMemory(address, size, physical);
    };
    if (REXCVAR_GET(native_draw_sequencer))
        shaderReads = guest::SnapshotReads(std::move(shaderReads));
    config.readShaders = [shaderReads = std::move(shaderReads)](const guest::NativeDrawSubmission& submission, guest::NativeDrawShaderWords& words, std::string& error)
    {
        return guest::ReadNativeDrawShaderWords(submission, shaderReads, words, error);
    };
#if REREVVED_DEVELOPER
    if (guest::VsCodeWatchRequested())
    {
        // The watch runs on a guest thread at draw entry. Pages can become
        // inaccessible after the access query.
        guest::InstallVsCodeWatch([](std::uint64_t address, std::uint64_t size, bool physical)
                                  {
                                      const auto                view = gpu::guest_d3d::ViewGuestMemory(address, size, physical);
                                      std::vector<std::uint8_t> bytes(view.size());
                                      if (!guest::CopyGuestGuarded(bytes.data(), view.data(), bytes.size()))
                                          throw std::runtime_error("vs_watch_memory_unreadable");
                                      return guest::GuestBytes(std::move(bytes));
                                  });
    }
#endif
    config.mode                  = NativeDrawModeFromFlags();
    config.resolveGraph          = REXCVAR_GET(native_resolve_graph);
    config.eagerGraphSegments    = config.resolveGraph && REXCVAR_GET(native_eager_graph_segments);
    config.resolveAlias          = config.resolveGraph && REXCVAR_GET(native_resolve_alias);
    config.adoptGammaWithoutWork = gpu::guest_d3d::GammaApplyRequested();
    // The lock, unlock, tiling and free hooks reach the registry the
    // assembler holds for as long as it lives.
    if (REXCVAR_GET(native_surface_registry))
    {
        config.surfaces = std::make_shared<guest::ResolveSurfaceRegistry>();
        // Deferred draws bind what they would have at their entry.
        if (REXCVAR_GET(native_draw_sequencer))
            config.surfaces->SetBarrier([]
                                        {
                                            guest::DrainNativeDraws();
                                        });
        guest::PublishResolveSurfaces(config.surfaces);
    }
#if REREVVED_DEVELOPER
    config.timing = nativeFrameTiming != nullptr;
#endif
    config.mirroredReadMinBytes = std::size_t(REXCVAR_GET(native_read_mirror_min_kib)) << 10;
    const auto refuse           = [](const std::string& failure)
    {
        REXLOG_ERROR("{}", failure);
        rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, failure);
        return false;
    };
    // Families read guest memory through the assembler created below, which
    // owns them, so a texture sampled after its off-screen resolve sees it.
    auto frames = std::make_shared<std::atomic<guest::NativeFrameAssembler*>>(nullptr);
#if REREVVED_DEVELOPER
    const auto readBytes = [frames, timing = nativeFrameTiming, surfaces = config.surfaces](std::uint64_t address, std::uint64_t size, bool physical)
    {
        auto* assembler = frames->load(std::memory_order_acquire);
        if (!assembler || !physical || address > UINT32_MAX || size > UINT32_MAX)
            return guest::GuestBytes(gpu::guest_d3d::ReadGuestMemory(address, size, physical));
        guest::GuestBytes                              bytes;
        std::string                                    failure;
        guest::NativeFrameAssembler::ReadResolvedTrace trace;
        std::uint64_t                                  viewed = 0;
        // Counts the read, including any wait for a resolve's readback, by site.
        const struct Timed
        {
            NativeFrameTiming*                                    timing;
            const guest::ResolveSurfaceRegistry*                  surfaces;
            std::uint64_t                                         address, size;
            const guest::NativeFrameAssembler::ReadResolvedTrace& trace;
            const std::uint64_t&                                  viewed;
            std::chrono::steady_clock::time_point                 started;
            ~Timed()
            {
                if (!timing)
                    return;
                const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
                timing->reads.fetch_add(1, std::memory_order_relaxed);
                timing->readNanoseconds.fetch_add(elapsed, std::memory_order_relaxed);
                auto&      build   = CurrentNativeBuildReads();
                const auto site    = guest::CurrentGuestReadSite();
                const bool outside = !build.building && site == guest::GuestReadSite::Other;
                auto&      counted = timing->readSites[outside ? NativeFrameTiming::kOutsideBuild : std::size_t(site)];
                const auto entry   = surfaces ? surfaces->Overlapping(address, address + size) : std::nullopt;
                const bool range   = trace.overlapped || entry;
                const bool texture = site == guest::GuestReadSite::Texture || site == guest::GuestReadSite::MirrorTexture;
                counted.reads.fetch_add(1, std::memory_order_relaxed);
                counted.nanoseconds.fetch_add(elapsed, std::memory_order_relaxed);
                counted.lockNanoseconds.fetch_add(trace.lockNanoseconds, std::memory_order_relaxed);
                counted.viewNanoseconds.fetch_add(viewed, std::memory_order_relaxed);
                counted.bytes.fetch_add(size, std::memory_order_relaxed);
                counted.images.fetch_add(trace.overlapped, std::memory_order_relaxed);
                counted.flushes.fetch_add(trace.flushed, std::memory_order_relaxed);
                counted.surfaces.fetch_add(entry.has_value(), std::memory_order_relaxed);
                build.textureRead    = build.textureRead || texture;
                build.textureResolve = build.textureResolve || (texture && range);
                if (range && !counted.logged.exchange(true, std::memory_order_relaxed))
                    try
                    {
                        const auto found = entry.value_or(std::array<std::uint64_t, 3>{});
                        REXLOG_INFO("Native frame reads: first resolve-range read site={} range={:08X}+{:X} caller={:08X} building={} image_mark={} image={:08X}-{:08X} flushed={} surface_mark={} surface={:08X}-{:08X}",
                                    outside ? "outside" : guest::kGuestReadSiteNames[std::size_t(site)],
                                    address,
                                    size,
                                    build.caller,
                                    build.building,
                                    trace.mark,
                                    trace.begin,
                                    trace.end,
                                    trace.flushed,
                                    found[0],
                                    found[1],
                                    found[2]);
                    }
                    catch (...)
                    {
                    }
            }
        } timed{ timing.get(), timing ? surfaces.get() : nullptr, address, size, trace, viewed, timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{} };
        if (!assembler->ReadResolved(static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(size), [&]
                                     {
                                         if (!timing)
                                             return gpu::guest_d3d::ViewGuestMemory(address, size, physical);
                                         const auto viewing = std::chrono::steady_clock::now();
                                         const auto view    = gpu::guest_d3d::ViewGuestMemory(address, size, physical);
                                         viewed += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - viewing).count());
                                         return view;
                                     },
                                     bytes,
                                     failure,
                                     timing ? &trace : nullptr))
        {
            REXLOG_ERROR("Native draws: resolved read at {:08X}+{:X}: {}", address, size, failure);
            return guest::GuestBytes{};
        }
        return bytes;
    };
#else
    const auto readBytes = [frames](std::uint64_t address, std::uint64_t size, bool physical)
    {
        auto* assembler = frames->load(std::memory_order_acquire);
        if (!assembler || !physical || address > UINT32_MAX || size > UINT32_MAX)
            return guest::GuestBytes(gpu::guest_d3d::ReadGuestMemory(address, size, physical));
        guest::GuestBytes bytes;
        std::string       failure;
        if (!assembler->ReadResolved(static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(size), [&]
                                     {
                                         return gpu::guest_d3d::ViewGuestMemory(address, size, physical);
                                     },
                                     bytes,
                                     failure))
        {
            REXLOG_ERROR("Native draws: resolved read at {:08X}+{:X}: {}", address, size, failure);
            return guest::GuestBytes{};
        }
        return bytes;
    };
#endif
    // A family asks for a fetch's resolved texture before reading its bytes.
    guest::ReadMemory read = readBytes;
    // Shader objects a family read stay cached while guest memory holds the
    // bytes they were read from. A physical range under an unwritten resolve
    // image reads differently from guest memory, so it has no view.
    read.view            = guest::MakeResolvedGuestView([frames]() -> const guest::NativeFrameAssembler*
                                                        {
                                                 return frames->load(std::memory_order_acquire);
                                                        },
                                                        [](std::uint64_t address, std::uint64_t size, bool physical)
                                                        {
                                                 return gpu::guest_d3d::ViewGuestMemory(address, size, physical);
                                                        });
    read.shaderCache     = std::move(shaderCache);
    read.resolvedTexture = [frames](const std::array<std::uint32_t, 6>& fetch, std::uint64_t opMark, guest::NativeResolvedTextureMiss& miss) -> std::optional<guest::NativeResolvedTexture>
    {
        if (auto* assembler = frames->load(std::memory_order_acquire))
            return assembler->ResolvedTexture(fetch, opMark, miss);
        miss = guest::NativeResolvedTextureMiss::Unmodeled;
        return std::nullopt;
    };
    // native_resource_mirrors refresh dirty copies once through readBytes;
    // write and ledger/resolve notices keep the registry current.
    if (REXCVAR_GET(native_resource_mirrors))
    {
        auto*                               mirrorMemory = runtime() ? runtime()->memory() : nullptr;
        guest::GuestResourceMirrors::Config mirrorConfig;
        if (mirrorMemory)
            mirrorConfig.watch = [mirrorMemory](std::uint32_t physical, std::uint32_t length)
            {
                return gpu::WatchGuestPhysicalWrites(*mirrorMemory, physical, length);
            };
        const auto mirrors = std::make_shared<guest::GuestResourceMirrors>(std::move(mirrorConfig));
        if (mirrorMemory)
            mirrors->Hold(std::shared_ptr<void>(mirrorMemory->RegisterPhysicalMemoryInvalidationCallback(&guest::GuestResourceMirrors::WriteNotice, mirrors.get()),
                                                [mirrorMemory](void* handle)
                                                {
                                                    mirrorMemory->UnregisterPhysicalMemoryInvalidationCallback(handle);
                                                }));
        read.mirroredResource = [mirrors, readBytes](const guest::GuestResourceRead& request)
        {
            return mirrors->Bind(request, [&](std::uint32_t address, std::uint32_t size)
                                 {
                                     return readBytes(address, size, true);
                                 });
        };
        config.resourceWritten = [mirrors](std::uint64_t begin, std::uint64_t end, bool resolve)
        {
            mirrors->Written(begin, end, resolve);
        };
        guest::PublishGuestResourceMirrors(mirrors);
    }
    // A deferred draw's builds read its snapshot.
    if (REXCVAR_GET(native_draw_sequencer))
        read = guest::SnapshotReads(std::move(read));
    std::vector<guest::NativeDrawFamily> families;
    std::string                          error;
    for (const auto& family : guest::NativeDrawFamilyRegistry())
    {
        const auto source = rex::cvar::Query<std::string>(family.option);
        if (source.empty())
            continue;
        guest::NativeDrawFamily admitted;
        std::string             summary;
        if (!family.admit(std::filesystem::path(source), read, admitted, summary, error))
            return refuse(fmt::format("{} is not a usable {}: {}", family.option, family.source, error));
        REXLOG_INFO("Native draws: {} family admitted from {}{}{}", family.name, source, summary.empty() ? "" : " ", summary);
        families.push_back(std::move(admitted));
    }
    if (!families.empty())
    {
        config.build = guest::ComposeNativeDrawFamilies(std::move(families));
#if REREVVED_DEVELOPER
        // Counts each family build, including its resolved reads and decodes.
        if (nativeFrameTiming)
            config.build = [build = std::move(config.build), timing = nativeFrameTiming](const guest::NativeDrawSubmission& submission, gpu::NativeDrawReplayRecipe& draw, std::string& detail)
            {
                const auto started = std::chrono::steady_clock::now();
                // Restores the enclosing state even when the build throws.
                struct Scope
                {
                    NativeBuildReads& reads;
                    NativeBuildReads  outer;
                    ~Scope()
                    {
                        reads = outer;
                    }
                } scope{ CurrentNativeBuildReads(), std::exchange(CurrentNativeBuildReads(), NativeBuildReads{ true, false, false, submission.caller }) };
                const bool built = build(submission, draw, detail);
                const auto own   = scope.reads;
                timing->builds.fetch_add(1, std::memory_order_relaxed);
                timing->buildNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
                if (built)
                {
                    const std::size_t source = own.textureResolve ? 0 : own.textureRead ? 1
                                                                                        : 2;
                    std::lock_guard   lock(timing->versionsMutex);
                    for (const auto& texture : draw.textures)
                        if (texture.textureVersion)
                            if (auto& seen = timing->versions[texture.textureVersion.get()]; seen.expired())
                            {
                                seen = texture.textureVersion;
                                timing->freshVersions[source].fetch_add(1, std::memory_order_relaxed);
                                timing->freshBytes[source].fetch_add(texture.textureVersion->Payload()->size(), std::memory_order_relaxed);
                            }
                }
                return built;
            };
#endif
        // Proves the scene's full-target passes overwrite their uncleared targets.
        config.coverage    = guest::MakeSceneDrawCoverage(read);
        config.observeDraw = [](unsigned ordinal, const guest::NativeDrawSubmission& submission, const gpu::NativeDrawReplayRecipe& draw)
        {
            try
            {
                REXLOG_INFO("Native visibility: admitted draw={} {}", ordinal, guest::DescribeNativeDrawVisibility(submission, draw));
            }
            catch (const std::exception& exception)
            {
                REXLOG_INFO("Native visibility: admitted draw={} diagnostic_unavailable={}", ordinal, exception.what());
            }
        };
    }
#if defined(_WIN32)
    if (nativePresentation)
    {
        gpu::D3D12CompletionSourceConfig source;
        source.producer = nativePresentation;
        source.census   = NativeDrawModeFromFlags() == guest::NativeDrawMode::Census;
#if REREVVED_DEVELOPER
        source.timeEncodes = nativeFrameTiming != nullptr;
        nativePresentation->EnableProducerTiming(nativeFrameTiming != nullptr);
#endif
        source.failed = [](const std::string& detail)
        {
            REXLOG_ERROR("Native completion source failed: {}", detail);
        };
        source.observeOutput = [](const gpu::NativeDrawReplayResult& result)
        {
            bool        nonBlack = false, nonTransparent = false;
            const auto& pixels = result.resolvedOutput;
            // The gamma readback is packed little-endian R10G10B10A2.
            for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
            {
                nonBlack |= pixels[i] || pixels[i + 1] || pixels[i + 2] || (pixels[i + 3] & 0x3F);
                nonTransparent |= (pixels[i + 3] & 0xC0) != 0;
                if (nonBlack && nonTransparent)
                    break;
            }
            REXLOG_INFO("Native visibility: published frame={} gamma_pixels={} non_black={} non_transparent={}",
                        result.presenterToken,
                        pixels.size() / 4,
                        unsigned(nonBlack),
                        unsigned(nonTransparent));
        };
        nativeCompletion  = std::make_shared<gpu::D3D12CompletionSource>(std::move(source));
        config.completion = nativeCompletion.get();
        // The session created its producer's device before guest video starts.
        config.castableGuestWords = nativePresentation->ProducerCastableGuestWords();
    }
#endif
    // The SDK reports the first frontbuffer write after scanout; before swap it
    // makes form unknown. Callbacks and unregister share the global memory
    // lock, so no callback outlives its handle.
    auto* memory = runtime() ? runtime()->memory() : nullptr;
    if (memory)
    {
        config.watchWrites = [memory](std::uint32_t physical, std::uint32_t length)
        {
            memory->EnablePhysicalMemoryAccessCallbacks(physical, length, true, false);
        };
        config.watchMirror = [memory](std::uint32_t physical, std::uint32_t length)
        {
            return gpu::WatchGuestPhysicalWrites(*memory, physical, length);
        };
    }
    nativeFrames = std::make_shared<guest::NativeFrameAssembler>(std::move(config));
    frames->store(nativeFrames.get(), std::memory_order_release);
#if REREVVED_DEVELOPER && defined(_WIN32)
    if (nativeFrameTiming)
    {
        nativeFrameTiming->producer   = nativePresentation;
        nativeFrameTiming->completion = nativeCompletion;
    }
#endif
    if (memory)
    {
        nativeWriteWatch = std::shared_ptr<void>(memory->RegisterPhysicalMemoryInvalidationCallback(&guest::NativeFrameAssembler::WriteNotice, nativeFrames.get()),
                                                 [memory](void* handle)
                                                 {
                                                     memory->UnregisterPhysicalMemoryInvalidationCallback(handle);
                                                 });
    }
    gpu::completion::PublishGuestRenderAssembler(nativeFrames);
    return true;
}

std::optional<std::string> App::assembleNativeSwap(const rex::system::DetachedVideoSwap& swap)
{
    static const auto               gamma = identityGammaTable();
    gpu::guest_d3d::NativeFrameSwap frame;
#if REREVVED_DEVELOPER
    // Recorded before the drain, so a swap stalled in it is still an arrival.
    gpu::cadence::SwapArrived();
#endif
    // Every draw before the swap has its verdict before it is numbered.
    if (nativeDraws)
        nativeDraws->Drain();
    frame.sequence = ++nativeSwapSequence;
#if REREVVED_DEVELOPER
    gpu::guest_d3d::NoteDrawEffectSwap(frame.sequence);
#endif
    frame.numberedThrough     = nativeDraws ? nativeDraws->NumberedThrough() : 0;
    frame.frontbufferVirtual  = swap.frontbuffer_virtual;
    frame.frontbufferPhysical = swap.frontbuffer_physical;
    std::copy(std::begin(swap.fetch), std::end(swap.fetch), frame.fetch.begin());
    frame.textureFormat = swap.texture_format;
    frame.colorSpace    = swap.color_space;
    frame.width         = swap.width;
    frame.height        = swap.height;
    // Without native_gamma_apply the table is the assumed identity ramp; the
    // observer only counts what the guest's own table would have been.
    std::optional<gpu::guest_d3d::GammaSwapView> gammaView;
    if (gpu::guest_d3d::GammaObserveRequested())
        gammaView = gpu::guest_d3d::PublishedGammaObserver().BeginSwap(gamma);
    const auto gammaChoice = gpu::guest_d3d::ChooseSwapGamma(gpu::guest_d3d::GammaApplyRequested(), gammaView, gamma);
    frame.gammaTable       = gammaChoice.table;
    frame.gammaAssumed     = gammaChoice.assumed;
    if (nativeDraws)
        nativeDraws->CloseFrame();
    if (nativeDraws && frame.sequence % 600 == 0 && REXCVAR_GET(native_draw_sequencer))
    {
        const auto sequencer = nativeDraws->Sequencer();
        REXLOG_INFO("Native draw sequencer: VdSwap {} deferred={} drains={} drain_ms={:.1f} full_ms={:.1f} late_reads={}",
                    frame.sequence,
                    sequencer.posted,
                    sequencer.drains,
                    sequencer.drainNanoseconds / 1e6,
                    sequencer.fullNanoseconds / 1e6,
                    nativeFrames->ReadTemplates().LateReads());
    }
#if REREVVED_DEVELOPER
    std::chrono::steady_clock::time_point assemblyStart;
    if (nativeFrameTiming)
        assemblyStart = std::chrono::steady_clock::now();
#endif
    const auto result = nativeFrames->Swap(frame);
    if (gammaView && result.status != gpu::guest_d3d::NativeFrameStatus::Failed && result.status != gpu::guest_d3d::NativeFrameStatus::Stopped)
        gpu::guest_d3d::PublishedGammaObserver().EndSwap(*gammaView, result.status == gpu::guest_d3d::NativeFrameStatus::Empty);
#if REREVVED_DEVELOPER
    if (gpu::NativeCadenceEnabled())
        gpu::cadence::RecordSwapAssembled(frame.sequence, gpu::guest_d3d::NativeFrameStatusName(result.status));
    if (nativeFrameTiming)
        reportNativeFrameTiming(std::chrono::steady_clock::now() - assemblyStart);
#endif
    if (frame.sequence <= kLoggedNativeSwaps)
    {
        const auto&   coverage = result.coverage;
        std::uint64_t skipped = 0, rejected = 0;
        for (std::size_t i = 0; i < coverage.skipped.size(); ++i)
        {
            skipped += coverage.skipped[i];
            rejected += coverage.rejected[i];
        }
        REXLOG_INFO("Native frames: VdSwap {} epoch {} status={} attempted={} admitted={} skipped={} rejected={} host_n={} gamma_assumed={} detail={}",
                    frame.sequence,
                    result.epoch,
                    gpu::guest_d3d::NativeFrameStatusName(result.status),
                    coverage.attempted,
                    coverage.admitted,
                    skipped,
                    rejected,
                    result.hostFence,
                    result.assumptions.gammaAssumed,
                    result.detail.empty() ? std::string("none") : result.detail);
    }
    if (result.status != gpu::guest_d3d::NativeFrameStatus::Failed)
        return std::nullopt;
    return result.detail.empty() ? std::string("frame_failed") : result.detail;
}

#if REREVVED_DEVELOPER
void App::reportNativeFrameTiming(std::chrono::steady_clock::duration assembly)
{
    auto&           t = *nativeFrameTiming;
    std::lock_guard lock(t.mutex);
    const auto      now     = std::chrono::steady_clock::now();
    const bool      graph   = REXCVAR_GET(native_resolve_graph);
    const auto      current = [&]
    {
        NativeFrameTiming::Totals totals;
        totals.reads            = t.reads.load(std::memory_order_relaxed);
        totals.readNanoseconds  = t.readNanoseconds.load(std::memory_order_relaxed);
        totals.builds           = t.builds.load(std::memory_order_relaxed);
        totals.buildNanoseconds = t.buildNanoseconds.load(std::memory_order_relaxed);
#if defined(_WIN32)
        if (const auto producer = t.producer.lock())
        {
            totals.timing = producer->ProducerTiming();
            totals.cache  = producer->ProducerCache();
        }
        if (const auto completion = t.completion.lock())
        {
            totals.encodes = completion->GuestImageEncodes();
            if (graph)
            {
                totals.graphPeeks         = completion->GraphPeekWaits();
                totals.graphPeekRecording = completion->GraphPeekRecordingNanoseconds();
            }
        }
#endif
        if (graph && nativeFrames)
            totals.graph = nativeFrames->ResolveGraphStats();
        if (nativeFrames)
            totals.mirror = nativeFrames->ReadMirror();
        return totals;
    };
    // Calls before the first swap's baseline belong to no window.
    const auto apiCalls  = gpu::guest_d3d::TakeGuestApiCounts();
    const auto drawRoute = gpu::guest_d3d::TakeNativeDrawCensus();
    if (t.windowStart == std::chrono::steady_clock::time_point{})
    {
        t.windowStart = t.lastSwap = now;
        t.totals                   = current();
        return;
    }
    ++t.swaps;
    for (std::size_t role = 0; role < apiCalls.size(); ++role)
        t.apiCalls[role] += apiCalls[role];
    for (std::size_t hook = 0; hook < gpu::guest_d3d::kNativeDrawHookCount; ++hook)
    {
        t.drawCensus.roots[hook] += drawRoute.roots[hook];
        t.drawCensus.nested[hook] += drawRoute.nested[hook];
        t.drawCensus.unread[hook] += drawRoute.unread[hook];
        for (std::size_t flags = 0; flags < 256; ++flags)
            t.drawCensus.tile[hook][flags] += drawRoute.tile[hook][flags];
    }
    t.assembly += assembly;
    t.longestSwap = std::max(t.longestSwap, now - t.lastSwap);
    t.lastSwap    = now;
    if (now - t.windowStart < std::chrono::seconds(1))
        return;
    const auto  totals = current();
    const auto& last   = t.totals;
    const auto  ms     = [](auto duration)
    {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    const auto nsMs = [](std::uint64_t later, std::uint64_t earlier)
    {
        return double(later - earlier) / 1e6;
    };
    const double window = ms(now - t.windowStart);
    std::string  phases; // recording phases, texture outcomes, then guest D3D calls
    for (std::size_t phase = 0; phase < totals.timing.recordingPhaseNanoseconds.size(); ++phase)
        phases += fmt::format(" rec_{}_ms={:.1f}", gpu::NativeDrawReplayTimingStats::kRecordingPhases[phase], nsMs(totals.timing.recordingPhaseNanoseconds[phase], last.timing.recordingPhaseNanoseconds[phase]));
    for (std::size_t kind = 0; kind < totals.timing.drawUploadBytes.size(); ++kind)
        phases += fmt::format(" draw_upload_{}_mb={:.1f}", gpu::NativeDrawReplayTimingStats::kDrawUploadKinds[kind], double(totals.timing.drawUploadBytes[kind] - last.timing.drawUploadBytes[kind]) / 1e6);
    for (std::size_t outcome = 0; outcome < totals.timing.textureOutcomes.size(); ++outcome)
    {
        const auto name = gpu::NativeDrawReplayTimingStats::kTextureOutcomes[outcome];
        phases += fmt::format(" tex_{}={}", name, totals.timing.textureOutcomes[outcome] - last.timing.textureOutcomes[outcome]);
        if (outcome >= gpu::NativeDrawReplayTimingStats::kFirstUploadOutcome)
            phases += fmt::format(" tex_{}_mb={:.1f}", name, double(totals.timing.textureOutcomeBytes[outcome] - last.timing.textureOutcomeBytes[outcome]) / 1e6);
    }
    // Upload pool acquisitions in the window, and the bytes it holds now.
    phases += fmt::format(" upload_pool_hits={} upload_pool_new={} upload_pool_evictions={} upload_pool_idle_mb={:.1f} upload_pool_live_mb={:.1f}",
                          totals.cache.uploadPoolHits - last.cache.uploadPoolHits,
                          totals.cache.uploadPoolCreations - last.cache.uploadPoolCreations,
                          totals.cache.uploadPoolEvictions - last.cache.uploadPoolEvictions,
                          double(totals.cache.uploadPoolIdleBytes) / 1e6,
                          double(totals.cache.uploadPoolLiveBytes) / 1e6);
    // Texture, attachment and resolve pool hits and evictions in the window,
    // and the bytes they hold now; `textures` counts the CPU textures the
    // pool missed. Then the version store's, likewise.
    phases += fmt::format(" texture_pool_hits={} texture_pool_evictions={} texture_pool_idle_mb={:.1f} texture_pool_pending_mb={:.1f}"
                          " attachment_pool_hits={} attachment_pool_evictions={} attachment_pool_idle_mb={:.1f}"
                          " resolve_pool_hits={} resolve_pool_evictions={} resolve_pool_idle_mb={:.1f} resolve_pool_pending_mb={:.1f}"
                          " version_texture_hits={} version_texture_uploads={} version_texture_evictions={} version_texture_mb={:.1f}",
                          totals.cache.texturePoolHits - last.cache.texturePoolHits,
                          totals.cache.texturePoolEvictions - last.cache.texturePoolEvictions,
                          double(totals.cache.texturePoolIdleBytes) / 1e6,
                          double(totals.cache.texturePoolPendingBytes) / 1e6,
                          totals.cache.attachmentPoolHits - last.cache.attachmentPoolHits,
                          totals.cache.attachmentPoolEvictions - last.cache.attachmentPoolEvictions,
                          double(totals.cache.attachmentPoolIdleBytes) / 1e6,
                          totals.cache.resolvePoolHits - last.cache.resolvePoolHits,
                          totals.cache.resolvePoolEvictions - last.cache.resolvePoolEvictions,
                          double(totals.cache.resolvePoolIdleBytes) / 1e6,
                          double(totals.cache.resolvePoolPendingBytes) / 1e6,
                          totals.cache.versionTextureHits - last.cache.versionTextureHits,
                          totals.cache.versionTextureUploads - last.cache.versionTextureUploads,
                          totals.cache.versionTextureEvictions - last.cache.versionTextureEvictions,
                          double(totals.cache.versionTextureBytes) / 1e6);
    // Draw stream callers that blocked before the stream took their request,
    // and on a synchronous request's result; image reads wait in peek_*.
    phases += fmt::format(" stream_queue_waits={} stream_queue_wait_ms={:.1f} stream_future_waits={} stream_future_wait_ms={:.1f} stream_busy_ms={:.1f} draw_upload_resident_vertex_mb={:.1f} draw_upload_resident_index_mb={:.1f}"
                          " constant_repeat_prev_vertex_mb={:.1f} constant_repeat_prev_pixel_mb={:.1f} constant_repeat_prev_shared_mb={:.1f}"
                          " constant_repeat_chunk_vertex_mb={:.1f} constant_repeat_chunk_pixel_mb={:.1f} constant_repeat_chunk_shared_mb={:.1f}"
                          " texture_descriptor_reuses={} texture_srv_writes={} rtv_writes={}",
                          totals.timing.streamQueueWaits - last.timing.streamQueueWaits,
                          nsMs(totals.timing.streamQueueWaitNanoseconds, last.timing.streamQueueWaitNanoseconds),
                          totals.timing.streamFutureWaits - last.timing.streamFutureWaits,
                          nsMs(totals.timing.streamFutureWaitNanoseconds, last.timing.streamFutureWaitNanoseconds),
                          nsMs(totals.timing.streamBusyNanoseconds, last.timing.streamBusyNanoseconds),
                          double(totals.timing.residentGeometryBytes[0] - last.timing.residentGeometryBytes[0]) / 1e6,
                          double(totals.timing.residentGeometryBytes[1] - last.timing.residentGeometryBytes[1]) / 1e6,
                          double(totals.timing.constantRepeatPreviousBytes[0] - last.timing.constantRepeatPreviousBytes[0]) / 1e6,
                          double(totals.timing.constantRepeatPreviousBytes[1] - last.timing.constantRepeatPreviousBytes[1]) / 1e6,
                          double(totals.timing.constantRepeatPreviousBytes[2] - last.timing.constantRepeatPreviousBytes[2]) / 1e6,
                          double(totals.timing.constantRepeatChunkBytes[0] - last.timing.constantRepeatChunkBytes[0]) / 1e6,
                          double(totals.timing.constantRepeatChunkBytes[1] - last.timing.constantRepeatChunkBytes[1]) / 1e6,
                          double(totals.timing.constantRepeatChunkBytes[2] - last.timing.constantRepeatChunkBytes[2]) / 1e6,
                          totals.timing.textureDescriptorReuses - last.timing.textureDescriptorReuses,
                          totals.timing.textureSrvWrites - last.timing.textureSrvWrites,
                          totals.timing.rtvWrites - last.timing.rtvWrites);
    // Submissions that ended a frame command batch at its draw, sampler or
    // command bound, and frame sampler tables written, reused, and the batches
    // a full sampler heap ended.
    phases += fmt::format(" chunk_submissions={} sampler_tables={} sampler_table_hits={} sampler_table_flushes={}",
                          totals.timing.chunkSubmissions - last.timing.chunkSubmissions,
                          totals.cache.samplerTables - last.cache.samplerTables,
                          totals.cache.samplerTableHits - last.cache.samplerTableHits,
                          totals.cache.samplerTableFlushes - last.cache.samplerTableFlushes);
    // Current and peak bytes charged to the ownership limit, followed by this
    // window's attachment-pool misses and evicted bytes.
    phases += fmt::format(" owned_mb={:.1f} owned_peak_mb={:.1f} attachment_pool_misses={} attachment_pool_evicted_mb={:.1f}",
                          double(totals.cache.ownedChargeBytes) / 1e6,
                          double(totals.cache.ownedChargePeakBytes) / 1e6,
                          totals.cache.attachmentPoolMisses - last.cache.attachmentPoolMisses,
                          double(totals.cache.attachmentPoolEvictedBytes - last.cache.attachmentPoolEvictedBytes) / 1e6);
    phases += fmt::format(" attachment_creates={} attachment_create_ms={:.1f}", totals.timing.attachmentCreations - last.timing.attachmentCreations, nsMs(totals.timing.attachmentCreateNanoseconds, last.timing.attachmentCreateNanoseconds));
    // Native vertex residency, appended: window counts and bytes, then the
    // gauges and operational events (resident_bytes is the fixed chunk footprint).
    phases += fmt::format(" resident_hits={} resident_bytes_avoided={} resident_misses={} resident_copy_bytes={} resident_fallbacks={}",
                          totals.timing.residentHits - last.timing.residentHits,
                          totals.timing.residentBytesAvoided - last.timing.residentBytesAvoided,
                          totals.timing.residentMisses - last.timing.residentMisses,
                          totals.timing.residentCopyBytes - last.timing.residentCopyBytes,
                          totals.timing.residentFallbacks - last.timing.residentFallbacks);
    for (std::size_t reason = 0; reason < totals.timing.residentFallbackReasons.size(); ++reason)
        phases += fmt::format(" resident_fallback_{}={}", gpu::NativeDrawReplayTimingStats::kResidentFallbackReasons[reason], totals.timing.residentFallbackReasons[reason] - last.timing.residentFallbackReasons[reason]);
    phases += fmt::format(" resident_bytes={} resident_entries={} resident_payload_bytes={} resident_retired_bytes={} resident_evictions={} resident_expiries={} resident_clears={} resident_upload_failures={} resident_init_failures={}",
                          totals.timing.residentBytes,
                          totals.timing.residentEntries,
                          totals.timing.residentPayloadBytes,
                          totals.timing.residentRetiredBytes,
                          totals.timing.residentEvictions - last.timing.residentEvictions,
                          totals.timing.residentExpiries - last.timing.residentExpiries,
                          totals.timing.residentClears - last.timing.residentClears,
                          totals.timing.residentUploadFailures - last.timing.residentUploadFailures,
                          totals.timing.residentInitFailures - last.timing.residentInitFailures);
    phases += gpu::guest_d3d::FormatGuestApiCounts(t.apiCalls);
    phases += gpu::guest_d3d::FormatNativeDrawCensus(t.drawCensus);
    const auto vsIdentity = gpu::guest_d3d::TakeVertexShaderIdentityCounts();
    phases += fmt::format(" vs_identity_hits={} vs_identity_misses={} vs_fetch_patches={} vs_identity_bytes={}",
                          vsIdentity.hits,
                          vsIdentity.misses,
                          vsIdentity.patches,
                          vsIdentity.guestBytes);
    if (gpu::guest_d3d::IndexedBufferConsumeRequested())
        phases += gpu::guest_d3d::FormatIndexedBufferConsumeCounts(gpu::guest_d3d::TakeIndexedBufferConsumeCounts());
    if (gpu::guest_d3d::UserPointerConsumeRequested())
        phases += gpu::guest_d3d::FormatUserPointerConsumeCounts(gpu::guest_d3d::TakeUserPointerConsumeCounts());
    if (gpu::guest_d3d::VsCodeWatchRequested())
        phases += gpu::guest_d3d::FormatVsCodeWatchCounts(gpu::guest_d3d::TakeVsCodeWatchCounts());
    if (gpu::guest_d3d::LowererConsumeRequested())
        phases += gpu::guest_d3d::FormatLowererConsumeCounts(gpu::guest_d3d::TakeLowererConsumeCounts());
    if (gpu::guest_d3d::LowererExitCheckRequested())
        phases += gpu::guest_d3d::FormatLowererExitCounts(gpu::guest_d3d::TakeLowererExitCounts());
    if (gpu::guest_d3d::WindowWriterConsumeRequested())
        phases += gpu::guest_d3d::FormatWindowWriterConsumeCounts(gpu::guest_d3d::TakeWindowWriterConsumeCounts());
    if (gpu::guest_d3d::WindowWriterExitCheckRequested())
        phases += gpu::guest_d3d::FormatWindowWriterExitCounts(gpu::guest_d3d::TakeWindowWriterExitCounts());
    if (gpu::guest_d3d::ResolveConsumeRequested())
        phases += gpu::guest_d3d::FormatResolveConsumeCounts(gpu::guest_d3d::TakeResolveConsumeCounts());
    if (gpu::guest_d3d::ClearConsumeRequested())
        phases += gpu::guest_d3d::FormatClearConsumeCounts(gpu::guest_d3d::TakeClearConsumeCounts());
    if (gpu::guest_d3d::SetPredicationConsumeRequested())
        phases += gpu::guest_d3d::FormatSetPredicationConsumeCounts(gpu::guest_d3d::TakeSetPredicationConsumeCounts());
    if (gpu::guest_d3d::SetPredicationExitCheckRequested())
        phases += gpu::guest_d3d::FormatSetPredicationExitCounts(gpu::guest_d3d::TakeSetPredicationExitCounts());
    if (gpu::guest_d3d::SetPredicationPreSwapReportingRequested())
        phases += gpu::guest_d3d::FormatSetPredicationPreSwapCounts(gpu::guest_d3d::TakeSetPredicationPreSwapCounts());
    if (gpu::guest_d3d::GammaObserveRequested())
        phases += gpu::guest_d3d::FormatGammaCounts(gpu::guest_d3d::PublishedGammaObserver().Take());
    if (gpu::guest_d3d::TilingProbeRequested())
        phases += gpu::guest_d3d::FormatTilingProbeCounts(gpu::guest_d3d::PublishedTilingProbe().Take());
    if (gpu::guest_d3d::CompletionProbeRequested())
        phases += gpu::guest_d3d::ReportCompletionProbe();
    if (gpu::guest_d3d::OriginalResidueProbeRequested())
        gpu::guest_d3d::ReportOriginalResidueProbe();
    if (gpu::guest_d3d::TilingRestoreReportingRequested())
        phases += gpu::guest_d3d::ReportTilingRestore();
    if (gpu::guest_d3d::VsReadbackProbeRequested())
        phases += gpu::guest_d3d::ReportVsReadbackProbe();
    if (gpu::guest_d3d::ResolveDeriveCheckRequested())
        phases += gpu::guest_d3d::FormatResolveDeriveCounts(gpu::guest_d3d::TakeResolveDeriveCounts());
    if (gpu::guest_d3d::ResolveEffectCheckRequested())
        phases += gpu::guest_d3d::FormatResolveEffectCounts(gpu::guest_d3d::TakeResolveEffectCounts());
    if (gpu::guest_d3d::ClearDeriveCheckRequested())
        phases += gpu::guest_d3d::FormatClearDeriveCounts(gpu::guest_d3d::TakeClearDeriveCounts());
    if (gpu::guest_d3d::ClearEffectCheckRequested())
    {
        phases += gpu::guest_d3d::FormatClearEffectCounts(gpu::guest_d3d::TakeClearEffectCounts());
        phases += gpu::guest_d3d::FormatClearExitCounts(gpu::guest_d3d::TakeClearExitCounts());
    }
    if (const auto mirrors = gpu::guest_d3d::PublishedGuestResourceMirrors())
        phases += gpu::guest_d3d::FormatGuestResourceMirrorCounts(mirrors->TakeCounts());
    // Ring stub calls since the previous timing line, with native_ring_stubs.
    phases += gpu::guest_d3d::FormatRingStubCounts(gpu::guest_d3d::TakeRingStubCounts());
#if REREVVED_DEVELOPER
    // Faults, entry checks and waits of native_ring_consume_publish; terminal outcomes are the or_* fields.
    if (gpu::guest_d3d::RingConsumeCvar())
        phases += gpu::guest_d3d::FormatRingConsumeCounts(gpu::guest_d3d::TakeRingConsumeCounts());
    if (gpu::guest_d3d::VideoScalerStubReportingRequested())
        phases += gpu::guest_d3d::ReportVideoScalerStub();
    if (gpu::guest_d3d::VideoDisplayModeStubReportingRequested())
        phases += gpu::guest_d3d::ReportVideoDisplayModeStub();
    if (gpu::guest_d3d::VideoImportStubsReportingRequested())
        phases += gpu::guest_d3d::ReportVideoImportStubs();
#endif
    // The bracket list shape the tiled consume observes at EndTiling, next to the tiling counters.
    if (gpu::guest_d3d::TileListObservationRequested())
        phases += gpu::guest_d3d::FormatTileListCounts(gpu::guest_d3d::TakeTileListCounts());
    // The observer bit first, so without the registry no publication lock is taken.
    if (gpu::guest_d3d::guestResourceObservers.load(std::memory_order_relaxed) & gpu::guest_d3d::kGuestResolveSurfaces)
        if (const auto surfaces = gpu::guest_d3d::PublishedResolveSurfaces())
        {
            phases += gpu::guest_d3d::FormatResolveSurfaceCounts(surfaces->TakeCounts());
            phases += fmt::format(" surface_conversions={} surface_conversion_ms={:.1f}", totals.timing.surfaceConversions - last.timing.surfaceConversions, nsMs(totals.timing.surfaceConversionNanoseconds, last.timing.surfaceConversionNanoseconds));
            // Each distinct resolve and fetch shape pair, once, the window it first missed by form.
            for (const auto& pair : surfaces->TakeNewFormPairs())
                REXLOG_INFO("Native surface form miss: {}", pair.Describe(gpu::guest_d3d::kNativeAliasFormNames));
            // Each pixel shader that first bound a converted (base-level) depth texture.
            for (const auto& [shader, caller] : surfaces->TakeNewDepthConsumers())
                REXLOG_INFO("Native surface k_24_8 view bound by ps {:016X} from {:08X}", shader, caller);
        }
    REXLOG_INFO("Native frame timing: window_ms={:.0f} swaps={} fps={:.1f} longest_swap_ms={:.1f} swap_assembly_ms={:.1f} resolved_reads={} resolved_read_ms={:.1f} "
                "encodes={} encode_ms={:.1f} submissions={} recorded_draws={} recording_ms={:.1f} fence_wait_ms={:.1f} texture_upload_ms={:.1f} "
                "draw_upload_ms={:.1f} pipelines={} pipeline_ms={:.1f} textures={} texture_uploads={} texture_matches={} texture_prepare_ms={:.1f} "
                "draw_builds={} draw_build_ms={:.1f}{}",
                window,
                t.swaps,
                double(t.swaps) * 1000.0 / window,
                ms(t.longestSwap),
                ms(t.assembly),
                totals.reads - last.reads,
                nsMs(totals.readNanoseconds, last.readNanoseconds),
                totals.encodes.first - last.encodes.first,
                nsMs(totals.encodes.second, last.encodes.second),
                totals.timing.queueSubmissions - last.timing.queueSubmissions,
                totals.timing.recordedDraws - last.timing.recordedDraws,
                nsMs(totals.timing.recordingNanoseconds, last.timing.recordingNanoseconds),
                nsMs(totals.timing.fenceWaitNanoseconds, last.timing.fenceWaitNanoseconds),
                nsMs(totals.timing.textureUploadNanoseconds, last.timing.textureUploadNanoseconds),
                nsMs(totals.timing.drawUploadNanoseconds, last.timing.drawUploadNanoseconds),
                totals.cache.pipelineCreations - last.cache.pipelineCreations,
                nsMs(totals.timing.pipelineNanoseconds, last.timing.pipelineNanoseconds),
                totals.cache.textureCreations - last.cache.textureCreations,
                totals.cache.textureUploadCreations - last.cache.textureUploadCreations,
                totals.cache.textureContentReuses - last.cache.textureContentReuses,
                nsMs(totals.timing.texturePrepareNanoseconds, last.timing.texturePrepareNanoseconds),
                totals.builds - last.builds,
                nsMs(totals.buildNanoseconds, last.buildNanoseconds),
                phases);
    // Attachment pool misses since the renderer was created, by take site and
    // by shape, after a window that missed.
    if (totals.cache.attachmentPoolMisses != last.cache.attachmentPoolMisses)
    {
        const auto& cache  = totals.cache;
        auto        missed = fmt::format("frames={} untracked={}", cache.attachmentMissFrames, cache.attachmentMissUntracked);
        for (std::size_t site = 0; site < cache.attachmentSiteMisses.size(); ++site)
            missed += fmt::format(" site_{}={}", gpu::NativeDrawReplayCacheStats::kAttachmentTakeSites[site], cache.attachmentSiteMisses[site]);
        for (const auto& shape : cache.attachmentMissShapes)
            if (shape.misses)
            {
                missed += fmt::format(" miss_{}x{}x{}_f{}_g{:x}{}={}", shape.width, shape.height, shape.samples, shape.format, shape.flags, shape.castable ? "_c" : "", shape.misses);
                for (std::size_t gap = 0; gap < shape.gaps.size(); ++gap)
                    missed += fmt::format("{}{}", gap ? '/' : ':', shape.gaps[gap]);
            }
        REXLOG_INFO("Native attachment misses: {}", missed);
    }
    {
        std::string sites;
        for (std::size_t site = 0; site < t.readSites.size(); ++site)
        {
            auto&      counted = t.readSites[site];
            const auto reads   = counted.reads.exchange(0, std::memory_order_relaxed);
            const auto name    = site < gpu::guest_d3d::kGuestReadSiteNames.size() ? gpu::guest_d3d::kGuestReadSiteNames[site] : "outside";
            const auto ns      = counted.nanoseconds.exchange(0, std::memory_order_relaxed);
            const auto lockNs  = counted.lockNanoseconds.exchange(0, std::memory_order_relaxed);
            const auto viewNs  = counted.viewNanoseconds.exchange(0, std::memory_order_relaxed);
            const auto bytes   = counted.bytes.exchange(0, std::memory_order_relaxed);
            const auto images  = counted.images.exchange(0, std::memory_order_relaxed);
            const auto flushes = counted.flushes.exchange(0, std::memory_order_relaxed);
            const auto entries = counted.surfaces.exchange(0, std::memory_order_relaxed);
            if (reads)
                sites += fmt::format(" read_{0}={1} read_{0}_ms={2:.1f} read_{0}_lock_ms={3:.1f} read_{0}_view_ms={4:.1f} read_{0}_mb={5:.1f} read_{0}_images={6} read_{0}_flushes={7} read_{0}_surfaces={8}",
                                     name,
                                     reads,
                                     double(ns) / 1e6,
                                     double(lockNs) / 1e6,
                                     double(viewNs) / 1e6,
                                     double(bytes) / 1e6,
                                     images,
                                     flushes,
                                     entries);
        }
        constexpr std::array<const char*, 3> kSources{ "resolve", "read", "mirror" };
        for (std::size_t source = 0; source < kSources.size(); ++source)
            sites += fmt::format(" tex_fresh_{0}={1} tex_fresh_{0}_mb={2:.1f}", kSources[source], t.freshVersions[source].exchange(0, std::memory_order_relaxed), double(t.freshBytes[source].exchange(0, std::memory_order_relaxed)) / 1e6);
        {
            std::lock_guard versions(t.versionsMutex);
            std::erase_if(t.versions, [](const auto& entry)
                          {
                              return entry.second.expired();
                          });
        }
        REXLOG_INFO("Native frame reads: window_ms={:.0f} swaps={}{}", window, t.swaps, sites);
    }
    if (graph)
    {
        auto aliasMisses = std::format(" castable={} alias_hits={} alias_reproduced={}", unsigned(totals.graph.castable), totals.graph.aliasHits - last.graph.aliasHits, totals.graph.aliasReproduced - last.graph.aliasReproduced);
        for (std::size_t which = 0; which < gpu::guest_d3d::kNativeAliasMissNames.size(); ++which)
            aliasMisses += std::format(" alias_miss_{}={}", gpu::guest_d3d::kNativeAliasMissNames[which], totals.graph.aliasMisses[which] - last.graph.aliasMisses[which]);
        for (std::size_t which = 0; which < gpu::guest_d3d::kNativeAliasFormNames.size(); ++which)
            aliasMisses += std::format(" alias_form_{}={}", gpu::guest_d3d::kNativeAliasFormNames[which], totals.graph.aliasForms[which] - last.graph.aliasForms[which]);
        REXLOG_INFO("Native resolve graph: continuations={} replays={} fallback_not_ready={} fallback_rg16snorm={} "
                    "fallback_no_source={} fallback_swap_batch={} peek_waits={} peek_wait_ms={:.1f} peek_recording_ms={:.1f}{}",
                    totals.graph.continuations - last.graph.continuations,
                    totals.graph.replays - last.graph.replays,
                    totals.graph.notReady - last.graph.notReady,
                    totals.graph.signed16 - last.graph.signed16,
                    totals.graph.noSource - last.graph.noSource,
                    totals.graph.swapBatch - last.graph.swapBatch,
                    totals.graphPeeks.first - last.graphPeeks.first,
                    nsMs(totals.graphPeeks.second, last.graphPeeks.second),
                    nsMs(totals.graphPeekRecording, last.graphPeekRecording),
                    aliasMisses);
    }
    const auto plain = [&](std::size_t slot)
    {
        return std::pair{ totals.mirror.smallReads[slot] - last.mirror.smallReads[slot], double(totals.mirror.smallBytes[slot] - last.mirror.smallBytes[slot]) / 1e6 };
    };
    REXLOG_INFO("Native read mirror: hits={} hit_mb={:.1f} watched={} watched_mb={:.1f} misses={} miss_mb={:.1f} small_4k={} small_4k_mb={:.1f} "
                "small_64k={} small_64k_mb={:.1f} small_256k={} small_256k_mb={:.1f} small_1m={} small_1m_mb={:.1f}",
                totals.mirror.hits - last.mirror.hits,
                double(totals.mirror.hitBytes - last.mirror.hitBytes) / 1e6,
                totals.mirror.watchedHits - last.mirror.watchedHits,
                double(totals.mirror.watchedHitBytes - last.mirror.watchedHitBytes) / 1e6,
                totals.mirror.misses - last.mirror.misses,
                double(totals.mirror.missBytes - last.mirror.missBytes) / 1e6,
                plain(0).first,
                plain(0).second,
                plain(1).first,
                plain(1).second,
                plain(2).first,
                plain(2).second,
                plain(3).first,
                plain(3).second);
    t.windowStart = now;
    t.swaps       = 0;
    t.apiCalls    = {};
    t.drawCensus  = {};
    t.assembly = t.longestSwap = {};
    t.totals                   = totals;
}
#endif

// Every guest draw feeds the assembler, then the ledger: the assembler records
// a draw before the ledger can ask to flush it.
bool App::installNativeDraws()
{
    auto ledger = guestVideo->Ledger();
    if (!ledger || !nativeFrames)
        return false;
    namespace guest                                     = gpu::guest_d3d;
    auto*                                        frames = nativeFrames.get();
    const std::weak_ptr<gpu::GuestVideoServices> video  = guestVideo;

    // First rejected submissions per ring generation, logged on the drawing thread.
    struct Rejections
    {
        std::mutex    mutex;
        std::uint32_t ring = 0, logged = 0;
    };

    auto rejections = std::make_shared<Rejections>();
    // native_surface_registry: resolve images may wait on the host for a guest
    // lock, then land through the ledger's image-write gate.
    frames->SetGuestImageWrites([weak = std::weak_ptr<gpu::completion::CompletionLedger>(ledger)](const std::function<void(const gpu::completion::GuestImageWriter&)>& body)
                                {
                                    const auto held = weak.lock();
                                    return held && held->WithImageWrites(body);
                                });
    guest::NativeDrawDispatchConfig dispatch;
    dispatch.numberOperation = [frames]
    {
        return frames->NumberOperation();
    };
    dispatch.mode  = NativeDrawModeFromFlags();
    dispatch.admit = [frames](const guest::NativeDrawSubmission& submission)
    {
        return frames->Admit(submission);
    };
    // Known identities build from entry snapshots with detached ledger records;
    // unknown draws establish read templates here.
    const bool sequenced = REXCVAR_GET(native_draw_sequencer);

    struct Deferred
    {
        guest::GuestReadSnapshot                                       snapshot;
        std::optional<gpu::completion::CompletionLedger::DetachedDraw> record;
    };

    const auto deferredOf = [](const guest::NativeDrawSubmission& submission)
    {
        return static_cast<const Deferred*>(submission.deferral.get());
    };
    dispatch.sequenced = sequenced;
    if (sequenced)
    {
        dispatch.defer = [frames, ledger](const guest::NativeDrawSubmission& submission) -> std::shared_ptr<const void>
        {
            auto       deferred = std::make_shared<Deferred>();
            const auto view     = [](std::uint64_t address, std::uint64_t size, bool physical)
            {
                return gpu::guest_d3d::ViewGuestMemory(address, size, physical);
            };
            if (!frames->ReadTemplates().Capture(submission, view, deferred->snapshot))
                return nullptr;
            ledger->BeginDraw(submission.hook, submission.caller);
            deferred->record = ledger->DetachDraw();
            return deferred;
        };
        dispatch.abandoned = [ledger, deferredOf](const guest::NativeDrawSubmission& submission)
        {
            if (const auto* deferred = deferredOf(submission); deferred && deferred->record)
                ledger->AbandonDraw(*deferred->record);
        };
    }
    // A held direct lowerer root opens its ledger draw at its commit, where
    // 0x826A2FF0 publishes its PM4: a failed lowering opens none, and one never
    // committed is recorded where its refusal arrives (FinishDraw without a begin).
    dispatch.began = [frames, ledger, sequenced, deferredOf](const guest::NativeDrawSubmission& submission)
    {
        const auto* deferred = deferredOf(submission);
        if (!sequenced)
            frames->Began(submission);
        else
            frames->BeganSequenced(submission, deferred ? &deferred->snapshot : nullptr);
        if (!deferred && !guest::NativeDrawDefersToCommit(submission.hook, submission.caller))
            ledger->BeginDraw(submission.hook, submission.caller);
    };
    dispatch.committed = [frames, ledger](const guest::NativeDrawSubmission& submission)
    {
        frames->Committed(submission);
        ledger->BeginDraw(submission.hook, submission.caller);
    };
    dispatch.recorded = [frames, ledger, video, rejections, deferredOf](const guest::NativeDrawSubmission& submission, std::optional<guest::NativeDrawReason> verdict)
    {
        std::string refusal;
        const auto  mark = frames->Recorded(submission, verdict, &refusal);
        if (const auto* deferred = deferredOf(submission); deferred && deferred->record)
            ledger->FinishDraw(*deferred->record, mark, submission.hook, submission.caller, verdict, refusal);
        else
            ledger->FinishDraw(mark, submission.hook, submission.caller, verdict, refusal);
        if (!verdict)
            return;
        {
            std::lock_guard lock(rejections->mutex);
            const auto      service = video.lock();
            const auto      ring    = service ? service->RingGeneration() : 0;
            if (ring != rejections->ring)
            {
                rejections->ring   = ring;
                rejections->logged = 0;
            }
            if (rejections->logged >= kLoggedRejections)
                return;
            ++rejections->logged;
        }
        REXLOG_WARN("Native draws: rejected {}: {} refusal={}",
                    guest::NativeDrawReasonName(*verdict),
                    guest::DescribeNativeDrawSubmission(submission, gpu::guest_d3d::ReadGuestMemory),
                    refusal.empty() ? std::string("none") : refusal);
    };
    // The ledger reports the session failure, naming the draw's hook and caller.
    dispatch.failed = [](const gpu::guest_d3d::NativeDrawFailure&)
    {
    };
    nativeDraws = std::make_unique<gpu::guest_d3d::NativeDrawDispatcher>(std::move(dispatch));
    if (!gpu::guest_d3d::InstallNativeDrawDispatcher(*nativeDraws))
    {
        REXLOG_ERROR("Native draw dispatcher could not be installed: another dispatcher is active");
        nativeDraws.reset();
        return false;
    }
    return true;
}

bool App::removeNativeDraws()
{
    if (!nativeDraws)
        return true;
    if (gpu::guest_d3d::RemoveNativeDrawDispatcher(gpu::kVblankJoinTimeout))
    {
        nativeDraws.reset();
        return true;
    }
    // A hook may still hold it; the dispatcher must outlive that hook.
    REXLOG_ERROR("Native draw dispatcher withdrawal timed out; it is retained until process exit");
    (void)nativeDraws.release();
    return false;
}

void App::closeNativeCompletion()
{
    if (nativeCompletion)
        nativeCompletion->Close();
}

// Draw hooks, the ledger and the VdSwap delegate reach the assembler and its
// source; without a proved withdrawal both are retained until process exit.
void App::releaseNativeFrames(bool destroy)
{
    if (!destroy)
    {
        if (nativeWriteWatch)
            (void)new std::shared_ptr<void>(nativeWriteWatch);
        if (nativeFrames)
            (void)new std::shared_ptr<gpu::guest_d3d::NativeFrameAssembler>(nativeFrames);
        if (nativeCompletion)
            (void)new std::shared_ptr<gpu::D3D12CompletionSource>(nativeCompletion);
    }
    nativeWriteWatch.reset();
    nativeFrames.reset();
    nativeCompletion.reset();
}

void App::hardExitAfterMissedVblankJoin()
{
    // Runtime teardown would free state a running vblank thread still uses, so
    // exit the way the SDK exits after closing a title.
#if defined(_WIN32)
    if (nativePresentation)
        nativePresentation->FlushTrace();
#if REREVVED_DEVELOPER
    {
        std::string cadenceError;
        if (!gpu::CloseNativeCadenceTrace(cadenceError))
            REXLOG_ERROR("Native cadence trace close failed: {}", cadenceError);
    }
    {
        std::string effectError;
        if (!gpu::guest_d3d::CloseDrawEffectProbe(effectError))
            REXLOG_ERROR("Native draw effect probe close failed: {}", effectError);
    }
#endif
#endif
    REXLOG_ERROR("Native vblank thread missed its join; hard-exiting before Runtime teardown");
    // The SDK's Discord thread must be joined before rexruntime.dll detaches.
    rerevved::StopPresence();
    rex::FlushLogging();
    std::_Exit(0);
}

void App::stopGuestVideoProducers()
{
    if (!guestVideo)
        return;
    const auto timeout = nativeDrain ? gpu::VblankJoinTimeout(std::chrono::steady_clock::now(), nativeDrain->deadline.Expiry()) : gpu::kVblankJoinTimeout;
    const bool stopped = guestVideo->StopProducers(timeout);
#if defined(_WIN32)
    if (nativePresentation)
        nativePresentation->Event("guest_video_stopped", 0, stopped ? 1 : 0);
#else
    (void)stopped;
#endif
}

void App::nativePresentationTick(gpu::PresentResult result)
{
#if defined(_WIN32)
    if (nativeClosing)
        return;
    using gpu::PresentOutcome;
    if (result.outcome == PresentOutcome::Failed || result.outcome == PresentOutcome::DeviceLost || result.outcome == PresentOutcome::Timeout)
    {
        REXLOG_ERROR("Native presentation failed: operation={} result={} code={}", result.operation, static_cast<unsigned>(result.outcome), result.nativeCode);
        if (!NativeScriptActive())
            rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, "Native presentation failed. See the log for details.");
        if (OnWindowCloseRequested())
            window()->RequestClose();
        return;
    }
#if REREVVED_DEVELOPER
    if (!REXCVAR_GET(native_present_script).empty())
    {
        if (result.newFrame)
            ++nativeScriptFrames;
        if (nativeScriptFrames == 1 && nativeScriptStage == 0)
        {
            nativeScriptStage = 1;
            nativePresentation->Event("script_resize");
            SetWindowPos(static_cast<HWND>(window()->GetNativeWindowHandle()), nullptr, 0, 0, 960, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            requestNativeExtent();
            OnWindowFocusChanged(true);
        }
        else if (nativeScriptFrames == 2 && nativeScriptStage == 1)
        {
            nativeScriptStage = 2;
            // Inject production handlers; keep the OS window hidden throughout.
            nativePresentation->Event("script_minimize");
            OnWindowMinimized();
        }
        else if (nativeScriptStage == 2 && result.outcome == PresentOutcome::Suspended)
        {
            nativeScriptStage = 3;
            nativePresentation->Event("script_restore");
            OnWindowRestored();
        }
        else if (nativeScriptFrames >= 4 && nativeScriptStage == 3)
        {
            nativeScriptStage = 4;
            nativePresentation->Event("script_overlay_open");
            rex::ui::KeyEvent key(window(), SettingsOverlayKey(), 1, false, false, false, false, false);
            OnKeyDown(key);
        }
        else if (nativeScriptStage == 4 && result.overlayDraws)
        {
            nativeScriptStage = 6;
            // Inject SDK listener events on the UI thread; no Runtime participates.
            auto*             input = static_cast<rex::ui::WindowInputListener*>(imgui_drawer());
            rex::ui::KeyEvent key(window(), rex::ui::VirtualKey::kA, 1, false, false, false, false, false);
            input->OnKeyChar(key);
            const auto          wheel = imgui_drawer()->GetIO().MouseWheel;
            rex::ui::MouseEvent mouse(window(), rex::ui::MouseEvent::Button::kNone, 100, 100, 0, rex::ui::MouseEvent::kScrollPerDetent);
            input->OnMouseWheel(mouse);
            nativePresentation->Event("script_overlay_input", 0, (key.is_handled() ? 1 : 0) | (imgui_drawer()->GetIO().MouseWheel != wheel ? 2 : 0));
        }
        else if (nativeScriptStage == 6 && result.overlayDraws)
        {
            nativeScriptStage = 5;
            nativePresentation->Event("overlay_input_capture", 0, IsOverlayInputCaptured() ? 1 : 0);
            nativePresentation->Event("script_overlay_close");
            rex::ui::KeyEvent key(window(), SettingsOverlayKey(), 1, false, false, false, false, false);
            OnKeyDown(key);
        }
        else if (nativeScriptStage == 5 && result.overlaySequence && !result.overlayDraws && ++nativeScriptClearFrames == 3)
        {
            nativePresentation->Event("script_close");
            const auto handle = static_cast<HWND>(window()->GetNativeWindowHandle());
            PostMessageW(handle, WM_CLOSE, 0, 0);
            PostMessageW(handle, WM_CLOSE, 0, 0);
            return;
        }
    }
#endif
    if (auto* imgui = imgui_drawer())
    {
        auto*          drawer     = static_cast<gpu::NativeImmediateDrawer*>(immediate_drawer());
        const auto     previous   = nativeOverlays->Latest();
        const bool     hadOverlay = previous && !previous->batches.empty();
        const uint32_t width = window()->GetActualPhysicalWidth(), height = window()->GetActualPhysicalHeight();
        if (imgui->HasDialogs() && width && height && nativeOverlays->Ready())
        {
            rex::ui::AppUIDrawContext context(width, height);
            imgui->Draw(context);
            nativePresentation->RequestRepaint();
        }
        else if (hadOverlay)
        {
            drawer->Clear();
            nativePresentation->RequestRepaint();
        }
        if (nativeOverlays->Failed())
        {
            REXLOG_ERROR("Native overlay drawer failed");
            nativePresentation->Event("overlay_failed");
            if (!NativeScriptActive())
                rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, "Native presentation failed. See the log for details.");
            if (OnWindowCloseRequested())
                window()->RequestClose();
            return;
        }
    }
    nativePresentation->Advance();
#endif
}

void App::OnWindowMinimized()
{
#if defined(_WIN32)
    if (nativePresentation && !nativeClosing)
        nativePresentation->SetMinimized(true);
#endif
}

void App::OnWindowRestored()
{
#if defined(_WIN32)
    if (nativePresentation && !nativeClosing)
    {
        nativePresentation->SetMinimized(false);
        requestNativeExtent();
        nativePresentation->RequestRepaint();
    }
#endif
}

void App::OnDpiScaleChanged(float scale)
{
    (void)scale;
    requestNativeExtent();
}

void App::requestNativeExtent()
{
    if (rendererBackend == gpu::RendererBackend::Native && window())
        OnWindowPixelSizeChanged(window()->GetActualPhysicalWidth(), window()->GetActualPhysicalHeight());
}

void App::OnBeforeHardExit()
{
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::ReportCompletionProbe(true);
    rerevved::gpu::guest_d3d::ReportTilingRestore(true);
    rerevved::gpu::guest_d3d::ReportVsReadbackProbe(true);
    rerevved::gpu::guest_d3d::ReportVideoScalerStub(true);
    rerevved::gpu::guest_d3d::ReportVideoDisplayModeStub(true);
    rerevved::gpu::guest_d3d::ReportVideoImportStubs(true);
#endif
#if defined(_WIN32)
    // No overlay teardown here: terminated guest threads may still poll input.
    if (runtime() && nativePresentation)
    {
        closeNativeCompletion();
        if (!nativeDrain)
            nativeDrain = nativePresentation->Close();
        nativeDrain->result.wait_until(nativeDrain->deadline.Expiry());
        // The process exits without destructors, so buffered trace events are written now.
        nativePresentation->FlushTrace();
#if REREVVED_DEVELOPER
        {
            std::string cadenceError;
            if (!gpu::CloseNativeCadenceTrace(cadenceError))
                REXLOG_ERROR("Native cadence trace close failed: {}", cadenceError);
        }
        {
            std::string effectError;
            if (!gpu::guest_d3d::CloseDrawEffectProbe(effectError))
                REXLOG_ERROR("Native draw effect probe close failed: {}", effectError);
        }
#endif
    }
#endif
    // TerminateTitle may leave guest stragglers; close the census boundary.
#if REREVVED_DEVELOPER
    rerevved::gpu::guest_d3d::CloseOriginalResidueProbe();
    rerevved::gpu::guest_d3d::ReportOriginalResidueProbe(true);
#endif
#if REREVVED_DEVELOPER
    if (rerevved::gpu::guest_d3d::RingConsumeCvar())
        rerevved::gpu::guest_d3d::ReportRingConsume();
#endif
    // The SDK's Discord thread must be joined before the DLL detaches; the
    // native close path reaches here without OnShutdown. Repeated calls are no-ops.
    rerevved::StopPresence();
}

void App::OnKeyDown(rex::ui::KeyEvent& event)
{
    rex::ui::ProcessKeyEvent(event);
}

} // namespace rerevved
