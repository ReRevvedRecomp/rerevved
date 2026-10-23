#include "gpu/diagnostics/scene_resource_capture.h"
#include "gpu/diagnostics/xenos_frame_reference.h"
#include "logging_scope.hpp"
#include "production_observers.hpp"

#include <rex/graphics/draw_capture_mailbox.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

using namespace rerevved::gpu::diagnostics;
using namespace rex::graphics::diagnostic;

namespace
{

bool waitFor(const std::function<bool()>& ready)
{
    for (int i = 0; i < 100; ++i)
    {
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::filesystem::path freshDirectory(const char* mode)
{
    const auto directory = std::filesystem::temp_directory_path() / ("xenos-frame-reference-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + mode);
    std::filesystem::remove_all(directory);
    return directory;
}

// Publishes a one-draw frame for a claimed request.
void publishClaimed(DrawCaptureMailbox& mailbox, DrawCaptureMailbox::Token claimed, std::uint64_t start, std::uint64_t end)
{
    auto snapshot         = std::make_shared<DrawCaptureFrameSnapshot>();
    snapshot->start_frame = start;
    snapshot->end_frame   = end;
    DrawCaptureFrameEvent draw;
    draw.id                           = 1;
    draw.kind                         = DrawCaptureFrameEvent::Kind::kDraw;
    draw.draw.draw.registers          = { 1, 2, 3 };
    draw.draw.guest_pixel_shader_hash = 0xA8D1E41B3FBB2D15ULL;
    snapshot->events.push_back(draw);
    auto& swap         = snapshot->final_swap;
    swap.output_format = 24;
    swap.output_width  = 2;
    swap.output_height = 1;
    swap.output_bytes  = { 1, 2, 3, 4, 5, 6, 7, 8 };
    swap.source_bytes  = { 9, 10, 11, 12 };
    swap.gamma.gamma_256.assign(256, 7);
    UNSCOPED_INFO("publish");
    REQUIRE(mailbox.PublishFrame(claimed, snapshot));
}

// The producer's side: claim the pending request and publish a one-draw frame
// spanning frames 10 and 11.
void publish(DrawCaptureMailbox& mailbox)
{
    const auto claimed = mailbox.ClaimFrame(mailbox.GetFrameRequestToken());
    UNSCOPED_INFO("the producer claims the request");
    REQUIRE(bool(claimed));
    publishClaimed(mailbox, claimed, 10, 11);
}

// Boundary 3 opens the request; reference.toml keeps boundary and SDK span.
void arm()
{
    const auto  directory = freshDirectory("arm");
    std::string error;
    UNSCOPED_INFO("arm frame 0 is refused");
    REQUIRE((!StartXenosFrameReference(directory, 0, error) && !error.empty()));
    std::filesystem::create_directories(directory / "taken");
    UNSCOPED_INFO("an existing directory is refused");
    REQUIRE(!StartXenosFrameReference(directory / "taken", 3, error));
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory / "run", 3, error));
    UNSCOPED_INFO("one session per process");
    REQUIRE(!StartXenosFrameReference(directory / "second", 3, error));

    const auto mailbox = GetDrawCaptureMailbox();
    NotifyXenosFrameReferenceBoundary();
    NotifyXenosFrameReferenceBoundary();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    UNSCOPED_INFO("no request before boundary 3");
    REQUIRE(!mailbox->GetFrameRequestToken());
    NotifyXenosFrameReferenceBoundary();
    UNSCOPED_INFO("boundary 3 requests a frame before it returns");
    REQUIRE(bool(mailbox->GetFrameRequestToken()));
    NotifyXenosFrameReferenceBoundary();
    publish(*mailbox);

    const auto report = directory / "run" / "reference.toml";
    UNSCOPED_INFO("reference.toml is written");
    REQUIRE(waitFor([&]
                    {
                        return std::filesystem::exists(report);
                    }));
    StopXenosFrameReference();
    const auto table = toml::parse_file(report.string());
    UNSCOPED_INFO("the report records the request and the frame span");
    REQUIRE((table["status"].value_or(std::string()) == "complete" && table["arm_frame"].value_or<std::int64_t>(0) == 3 &&
             table["request_boundary"].value_or<std::int64_t>(0) == 3 && table["start_frame"].value_or<std::int64_t>(0) == 10 &&
             table["end_frame"].value_or<std::int64_t>(0) == 11 && table["event_count"].value_or<std::int64_t>(0) == 1));
    const auto        manifest = toml::parse_file((directory / "run" / "input" / "manifest.toml").string());
    std::vector<char> bytes;
    {
        std::ifstream file(directory / "run" / "input" / manifest["reference"]["file"].value_or(std::string()), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    UNSCOPED_INFO("the snapshot is saved with its RGB10 output");
    REQUIRE((bytes == std::vector<char>{ 1, 2, 3, 4, 5, 6, 7, 8 } && manifest["events"][0]["guest_pixel_hash"].value_or(std::string()) == "A8D1E41B3FBB2D15"));
    std::filesystem::remove_all(directory);
}

// Worker delay across boundaries 3..5 keeps boundary 3's original request.
void delayed()
{
    const auto  directory = freshDirectory("delayed");
    std::string error;
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory, 3, error));
    const auto mailbox = GetDrawCaptureMailbox();
    NotifyXenosFrameReferenceBoundary();
    NotifyXenosFrameReferenceBoundary();
    UNSCOPED_INFO("no request before boundary 3");
    REQUIRE(!mailbox->GetFrameRequestToken());
    NotifyXenosFrameReferenceBoundary();
    const auto atArm = mailbox->GetFrameRequestToken();
    NotifyXenosFrameReferenceBoundary();
    NotifyXenosFrameReferenceBoundary();
    UNSCOPED_INFO("boundary 3 holds the only request");
    REQUIRE((atArm && mailbox->GetFrameRequestToken() == atArm));
    publish(*mailbox);
    const auto report = directory / "reference.toml";
    UNSCOPED_INFO("reference.toml is written");
    REQUIRE(waitFor([&]
                    {
                        return std::filesystem::exists(report);
                    }));
    StopXenosFrameReference();
    const auto table = toml::parse_file(report.string());
    UNSCOPED_INFO("a delayed worker records the arm boundary");
    REQUIRE((table["status"].value_or(std::string()) == "complete" && table["request_boundary"].value_or<std::int64_t>(0) == 3));
    std::filesystem::remove_all(directory);
}

// After arm, closing before arrival frees the mailbox and reports failure.
void cancel()
{
    const auto  directory = freshDirectory("cancel");
    std::string error;
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory, 2, error));
    const auto mailbox = GetDrawCaptureMailbox();
    NotifyXenosFrameReferenceBoundary();
    NotifyXenosFrameReferenceBoundary();
    UNSCOPED_INFO("boundary 2 requests a frame");
    REQUIRE(bool(mailbox->GetFrameRequestToken()));
    StopXenosFrameReference();
    UNSCOPED_INFO("closing releases the request");
    REQUIRE(!mailbox->GetFrameRequestToken());
    const auto token = mailbox->StartFrame();
    UNSCOPED_INFO("the mailbox is free again");
    REQUIRE((bool(token) && mailbox->Cancel(token)));
    const auto table = toml::parse_file((directory / "reference.toml").string());
    UNSCOPED_INFO("a close after the request reports the failure");
    REQUIRE((table["status"].value_or(std::string()) == "failed" && table["request_boundary"].value_or<std::int64_t>(0) == 2 &&
             table["error"].value_or(std::string()) == "the game closed before the requested frame arrived"));
    std::filesystem::remove_all(directory);
}

// Closing before the arm frame makes no request and reports the failure.
void stop()
{
    const auto  directory = freshDirectory("stop");
    std::string error;
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory, 5, error));
    for (int i = 0; i < 4; ++i)
        NotifyXenosFrameReferenceBoundary();
    StopXenosFrameReference();
    UNSCOPED_INFO("no request before the arm frame");
    REQUIRE(!GetDrawCaptureMailbox()->GetFrameRequestToken());
    const auto table = toml::parse_file((directory / "reference.toml").string());
    UNSCOPED_INFO("an early close reports the failure");
    REQUIRE((table["status"].value_or(std::string()) == "failed" && table["error"].value_or(std::string()) == "the game closed before the arm frame" &&
             !std::filesystem::exists(directory / "input")));
    std::filesystem::remove_all(directory);
}

// A draw state for the digest trigger: G at 0, the pixel shader object at 0x4000 and its
// microcode at physical 0x100, in the layout the scene capture's shader reader walks.
std::array<std::uint8_t, 0x5000> guestMemory{};
std::array<std::uint8_t, 0x200>  physicalMemory{};

std::span<const std::uint8_t> fixtureMemory(std::uint64_t address, std::uint64_t size, bool physical)
{
    if (physical && address + size <= physicalMemory.size())
        return std::span(physicalMemory).subspan(address, size);
    if (!physical && address + size <= guestMemory.size())
        return std::span(guestMemory).subspan(address, size);
    throw std::runtime_error("frame reference fixture extent");
}

void putWord(std::size_t offset, std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        guestMemory[offset + i] = std::uint8_t(value >> (24 - 8 * i));
}

constexpr std::array<std::uint8_t, 16> shaderA{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
constexpr std::array<std::uint8_t, 16> shaderB{ 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };

struct FakeObserver final : DrawCaptureFrameInputObserver
{
    bool OnFrameBegin(const DrawCaptureFrameInputSwapView&) override
    {
        return true;
    }

    bool OnDraw(const DrawCaptureFrameInputDrawView&) override
    {
        return true;
    }

    bool OnCopy(const DrawCaptureFrameInputCopyView&) override
    {
        return true;
    }

    bool OnSwap(const DrawCaptureFrameInputSwapView&) override
    {
        return true;
    }

    bool OnFrameEnd(std::uint64_t, std::uint64_t, bool) override
    {
        return true;
    }
};

void setUpFixture()
{
    SetSceneTestMemoryReader(fixtureMemory);
    putWord(0x3194, 0x4000);
    putWord(0x4000 + 24, 0x100);
    putWord(0x4000 + 64, 0x100);
    putWord(0x4000 + 40 + 0x100 + 4, 16);
}

// One guest draw through the scene hook entry, with the given pixel shader microcode.
void draw(const std::array<std::uint8_t, 16>& code)
{
    std::copy(code.begin(), code.end(), physicalMemory.begin() + 0x100);
    PPCContext ctx{};
    ctx.r1.u32 = 0x1000;
    // Without a pending watch the hooks no longer observe, as in the title.
    if (auto* hook = sceneCaptureSession.load())
        BeginSceneEntry(hook, 0x826A3568, ctx);
}

// Waits for the worker to see the arm file, then returns the boundary count at which the request appeared.
std::uint64_t notifyUntilRequested(DrawCaptureMailbox& mailbox, std::uint64_t boundaries)
{
    for (int i = 0; i < 200 && !mailbox.GetFrameRequestToken(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        NotifyXenosFrameReferenceBoundary();
        ++boundaries;
    }
    UNSCOPED_INFO("the arm file requests a frame");
    REQUIRE(bool(mailbox.GetFrameRequestToken()));
    return boundaries;
}

// The SDK command processor's order around one swap: poll the mailbox request, deliver the
// swap and frame end to the frame input observer, then start an armed capture with the frame
// that follows, or publish the capture that frame completed.
struct FakeGpu
{
    DrawCaptureMailbox&       mailbox;
    std::uint64_t             frame = 1;
    DrawCaptureMailbox::Token armed, capturing;
    std::uint64_t             captureStart = 0;
    std::uint64_t             open         = 0;

    DrawCaptureMailbox::ObserverToken observer()
    {
        const auto token = mailbox.GetFrameInputObserverToken();
        if (token.value != open)
            open = 0;
        return token;
    }

    void draw(const std::array<std::uint8_t, 16>& code, std::uint64_t hash)
    {
        std::array<std::uint32_t, 4> words{};
        for (std::size_t i = 0; i < code.size(); ++i)
            words[i / 4] |= std::uint32_t(code[i]) << (24 - 8 * (i % 4));
        const auto token = observer();
        if (!token || !open)
            return;
        DrawCaptureFrameInputDrawView view;
        view.frame                = frame;
        view.pixel_shader.present = true;
        view.pixel_shader.hash    = hash;
        view.pixel_shader.words   = words;
        if (!mailbox.DispatchFrameInputDraw(token, view))
            open = 0;
    }

    void swap()
    {
        if (!armed && !capturing)
            armed = mailbox.GetFrameRequestToken();
        const auto                    token = observer();
        DrawCaptureFrameInputSwapView view;
        view.frame = frame;
        if (token && !open && mailbox.DispatchFrameInputBegin(token, view))
            open = token.value;
        if (open && !mailbox.DispatchFrameInputSwap(token, view))
            open = 0;
        const auto ended = frame++;
        if (open && !mailbox.DispatchFrameInputEnd(token, ended, 0, true))
            open = 0;
        if (capturing)
            publishClaimed(mailbox, std::exchange(capturing, {}), captureStart, frame);
        else if (armed)
        {
            capturing    = mailbox.ClaimFrame(std::exchange(armed, {}));
            captureStart = frame;
            UNSCOPED_INFO("the fake GPU claims the request");
            REQUIRE(bool(capturing));
        }
    }
};

// One guest frame as the title issues it: draws on the CPU, then the VdSwap boundary.
void guestFrame(bool digestDraw)
{
    draw(shaderB);
    if (digestDraw)
        draw(shaderA);
    NotifyXenosFrameReferenceBoundary();
}

// The same frame when the command processor runs it.
void gpuFrame(FakeGpu& gpu, bool digestDraw)
{
    gpu.draw(shaderB, 0xB);
    if (digestDraw)
        gpu.draw(shaderA, 0xA);
    gpu.swap();
}

toml::table finishDigest(const std::filesystem::path& directory, DrawCaptureMailbox& mailbox)
{
    const auto report = directory / "reference.toml";
    UNSCOPED_INFO("reference.toml is written");
    REQUIRE(waitFor([&]
                    {
                        return std::filesystem::exists(report);
                    }));
    UNSCOPED_INFO("window 1 retires the digest trigger and holds no draw watch");
    REQUIRE((!mailbox.HasFrameInputObserver() && sceneCaptureSession.load() == nullptr));
    StopXenosFrameReference();
    return toml::parse_file(report.string());
}

XenosFrameReferenceOptions digestOptions(std::uint32_t delay)
{
    XenosFrameReferenceOptions options;
    options.pixelDigest = ScenePixelWordDigest(shaderA).substr(0, 10);
    options.armDelay    = delay;
    return options;
}

// First in-step GPU digest frame selects capture armDelay+1 swaps later.
void digest()
{
    setUpFixture();
    const auto  directory = freshDirectory("digest");
    std::string error;
    const auto  options = digestOptions(1);
    UNSCOPED_INFO("a malformed digest is refused");
    REQUIRE(!StartXenosFrameReference(directory / "bad", XenosFrameReferenceOptions{ 0, "nothex!!", 0, 1 }, error));
    UNSCOPED_INFO("windows outside 1..16 are refused");
    REQUIRE(!StartXenosFrameReference(directory / "windows", XenosFrameReferenceOptions{ 0, "", 0, 17 }, error));
    const auto mailbox = GetDrawCaptureMailbox();
    {
        // Another consumer holding the frame input observer refuses the digest trigger.
        const auto other = mailbox->RegisterFrameInputObserver(std::make_shared<FakeObserver>());
        UNSCOPED_INFO("the fixture holds the observer");
        REQUIRE(bool(other));
        UNSCOPED_INFO("a held observer refuses the digest trigger");
        REQUIRE(!StartXenosFrameReference(directory / "busy", options, error));
        UNSCOPED_INFO("the fixture releases the observer");
        REQUIRE(mailbox->UnregisterFrameInputObserver(other));
    }
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory / "run", options, error));
    UNSCOPED_INFO("the digest trigger observes the command processor");
    REQUIRE(mailbox->HasFrameInputObserver());
    FakeGpu gpu{ *mailbox };
    for (int frame = 1; frame <= 2; ++frame)
        gpuFrame(gpu, false);
    UNSCOPED_INFO("a different pixel shader does not request");
    REQUIRE(!mailbox->GetFrameRequestToken());
    // GPU frame 3 holds the digest draw; with delay 1 the swap ending frame 4 starts the capture.
    gpu.draw(shaderA, 0xA);
    gpu.draw(shaderA, 0xA);
    UNSCOPED_INFO("the delay holds the request back");
    REQUIRE(!mailbox->GetFrameRequestToken());
    gpu.swap();
    UNSCOPED_INFO("the frame end reserves the mailbox");
    REQUIRE(bool(mailbox->GetFrameRequestToken()));
    gpuFrame(gpu, false);
    gpuFrame(gpu, false);
    const auto table = finishDigest(directory / "run", *mailbox);
    UNSCOPED_INFO("the report records the digest frame and the bound frame");
    REQUIRE((table["status"].value_or(std::string()) == "complete" && table["trigger"].value_or(std::string()) == "pixel_digest" &&
             table["digest_gpu_frame"].value_or<std::int64_t>(0) == 3 && table["request_gpu_frame"].value_or<std::int64_t>(0) == 3 &&
             table["start_frame"].value_or<std::int64_t>(0) == 5 && table["arm_delay"].value_or<std::int64_t>(0) == 1 &&
             table["arm_pixel_digest"].value_or(std::string()) == options.pixelDigest));
    std::filesystem::remove_all(directory);
}

// A GPU one swap behind still captures the frame after its digest frame.
void digestBehind()
{
    setUpFixture();
    const auto  directory = freshDirectory("digest-behind");
    std::string error;
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory, digestOptions(0), error));
    const auto    mailbox = GetDrawCaptureMailbox();
    FakeGpu       gpu{ *mailbox };
    constexpr int digestFrame = 3;
    guestFrame(false);
    for (int frame = 2; frame <= 7; ++frame)
    {
        guestFrame(frame == digestFrame);
        gpuFrame(gpu, frame - 1 == digestFrame);
    }
    const auto table = finishDigest(directory, *mailbox);
    UNSCOPED_INFO("a lagging command processor captures the frame after the digest frame");
    REQUIRE((table["status"].value_or(std::string()) == "complete" && table["trigger"].value_or(std::string()) == "pixel_digest" &&
             table["digest_gpu_frame"].value_or<std::int64_t>(0) == digestFrame && table["start_frame"].value_or<std::int64_t>(0) == digestFrame + 1));
    std::filesystem::remove_all(directory);
}

// The arm file wins window 1 before any digest draw; the digest trigger then stops observing.
void digestFileFirst()
{
    setUpFixture();
    const auto  directory = freshDirectory("digest-file-first");
    std::string error;
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory, digestOptions(0), error));
    const auto mailbox = GetDrawCaptureMailbox();
    std::ofstream(directory / "arm") << "fixture";
    notifyUntilRequested(*mailbox, 0);
    FakeGpu gpu{ *mailbox };
    gpuFrame(gpu, false);
    gpuFrame(gpu, true);
    const auto table = finishDigest(directory, *mailbox);
    UNSCOPED_INFO("the file trigger owns window 1");
    REQUIRE((table["status"].value_or(std::string()) == "complete" && table["trigger"].value_or(std::string()) == "file" &&
             !table.contains("request_gpu_frame")));
    std::filesystem::remove_all(directory);
}

// <directory>/arm opens window 1 with no arm frame; the boundary after the worker sees it reserves the mailbox.
void file()
{
    const auto  directory = freshDirectory("file");
    std::string error;
    UNSCOPED_INFO("a file-only reference starts: " + error);
    REQUIRE(StartXenosFrameReference(directory, XenosFrameReferenceOptions{}, error));
    const auto    mailbox    = GetDrawCaptureMailbox();
    std::uint64_t boundaries = 0;
    for (int i = 0; i < 3; ++i)
    {
        NotifyXenosFrameReferenceBoundary();
        ++boundaries;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    NotifyXenosFrameReferenceBoundary();
    ++boundaries;
    UNSCOPED_INFO("no request without the arm file");
    REQUIRE(!mailbox->GetFrameRequestToken());
    std::ofstream(directory / "arm") << "fixture";
    boundaries = notifyUntilRequested(*mailbox, boundaries);
    // Later boundaries before the worker runs do not move the request.
    const auto atArm = mailbox->GetFrameRequestToken();
    NotifyXenosFrameReferenceBoundary();
    UNSCOPED_INFO("the arm boundary holds the only request");
    REQUIRE(mailbox->GetFrameRequestToken() == atArm);
    publish(*mailbox);
    const auto report = directory / "reference.toml";
    UNSCOPED_INFO("reference.toml is written");
    REQUIRE(waitFor([&]
                    {
                        return std::filesystem::exists(report);
                    }));
    StopXenosFrameReference();
    const auto table = toml::parse_file(report.string());
    UNSCOPED_INFO("the report records the file trigger and the boundary that reserved the mailbox");
    REQUIRE((table["status"].value_or(std::string()) == "complete" && table["trigger"].value_or(std::string()) == "file" &&
             table["request_boundary"].value_or<std::int64_t>(0) == std::int64_t(boundaries) && table["window"].value_or<std::int64_t>(0) == 1));
    std::filesystem::remove_all(directory);
}

// Window k >= 2 waits for arm-<k>, however early another window's file appears, and writes window-<k>.
void windows()
{
    const auto                 directory = freshDirectory("windows");
    std::string                error;
    XenosFrameReferenceOptions options;
    options.armFrame = 2;
    options.windows  = 3;
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory, options, error));
    const auto    mailbox    = GetDrawCaptureMailbox();
    std::uint64_t boundaries = 0;
    const auto    finish     = [&](const std::filesystem::path& window, const char* trigger, std::uint64_t boundary)
    {
        publish(*mailbox);
        const auto report = window / "reference.toml";
        UNSCOPED_INFO("the window report is written");
        REQUIRE(waitFor([&]
                        {
                            return std::filesystem::exists(report);
                        }));
        const auto table = toml::parse_file(report.string());
        UNSCOPED_INFO("each window saves its own frame and records its trigger");
        REQUIRE((table["status"].value_or(std::string()) == "complete" && table["trigger"].value_or(std::string()) == trigger &&
                 table["request_boundary"].value_or<std::int64_t>(0) == std::int64_t(boundary) &&
                 std::filesystem::exists(window / "input" / "manifest.toml")));
    };
    for (int i = 0; i < 2; ++i)
    {
        NotifyXenosFrameReferenceBoundary();
        ++boundaries;
    }
    UNSCOPED_INFO("window 1 requests at the arm frame");
    REQUIRE(bool(mailbox->GetFrameRequestToken()));
    finish(directory, "arm_frame", 2);
    UNSCOPED_INFO("the mailbox is free for the next window");
    REQUIRE(waitFor([&]
                    {
                        return !mailbox->GetFrameRequestToken();
                    }));
    // arm-3 must not open window 2.
    std::ofstream(directory / "arm-3") << "fixture";
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (int i = 0; i < 3; ++i)
    {
        NotifyXenosFrameReferenceBoundary();
        ++boundaries;
    }
    UNSCOPED_INFO("arm-3 does not open window 2");
    REQUIRE(!mailbox->GetFrameRequestToken());
    std::ofstream(directory / "arm-2") << "fixture";
    boundaries = notifyUntilRequested(*mailbox, boundaries);
    finish(directory / "window-2", "file", boundaries);
    UNSCOPED_INFO("the mailbox is free for window 3");
    REQUIRE(waitFor([&]
                    {
                        return !mailbox->GetFrameRequestToken();
                    }));
    // arm-3 already exists: window 3 opens as soon as window 2 is done.
    boundaries = notifyUntilRequested(*mailbox, boundaries);
    finish(directory / "window-3", "file", boundaries);
    StopXenosFrameReference();
    UNSCOPED_INFO("no window beyond the configured count");
    REQUIRE(!std::filesystem::exists(directory / "window-4"));
    std::filesystem::remove_all(directory);
}

// Closing between windows leaves no report for the window that never opened.
void windowsStop()
{
    const auto                 directory = freshDirectory("windows-stop");
    std::string                error;
    XenosFrameReferenceOptions options;
    options.armFrame = 1;
    options.windows  = 2;
    UNSCOPED_INFO("start: " + error);
    REQUIRE(StartXenosFrameReference(directory, options, error));
    const auto mailbox = GetDrawCaptureMailbox();
    NotifyXenosFrameReferenceBoundary();
    publish(*mailbox);
    UNSCOPED_INFO("window 1 report is written");
    REQUIRE(waitFor([&]
                    {
                        return std::filesystem::exists(directory / "reference.toml");
                    }));
    StopXenosFrameReference();
    UNSCOPED_INFO("an unopened window leaves nothing behind");
    REQUIRE((!std::filesystem::exists(directory / "window-2") && !mailbox->GetFrameRequestToken()));
    std::filesystem::remove_all(directory);
}

// Ends the session and the logger however a case ends.
void runMode(void (*mode)())
{
    rerevved::tests::ScopedLogging logging;

    struct StopOnExit
    {
        ~StopOnExit()
        {
            StopXenosFrameReference();
            // Ends the capture workers, then drops the fixture memory reader.
            StopAndResetSceneResourceCaptureForTests();
        }
    } stop;

    mode();
}

} // namespace

TEST_CASE("xenos_frame_reference_arm", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(arm);
}

TEST_CASE("xenos_frame_reference_delayed", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(delayed);
}

TEST_CASE("xenos_frame_reference_cancel", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(cancel);
}

TEST_CASE("xenos_frame_reference_stop", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(stop);
}

TEST_CASE("xenos_frame_reference_digest", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(digest);
}

TEST_CASE("xenos_frame_reference_digest_behind", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(digestBehind);
}

TEST_CASE("xenos_frame_reference_digest_file_first", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(digestFileFirst);
}

TEST_CASE("xenos_frame_reference_file", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(file);
}

TEST_CASE("xenos_frame_reference_windows", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(windows);
}

TEST_CASE("xenos_frame_reference_windows_stop", "[ctest][xenos-frame-reference][timeout-30]")
{
    runMode(windowsStop);
}
