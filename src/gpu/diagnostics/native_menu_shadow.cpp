#include "native_menu_shadow.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include <rex/crypto/sha256.h>
#include <rex/cvar.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/logging.h>
#include <toml++/toml.hpp>

#include "gpu/d3d12/native_menu_panel.h"
#include "gpu/d3d12/native_renderer_d3d12.h"

namespace rerevved::gpu::diagnostics
{
namespace
{

constexpr std::uint64_t kVertexHash = 0x11213E38D7154104ULL;
constexpr std::uint64_t kPixelHash  = 0x3A92D78FE55C7B83ULL;

bool readBytes(const std::filesystem::path& path,
               std::size_t                  maximum,
               std::vector<std::uint8_t>&   bytes)
{
    std::error_code error;
    const auto      size = std::filesystem::file_size(path, error);
    if (error || !size || size > maximum)
    {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    bytes.resize(static_cast<std::size_t>(size));
    return static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()));
}

bool writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    output.close();
    return static_cast<bool>(output);
}

std::string digest(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(std::string_view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

} // namespace

struct NativeMenuShadow::Impl
{
    std::mutex                                           lifecycleMutex;
    std::mutex                                           waitMutex;
    std::condition_variable                              wake;
    bool                                                 stopping = false;
    std::thread                                          worker;
    std::filesystem::path                                outputDirectory;
    std::vector<std::uint8_t>                            vertexDxil;
    std::vector<std::uint8_t>                            pixelDxil;
    rex::graphics::diagnostic::DrawCaptureMailbox::Token token;

    void Run();
    void ClearCaptureFlag();
    void Compare(const NativeMenuPanelView&    view,
                 std::span<const std::uint8_t> before,
                 std::span<const std::uint8_t> after,
                 std::uint64_t                 frame,
                 std::uint64_t                 submission);
    void WriteReport(toml::table& report);
};

void NativeMenuShadow::Impl::ClearCaptureFlag()
{
    const auto capturePath = (outputDirectory / "capture").generic_string();
    if (rex::cvar::GetFlagByName("d3d12_capture_draw") == capturePath)
    {
        rex::cvar::SetFlagByName("d3d12_capture_draw", "");
    }
}

void NativeMenuShadow::Impl::WriteReport(toml::table& report)
{
    const auto    temporaryPath = outputDirectory / "comparison.tmp.toml";
    std::ofstream output(temporaryPath);
    output << report;
    output.close();
    std::error_code error;
    if (output)
    {
        std::filesystem::rename(temporaryPath, outputDirectory / "comparison.toml", error);
    }
    if (!output || error)
    {
        REXLOG_ERROR("Could not publish the live native comparison report");
    }
}

void NativeMenuShadow::Impl::Compare(const NativeMenuPanelView&    view,
                                     std::span<const std::uint8_t> before,
                                     std::span<const std::uint8_t> after,
                                     std::uint64_t                 frame,
                                     std::uint64_t                 submission)
{
    toml::table report{
        { "schema_version", 1 },
        { "status", "failed" },
        { "mode", "live_in_process_shadow_draw" },
        { "input_boundary", "xenos_completed_submission_owned_bytes" },
        { "frame", static_cast<std::int64_t>(frame) },
        { "submission", static_cast<std::int64_t>(submission) },
        { "request_token", static_cast<std::int64_t>(token.value) },
        { "edram_before_sha256", digest(before) },
        { "edram_after_sha256", digest(after) },
        { "vertex_dxil_sha256", digest(vertexDxil) },
        { "pixel_dxil_sha256", digest(pixelDxil) },
    };
    auto fail = [&](const std::string& reason)
    {
        ClearCaptureFlag();
        report.insert_or_assign("error", reason);
        WriteReport(report);
        REXLOG_ERROR("Live native menu comparison failed: {}", reason);
    };
    std::string               error;
    NativeDrawReplayRecipe    recipe;
    std::vector<std::uint8_t> reference;
    if (!BuildNativeMenuPanelRecipe(view, vertexDxil, pixelDxil, before, recipe, error) ||
        !DecodeNativeMenuPanelColor(after, view.registers, reference, error))
    {
        fail(error);
        return;
    }
    recipe.outputPath = outputDirectory / "native-samples.rgba";
    NativeRendererD3D12 renderer;
    const auto          result = renderer.ReplayOffscreen(recipe);
    renderer.Shutdown();
    if (!result.success)
    {
        fail(result.error);
        return;
    }
    std::vector<std::uint8_t> native;
    if (!readBytes(result.outputPath, reference.size(), native) || native.size() != reference.size() ||
        !writeBytes(outputDirectory / "reference-samples.rgba", reference))
    {
        fail("could not read or write the complete comparison sample planes");
        return;
    }
    std::uint64_t changed       = 0;
    std::uint64_t mismatches    = 0;
    std::uint64_t absoluteError = 0;
    unsigned      maxError      = 0;
    const auto    planeSize     = recipe.initialSample0.size();
    for (std::size_t index = 0; index < reference.size(); ++index)
    {
        const auto initial = index < planeSize ? recipe.initialSample0[index] : recipe.initialSample1[index - planeSize];
        changed += index % 4 != 3 && reference[index] != initial;
        const unsigned difference = static_cast<unsigned>(std::abs(int(native[index]) - int(reference[index])));
        mismatches += difference != 0;
        absoluteError += difference;
        maxError = std::max(maxError, difference);
    }
    report.insert("sample_bytes", static_cast<std::int64_t>(native.size()));
    report.insert("reference_changed_rgb_channels", static_cast<std::int64_t>(changed));
    report.insert("differing_channels", static_cast<std::int64_t>(mismatches));
    report.insert("maximum_channel_error", static_cast<std::int64_t>(maxError));
    report.insert("mean_absolute_channel_error", static_cast<double>(absoluteError) / native.size());
    report.insert("native_samples_sha256", digest(native));
    report.insert("reference_samples_sha256", digest(reference));
    if (!changed || maxError > 2)
    {
        fail(!changed ? "the live reference draw changed no RGB sample channels" : "native sample difference exceeded the established two-byte tolerance");
        return;
    }
    report.insert_or_assign("status", "complete");
    ClearCaptureFlag();
    WriteReport(report);
    REXLOG_INFO("Live native menu comparison complete: frame={}, submission={}, changed_channels={}, max_error={}",
                frame,
                submission,
                changed,
                maxError);
}

NativeMenuShadow::NativeMenuShadow()
: impl(std::make_unique<Impl>())
{
}

void NativeMenuShadow::Impl::Run()
{
    const auto mailbox = rex::graphics::diagnostic::GetDrawCaptureMailbox();
    auto       fail    = [&](const char* status, const std::string& error)
    {
        ClearCaptureFlag();
        toml::table report{
            { "schema_version", 1 },
            { "status", status },
            { "mode", "live_in_process_shadow_draw" },
            { "error", error },
        };
        WriteReport(report);
        REXLOG_INFO("Live native menu comparison {}: {}", status, error);
    };
    try
    {
        std::shared_ptr<const rex::graphics::diagnostic::DrawCaptureSnapshot> snapshot;
        std::chrono::steady_clock::time_point                                 armedAt{};
        for (;;)
        {
            {
                std::lock_guard waitLock(waitMutex);
                if (stopping)
                {
                    fail("cancelled", "game closed before the live snapshot was acquired");
                    return;
                }
            }
            snapshot = mailbox->TryTake(token);
            if (snapshot)
            {
                break;
            }
            if (!mailbox->IsPending(token))
            {
                fail("failed", "the Xenos producer ended the capture without a completed snapshot");
                return;
            }
            std::error_code fileError;
            const auto      now = std::chrono::steady_clock::now();
            if (armedAt == std::chrono::steady_clock::time_point{} &&
                std::filesystem::is_regular_file(outputDirectory / "capture" / "arm", fileError))
            {
                armedAt = now;
            }
            if (armedAt != std::chrono::steady_clock::time_point{} &&
                now - armedAt > std::chrono::seconds(60))
            {
                ClearCaptureFlag();
                mailbox->Cancel(token);
                fail("failed", "no completed matching panel draw arrived within 60 seconds of arming");
                return;
            }
            std::unique_lock waitLock(waitMutex);
            wake.wait_for(waitLock, std::chrono::milliseconds(100), [&]
                          {
                              return stopping;
                          });
        }
        const auto& draw = *snapshot;
        // This subset has no topology conversion or offset adjustment. Reject
        // a changed host binding before interpreting the original DMA bytes.
        if (!draw.frame || !draw.submission || !draw.half_pixel_offset ||
            draw.primitive_type != 4 || draw.guest_primitive_type != 4 ||
            draw.host_primitive_type != 4 || draw.native_topology != 4 ||
            draw.host_vertex_shader_type != 0 || draw.tessellation_mode != 0 ||
            draw.host_primitive_reset_enabled || draw.used_texture_mask != 0 ||
            draw.normalized_color_mask != 15 || !draw.color_target_written ||
            !draw.indexed || !draw.index.present || draw.native_instance_count != 1 ||
            draw.native_start_vertex != 0 || draw.native_start_index != 0 ||
            draw.native_base_vertex != 0 || draw.native_start_instance != 0 ||
            draw.host_index_format != 0 || draw.host_shader_index_endian != 1 ||
            draw.requested_index_count != draw.index.count ||
            draw.guest_draw_vertex_count != draw.index.count ||
            draw.host_draw_vertex_count != draw.index.count ||
            draw.native_index_count != draw.index.count ||
            draw.guest_index_base != draw.index.guest_base ||
            draw.vertex_fetches.size() != 1 || draw.vertex_fetches[0].fetch_constant != 95)
        {
            fail("failed", "live draw metadata differs from the proved panel submission contract");
            return;
        }
        const auto&         fetch = draw.vertex_fetches[0];
        NativeMenuPanelView view;
        view.registers         = draw.registers;
        view.vertexUcodeDwords = draw.vertex_ucode;
        view.pixelUcodeDwords  = draw.pixel_ucode;
        view.vertexBytes       = fetch.bytes;
        view.vertexGuestBase   = fetch.base;
        view.indexBytes        = draw.index.bytes;
        view.indexCount        = draw.index.count;
        view.indexFormat       = draw.index.format;
        view.indexEndian       = draw.index.endianness;
        view.vertexShaderHash  = draw.vertex_shader_hash;
        view.pixelShaderHash   = draw.pixel_shader_hash;
        Compare(view, draw.edram_before, draw.edram_after, draw.frame, draw.submission);
    }
    catch (const std::exception& exception)
    {
        ClearCaptureFlag();
        mailbox->Cancel(token);
        fail("failed", exception.what());
    }
}

NativeMenuShadow::~NativeMenuShadow()
{
    Stop();
}

bool NativeMenuShadow::Start(const std::filesystem::path& outputDirectory,
                             const std::filesystem::path& shaderDirectory,
                             std::string&                 error)
{
    std::lock_guard lifecycleLock(impl->lifecycleMutex);
    if (impl->worker.joinable())
    {
        error = "a live menu comparison is already running";
        return false;
    }
    if (!rex::cvar::GetFlagInfo("d3d12_capture_draw") ||
        !rex::cvar::GetFlagByName("d3d12_capture_draw").empty() ||
        !rex::cvar::GetFlagByName("d3d12_capture_frame").empty())
    {
        error = "the Xenos D3D12 capture slot must be available";
        return false;
    }
    if (!readBytes(shaderDirectory / "vs.dxil", 4U * 1024U * 1024U, impl->vertexDxil) ||
        !readBytes(shaderDirectory / "ps.dxil", 4U * 1024U * 1024U, impl->pixelDxil) ||
        !ValidateNativeMenuPanelShaders(impl->vertexDxil, impl->pixelDxil, error))
    {
        if (error.empty())
        {
            error = "could not read the bounded panel shader files";
        }
        return false;
    }
    std::error_code directoryError;
    if (!std::filesystem::create_directories(outputDirectory, directoryError) || directoryError)
    {
        error = "could not reserve a fresh comparison output directory";
        return false;
    }
    impl->outputDirectory = outputDirectory;
    // The request token owns the CPU handoff; the SDK retains GPU readbacks
    // independently if cancellation occurs before their submission completes.
    const auto                                    mailbox = rex::graphics::diagnostic::GetDrawCaptureMailbox();
    rex::graphics::diagnostic::DrawCaptureRequest request;
    request.output_path        = outputDirectory / "capture";
    request.vertex_shader_hash = kVertexHash;
    request.pixel_shader_hash  = kPixelHash;
    impl->token                = mailbox->Start(request);
    if (!impl->token)
    {
        error = "the live draw handoff is already reserved";
        return false;
    }
    if (!rex::cvar::SetFlagByName("d3d12_capture_draw", request.output_path.generic_string()))
    {
        impl->ClearCaptureFlag();
        mailbox->Cancel(impl->token);
        error = "could not configure the Xenos draw capture";
        return false;
    }
    try
    {
        impl->stopping = false;
        impl->worker   = std::thread(&Impl::Run, impl.get());
    }
    catch (const std::exception& exception)
    {
        impl->ClearCaptureFlag();
        mailbox->Cancel(impl->token);
        error = exception.what();
        return false;
    }
    REXLOG_INFO("Native menu comparison waiting for {}/capture/arm", outputDirectory.string());
    return true;
}

void NativeMenuShadow::Stop()
{
    std::lock_guard lifecycleLock(impl->lifecycleMutex);
    if (!impl->worker.joinable())
    {
        return;
    }
    {
        std::lock_guard waitLock(impl->waitMutex);
        impl->stopping = true;
    }
    impl->ClearCaptureFlag();
    rex::graphics::diagnostic::GetDrawCaptureMailbox()->Cancel(impl->token);
    impl->wake.notify_all();
    // The worker drains its own native submission before returning. No Xenos
    // callback references this object, and no guest pointer crosses the handoff.
    impl->worker.join();
}

} // namespace rerevved::gpu::diagnostics
