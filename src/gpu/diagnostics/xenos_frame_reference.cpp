#include "xenos_frame_reference.h"

#include "draw_capture_frame_inputs.h"
#include "scene_resource_capture.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/crypto/sha256.h>
#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/logging.h>
#include <toml++/toml.hpp>

namespace rerevved::gpu::diagnostics
{
namespace
{

using namespace rex::graphics::diagnostic;

struct Session
{
    std::filesystem::path               directory;
    XenosFrameReferenceOptions          options;
    std::shared_ptr<DrawCaptureMailbox> mailbox;
    std::atomic<std::uint64_t>          frame{ 0 };
    std::mutex                          mutex;
    std::condition_variable             wake;
    bool                                stopping = false;
    // Per window. Triggers read the atomics without the mutex; the mutex orders their writes
    // with the request fields, which the firing trigger sets after reserving the mailbox itself.
    std::uint32_t              window = 1;
    std::atomic<bool>          armed{ false }, fileArm{ false };
    std::atomic<std::uint64_t> frameDue{ 0 };
    bool                       requested       = false;
    std::uint64_t              requestBoundary = 0, requestGpuFrame = 0, digestGpuFrame = 0;
    bool                       gpuBound = false;
    const char*                trigger  = "";
    DrawCaptureMailbox::Token  token;
    // Set once window 1 is reserved by any trigger; the digest observer then stops.
    std::atomic<bool>          digestDone{ true };
    std::atomic<std::uint64_t> observer{ 0 };
    std::thread                worker;
};

std::mutex lifecycle;
// One session per process. It is never freed, so a boundary notification that
// loaded the pointer before Stop cannot touch released memory.
std::unique_ptr<Session> session;
std::atomic<Session*>    active{ nullptr };

std::string digest(std::span<const std::uint8_t> bytes)
{
    return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

// The pixel word digest of host-order microcode words: each word's bytes, least significant first.
std::string wordDigest(std::span<const std::uint32_t> words)
{
    std::vector<std::uint8_t> bytes(words.size_bytes());
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(words[i / 4] >> (8 * (i % 4)));
    return digest(bytes);
}

// Reserves the mailbox for the current window on the thread that fired the trigger, so a
// delayed worker cannot move the request. A failed reservation is still handed over so the
// worker reports it.
void request(Session& s, const char* trigger, std::uint64_t boundary, const std::uint64_t* gpuFrames = nullptr) noexcept
{
    if (s.armed.exchange(true, std::memory_order_acq_rel))
        return;
    s.digestDone.store(true, std::memory_order_release);
    DrawCaptureMailbox::Token token;
    try
    {
        token = s.mailbox->StartFrame();
        std::lock_guard lock(s.mutex);
        // Stop already released the worker; nothing would take this frame.
        if (s.stopping)
        {
            s.mailbox->Cancel(token);
            return;
        }
        s.token           = token;
        s.requestBoundary = boundary;
        s.gpuBound        = gpuFrames != nullptr;
        s.requestGpuFrame = gpuFrames ? gpuFrames[0] : 0;
        s.digestGpuFrame  = gpuFrames ? gpuFrames[1] : 0;
        s.trigger         = trigger;
        s.requested       = true;
        s.wake.notify_all();
    }
    catch (...)
    {
        try
        {
            s.mailbox->Cancel(token);
        }
        catch (...)
        {
        }
    }
}

// At processor swap poll/capture boundary, first matching digest draw selects a later
// frame independent of guest queue depth. Reserve after poll ending digest+delay- 1,
// before ending digest+delay: capture owns digest+delay+1.
class DigestObserver final : public DrawCaptureFrameInputObserver
{
public:
    explicit DigestObserver(Session& session)
    : s(session)
    {
    }

    bool OnFrameBegin(const DrawCaptureFrameInputSwapView&) override
    {
        return live();
    }

    bool OnDraw(const DrawCaptureFrameInputDrawView& draw) override
    {
        if (!live())
            return false;
        if (matched || !draw.pixel_shader.present || !matches(draw.pixel_shader))
            return true;
        matched     = true;
        digestFrame = draw.frame;
        remaining   = s.options.armDelay;
        // With no delay the swap ending this frame is the one to start at, and its poll is still ahead.
        return remaining ? true : reserve(draw.frame);
    }

    bool OnCopy(const DrawCaptureFrameInputCopyView&) override
    {
        return live();
    }

    bool OnSwap(const DrawCaptureFrameInputSwapView&) override
    {
        return live();
    }

    bool OnFrameEnd(std::uint64_t frame, std::uint64_t, bool) override
    {
        if (!live())
            return false;
        if (matched && !--remaining)
            return reserve(frame);
        return true;
    }

private:
    bool live() const noexcept
    {
        return !s.digestDone.load(std::memory_order_acquire);
    }

    // Digests are cached by the SDK shader hash so each shader is hashed once.
    bool matches(const DrawCaptureFrameInputShaderView& shader)
    {
        const auto [entry, inserted] = seen.try_emplace(shader.hash, false);
        if (inserted)
            entry->second = wordDigest(shader.words).starts_with(s.options.pixelDigest);
        return entry->second;
    }

    // A false result tells the SDK to stop calling this observer.
    bool reserve(std::uint64_t gpuFrame) noexcept
    {
        const std::uint64_t frames[] = { gpuFrame, digestFrame };
        request(s, "pixel_digest", s.frame.load(std::memory_order_relaxed), frames);
        return false;
    }

    Session&                                s;
    bool                                    matched     = false;
    std::uint64_t                           digestFrame = 0;
    std::uint32_t                           remaining   = 0;
    std::unordered_map<std::uint64_t, bool> seen;
};

// Stops the digest observer. Unregistering waits for a callback in flight, which may take
// the session mutex, so callers must not hold it.
void retireObserver(Session& s) noexcept
{
    s.digestDone.store(true, std::memory_order_release);
    if (const auto token = s.observer.exchange(0))
    {
        try
        {
            s.mailbox->UnregisterFrameInputObserver({ token });
        }
        catch (...)
        {
        }
    }
}

std::string armFileName(std::uint32_t window)
{
    return window == 1 ? "arm" : "arm-" + std::to_string(window);
}

// Serves one window. Returns false when the game closed before the window opened.
bool runWindow(Session& s)
{
    std::uint32_t window;
    {
        std::lock_guard lock(s.mutex);
        window = s.window;
    }
    const auto  directory = window == 1 ? s.directory : s.directory / ("window-" + std::to_string(window));
    toml::table report{ { "schema_version", 1 }, { "status", "failed" }, { "window", std::int64_t(window) }, { "arm_frame", std::int64_t(window == 1 ? s.options.armFrame : 0) } };
    if (window == 1 && !s.options.pixelDigest.empty())
    {
        report.insert("arm_pixel_digest", s.options.pixelDigest);
        report.insert("arm_delay", std::int64_t(s.options.armDelay));
    }
    const auto&               mailbox = s.mailbox;
    DrawCaptureMailbox::Token token;
    bool                      opened = false;
    try
    {
        std::unique_lock lock(s.mutex);
        // The file trigger is polled here; the boundary, not this thread, reserves the mailbox.
        const auto armFile = s.directory / armFileName(window);
        while (!s.stopping && !s.requested)
        {
            if (!s.fileArm.load(std::memory_order_relaxed))
            {
                lock.unlock();
                std::error_code error;
                const bool      present = std::filesystem::is_regular_file(armFile, error);
                lock.lock();
                if (present)
                    s.fileArm.store(true, std::memory_order_release);
            }
            s.wake.wait_for(lock, std::chrono::milliseconds(50), [&]
                            {
                                return s.stopping || s.requested;
                            });
        }
        token = s.token;
        if (!s.requested)
        {
            // A later window that never opened leaves nothing behind.
            if (window > 1)
                return false;
            throw std::runtime_error("the game closed before the arm frame");
        }
        opened = true;
        report.insert("request_boundary", std::int64_t(s.requestBoundary));
        report.insert("trigger", s.trigger);
        const auto gpuBound        = s.gpuBound;
        const auto requestGpuFrame = s.requestGpuFrame;
        if (gpuBound)
        {
            report.insert("digest_gpu_frame", std::int64_t(s.digestGpuFrame));
            report.insert("request_gpu_frame", std::int64_t(requestGpuFrame));
        }
        if (window == 1)
        {
            lock.unlock();
            retireObserver(s);
            lock.lock();
        }
        if (window > 1 && !std::filesystem::create_directories(directory))
            throw std::runtime_error("the frame reference window directory already exists");
        if (!token)
            throw std::runtime_error("the Xenos capture mailbox is already reserved");
        const auto armedAt = std::chrono::steady_clock::now();
        for (;;)
        {
            if (const auto snapshot = mailbox->TryTakeFrame(token))
            {
                lock.unlock();
                // The SDK frame that begins at the bound swap is numbered after the reserving callback's frame.
                if (gpuBound && snapshot->start_frame <= requestGpuFrame)
                    throw std::runtime_error("the SDK frame starts before the digest boundary");
                SaveDrawCaptureFrameInputs(directory / "input", *snapshot);
                const auto& swap = snapshot->final_swap;
                report.insert("start_frame", std::int64_t(snapshot->start_frame));
                report.insert("end_frame", std::int64_t(snapshot->end_frame));
                report.insert("event_count", std::int64_t(snapshot->events.size()));
                report.insert("output_format", swap.output_format);
                report.insert("output_width", swap.output_width);
                report.insert("output_height", swap.output_height);
                report.insert("reference_sha256", digest(swap.output_bytes));
                report.insert("source_sha256", digest(swap.source_bytes));
                report.insert_or_assign("status", "complete");
                break;
            }
            if (!mailbox->IsPending(token))
                throw std::runtime_error("the Xenos producer ended the frame capture without a completed snapshot");
            if (std::chrono::steady_clock::now() - armedAt > std::chrono::seconds(60))
                throw std::runtime_error("no completed frame arrived within 60 seconds of the request");
            if (s.wake.wait_for(lock, std::chrono::milliseconds(100), [&]
                                {
                                    return s.stopping;
                                }))
                throw std::runtime_error("the game closed before the requested frame arrived");
        }
    }
    catch (const std::exception& exception)
    {
        report.insert_or_assign("error", exception.what());
        REXLOG_ERROR("Xenos frame reference failed: {}", exception.what());
    }
    mailbox->Cancel(token);
    try
    {
        {
            std::ofstream file(directory / "reference.tmp.toml", std::ios::binary);
            file << report;
            file.close();
            if (!file)
                throw std::runtime_error("could not write the frame reference report");
        }
        std::filesystem::rename(directory / "reference.tmp.toml", directory / "reference.toml");
        REXLOG_INFO("Xenos frame reference {}: {}", report["status"].value_or(std::string()), (directory / "reference.toml").string());
    }
    catch (const std::exception& exception)
    {
        REXLOG_ERROR("{}", exception.what());
    }
    if (!opened)
        return false;
    // Re-arm for the next window. Clearing armed last lets a boundary see the reset state.
    std::lock_guard lock(s.mutex);
    s.requested       = false;
    s.token           = {};
    s.trigger         = "";
    s.gpuBound        = false;
    s.requestGpuFrame = 0;
    s.digestGpuFrame  = 0;
    s.fileArm.store(false, std::memory_order_relaxed);
    s.frameDue.store(0, std::memory_order_relaxed);
    ++s.window;
    s.armed.store(false, std::memory_order_release);
    return !s.stopping;
}

void run(Session& s)
{
    for (std::uint32_t window = 1; window <= s.options.windows; ++window)
        if (!runWindow(s))
            break;
}

} // namespace

bool StartXenosFrameReference(const std::filesystem::path& directory, const XenosFrameReferenceOptions& options, std::string& error)
{
    std::lock_guard lock(lifecycle);
    try
    {
        std::string digestPrefix;
        if (!options.pixelDigest.empty() && !NormalizeScenePixelDigest(options.pixelDigest, digestPrefix))
            throw std::runtime_error("the frame reference arm digest needs 8 to 64 hex digits of a pixel shader word digest");
        if (!options.windows || options.windows > 16)
            throw std::runtime_error("the frame reference runs 1..16 windows");
        if (session || !std::filesystem::create_directories(directory))
            throw std::runtime_error("a frame reference needs one session and a fresh directory");
        auto created                 = std::make_unique<Session>();
        created->directory           = directory;
        created->options             = options;
        created->options.pixelDigest = digestPrefix;
        created->mailbox             = GetDrawCaptureMailbox();
        created->frameDue.store(options.armFrame);
        if (!digestPrefix.empty())
        {
            created->digestDone.store(false);
            created->observer = created->mailbox->RegisterFrameInputObserver(std::make_shared<DigestObserver>(*created)).value;
            if (!created->observer)
                throw std::runtime_error("the digest trigger needs the SDK frame input observer, which another consumer holds");
        }
        try
        {
            created->worker = std::thread(run, std::ref(*created));
        }
        catch (...)
        {
            retireObserver(*created);
            throw;
        }
        session = std::move(created);
        active.store(session.get(), std::memory_order_release);
        REXLOG_INFO("Xenos frame reference waiting for {}", options.armFrame ? "VdSwap boundary " + std::to_string(options.armFrame) : "its arm file or draw trigger");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool StartXenosFrameReference(const std::filesystem::path& directory, std::uint32_t armFrame, std::string& error)
{
    if (!armFrame)
    {
        error = "a frame reference needs one session, a nonzero arm frame and a fresh directory";
        return false;
    }
    XenosFrameReferenceOptions options;
    options.armFrame = armFrame;
    return StartXenosFrameReference(directory, options, error);
}

void NotifyXenosFrameReferenceBoundary() noexcept
{
    auto* s = active.load(std::memory_order_acquire);
    if (!s)
        return;
    const auto boundary = s->frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (s->armed.load(std::memory_order_acquire))
        return;
    if (s->fileArm.load(std::memory_order_acquire))
        request(*s, "file", boundary);
    else if (const auto due = s->frameDue.load(std::memory_order_acquire); due && boundary >= due)
        request(*s, "arm_frame", boundary);
}

void StopXenosFrameReference()
{
    std::lock_guard lock(lifecycle);
    if (!session || !session->worker.joinable())
        return;
    active.store(nullptr, std::memory_order_release);
    retireObserver(*session);
    {
        std::lock_guard wait(session->mutex);
        session->stopping = true;
    }
    session->wake.notify_all();
    session->worker.join();
}

} // namespace rerevved::gpu::diagnostics
