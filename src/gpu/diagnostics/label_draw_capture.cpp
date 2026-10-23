#include "label_draw_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <rex/cvar.h>
#include <rex/graphics/diagnostic_draw_capture.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <stdexcept>
#include <thread>
#include <toml++/toml.hpp>
#include <utility>

namespace rerevved::gpu::diagnostics
{
namespace
{

using namespace guest_d3d;
using namespace rex::graphics::diagnostic;

constexpr std::uint64_t kEdramBytes = 10 * 1024 * 1024;

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("label_capture_word_extent");
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void require(bool condition, const char* reason)
{
    if (!condition)
        throw std::runtime_error(reason);
}

void write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    file.close();
    require(bool(file), "label_capture_save_failed");
}

std::vector<std::uint8_t> bigEndianWords(std::span<const std::uint32_t> source)
{
    std::vector<std::uint8_t> bytes(source.size_bytes());
    for (std::size_t i = 0; i < source.size(); ++i)
        for (unsigned c = 0; c < 4; ++c)
            bytes[i * 4 + c] = static_cast<std::uint8_t>(source[i] >> (24 - c * 8));
    return bytes;
}

std::mutex                        lifecycleMutex;
std::atomic<bool>                 armed{ false };
thread_local LabelAdapterCapture* activeAdapter = nullptr;

} // namespace

struct LabelAdapterCapture
{
    std::filesystem::path                directory;
    ReadMemory                           read;
    std::string                          pixelWordDigest;
    bool                                 xenos = false;
    LabelDrawInputs                      inputs;
    DrawCaptureMailbox::Token            token;
    std::thread                          worker;
    std::mutex                           mutex; // guards `stopping` for the worker's wait
    std::condition_variable              wake;
    bool                                 claimed = false, stopping = false, patchObserved = false;
    std::vector<std::uint8_t>            originalVertexCode;
    std::string                          originalVertexSource, observationError, rejectedDumpError, lastIdentityReadError;
    std::map<std::string, std::uint64_t> rejectionCounts;
    std::uint64_t                        foreignCount = 0, candidateCount = 0, rejections = 0, identityReadFailures = 0;

    // Result state shared with the Xenos worker.
    std::mutex  resultMutex;
    bool        inputsSaved = false, xenosCorrelated = false;
    std::string error, xenosStatus, xenosError, originalVertexDigest;

    void publish()
    {
        std::lock_guard lock(resultMutex);
        toml::table     histogram;
        for (const auto& [reason, count] : rejectionCounts)
            histogram.insert(reason, static_cast<std::int64_t>(count));
        toml::table result{ { "complete", inputsSaved && error.empty() }, { "error", error }, { "boundary", 0x826A3000LL }, { "family", "label" }, { "adapter_before_original", true }, { "foreign_count", static_cast<std::int64_t>(foreignCount) }, { "identity_read_failure_count", static_cast<std::int64_t>(identityReadFailures) }, { "last_identity_read_error", lastIdentityReadError }, { "candidate_count", static_cast<std::int64_t>(candidateCount) }, { "rejected_candidate_count", static_cast<std::int64_t>(rejections) }, { "rejection_counts", std::move(histogram) }, { "rejected_inputs", rejections ? "rejected-0001" : "" }, { "rejected_dump_error", rejectedDumpError } };
        if (inputsSaved)
        {
            result.insert("vertex_count", inputs.arguments.vertexCount);
            result.insert("texture_bytes", static_cast<std::int64_t>(inputs.textureBytes.size()));
            result.insert("original_vs_source", originalVertexSource);
            result.insert("original_vs_word_digest", originalVertexDigest);
            result.insert("original_vs_matches_pinned", originalVertexDigest == kLabelVertexWordDigest);
        }
        result.insert("xenos_status", xenosStatus);
        result.insert("xenos_error", xenosError);
        result.insert("xenos_correlated", xenosCorrelated);
        std::ofstream file(directory / "result.toml");
        file << result;
        file.close();
        if (!file)
            REXLOG_ERROR("Could not write label adapter capture result");
    }

    void setError(const std::string& message)
    {
        {
            std::lock_guard lock(resultMutex);
            error = message;
        }
        publish();
        REXLOG_ERROR("Label adapter capture failed: {}", message);
    }

    void setXenos(const char* status, const std::string& message, bool correlated)
    {
        {
            std::lock_guard lock(resultMutex);
            xenosStatus     = status;
            xenosError      = message;
            xenosCorrelated = correlated;
        }
        publish();
    }

    void reject(const std::string& reason)
    {
        ++rejectionCounts[reason];
        if (++rejections != 1)
            return;
        std::string dumpError;
        if (SaveLabelDrawInputs(directory / "rejected-0001", inputs, dumpError))
        {
            std::ofstream file(directory / "rejected-0001" / "reason.toml");
            file << toml::table{ { "reason", reason }, { "before_original", true }, { "inputs_complete", inputs.inputsComplete } };
        }
        else
            rejectedDumpError = dumpError;
    }

    void clearFlag()
    {
        if (!xenos)
            return;
        const auto path = (directory / "xenos").generic_string();
        if (rex::cvar::GetFlagByName("d3d12_capture_draw") == path)
            rex::cvar::SetFlagByName("d3d12_capture_draw", "");
    }

    // The snapshot is not gated: the Xenos draw may be a later batch than the
    // adapter's, so each comparison is recorded for the reader to judge.
    bool saveReference(const DrawCaptureSnapshot& s)
    {
        require(s.registers.size() == 0x5003, "label_reference_register_extent");
        require(s.edram_before.size() == kEdramBytes && s.edram_after.size() == kEdramBytes, "label_reference_edram_extent");
        const auto out = directory / "xenos";
        const auto vs = bigEndianWords(s.vertex_ucode), ps = bigEndianWords(s.pixel_ucode);
        write(out / "registers.be.bin", bigEndianWords(s.registers));
        write(out / "vs.be.bin", vs);
        write(out / "ps.be.bin", ps);
        write(out / "edram-before.bin", s.edram_before);
        write(out / "edram-after.bin", s.edram_after);
        toml::array fetches;
        bool        verticesEqual = false;
        for (std::size_t i = 0; i < s.vertex_fetches.size(); ++i)
        {
            const auto& fetch   = s.vertex_fetches[i];
            const auto  payload = fetch.Payload();
            write(out / ("vertex-fetch-" + std::to_string(i) + ".be.bin"), payload);
            fetches.push_back(toml::table{ { "fetch_constant", fetch.fetch_constant }, { "base", fetch.base }, { "size", static_cast<std::int64_t>(payload.size()) } });
            verticesEqual |= fetch.fetch_constant == 95 && fetch.availability == DrawCaptureAvailability::kComplete &&
                             payload.size() >= inputs.vertices.size() &&
                             std::equal(inputs.vertices.begin(), inputs.vertices.end(), payload.begin());
        }
        // Texture fetch constant 0 starts the fetch block at register 0x4800.
        bool textureFetchEqual = true;
        for (std::size_t i = 0; i < inputs.textureFetch.size(); ++i)
            textureFetchEqual &= s.registers[0x4800 + i] == inputs.textureFetch[i];
        const bool    vsEqual = vs == originalVertexCode, psEqual = ps == inputs.pixelCode;
        const bool    correlated = vsEqual && psEqual && verticesEqual && textureFetchEqual;
        std::ofstream file(out / "event.toml");
        file << toml::table{ { "reference_schema", 1 }, { "complete", true }, { "frame", static_cast<std::int64_t>(s.frame) }, { "submission", static_cast<std::int64_t>(s.submission) }, { "vertex_shader_hash", static_cast<std::int64_t>(s.vertex_shader_hash) }, { "pixel_shader_hash", static_cast<std::int64_t>(s.pixel_shader_hash) }, { "used_texture_mask", s.used_texture_mask }, { "native_vertex_count", s.native_vertex_count }, { "window_offset", s.registers[0x2080] }, { "half_pixel_offset", s.half_pixel_offset }, { "vertex_fetches", std::move(fetches) }, { "vs_equals_original", vsEqual }, { "ps_equals_adapter", psEqual }, { "vertices_equal_adapter", verticesEqual }, { "texture_fetch_equals_adapter", textureFetchEqual }, { "correlated", correlated } };
        file.close();
        require(bool(file), "label_reference_metadata_save");
        return correlated;
    }

    void collect()
    {
        const auto mailbox = GetDrawCaptureMailbox();
        try
        {
            const auto                                 deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            std::shared_ptr<const DrawCaptureSnapshot> snapshot;
            while (!(snapshot = mailbox->TryTake(token)))
            {
                std::unique_lock lock(mutex);
                if (stopping)
                    throw std::runtime_error("label_reference_cancelled_at_shutdown");
                if (!mailbox->IsPending(token) || std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("label_reference_not_completed");
                wake.wait_for(lock, std::chrono::milliseconds(100), [&]
                              {
                                  return stopping;
                              });
            }
            clearFlag();
            const bool correlated = saveReference(*snapshot);
            setXenos("complete", "", correlated);
            REXLOG_INFO("Label Xenos reference saved: correlated={}", correlated);
        }
        catch (const std::exception& exception)
        {
            clearFlag();
            mailbox->Cancel(token);
            setXenos("unavailable", exception.what(), false);
            REXLOG_WARN("Label Xenos reference unavailable: {}", exception.what());
        }
    }
};

namespace
{

std::shared_ptr<LabelAdapterCapture> session;

NativeDrawSubmission submissionFrom(const PPCContext& ctx)
{
    NativeDrawSubmission submission;
    submission.hook      = NativeDrawHook::NonindexedUp;
    submission.caller    = static_cast<std::uint32_t>(ctx.lr);
    submission.stack     = ctx.r1.u32;
    submission.arguments = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
    return submission;
}

} // namespace

bool StartLabelAdapterCapture(const std::filesystem::path& directory, LabelAdapterOptions options, std::string& error)
{
    std::lock_guard                      lock(lifecycleMutex);
    std::shared_ptr<LabelAdapterCapture> candidate;
    try
    {
        require(!session && options.read, "label_capture_session_unavailable");
        require(!options.xenos || (rex::cvar::GetFlagInfo("d3d12_capture_draw") &&
                                   rex::cvar::GetFlagByName("d3d12_capture_draw").empty() &&
                                   rex::cvar::GetFlagByName("d3d12_capture_frame").empty()),
                "label_capture_slot_unavailable");
        require(std::filesystem::create_directories(directory), "label_capture_directory_not_fresh");
        candidate                  = std::make_shared<LabelAdapterCapture>();
        candidate->directory       = directory;
        candidate->read            = std::move(options.read);
        candidate->pixelWordDigest = options.pixelWordDigest.empty() ? std::string(kLabelPixelWordDigest) : std::move(options.pixelWordDigest);
        candidate->xenos           = options.xenos;
        candidate->xenosStatus     = options.xenos ? "armed" : "disabled";
        if (options.xenos)
        {
            // Arm now: the first matching label draw is the first one the GPU
            // processes after the next swap, which is the adapter's draw.
            DrawCaptureRequest request;
            request.output_path        = directory / "xenos";
            request.vertex_shader_hash = kLabelVertexShaderHash;
            request.pixel_shader_hash  = kLabelPixelShaderHash;
            candidate->token           = GetDrawCaptureMailbox()->Start(request);
            require(candidate->token && std::filesystem::create_directory(request.output_path), "label_reference_request_failed");
            std::ofstream marker(request.output_path / kDrawCaptureArmMarker);
            marker << "capture\n";
            marker.close();
            require(bool(marker) && rex::cvar::SetFlagByName("d3d12_capture_draw", request.output_path.generic_string()), "label_reference_arm_failed");
        }
        session = std::move(candidate);
        armed.store(true, std::memory_order_release);
        REXLOG_INFO("Label adapter capture armed: {}", directory.string());
        return true;
    }
    catch (const std::exception& exception)
    {
        if (candidate)
        {
            candidate->clearFlag();
            if (candidate->token)
                GetDrawCaptureMailbox()->Cancel(candidate->token);
        }
        error = exception.what();
        return false;
    }
}

std::shared_ptr<LabelAdapterCapture> BeginLabelAdapterDraw(const NativeDrawSubmission& submission)
{
    if (!armed.load(std::memory_order_acquire) || submission.caller != kLabelDrawCaller)
        return {};
    std::lock_guard lock(lifecycleMutex);
    if (!session || session->claimed)
        return {};
    std::string error;
    const bool  read = ReadLabelDrawInputsForDigest(session->pixelWordDigest, submission, session->read, session->inputs, error);
    if (!session->inputs.labelCandidate)
    {
        if (error.empty())
            ++session->foreignCount;
        else
        {
            // The call site and tuple matched but the identity was unreadable.
            ++session->identityReadFailures;
            session->lastIdentityReadError = error;
        }
        return {};
    }
    ++session->candidateCount;
    if (!read)
    {
        session->reject(error);
        return {};
    }
    session->claimed = true;
    armed.store(false, std::memory_order_release);
    if (!SaveLabelDrawInputs(session->directory / "adapter", session->inputs, error))
    {
        session->clearFlag();
        if (session->token)
            GetDrawCaptureMailbox()->Cancel(session->token);
        session->setError(error);
        return {};
    }
    activeAdapter = session.get();
    return session;
}

std::shared_ptr<LabelAdapterCapture> BeginLabelAdapterDraw(const PPCContext& ctx)
{
    if (!armed.load(std::memory_order_acquire) || ctx.lr != kLabelDrawCaller)
        return {};
    return BeginLabelAdapterDraw(submissionFrom(ctx));
}

void ObserveLabelAdapterPatch(std::uint32_t shader, std::uint32_t code, std::uint32_t patchState, std::uint32_t variant, std::uint64_t caller)
{
    if (!activeAdapter)
        return;
    try
    {
        const auto& in = activeAdapter->inputs;
        if (shader != in.vertexObject || patchState != in.arguments.graphics + 0x30F0 || variant != in.variant ||
            (caller != 0x826ADB70 && caller != 0x826ADD10))
            throw std::runtime_error("label_original_patch_boundary");
        // The inline copy is complete once the original fetch patch returns.
        activeAdapter->originalVertexCode   = activeAdapter->read(code, word(in.vertexMetadata, 4), false);
        activeAdapter->originalVertexSource = "patched_inline_copy";
        activeAdapter->patchObserved        = true;
    }
    catch (const std::exception& exception)
    {
        activeAdapter->observationError = exception.what();
    }
}

void ObserveLabelAdapterSelection(std::uint32_t graphics, std::uint32_t shader, std::uint32_t conditional, std::uint32_t variant)
{
    if (!activeAdapter)
        return;
    try
    {
        const auto& in = activeAdapter->inputs;
        if (graphics != in.arguments.graphics || shader != in.vertexObject || conditional != UINT32_MAX)
            throw std::runtime_error("label_original_conditional_selection");
        // A patch observation holds the inline copy that linkage modified.
        if (variant == UINT32_MAX || activeAdapter->patchObserved)
            return;
        if (variant != in.variant)
            throw std::runtime_error("label_original_variant_mismatch");
        const auto resource = word(activeAdapter->read(std::uint64_t(shader) + 32, 4, false), 0);
        const auto raw      = std::uint64_t(resource) + word(in.vertexMetadata, 0);
        require(raw <= UINT32_MAX, "label_original_shader_address");
        activeAdapter->originalVertexCode   = activeAdapter->read((raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000), word(in.vertexMetadata, 4), true);
        activeAdapter->originalVertexSource = "retained_resource";
    }
    catch (const std::exception& exception)
    {
        activeAdapter->observationError = exception.what();
    }
}

void FinishLabelAdapterDraw(const std::shared_ptr<LabelAdapterCapture>& capture, std::uint32_t result)
{
    if (!capture)
        return;
    activeAdapter = nullptr;
    // Serialize worker creation with shutdown, which joins that worker before
    // releasing its session. No late draw can start a thread after Stop.
    std::lock_guard lock(lifecycleMutex);
    if (session != capture)
        return;
    try
    {
        if (!capture->observationError.empty())
            throw std::runtime_error(capture->observationError);
        const auto& in    = capture->inputs;
        const auto  state = capture->read(in.arguments.graphics, 0x3500, false);
        write(capture->directory / "original-state.be.bin", state);
        // UP wrappers return memcpy's destination, the ring copy of the vertices.
        const auto output = word(state, 0x3488);
        if (!output || result != output || word(state, 0x30) != word(state, 0x3484))
            throw std::runtime_error("label_original_abi_completion");
        const auto vertices = capture->read(output, in.vertices.size(), false);
        write(capture->directory / "original-vertices.be.bin", vertices);
        if (vertices != in.vertices)
            throw std::runtime_error("label_original_geometry_copy");
        if (capture->originalVertexCode.empty())
            throw std::runtime_error("label_original_vertex_shader_unobserved");
        write(capture->directory / "original-vs.be.bin", capture->originalVertexCode);
        {
            std::lock_guard resultLock(capture->resultMutex);
            capture->originalVertexDigest = LabelMicrocodeWordDigest(capture->originalVertexCode);
            capture->inputsSaved          = true;
            if (capture->xenos)
                capture->xenosStatus = "pending";
        }
        capture->publish();
        REXLOG_INFO("Label adapter capture complete: {} vertices, {} texture bytes", in.arguments.vertexCount, in.textureBytes.size());
        if (capture->xenos)
            capture->worker = std::thread([capture]
                                          {
                                              capture->collect();
                                          });
    }
    catch (const std::exception& exception)
    {
        capture->clearFlag();
        if (capture->token)
            GetDrawCaptureMailbox()->Cancel(capture->token);
        capture->setError(exception.what());
    }
}

void StopLabelAdapterCapture()
{
    std::shared_ptr<LabelAdapterCapture> capture;
    {
        std::lock_guard lock(lifecycleMutex);
        armed.store(false, std::memory_order_release);
        capture = std::exchange(session, {});
    }
    if (!capture)
        return;
    {
        std::lock_guard lock(capture->mutex);
        capture->stopping = true;
    }
    capture->wake.notify_all();
    if (capture->worker.joinable())
        capture->worker.join();
    capture->clearFlag();
    if (capture->token)
        GetDrawCaptureMailbox()->Cancel(capture->token);
    bool        saved;
    bool        failed;
    std::string status;
    {
        std::lock_guard lock(capture->resultMutex);
        saved  = capture->inputsSaved;
        failed = !capture->error.empty();
        status = capture->xenosStatus;
    }
    if (failed)
        return;
    if (!saved)
    {
        capture->setError(capture->rejections ? "label_capture_stopped_after_rejected_candidates: " + std::to_string(capture->rejections) + " rejections" : "label_capture_stopped_before_matching_draw");
        return;
    }
    // Only a worker that never ran leaves these states; it records its own.
    if (status == "armed" || status == "pending")
        capture->setXenos("unavailable", status == "armed" ? "stopped_before_matching_draw" : "stopped_before_snapshot", false);
}

} // namespace rerevved::gpu::diagnostics
