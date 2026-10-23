// Presents clear-only default or --fixture Prepared frame through hidden Win32
// Plume; exit 77 skips unavailable environment.
#include "native_presentation_fixture.h"

#include "gpu/d3d12/native_renderer_d3d12.h"
#include "gpu/present/native_immediate_drawer.h"
#include "gpu/present/native_presentation.h"
#include "gpu/present/plume_host.h"
#include "logging_scope.hpp"
#include "manual_cases.hpp"
#include "native_render_frame_test_access.h"
#include "stream_queue_fixture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <rex/logging.h>
#include <wrl/client.h>

using namespace rerevved::gpu;
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;

namespace rerevved::gpu::detail
{

struct PlumeHostTestAccess
{
    static void BeforeResize(PlumeHost& host, std::function<void()> callback)
    {
        host.beforeResizeForTesting(std::move(callback));
    }
};

struct NativePresentationTestAccess
{
    static ID3D12CommandQueue* Queue(const NativePresentation& session)
    {
        return session.commandQueueForTesting();
    }
};

} // namespace rerevved::gpu::detail

namespace
{

constexpr std::uint32_t kSourceWidth  = 1280;
constexpr std::uint32_t kSourceHeight = 720;

// Counts validation messages when the D3D12 debug layer is on.
bool             g_debugLayer = false;
std::atomic<int> g_debugErrors{ 0 };

void CALLBACK debugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void*)
{
    // Warnings print once per message id; only errors and corruption fail.
    static std::mutex    mutex;
    static std::set<int> seenWarnings;
    std::lock_guard      lock(mutex);
    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
    {
        ++g_debugErrors;
    }
    else if (severity != D3D12_MESSAGE_SEVERITY_WARNING || !seenWarnings.insert(static_cast<int>(id)).second)
    {
        return;
    }
    std::cerr << "d3d12 validation (severity " << static_cast<int>(severity) << ", id " << static_cast<int>(id) << "): " << description << '\n';
}

void hookDebugMessages(ID3D12Device* device)
{
    ComPtr<ID3D12InfoQueue1> queue;
    DWORD                    cookie = 0;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue))))
    {
        queue->RegisterMessageCallback(debugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    }
}

struct Unavailable
{
    std::string reason;
};

// Only a missing GPU or window is a skip; any other creation failure is a defect.
[[noreturn]] void creationFailed(const std::string& error)
{
    if (error.rfind("no usable D3D12 device", 0) == 0 || error.rfind("no usable window", 0) == 0)
    {
        throw Unavailable{ error };
    }
    throw std::runtime_error("presentation session creation failed: " + error);
}

// ---------------------------------------------------------------------------
// Gamma ramps, the clear-only frame and its exact model
// ---------------------------------------------------------------------------

struct GammaSet
{
    std::array<std::uint32_t, 256> entries{};
    std::array<std::uint32_t, 256> red{};
    std::array<std::uint32_t, 256> green{};
    std::array<std::uint32_t, 256> blue{};
};

// Distinct non-identity ramps per channel, so a double application, a channel
// swap or a missing ramp changes every affected pixel.
GammaSet makeGamma(int generation)
{
    GammaSet set;
    for (std::uint32_t index = 0; index < 256; ++index)
    {
        if (generation == 0)
        {
            set.red[index]   = index * 4;
            set.green[index] = 1023 - index * 4;
            set.blue[index]  = (index * index * 1023 + 32512) / 65025;
        }
        else
        {
            set.red[index]   = 1023 - index * 4;
            set.green[index] = (index * index * 1023 + 32512) / 65025;
            set.blue[index]  = (index * 37 + 5) % 1024;
        }
        set.entries[index] = PackGammaEntry(set.red[index], set.green[index], set.blue[index]);
    }
    return set;
}

std::vector<std::uint32_t> applyGamma(const std::vector<std::uint8_t>& rgba8, const GammaSet& gamma)
{
    std::vector<std::uint32_t> packed(rgba8.size() / 4);
    for (std::size_t pixel = 0; pixel < packed.size(); ++pixel)
    {
        packed[pixel] = PackRgb10A2(gamma.red[rgba8[pixel * 4]], gamma.green[rgba8[pixel * 4 + 1]], gamma.blue[rgba8[pixel * 4 + 2]]);
    }
    return packed;
}

struct ClearBox
{
    std::int32_t                left, top, right, bottom;
    std::array<std::uint8_t, 3> rgb;
};

std::vector<ClearBox> syntheticBoxes()
{
    std::vector<ClearBox> boxes;
    boxes.push_back({ 0, 0, kSourceWidth, kSourceHeight, { 0, 0, 0 } });
    for (std::int32_t stripe = 0; stripe < 32; ++stripe)
    {
        boxes.push_back({ stripe * 40, 0, (stripe + 1) * 40, kSourceHeight, { static_cast<std::uint8_t>(stripe * 8 + 3), static_cast<std::uint8_t>(255 - stripe * 8), static_cast<std::uint8_t>((stripe * 37 + 11) % 256) } });
    }
    // Asymmetric markers expose a flip, a rotation or a channel swap.
    boxes.push_back({ 0, 0, 100, 50, { 250, 240, 230 } });
    boxes.push_back({ 1180, 670, 1280, 720, { 5, 15, 25 } });
    boxes.push_back({ 600, 300, 700, 420, { 128, 64, 32 } });
    return boxes;
}

// Every 8-bit level appears once as a gray stripe of five columns.
std::vector<ClearBox> rampBoxes()
{
    std::vector<ClearBox> boxes;
    boxes.push_back({ 0, 0, kSourceWidth, kSourceHeight, { 0, 0, 0 } });
    for (std::int32_t level = 0; level < 256; ++level)
    {
        const auto value = static_cast<std::uint8_t>(level);
        boxes.push_back({ level * 5, 0, level * 5 + 5, kSourceHeight, { value, value, value } });
    }
    return boxes;
}

// Four ramps whose ten-bit entries together cover all 1024 values per channel.
GammaSet makeRampGamma(std::uint32_t phase)
{
    GammaSet set;
    for (std::uint32_t index = 0; index < 256; ++index)
    {
        set.red[index]     = index * 4 + phase;
        set.green[index]   = index * 4 + ((phase + 1) & 3U);
        set.blue[index]    = index * 4 + ((phase + 2) & 3U);
        set.entries[index] = PackGammaEntry(set.red[index], set.green[index], set.blue[index]);
    }
    return set;
}

std::vector<std::uint8_t> canvasOf(const std::vector<ClearBox>& boxes)
{
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(kSourceWidth) * kSourceHeight * 4, 0);
    for (const auto& box : boxes)
    {
        for (std::int32_t y = box.top; y < box.bottom; ++y)
        {
            for (std::int32_t x = box.left; x < box.right; ++x)
            {
                auto* pixel = canvas.data() + (static_cast<std::size_t>(y) * kSourceWidth + x) * 4;
                pixel[0]    = box.rgb[0];
                pixel[1]    = box.rgb[1];
                pixel[2]    = box.rgb[2];
                pixel[3]    = 255;
            }
        }
    }
    return canvas;
}

std::vector<std::uint8_t> syntheticCanvas()
{
    return canvasOf(syntheticBoxes());
}

NativeRenderFrame clearFrame(const GammaSet& gamma, const std::vector<ClearBox>& boxes, bool readback = true)
{
    NativeRenderFrame frame;
    frame.textures             = { { 1, kSourceWidth, kSourceHeight, NativeDrawReplayTextureFormat::Rgba8 } };
    frame.outputTextureId      = 1;
    frame.returnGpuOutput      = true;
    frame.returnResolvedOutput = readback;
    frame.gammaTable           = gamma.entries;
    NativeRenderFramePass pass;
    pass.target = { kSourceWidth, kSourceHeight, 1, NativeDrawReplayTargetFormat::Rgba8 };
    for (const auto& box : boxes)
    {
        NativeRenderFramePass::Clear clear;
        clear.rectangle  = { box.left, box.top, box.right, box.bottom };
        clear.color      = true;
        clear.colorValue = { box.rgb[0] / 255.0F, box.rgb[1] / 255.0F, box.rgb[2] / 255.0F, 1.0F };
        pass.clears.push_back(clear);
    }
    NativeRenderFrameResolve resolve;
    resolve.textureId = 1;
    resolve.width     = kSourceWidth;
    resolve.height    = kSourceHeight;
    resolve.samples   = { 0, 0 };
    pass.resolves.push_back(resolve);
    frame.passes.push_back(std::move(pass));
    std::string error;
    const bool  succeeded = ValidateNativeRenderFrame(frame, error);
    UNSCOPED_INFO(error);
    REQUIRE(succeeded);
    return frame;
}

NativeRenderFrame syntheticFrame(const GammaSet& gamma)
{
    return clearFrame(gamma, syntheticBoxes());
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETMINMAXINFO)
    {
        // A hidden test window must reach extents beyond the current monitor.
        auto* info           = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMaxTrackSize = { 16384, 16384 };
        info->ptMaxSize      = { 16384, 16384 };
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

class HiddenWindow
{
public:
    HiddenWindow(std::uint32_t clientWidth, std::uint32_t clientHeight)
    {
        const wchar_t* className = L"RerevvedNativePresentationTest";
        WNDCLASSEXW    windowClass{};
        windowClass.cbSize        = sizeof(windowClass);
        windowClass.lpfnWndProc   = windowProc;
        windowClass.hInstance     = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = className;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            throw Unavailable{ "window class registration failed" };
        }
        RECT rect{ 0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight) };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        window_ = CreateWindowExW(0, className, L"ReRevved presentation test", WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
        if (!window_)
        {
            throw Unavailable{ "window creation failed" };
        }
    }

    ~HiddenWindow()
    {
        if (window_)
        {
            DestroyWindow(window_);
        }
    }

    HiddenWindow(const HiddenWindow&)            = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;

    HWND Handle() const
    {
        return window_;
    }

    SurfaceExtent Client() const
    {
        RECT client{};
        GetClientRect(window_, &client);
        return { static_cast<std::uint32_t>(client.right - client.left), static_cast<std::uint32_t>(client.bottom - client.top) };
    }

    SurfaceExtent Resize(std::uint32_t clientWidth, std::uint32_t clientHeight)
    {
        RECT rect{ 0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight) };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(window_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return Client();
    }

private:
    HWND window_ = nullptr;
};

// ---------------------------------------------------------------------------
// Session plus producer
// ---------------------------------------------------------------------------

struct Options
{
    bool                  fixture = false;
    std::filesystem::path catalog;
    bool                  debugLayer = false;
    std::string           only;
};

class Harness
{
public:
    Harness(std::uint32_t width, std::uint32_t height, bool nearest = false, std::size_t offers = 2, bool idleCanvas = false)
    : window(width, height)
    {
        NativePresentationConfig config;
        config.nativeWindow    = window.Handle();
        config.nearestScaling  = nearest;
        config.maxQueuedOffers = offers;
        config.idleCanvas      = idleCanvas;
        std::string error;
        session = NativePresentation::Create(config, error);
        if (!session)
        {
            creationFailed(error);
        }
        auto                             publisher = session->Publisher();
        const NativeTitleOutputPublisher recording = [this, publisher](ID3D12Resource* resource, ID3D12Fence* fence, std::uint64_t value)
        {
            auto            lease = publisher(resource, fence, value);
            std::lock_guard lock(leaseMutex);
            if (lease)
            {
                leases.push_back(lease);
            }
            return lease;
        };
        if (g_debugLayer)
        {
            hookDebugMessages(session->Device());
        }
        const bool succeeded = renderer.StartTitleDrawStream(error, session->Device(), recording);
        UNSCOPED_INFO(error);
        REQUIRE(succeeded);
    }

    ~Harness()
    {
        Finish();
    }

    // Produces one frame, waits for native completion and returns its result.
    NativeDrawReplayResult Produce(NativeRenderFrame frame)
    {
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        std::string                              error;
        const bool                               succeeded = renderer.QueueRenderFrame(std::move(frame), ticket, error);
        UNSCOPED_INFO(error);
        REQUIRE(succeeded);
        UNSCOPED_INFO("native completion did not become ready");
        REQUIRE(ticket->Completion().wait_for(60s) == std::future_status::ready);
        return ticket->Completion().get();
    }

    std::shared_ptr<NativeOutputLease> LastLease()
    {
        std::lock_guard lock(leaseMutex);
        return leases.empty() ? nullptr : leases.back();
    }

    std::size_t LeaseCount()
    {
        std::lock_guard lock(leaseMutex);
        return leases.size();
    }

    PresentResult Pump(bool captureBackbuffer = false)
    {
        if (captureBackbuffer)
        {
            session->ArmBackbufferCapture();
        }
        auto deadline = OperationDeadline::After(30s);
        return session->PumpOnce(deadline);
    }

    PresentResult Repaint(bool captureBackbuffer = false)
    {
        if (captureBackbuffer)
        {
            session->ArmBackbufferCapture();
        }
        auto deadline = OperationDeadline::After(30s);
        return session->Repaint(deadline);
    }

    std::vector<std::uint32_t> Display()
    {
        std::vector<std::uint32_t> packed;
        SurfaceExtent              extent;
        std::string                error;
        auto                       deadline  = OperationDeadline::After(30s);
        const bool                 succeeded = session->CaptureDisplay(packed, extent, deadline, error);
        UNSCOPED_INFO(error);
        REQUIRE(succeeded);
        UNSCOPED_INFO("display texture keeps the producer extent");
        REQUIRE((extent.width == kSourceWidth && extent.height == kSourceHeight));
        return packed;
    }

    // Orderly stop: the producer stops before the host releases its device.
    NativePresentationShutdown Finish()
    {
        auto deadline = OperationDeadline::After(30s);
        return Finish(deadline);
    }

    NativePresentationShutdown Finish(OperationDeadline& deadline)
    {
        if (finished)
        {
            return {};
        }
        finished = true;
        session->CloseAdmission(deadline);
        renderer.Shutdown();
        return session->Shutdown(deadline);
    }

    HiddenWindow                                    window;
    std::unique_ptr<NativePresentation>             session;
    std::mutex                                      leaseMutex;
    std::vector<std::shared_ptr<NativeOutputLease>> leases;
    NativeRendererD3D12                             renderer;

private:
    bool finished = false;
};

PresentOutcome visibleOrOccluded(const PresentResult& result)
{
    UNSCOPED_INFO(std::string("present returned neither success nor occlusion: ") + result.operation);
    REQUIRE((result.outcome == PresentOutcome::Presented || result.outcome == PresentOutcome::Occluded));
    return result.outcome;
}

std::string hex(std::uint32_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

void compareWords(const std::vector<std::uint32_t>& actual, const std::vector<std::uint32_t>& expected, const std::string& label)
{
    UNSCOPED_INFO(label + ": size differs");
    REQUIRE(actual.size() == expected.size());
    const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin());
    if (mismatch.first != actual.end())
    {
        const auto index = static_cast<std::size_t>(mismatch.first - actual.begin());
        UNSCOPED_INFO(label + ": pixel " + std::to_string(index % kSourceWidth) + "," + std::to_string(index / kSourceWidth) + " is " + hex(*mismatch.first) + ", expected " + hex(*mismatch.second));
        REQUIRE(false);
    }
}

std::vector<std::uint32_t> wordsOf(const std::vector<std::uint8_t>& bytes)
{
    UNSCOPED_INFO("packed output is not a word array");
    REQUIRE(bytes.size() % 4 == 0);
    std::vector<std::uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

// Letterboxed RGBA8 swap chain image for a display of 'source' in 'target'.
// Only 1:1 and integer nearest scaling are modeled exactly.
std::vector<std::uint8_t> expectedBackbuffer(const std::vector<std::uint32_t>& packed, SurfaceExtent target, bool nearest)
{
    const SurfaceExtent       source{ kSourceWidth, kSourceHeight };
    const auto                box = ComputeLetterbox(source, target);
    std::vector<std::uint8_t> image(static_cast<std::size_t>(target.width) * target.height * 4, 0);
    for (std::size_t pixel = 3; pixel < image.size(); pixel += 4)
    {
        image[pixel] = 255;
    }
    const bool oneToOne = box.width == source.width && box.height == source.height;
    const bool integer  = nearest && box.width % source.width == 0 && box.height % source.height == 0 && box.width / source.width == box.height / source.height;
    UNSCOPED_INFO("the expected backbuffer model needs a 1:1 or integer nearest scale");
    REQUIRE((oneToOne || integer));
    const std::uint32_t scale = oneToOne ? 1 : box.width / source.width;
    for (std::uint32_t y = 0; y < box.height; ++y)
    {
        for (std::uint32_t x = 0; x < box.width; ++x)
        {
            const auto word = packed[static_cast<std::size_t>(y / scale) * source.width + x / scale];
            auto*      out  = image.data() + (static_cast<std::size_t>(box.y + y) * target.width + box.x + x) * 4;
            out[0]          = QuantizeUnorm10To8(word & 1023U);
            out[1]          = QuantizeUnorm10To8((word >> 10U) & 1023U);
            out[2]          = QuantizeUnorm10To8((word >> 20U) & 1023U);
        }
    }
    return image;
}

void compareBackbuffer(Harness& harness, const std::vector<std::uint32_t>& packed, SurfaceExtent target, bool nearest, const std::string& label)
{
    std::vector<std::uint8_t> actual;
    SurfaceExtent             extent;
    UNSCOPED_INFO(label + ": backbuffer capture missing");
    REQUIRE(harness.session->TakeBackbufferCapture(actual, extent));
    UNSCOPED_INFO(label + ": backbuffer extent differs from the swap chain");
    REQUIRE(extent == target);
    const auto expected = expectedBackbuffer(packed, target, nearest);
    UNSCOPED_INFO(label + ": backbuffer size differs");
    REQUIRE(actual.size() == expected.size());
    const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin());
    if (mismatch.first != actual.end())
    {
        const auto index = static_cast<std::size_t>(mismatch.first - actual.begin());
        UNSCOPED_INFO(label + ": backbuffer byte " + std::to_string(index) + " (pixel " + std::to_string(index / 4 % target.width) + "," + std::to_string(index / 4 / target.width) + ") is " + std::to_string(*mismatch.first) + ", expected " + std::to_string(*mismatch.second));
        REQUIRE(false);
    }
}

void requireRetired(const std::shared_ptr<NativeOutputLease>& lease, const std::string& label)
{
    UNSCOPED_INFO(label + ": lease retired by a copy fence");
    REQUIRE((lease && lease->State() == OutputLeaseState::Retired && lease->CopySubmissionId() != 0 && lease->AwaitRetirement(0ms)));
}

void accountForPresents(const PresentationCounters& counters, const std::string& label)
{
    UNSCOPED_INFO(label + ": every host submission has one present result");
    REQUIRE(counters.hostSubmissions == counters.presentedFrames + counters.repeats + counters.occluded + counters.outOfDate + counters.presentFailed);
}

void hostMappingTable()
{
    using plume::RenderPresentResult;
    using plume::RenderPresentStatus;
    using plume::RenderSubmitResult;
    using plume::RenderSubmitStatus;
    using plume::RenderWaitResult;
    using plume::RenderWaitStatus;
    const std::array<std::pair<RenderWaitStatus, HostStatus>, 4> waits = { { { RenderWaitStatus::SIGNALED, HostStatus::Ok }, { RenderWaitStatus::TIMEOUT, HostStatus::Timeout }, { RenderWaitStatus::DEVICE_LOST, HostStatus::DeviceLost }, { RenderWaitStatus::FAILED, HostStatus::Failed } } };
    for (const auto& [native, expected] : waits)
    {
        const auto mapped = MapWaitResult({ native, 0x1234 }, "wait");
        UNSCOPED_INFO("wait status mapping keeps the raw code");
        REQUIRE((mapped.status == expected && mapped.nativeCode == 0x1234));
    }
    const std::array<std::pair<RenderSubmitStatus, HostStatus>, 3> submits = { { { RenderSubmitStatus::SUBMITTED, HostStatus::Ok }, { RenderSubmitStatus::DEVICE_LOST, HostStatus::DeviceLost }, { RenderSubmitStatus::FAILED, HostStatus::Failed } } };
    for (const auto& [native, expected] : submits)
    {
        UNSCOPED_INFO("submit status mapping keeps the raw code");
        REQUIRE((MapSubmitResult({ native, -7 }, "submit").status == expected && MapSubmitResult({ native, -7 }, "submit").nativeCode == -7));
    }
    const std::array<std::pair<RenderPresentStatus, HostStatus>, 5> presents = { { { RenderPresentStatus::PRESENTED, HostStatus::Ok }, { RenderPresentStatus::OCCLUDED, HostStatus::Occluded }, { RenderPresentStatus::OUT_OF_DATE_OR_SUBOPTIMAL, HostStatus::OutOfDate }, { RenderPresentStatus::DEVICE_LOST, HostStatus::DeviceLost }, { RenderPresentStatus::FAILED, HostStatus::Failed } } };
    for (const auto& [native, expected] : presents)
    {
        UNSCOPED_INFO("present status mapping");
        REQUIRE(MapPresentResult({ native, 0x087A0001 }, "present").status == expected);
    }
    UNSCOPED_INFO("an unset result is a failure, never a success");
    REQUIRE((MapWaitResult({}, "wait").status == HostStatus::Failed && MapSubmitResult({}, "submit").status == HostStatus::Failed && MapPresentResult({}, "present").status == HostStatus::Failed));
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

struct Report
{
    std::vector<std::string> lines;

    void Add(const std::string& line)
    {
        lines.push_back(line);
        std::cout << line << '\n';
    }
};

void baselineScenario(Report& report)
{
    Harness harness(kSourceWidth, kSourceHeight);
    auto&   session = *harness.session;
    UNSCOPED_INFO("the hidden window client area");
    REQUIRE(harness.window.Client() == SurfaceExtent{ kSourceWidth, kSourceHeight });
    UNSCOPED_INFO("the swap chain follows the window");
    REQUIRE(session.SwapChainExtent() == SurfaceExtent{ kSourceWidth, kSourceHeight });

    // Nothing was published, so nothing is presented.
    const auto idle = harness.Pump();
    UNSCOPED_INFO("an empty session presents nothing");
    REQUIRE((idle.outcome == PresentOutcome::NoFrame && session.Counters().offered == 0 && session.Counters().presentedFrames == 0 && session.Counters().hostSubmissions == 0));

    const auto gamma1 = makeGamma(0);
    const auto gamma2 = makeGamma(1);
    const auto canvas = syntheticCanvas();
    const auto first  = applyGamma(canvas, gamma1);
    const auto second = applyGamma(canvas, gamma2);
    UNSCOPED_INFO("the two gamma generations differ");
    REQUIRE(first != second);

    // Frame 1.
    session.SetGammaGeneration(1);
    const auto produced1 = harness.Produce(syntheticFrame(gamma1));
    UNSCOPED_INFO(produced1.error);
    REQUIRE((produced1.success && produced1.presenterToken != 0));
    const auto lease1 = harness.LastLease();
    UNSCOPED_INFO("the producer published one lease after native completion");
    REQUIRE((lease1 && lease1->Token() == produced1.presenterToken && lease1->State() == OutputLeaseState::Published));
    compareWords(wordsOf(produced1.resolvedOutput), first, "producer output against the model");
    auto       deadline = OperationDeadline::After(30s);
    const auto before   = std::chrono::steady_clock::now();
    session.ArmBackbufferCapture();
    const auto shown1 = session.PumpOnce(deadline);
    const auto spent  = std::chrono::steady_clock::now() - before;
    const auto kind1  = visibleOrOccluded(shown1);
    UNSCOPED_INFO("the claimed frame is the published frame");
    REQUIRE((shown1.newFrame && !shown1.repeat && shown1.frameId == lease1->Token()));
    requireRetired(lease1, "frame 1");
    UNSCOPED_INFO("copy time is charged to the shared deadline");
    REQUIRE((deadline.Spent(DeadlinePhase::Copy) > 0ns && deadline.SpentTotal() <= spent + 1ms));
    compareWords(harness.Display(), first, "display texture against the post-gamma model");
    compareWords(harness.Display(), wordsOf(produced1.resolvedOutput), "display texture against the producer readback");
    compareBackbuffer(harness, first, { kSourceWidth, kSourceHeight }, false, "frame 1");
    {
        const auto counters = session.Counters();
        UNSCOPED_INFO("frame 1 counters");
        REQUIRE((counters.offered == 1 && counters.claimed == 1 && counters.copySubmitted == 1 && counters.copyRetired == 1 && counters.hostSubmissions == 1));
        UNSCOPED_INFO("frame identity and gamma generation");
        REQUIRE((counters.lastOfferedFrameId == lease1->Token() && counters.lastRetiredFrameId == lease1->Token() && counters.gammaGeneration == 1));
        accountForPresents(counters, "frame 1");
    }

    // Frame 2 with a new gamma ramp is applied once, not on top of frame 1.
    session.SetGammaGeneration(2);
    const auto produced2 = harness.Produce(syntheticFrame(gamma2));
    UNSCOPED_INFO(produced2.error);
    REQUIRE(produced2.success);
    const auto lease2 = harness.LastLease();
    UNSCOPED_INFO("frame ids increase");
    REQUIRE((lease2 != lease1 && lease2->Token() > lease1->Token()));
    visibleOrOccluded(harness.Pump(true));
    requireRetired(lease2, "frame 2");
    compareWords(harness.Display(), second, "second gamma generation");
    compareBackbuffer(harness, second, { kSourceWidth, kSourceHeight }, false, "frame 2");
    UNSCOPED_INFO("gamma generation advances");
    REQUIRE(session.Counters().gammaGeneration == 2);

    // A repaint keeps the frame identity and never reapplies gamma.
    const auto repaintBefore = session.Counters();
    const auto repainted     = harness.Repaint(true);
    visibleOrOccluded(repainted);
    UNSCOPED_INFO("a repaint shows the retained frame");
    REQUIRE((repainted.frameId == lease2->Token() && !repainted.newFrame));
    compareWords(harness.Display(), second, "display after repaint");
    compareBackbuffer(harness, second, { kSourceWidth, kSourceHeight }, false, "repaint");
    UNSCOPED_INFO("a repaint copies nothing");
    REQUIRE((session.Counters().copyRetired == repaintBefore.copyRetired && session.Counters().offered == repaintBefore.offered));
    if (repainted.outcome == PresentOutcome::Presented)
    {
        UNSCOPED_INFO("a repaint of a shown frame is a repeat");
        REQUIRE((repainted.repeat || session.Counters().presentedFrames == repaintBefore.presentedFrames + 1));
    }

    // Letterboxing keeps the source 1:1 inside a taller target.
    const auto tall = harness.window.Resize(kSourceWidth, 1000);
    UNSCOPED_INFO("the window reached the requested client area");
    REQUIRE(tall == SurfaceExtent{ kSourceWidth, 1000 });
    session.RequestExtent(tall);
    const auto resizesBefore = session.Counters().resizes;
    visibleOrOccluded(harness.Pump(true));
    UNSCOPED_INFO("one resize applied");
    REQUIRE((session.SwapChainExtent() == tall && session.Counters().resizes == resizesBefore + 1));
    compareBackbuffer(harness, second, tall, false, "letterbox");

    // A resize storm resolves to one retire-then-resize.
    const std::array<SurfaceExtent, 4> storm = { { { 1000, 800 }, { 1100, 700 }, { 1200, 760 }, { kSourceWidth, kSourceHeight } } };
    for (const auto& extent : storm)
    {
        UNSCOPED_INFO("storm extent");
        REQUIRE(harness.window.Resize(extent.width, extent.height) == extent);
        session.RequestExtent(extent);
    }
    const auto stormBefore = session.Counters();
    visibleOrOccluded(harness.Pump(true));
    UNSCOPED_INFO("a storm costs one drained resize");
    REQUIRE((session.Counters().resizes == stormBefore.resizes + 1 && session.Counters().resizeDrains == session.Counters().resizes));
    UNSCOPED_INFO("the storm ends at the latest extent");
    REQUIRE(session.SwapChainExtent() == SurfaceExtent{ kSourceWidth, kSourceHeight });
    compareBackbuffer(harness, second, { kSourceWidth, kSourceHeight }, false, "after storm");

    // A zero extent suspends presentation; a frame published meanwhile is dropped.
    const auto zeroBefore = session.Counters();
    session.RequestExtent({ 0, 0 });
    UNSCOPED_INFO("a zero extent suspends presentation");
    REQUIRE(harness.Pump().outcome == PresentOutcome::Suspended);
    const auto producedZero = harness.Produce(syntheticFrame(gamma1));
    UNSCOPED_INFO(producedZero.error);
    REQUIRE(producedZero.success);
    const auto zeroLease = harness.LastLease();
    UNSCOPED_INFO("an offer made while suspended cancels at once");
    REQUIRE((zeroLease->State() == OutputLeaseState::Cancelled && zeroLease->AwaitRetirement(0ms)));
    UNSCOPED_INFO("a zero extent never resizes");
    REQUIRE((session.Counters().droppedWhileSuspended == zeroBefore.droppedWhileSuspended + 1 && session.Counters().resizes == zeroBefore.resizes));
    UNSCOPED_INFO("the swap chain extent is untouched");
    REQUIRE(session.SwapChainExtent() == SurfaceExtent{ kSourceWidth, kSourceHeight });
    session.RequestExtent({ kSourceWidth, kSourceHeight });
    const auto resumed = harness.Pump(true);
    visibleOrOccluded(resumed);
    UNSCOPED_INFO("resuming repaints the retained frame");
    REQUIRE((resumed.frameId == lease2->Token() && !resumed.newFrame));
    compareBackbuffer(harness, second, { kSourceWidth, kSourceHeight }, false, "after zero extent");
    UNSCOPED_INFO("resuming at the applied extent needs no resize");
    REQUIRE(session.Counters().resizes == zeroBefore.resizes);

    // Minimize suspends the same way.
    session.SetMinimized(true);
    UNSCOPED_INFO("minimize suspends presentation");
    REQUIRE(harness.Pump().outcome == PresentOutcome::Suspended);
    const auto producedMinimized = harness.Produce(syntheticFrame(gamma2));
    UNSCOPED_INFO("an offer made while minimized cancels");
    REQUIRE((producedMinimized.success && harness.LastLease()->State() == OutputLeaseState::Cancelled));
    session.SetMinimized(false);
    visibleOrOccluded(harness.Pump());

    // Two unpumped frames: the newest wins and the older unclaimed offer cancels.
    const auto beforeDrop = session.Counters();
    UNSCOPED_INFO("first queued frame");
    REQUIRE(harness.Produce(syntheticFrame(gamma2)).success);
    const auto older = harness.LastLease();
    UNSCOPED_INFO("second queued frame");
    REQUIRE(harness.Produce(syntheticFrame(gamma1)).success);
    const auto newer = harness.LastLease();
    UNSCOPED_INFO("both offers wait unclaimed");
    REQUIRE((older->State() == OutputLeaseState::Published && newer->State() == OutputLeaseState::Published));
    const auto shownNewest = harness.Pump();
    visibleOrOccluded(shownNewest);
    UNSCOPED_INFO("the newest offer is presented");
    REQUIRE(shownNewest.frameId == newer->Token());
    UNSCOPED_INFO("the dropped offer cancels and releases its slot");
    REQUIRE((older->State() == OutputLeaseState::Cancelled && older->AwaitRetirement(0ms)));
    requireRetired(newer, "newest frame");
    UNSCOPED_INFO("the drop is counted and the older frame is never copied");
    REQUIRE((session.Counters().dropped == beforeDrop.dropped + 1 && session.Counters().lastRetiredFrameId == newer->Token()));
    compareWords(harness.Display(), first, "the newest offer carried the first gamma ramp");

    accountForPresents(session.Counters(), "baseline");
    const auto totals = session.Counters();
    report.Add("baseline: offered=" + std::to_string(totals.offered) + " dropped=" + std::to_string(totals.dropped) + " droppedWhileSuspended=" + std::to_string(totals.droppedWhileSuspended) + " copyRetired=" + std::to_string(totals.copyRetired) + " presentedFrames=" + std::to_string(totals.presentedFrames) + " repeats=" + std::to_string(totals.repeats) + " occluded=" + std::to_string(totals.occluded) + " resizes=" + std::to_string(totals.resizes) + " (first present " + (kind1 == PresentOutcome::Presented ? "S_OK" : "occluded") + ")");
    const auto done = harness.Finish();
    UNSCOPED_INFO("an orderly shutdown drains and releases everything");
    REQUIRE((done.drained && !done.retained && done.health == HostHealth::Healthy));
}

// A producer-queue gate keeps recorded frames from completing.
void gatedPublicationScenario(Report& report)
{
    Harness             harness(kSourceWidth, kSourceHeight);
    auto&               session  = *harness.session;
    const auto          gamma    = makeGamma(1);
    const auto          expected = applyGamma(syntheticCanvas(), gamma);
    ComPtr<ID3D12Fence> gate;
    UNSCOPED_INFO("gate fence");
    REQUIRE(SUCCEEDED(session.Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate))));
    auto* producerQueue = rerevved::gpu::detail::NativeRenderFrameTestAccess::Queue(harness.renderer);
    UNSCOPED_INFO("producer queue gate");
    REQUIRE((producerQueue && SUCCEEDED(producerQueue->Wait(gate.Get(), 1))));
    std::shared_ptr<NativeRenderFrameTicket> gated;
    std::string                              error;
    // Without a CPU readback the producer defers its completion wait.
    const bool succeeded = harness.renderer.QueueRenderFrame(clearFrame(gamma, syntheticBoxes(), false), gated, error);
    UNSCOPED_INFO(error);
    REQUIRE(succeeded);
    UNSCOPED_INFO("recording completes behind the gate");
    REQUIRE((gated->Recording().wait_for(30s) == std::future_status::ready && gated->Recording().get().success));
    UNSCOPED_INFO("native completion waits on the gate");
    REQUIRE(gated->Completion().wait_for(0ms) == std::future_status::timeout);
    const auto unpublished = harness.Pump();
    UNSCOPED_INFO("a recorded but incomplete frame is never published or presented");
    REQUIRE((unpublished.outcome == PresentOutcome::NoFrame && harness.LeaseCount() == 0 && session.Counters().offered == 0));
    UNSCOPED_INFO("release the gate");
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    UNSCOPED_INFO("native completion after the gate");
    REQUIRE((gated->Completion().wait_for(30s) == std::future_status::ready && gated->Completion().get().success));
    const auto lease = harness.LastLease();
    UNSCOPED_INFO("publication follows native completion");
    REQUIRE((lease && harness.LeaseCount() == 1 && lease->State() == OutputLeaseState::Published));
    visibleOrOccluded(harness.Pump());
    requireRetired(lease, "gated frame");
    compareWords(harness.Display(), expected, "gated frame content");
    report.Add("gated publication: recording ready before completion, nothing published until native completion");
}

// Every ten-bit input of every channel takes the same GPU path to eight bits.
void quantizationScenario(Report& report)
{
    Harness                                harness(kSourceWidth, kSourceHeight);
    const auto                             canvas = canvasOf(rampBoxes());
    std::array<std::set<std::uint32_t>, 3> seen;
    for (std::uint32_t phase = 0; phase < 4; ++phase)
    {
        const auto gamma  = makeRampGamma(phase);
        const auto packed = applyGamma(canvas, gamma);
        for (std::uint32_t level = 0; level < 256; ++level)
        {
            seen[0].insert(gamma.red[level]);
            seen[1].insert(gamma.green[level]);
            seen[2].insert(gamma.blue[level]);
        }
        const auto produced = harness.Produce(clearFrame(gamma, rampBoxes()));
        UNSCOPED_INFO(produced.error);
        REQUIRE(produced.success);
        compareWords(wordsOf(produced.resolvedOutput), packed, "ramp producer output");
        visibleOrOccluded(harness.Pump(true));
        compareWords(harness.Display(), packed, "ramp display texture");
        compareBackbuffer(harness, packed, { kSourceWidth, kSourceHeight }, false, "ramp phase " + std::to_string(phase));
    }
    UNSCOPED_INFO("the ramps cover every ten-bit value of every channel");
    REQUIRE((seen[0].size() == 1024 && seen[1].size() == 1024 && seen[2].size() == 1024));
    report.Add("quantization: all 1024 ten-bit values per channel reach the swap chain exactly");
}

void nearestScalingScenario(Report& report)
{
    Harness harness(2560, 1440, true);
    UNSCOPED_INFO("the large hidden window");
    REQUIRE(harness.window.Client() == SurfaceExtent{ 2560, 1440 });
    const auto gamma  = makeGamma(0);
    const auto packed = applyGamma(syntheticCanvas(), gamma);
    UNSCOPED_INFO("frame for 2x scaling");
    REQUIRE(harness.Produce(syntheticFrame(gamma)).success);
    visibleOrOccluded(harness.Pump(true));
    compareBackbuffer(harness, packed, { 2560, 1440 }, true, "2x nearest");
    report.Add("nearest 2x: exact " + std::to_string(2560 * 1440) + " pixels");
}

void overlaySnapshotChecks()
{
    using namespace rex::ui;
    auto                  mailbox = std::make_shared<NativeOverlayMailbox>();
    NativeImmediateDrawer drawer(mailbox);
    uint8_t               pixels[] = { 1, 2, 3, 4 };
    UNSCOPED_INFO("font creation before device readiness returns null");
    REQUIRE(!drawer.CreateTexture(1, 1, ImmediateTextureFilter::kLinear, true, pixels));
    mailbox->SetReady(true);
    drawer.Clear();
    UNSCOPED_INFO("clearing an unused drawer does not publish an empty snapshot");
    REQUIRE(!mailbox->Latest());
    auto texture = drawer.CreateTexture(1, 1, ImmediateTextureFilter::kLinear, true, pixels);
    UNSCOPED_INFO("lazy font creation recovers after device readiness");
    REQUIRE(texture != nullptr);
    ImmediateVertex  vertices[] = { { 1, 2, 0, 0, 0x12345678 }, { 3, 4, 1, 0, 0x12345678 }, { 5, 6, 1, 1, 0x12345678 } };
    uint16_t         indices[]  = { 0, 1, 2 };
    AppUIDrawContext context(128, 64);
    drawer.Begin(context, 128, 64);
    drawer.BeginDrawBatch({ vertices, 3, indices, 3 });
    ImmediateDraw draw;
    draw.count   = 3;
    draw.texture = texture.get();
    drawer.Draw(draw);
    drawer.EndDrawBatch();
    UNSCOPED_INFO("a partial frame is not published");
    REQUIRE(!mailbox->Latest());
    drawer.End();
    auto held     = mailbox->Latest();
    vertices[0].x = 99;
    indices[0]    = 2;
    pixels[0]     = 99;
    texture.reset();
    const auto& batch = held->batches.front();
    UNSCOPED_INFO("snapshot owns geometry independently of ImGui buffers");
    REQUIRE((batch.vertices[0].x == 1 && batch.indices[0] == 0));
    UNSCOPED_INFO("snapshot replaces the borrowed handle with owned immutable texture bytes");
    REQUIRE((!batch.commands[0].draw.texture && batch.commands[0].texture->rgba[0] == 1));
    std::weak_ptr<const NativeOverlayTexture> lifetime = batch.commands[0].texture;
    drawer.Clear();
    const auto cleared = mailbox->Latest()->sequence;
    drawer.Clear();
    UNSCOPED_INFO("repeated clear preserves the empty snapshot sequence");
    REQUIRE(mailbox->Latest()->sequence == cleared);
    UNSCOPED_INFO("replacement does not retire a consumer-held texture");
    REQUIRE(!lifetime.expired());
    held.reset();
    UNSCOPED_INFO("last consumer release retires texture ownership");
    REQUIRE(lifetime.expired());

    std::atomic<bool> finished = false, coherent = true;
    std::thread       consumer([&]
                               {
                             uint64_t sequence = 0;
                             while (!finished.load())
                             {
                                 auto snapshot = mailbox->Latest();
                                 if (snapshot)
                                 {
                                     if (snapshot->sequence < sequence || snapshot->width != snapshot->height)
                                         coherent.store(false);
                                     sequence = snapshot->sequence;
                                 }
                             }
                               });
    for (int i = 1; i <= 1000; ++i)
    {
        auto snapshot   = std::make_unique<NativeOverlaySnapshot>();
        snapshot->width = snapshot->height = float(i);
        mailbox->Publish(std::move(snapshot));
    }
    finished.store(true);
    consumer.join();
    UNSCOPED_INFO("concurrent handoff publishes whole monotonic snapshots");
    REQUIRE(coherent.load());
    mailbox->SetReady(false);
    drawer.Clear();
    UNSCOPED_INFO("closed admission rejects late UI frames and textures");
    REQUIRE((!mailbox->Latest() && !drawer.CreateTexture(1, 1, ImmediateTextureFilter::kNearest, false, pixels)));
    auto invalidMailbox = std::make_shared<NativeOverlayMailbox>();
    invalidMailbox->SetReady(true);
    NativeImmediateDrawer invalid(invalidMailbox);
    invalid.Begin(context, 128, 64);
    invalid.BeginDrawBatch({ vertices, 3, indices, 3 });
    draw.texture = nullptr;
    draw.count   = 4;
    invalid.Draw(draw);
    invalid.EndDrawBatch();
    invalid.End();
    UNSCOPED_INFO("invalid geometry fails visibly without publishing partial UI");
    REQUIRE((invalidMailbox->Failed() && !invalidMailbox->Latest()));
}

void overlayFixture(Harness& harness, const std::vector<uint32_t>& packed, const NativeRenderFrame& nativeFrame)
{
    using namespace rex::ui;
    auto mailbox = std::make_shared<NativeOverlayMailbox>();
    harness.session->SetOverlayMailbox(mailbox);
    NativeImmediateDrawer drawer(mailbox);
    const uint8_t         pixels[] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 64, 128, 192, 255 };
    auto                  texture  = drawer.CreateTexture(2, 2, ImmediateTextureFilter::kNearest, false, pixels);
    UNSCOPED_INFO("overlay texture creation");
    REQUIRE(texture != nullptr);
    AppUIDrawContext context(kSourceWidth, kSourceHeight);
    drawer.Begin(context, float(kSourceWidth), float(kSourceHeight));
    const auto quad = [&](float left, uint32_t color, ImmediateTexture* image, bool clip)
    {
        const ImmediateVertex vertices[] = {
            { left, 8, 0, 0, color }, { left + 32, 8, 1, 0, color }, { left + 32, 40, 1, 1, color }, { left, 40, 0, 1, color }
        };
        const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
        drawer.BeginDrawBatch({ vertices, 4, indices, 6 });
        ImmediateDraw draw;
        draw.count          = 6;
        draw.texture        = image;
        draw.scissor        = clip;
        draw.scissor_left   = 90.25F;
        draw.scissor_top    = 14.25F;
        draw.scissor_right  = 100.5F;
        draw.scissor_bottom = 21.5F;
        drawer.Draw(draw);
        drawer.EndDrawBatch();
    };
    quad(8, 0x804020E0, nullptr, false);
    quad(44, 0x80FFFFFF, texture.get(), false);
    quad(84, 0xFF00FF00, nullptr, true);
    drawer.End();
    std::weak_ptr<const NativeOverlayTexture> lifetime = mailbox->Latest()->batches[1].commands[0].texture;
    texture.reset();
    UNSCOPED_INFO("overlay-only change has no new native frame to claim");
    REQUIRE(harness.Pump().outcome == PresentOutcome::NoFrame);
    const auto before   = harness.session->Counters();
    const auto composed = harness.Repaint(true);
    visibleOrOccluded(composed);
    UNSCOPED_INFO("three overlay draws repaint without producing a new native frame");
    REQUIRE((composed.overlayDraws == 3 && !composed.newFrame));
    UNSCOPED_INFO("overlay repaint does not recopy or regamma native output");
    REQUIRE(harness.session->Counters().copySubmitted == before.copySubmitted);
    compareWords(harness.Display(), packed, "overlay leaves the native output bytes unchanged");
    std::vector<uint8_t> actual;
    SurfaceExtent        extent;
    UNSCOPED_INFO("overlay backbuffer capture");
    REQUIRE(harness.session->TakeBackbufferCapture(actual, extent));
    const auto  base = expectedBackbuffer(packed, extent, true);
    const float sx = float(extent.width) / kSourceWidth, sy = float(extent.height) / kSourceHeight;
    const auto  edge = [](float value)
    {
        return uint32_t(std::ceil(value - 0.5F));
    };
    UNSCOPED_INFO("overlay capture size");
    REQUIRE(actual.size() == base.size());
    for (uint32_t y = 0; y < extent.height; ++y)
        for (uint32_t x = 0; x < extent.width; ++x)
        {
            const size_t at        = (size_t(y) * extent.width + x) * 4;
            int          source[3] = {}, alpha = 0;
            if (x >= edge(8 * sx) && x < edge(40 * sx) && y >= edge(8 * sy) && y < edge(40 * sy))
            {
                source[0] = 224;
                source[1] = 32;
                source[2] = 64;
                alpha     = 128;
            }
            if (x >= edge(44 * sx) && x < edge(76 * sx) && y >= edge(8 * sy) && y < edge(40 * sy))
            {
                const size_t texel = ((y >= edge(24 * sy) ? 2 : 0) + (x >= edge(60 * sx) ? 1 : 0)) * 4;
                for (int channel = 0; channel < 3; ++channel)
                    source[channel] = pixels[texel + channel];
                alpha = 128;
            }
            if (x >= edge(90.25F * sx) && x < edge(100.5F * sx) && y >= edge(14.25F * sy) && y < edge(21.5F * sy))
            {
                source[0] = 0;
                source[1] = 255;
                source[2] = 0;
                alpha     = 255;
            }
            for (int channel = 0; channel < 4; ++channel)
            {
                const int expected = channel == 3 ? 255 : (source[channel] * alpha + base[at + channel] * (255 - alpha) + 127) / 255;
                // D3D12 UNORM blend precision permits rounding intermediate products.
                // D3D functional spec 17.5, Blending Precision (fixed point targets):
                // https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
                const int tolerance = alpha == 128 && channel < 3 ? 1 : 0;
                UNSCOPED_INFO("overlay CPU blend/scissor model at byte " + std::to_string(at + channel) + ": got " + std::to_string(actual[at + channel]) + ", expected " + std::to_string(expected));
                REQUIRE(std::abs(int(actual[at + channel]) - expected) <= tolerance);
            }
        }
    UNSCOPED_INFO("white and image staging buffers survive until the uploading slot retires");
    REQUIRE(harness.session->OverlayPendingTextureUploads() == 2);
    uint64_t lastFrame = composed.frameId;
    for (int i = 0; i < 3; ++i)
    {
        UNSCOPED_INFO("native producer continues with an open overlay");
        REQUIRE(harness.Produce(nativeFrame).success);
        const auto next = harness.Pump();
        visibleOrOccluded(next);
        UNSCOPED_INFO("overlay composition advances with newly claimed native leases");
        REQUIRE((next.newFrame && next.frameId > lastFrame && next.overlayDraws == 3));
        requireRetired(harness.LastLease(), "overlay native frame");
        lastFrame = next.frameId;
    }
    UNSCOPED_INFO("uploading slot retirement frees staging while sampled textures remain live");
    REQUIRE((harness.session->OverlayPendingTextureUploads() == 0 && harness.session->OverlayTextureCount() == 2));
    UNSCOPED_INFO("multiple batches use one geometry allocation per presentation slot");
    REQUIRE(harness.session->OverlayGeometryAllocations() == 2);
    for (int i = 0; i < 4; ++i)
        visibleOrOccluded(harness.Repaint());
    UNSCOPED_INFO("steady overlay frames reuse both slot upload buffers");
    REQUIRE(harness.session->OverlayGeometryAllocations() == 2);
    auto       larger  = std::make_unique<NativeOverlaySnapshot>(*mailbox->Latest());
    const auto batches = larger->batches.size();
    for (size_t i = 0; i < batches; ++i)
        larger->batches.push_back(mailbox->Latest()->batches[i]);
    mailbox->Publish(std::move(larger));
    for (int i = 0; i < 4; ++i)
        visibleOrOccluded(harness.Repaint());
    UNSCOPED_INFO("larger geometry grows each slot once, then reuses its capacity");
    REQUIRE(harness.session->OverlayGeometryAllocations() == 4);
    drawer.Clear();
    UNSCOPED_INFO("GPU slot retains deleted SDK texture handle's payload");
    REQUIRE(!lifetime.expired());
    visibleOrOccluded(harness.Repaint());
    UNSCOPED_INFO("other slot cannot retire the original frame's textures");
    REQUIRE(!lifetime.expired());
    visibleOrOccluded(harness.Repaint(true));
    UNSCOPED_INFO("slot retirement releases overlay texture, leaving only solid-color white");
    REQUIRE((lifetime.expired() && harness.session->OverlayTextureCount() == 1));
    compareBackbuffer(harness, packed, extent, true, "closed overlay repaint");
    UNSCOPED_INFO("invalid texture reports drawer failure");
    REQUIRE(!drawer.CreateTexture(0, 1, ImmediateTextureFilter::kNearest, false, nullptr));
    const auto failure = harness.Repaint();
    UNSCOPED_INFO("drawer failure reaches the presentation result");
    REQUIRE((failure.outcome == PresentOutcome::Failed && std::string(failure.operation) == "record overlay"));
    auto       deadline = OperationDeadline::After(2s);
    const auto shutdown = harness.Finish(deadline);
    UNSCOPED_INFO("drawer failure closes within the shared deadline");
    REQUIRE((shutdown.drained && !shutdown.retained && !deadline.Expired()));
}

// Guest boot before any producer frame: black canvas plus overlays, no frame claimed.
void idleCanvasScenario(Report& report)
{
    using namespace rex::ui;
    Harness plain(kSourceWidth, kSourceHeight, true);
    UNSCOPED_INFO("without an idle canvas nothing presents before the first frame");
    REQUIRE((plain.Pump().outcome == PresentOutcome::NoFrame && plain.Repaint().outcome == PresentOutcome::NoFrame));
    plain.Finish();

    Harness    harness(kSourceWidth, kSourceHeight, true, 2, true);
    const auto requireCanvas = [&](const std::string& label, bool overlay)
    {
        std::vector<uint8_t> actual;
        SurfaceExtent        extent;
        UNSCOPED_INFO(label + ": backbuffer capture");
        REQUIRE(harness.session->TakeBackbufferCapture(actual, extent));
        UNSCOPED_INFO(label + ": capture size");
        REQUIRE(actual.size() == size_t(extent.width) * extent.height * 4);
        const float sx   = float(extent.width) / kSourceWidth;
        const float sy   = float(extent.height) / kSourceHeight;
        const auto  edge = [](float value)
        {
            return uint32_t(std::ceil(value - 0.5F));
        };
        for (uint32_t y = 0; y < extent.height; ++y)
            for (uint32_t x = 0; x < extent.width; ++x)
            {
                const bool    inside      = overlay && x >= edge(8 * sx) && x < edge(40 * sx) && y >= edge(8 * sy) && y < edge(40 * sy);
                const uint8_t expected[4] = { 0, uint8_t(inside ? 255 : 0), 0, 255 };
                const size_t  at          = (size_t(y) * extent.width + x) * 4;
                for (int channel = 0; channel < 4; ++channel)
                {
                    UNSCOPED_INFO(label + ": byte " + std::to_string(at + channel) + " is " + std::to_string(actual[at + channel]));
                    REQUIRE(actual[at + channel] == expected[channel]);
                }
            }
    };

    const auto first = harness.Pump(true);
    visibleOrOccluded(first);
    UNSCOPED_INFO("the first idle canvas claims no frame");
    REQUIRE((first.frameId == 0 && !first.newFrame && first.overlaySequence == 0));
    requireCanvas("idle canvas", false);
    UNSCOPED_INFO("an unchanged idle canvas is not presented again");
    REQUIRE(harness.Pump().outcome == PresentOutcome::NoFrame);

    auto mailbox = std::make_shared<NativeOverlayMailbox>();
    harness.session->SetOverlayMailbox(mailbox);
    NativeImmediateDrawer drawer(mailbox);
    AppUIDrawContext      context(kSourceWidth, kSourceHeight);
    drawer.Begin(context, float(kSourceWidth), float(kSourceHeight));
    const ImmediateVertex vertices[] = {
        { 8, 8, 0, 0, 0xFF00FF00 }, { 40, 8, 1, 0, 0xFF00FF00 }, { 40, 40, 1, 1, 0xFF00FF00 }, { 8, 40, 0, 1, 0xFF00FF00 }
    };
    const uint16_t indices[] = { 0, 1, 2, 0, 2, 3 };
    drawer.BeginDrawBatch({ vertices, 4, indices, 6 });
    ImmediateDraw draw;
    draw.count = 6;
    drawer.Draw(draw);
    drawer.EndDrawBatch();
    drawer.End();

    const auto composed = harness.Pump(true);
    visibleOrOccluded(composed);
    UNSCOPED_INFO("an overlay change presents the idle canvas without a frame");
    REQUIRE((composed.frameId == 0 && !composed.newFrame && composed.overlayDraws == 1 && composed.overlaySequence != 0));
    requireCanvas("idle overlay", true);
    UNSCOPED_INFO("an unchanged overlay is not presented again");
    REQUIRE(harness.Pump().outcome == PresentOutcome::NoFrame);
    visibleOrOccluded(harness.Repaint(true));
    requireCanvas("idle overlay repaint", true);

    drawer.Clear();
    const auto cleared = harness.Pump(true);
    visibleOrOccluded(cleared);
    UNSCOPED_INFO("closing the overlay restores the idle canvas");
    REQUIRE((cleared.overlayDraws == 0 && cleared.frameId == 0));
    requireCanvas("idle overlay cleared", false);

    const auto counters = harness.session->Counters();
    UNSCOPED_INFO("idle presents never enter the frame ledger");
    REQUIRE((counters.copySubmitted == 0 && counters.presentedFrames == 0 && counters.hostSubmissions == 0));
    UNSCOPED_INFO("the idle canvas publishes no lease");
    REQUIRE(harness.LeaseCount() == 0);
    auto       deadline = OperationDeadline::After(5s);
    const auto shutdown = harness.Finish(deadline);
    UNSCOPED_INFO("idle canvas session drains");
    REQUIRE((shutdown.drained && !shutdown.retained));
    report.Add("idle canvas: black with exact overlay rectangle, no frame claimed");
}

void fixtureScenario(Report& report, const Options& options)
{
    Harness                             harness(kSourceWidth, kSourceHeight, true);
    const auto                          gamma = makeGamma(0);
    presentation_fixture::AdmittedFrame admitted;
    std::string                         error;
    const bool                          succeeded = presentation_fixture::BuildAdmittedFrame(options.catalog, gamma.entries, admitted, error);
    UNSCOPED_INFO(error);
    REQUIRE(succeeded);
    const auto produced = harness.Produce(admitted.frame);
    UNSCOPED_INFO(produced.error);
    REQUIRE((produced.success && produced.presenterToken != 0));
    const auto expected = applyGamma(admitted.expectedRgba8, gamma);
    const auto black    = PackRgb10A2(gamma.red[0], gamma.green[0], gamma.blue[0]);
    const auto drawn    = static_cast<std::size_t>(std::count_if(expected.begin(), expected.end(), [&](std::uint32_t word)
                                                                 {
                                                                  return word != black;
                                                                 }));
    UNSCOPED_INFO("the fixture draws a bounded non-black region");
    REQUIRE((drawn != 0 && drawn < expected.size()));
    compareWords(wordsOf(produced.resolvedOutput), expected, "fixture producer output against the expected post-gamma canvas");
    visibleOrOccluded(harness.Pump(true));
    requireRetired(harness.LastLease(), "fixture frame");
    compareWords(harness.Display(), expected, "fixture display texture against the expected post-gamma canvas");
    compareBackbuffer(harness, expected, { kSourceWidth, kSourceHeight }, false, "fixture");
    overlayFixture(harness, expected, admitted.frame);
    accountForPresents(harness.session->Counters(), "fixture");
    Harness scaled(2560, 1600, true);
    UNSCOPED_INFO("fixture for scaled and letterboxed overlays");
    REQUIRE(scaled.Produce(admitted.frame).success);
    visibleOrOccluded(scaled.Pump());
    overlayFixture(scaled, expected, admitted.frame);
    accountForPresents(scaled.session->Counters(), "scaled overlay fixture");
    report.Add("fixture rgba8-32-tiled: " + std::to_string(expected.size()) + " pixels exact, " + std::to_string(drawn) + " drawn");
}

ComPtr<ID3D12Resource> makeSourceResource(ID3D12Device* device, std::uint32_t width, std::uint32_t height, DXGI_FORMAT format)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = width;
    desc.Height           = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.Format           = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> resource;
    UNSCOPED_INFO("source resource");
    REQUIRE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&resource))));
    return resource;
}

ComPtr<ID3D12Fence> makeFence(ID3D12Device* device, std::uint64_t signaled)
{
    ComPtr<ID3D12Fence> fence;
    UNSCOPED_INFO("fence");
    REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    if (signaled)
    {
        UNSCOPED_INFO("fence signal");
        REQUIRE(SUCCEEDED(fence->Signal(signaled)));
    }
    return fence;
}

void invalidOfferScenario(Report& report)
{
    HiddenWindow             window(kSourceWidth, kSourceHeight);
    NativePresentationConfig config;
    config.nativeWindow = window.Handle();
    std::string error;
    auto        session = NativePresentation::Create(config, error);
    if (!session)
    {
        creationFailed(error);
    }
    auto* device = session->Device();
    if (g_debugLayer)
    {
        hookDebugMessages(device);
    }
    auto          offer              = session->Publisher();
    auto          resource           = makeSourceResource(device, 64, 48, DXGI_FORMAT_R10G10B10A2_UNORM);
    auto          fence              = makeFence(device, 5);
    std::uint64_t expectedRejections = 0;
    const auto    rejects            = [&](ID3D12Resource* source, ID3D12Fence* completion, std::uint64_t value, const char* why)
    {
        ++expectedRejections;
        UNSCOPED_INFO(why);
        REQUIRE(!offer(source, completion, value));
    };
    rejects(nullptr, fence.Get(), 5, "a missing resource is rejected");
    rejects(resource.Get(), nullptr, 5, "a missing fence is rejected");
    rejects(resource.Get(), fence.Get(), 0, "a zero fence value is rejected");
    rejects(resource.Get(), fence.Get(), 6, "an incomplete fence is not publication");
    rejects(resource.Get(), fence.Get(), UINT64_MAX, "the removal sentinel value is rejected");
    auto wrongFormat = makeSourceResource(device, 64, 48, DXGI_FORMAT_R8G8B8A8_UNORM);
    rejects(wrongFormat.Get(), fence.Get(), 5, "a resource with the wrong format is rejected");
    // D3D12 returns one device per adapter, so another adapter is the only
    // way to obtain a device that is not the session's.
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter>  warp;
    ComPtr<ID3D12Device>  foreignDevice;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
        SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&foreignDevice))) && foreignDevice->GetAdapterLuid().LowPart != device->GetAdapterLuid().LowPart)
    {
        auto foreignResource = makeSourceResource(foreignDevice.Get(), 64, 48, DXGI_FORMAT_R10G10B10A2_UNORM);
        auto foreignFence    = makeFence(foreignDevice.Get(), 5);
        rejects(foreignResource.Get(), foreignFence.Get(), 5, "owners from another device are rejected");
        rejects(foreignResource.Get(), fence.Get(), 5, "a resource from another device is rejected");
        rejects(resource.Get(), foreignFence.Get(), 5, "a fence from another device is rejected");
    }
    UNSCOPED_INFO("rejections are counted and leave the host healthy");
    REQUIRE((session->Counters().rejected == expectedRejections && session->Health() == HostHealth::Healthy && session->Counters().offered == 0));

    // A valid source of another extent replaces the display texture.
    auto lease = offer(resource.Get(), fence.Get(), 5);
    UNSCOPED_INFO("a complete native output is accepted");
    REQUIRE((lease && lease->State() == OutputLeaseState::Published));
    auto deadline = OperationDeadline::After(30s);
    session->ArmBackbufferCapture();
    const auto shown = session->PumpOnce(deadline);
    UNSCOPED_INFO("the small source is presented");
    REQUIRE((shown.outcome == PresentOutcome::Presented || shown.outcome == PresentOutcome::Occluded));
    requireRetired(lease, "small source");
    UNSCOPED_INFO("the display texture follows the producer extent");
    REQUIRE(session->DisplayExtent() == SurfaceExtent{ 64, 48 });

    // Removal sentinel: queued offers end and admission stops.
    auto queued = offer(resource.Get(), fence.Get(), 5);
    UNSCOPED_INFO("an offer waits in the queue");
    REQUIRE((queued && queued->State() == OutputLeaseState::Published));
    auto sentinelFence = makeFence(device, 0);
    if (SUCCEEDED(sentinelFence->Signal(UINT64_MAX)) && sentinelFence->GetCompletedValue() == UINT64_MAX)
    {
        UNSCOPED_INFO("a fence that reports the removal sentinel is never success");
        REQUIRE(!offer(resource.Get(), sentinelFence.Get(), 3));
        UNSCOPED_INFO("device loss cancels pending unclaimed offers");
        REQUIRE(queued->State() == OutputLeaseState::Cancelled);
        UNSCOPED_INFO("admission stops after device loss");
        REQUIRE(!offer(resource.Get(), fence.Get(), 5));
        const auto afterLoss = session->PumpOnce(deadline);
        UNSCOPED_INFO("a sentinel reports loss without proving actual device removal");
        REQUIRE((afterLoss.outcome == PresentOutcome::DeviceLost && session->Health() == HostHealth::DeviceLost && session->OwnershipRetained()));
        report.Add("invalid offers: rejected=" + std::to_string(session->Counters().rejected) + ", sentinel handled");
    }
    else
    {
        report.Add("invalid offers: rejected=" + std::to_string(session->Counters().rejected) + ", sentinel signal unavailable");
    }
    const bool reportedLoss = session->Health() == HostHealth::DeviceLost;
    if (reportedLoss)
    {
        UNSCOPED_INFO("the sentinel did not remove the device");
        REQUIRE(SUCCEEDED(device->GetDeviceRemovedReason()));
        session->InjectHostFault(HostOperation::Wait, HostStatus::DeviceLost);
    }
    auto close = OperationDeadline::After(30s);
    session->CloseAdmission(close);
    const auto done = session->Shutdown(close);
    UNSCOPED_INFO("an undrained sentinel loss retains live-device owners");
    REQUIRE((done.retained == reportedLoss && session->OwnershipRetained() == done.retained));
    UNSCOPED_INFO("shutdown preserves its health report");
    REQUIRE(session->Health() == done.health);
}

void copySubmitFailureScenario(Report& report)
{
    Harness    harness(kSourceWidth, kSourceHeight);
    auto&      session = *harness.session;
    const auto gamma   = makeGamma(0);
    session.InjectHostFault(HostOperation::Submit, HostStatus::Failed);
    UNSCOPED_INFO("native completion before the failing copy");
    REQUIRE(harness.Produce(syntheticFrame(gamma)).success);
    const auto lease = harness.LastLease();
    const auto shown = harness.Pump();
    UNSCOPED_INFO("an injected copy submit failure fails the pump");
    REQUIRE(shown.outcome == PresentOutcome::Failed);
    UNSCOPED_INFO("the failed copy terminates its lease and never releases the producer");
    REQUIRE((lease->State() == OutputLeaseState::Failed && lease->Failure() == OutputLeaseFailure::SubmitFailed && !lease->AwaitRetirement(0ms)));
    UNSCOPED_INFO("an unproved submit retains the ownership set");
    REQUIRE((session.Health() == HostHealth::Unproved && session.OwnershipRetained()));
    UNSCOPED_INFO("nothing was shown");
    REQUIRE((session.Counters().copyFailed == 1 && session.Counters().copyRetired == 0 && session.Counters().presentedFrames == 0));
    // No fallback: later frames are rejected, not shown some other way.
    const auto next = harness.Produce(syntheticFrame(gamma));
    UNSCOPED_INFO("the producer sees the closed admission");
    REQUIRE((!next.success && next.error.find("rejected") != std::string::npos));
    UNSCOPED_INFO("the session stays failed");
    REQUIRE(harness.Pump().outcome == PresentOutcome::Failed);
    UNSCOPED_INFO("the late offer is counted as rejected");
    REQUIRE(session.Counters().rejected >= 1);
    const auto done = harness.Finish();
    UNSCOPED_INFO("shutdown retains the unproved owners");
    REQUIRE((done.retained && done.health == HostHealth::Unproved));
    report.Add("copy submit failure: lease failed, ownership retained");
}

void copyTimeoutScenario(Report& report)
{
    Harness harness(kSourceWidth, kSourceHeight);
    auto&   session = *harness.session;
    session.InjectHostFault(HostOperation::Wait, HostStatus::Timeout);
    UNSCOPED_INFO("native completion before the copy wait");
    REQUIRE(harness.Produce(syntheticFrame(makeGamma(0))).success);
    const auto lease = harness.LastLease();
    const auto shown = harness.Pump();
    UNSCOPED_INFO("a copy wait timeout is reported as a timeout");
    REQUIRE(shown.outcome == PresentOutcome::Timeout);
    UNSCOPED_INFO("the timed out copy fails its lease");
    REQUIRE((lease->State() == OutputLeaseState::Failed && lease->Failure() == OutputLeaseFailure::Timeout));
    UNSCOPED_INFO("a live-device timeout retains the ownership set");
    REQUIRE((session.Health() == HostHealth::Unproved && session.OwnershipRetained()));
    const auto done = harness.Finish();
    UNSCOPED_INFO("shutdown after a timeout retains the owners");
    REQUIRE(done.retained);
    report.Add("copy wait timeout: lease failed with timeout, ownership retained");
}

void realCopyTimeoutScenario(Report& report)
{
    Harness harness(kSourceWidth, kSourceHeight);
    auto&   session = *harness.session;
    UNSCOPED_INFO("native completion before gating the host queue");
    REQUIRE(harness.Produce(syntheticFrame(makeGamma(0))).success);
    const auto lease = harness.LastLease();

    struct Gate
    {
        ComPtr<ID3D12Fence> fence;

        ~Gate()
        {
            if (fence)
                fence->Signal(1);
        }
    } gate;

    UNSCOPED_INFO("create the unsignaled host queue gate");
    REQUIRE(SUCCEEDED(session.Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate.fence))));
    auto* queue = detail::NativePresentationTestAccess::Queue(session);
    UNSCOPED_INFO("gate Plume's queue before its copy");
    REQUIRE((queue && SUCCEEDED(queue->Wait(gate.fence.Get(), 1))));
    auto       deadline = OperationDeadline::After(100ms);
    const auto start    = OperationDeadline::Clock::now();
    const auto shown    = session.PumpOnce(deadline);
    UNSCOPED_INFO("the real Plume timed wait returns TIMEOUT");
    REQUIRE((shown.outcome == PresentOutcome::Timeout && std::string(shown.operation) == "wait for slot fence"));
    UNSCOPED_INFO("the copy wait consumes the shared short budget");
    REQUIRE((deadline.Spent(DeadlinePhase::Copy) > 0ns && OperationDeadline::Clock::now() - start < 2s));
    UNSCOPED_INFO("timeout occurred on an unsignaled gate and a live device");
    REQUIRE((gate.fence->GetCompletedValue() == 0 && SUCCEEDED(session.Device()->GetDeviceRemovedReason())));
    UNSCOPED_INFO("timeout fails the actual submitted lease without releasing the source");
    REQUIRE((lease->State() == OutputLeaseState::Failed && lease->Failure() == OutputLeaseFailure::Timeout && !lease->AwaitReuse(0ms)));
    UNSCOPED_INFO("the timed-out copy never presents");
    REQUIRE((session.Counters().copySubmitted == 1 && session.Counters().copyRetired == 0 && session.Counters().hostSubmissions == 0));
    const auto done = harness.Finish(deadline);
    UNSCOPED_INFO("expired shutdown retains the blocked GPU ownership set");
    REQUIRE((!done.drained && done.retained && done.health == HostHealth::Unproved));
    UNSCOPED_INFO("retention remains observable after shutdown");
    REQUIRE((session.Health() == done.health && session.OwnershipRetained()));
    report.Add("real copy timeout: bounded Plume wait, failed lease, retained live-device owners");
}

void resizeZeroAreaRaceScenario(Report& report)
{
    HiddenWindow      window(128, 96);
    PlumeHost::Config config;
    config.nativeWindow = window.Handle();
    std::string error;
    auto        host = PlumeHost::Create(config, error);
    if (!host)
        creationFailed(error);
    if (g_debugLayer)
        hookDebugMessages(host->NativeDevice());
    const auto original = window.Client();
    detail::PlumeHostTestAccess::BeforeResize(*host, [&]
                                              {
                                                  UNSCOPED_INFO("make the client empty after the host's precheck");
                                                  REQUIRE(!window.Resize(0, 0).Valid());
                                              });
    auto          deadline = OperationDeadline::After(2s);
    SurfaceExtent actual;
    bool          performed = true;
    const auto    skipped   = host->ResizeSwapChain(deadline, actual, performed);
    UNSCOPED_INFO("Plume's zero-area false is a healthy skipped resize");
    REQUIRE((skipped.Ok() && !performed && host->Health() == HostHealth::Healthy));
    UNSCOPED_INFO("skipped resize rebuilds the existing backbuffer views");
    REQUIRE((host->Framebuffer(0) && host->Framebuffer(1)));
    window.Resize(original.width, original.height);
    deadline           = OperationDeadline::After(2s);
    const auto resumed = host->ResizeSwapChain(deadline, actual, performed);
    UNSCOPED_INFO("restoring the client recovers from the zero-area race");
    REQUIRE((resumed.Ok() && performed && actual == original && host->Health() == HostHealth::Healthy));
    UNSCOPED_INFO("resumed host drains");
    REQUIRE(host->Drain(deadline).Ok());
    report.Add("resize zero-area race: no resize, rebuilt views, healthy recovery");
}

void producerSlotReuseScenario(Report& report)
{
    {
        Harness harness(kSourceWidth, kSourceHeight);
        UNSCOPED_INFO("first slot publication");
        REQUIRE(harness.Produce(syntheticFrame(makeGamma(0))).success);
        const auto first = harness.LastLease();
        UNSCOPED_INFO("second slot publication");
        REQUIRE(harness.Produce(syntheticFrame(makeGamma(0))).success);
        const auto start = OperationDeadline::Clock::now();
        UNSCOPED_INFO("third frame reuses an unclaimed slot");
        REQUIRE(harness.Produce(syntheticFrame(makeGamma(1))).success);
        UNSCOPED_INFO("a slow host drops the first offer without a five-second stall");
        REQUIRE((OperationDeadline::Clock::now() - start < 2s && first->State() == OutputLeaseState::Cancelled));
        const auto newest = harness.LastLease();
        visibleOrOccluded(harness.Pump());
        requireRetired(newest, "newest frame after slot reuse");
        UNSCOPED_INFO("producer drop and older queue drop count exactly once");
        REQUIRE((harness.session->Counters().dropped == 2 && harness.session->Counters().stale == 0));
        harness.Finish();
    }
    for (bool submitted : { false, true })
    {
        Harness harness(kSourceWidth, kSourceHeight);
        UNSCOPED_INFO("first slot for consumer ownership");
        REQUIRE(harness.Produce(syntheticFrame(makeGamma(0))).success);
        const auto first = harness.LastLease();
        UNSCOPED_INFO("hold the first slot as a consumer");
        REQUIRE(first->Claim());
        if (submitted)
        {
            UNSCOPED_INFO("hold the slot through copy submission");
            REQUIRE(first->MarkCopySubmitted(1));
        }
        UNSCOPED_INFO("second slot while the first is claimed");
        REQUIRE(harness.Produce(syntheticFrame(makeGamma(0))).success);
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        std::string                              error;
        const bool                               succeeded = harness.renderer.QueueRenderFrame(syntheticFrame(makeGamma(1)), ticket, error);
        UNSCOPED_INFO(error);
        REQUIRE(succeeded);
        UNSCOPED_INFO("third frame cannot reuse a consumer-owned slot");
        REQUIRE(ticket->Completion().wait_for(100ms) == std::future_status::timeout);
        auto deadline = OperationDeadline::After(30ms);
        harness.session->CloseAdmission(deadline);
        UNSCOPED_INFO("the shared close deadline ends producer slot polling");
        REQUIRE((ticket->Completion().wait_for(1s) == std::future_status::ready && !ticket->Completion().get().success));
        UNSCOPED_INFO("producer never cancels a claimed or submitted lease");
        REQUIRE(first->State() == (submitted ? OutputLeaseState::CopySubmitted : OutputLeaseState::Claimed));
        const auto start = OperationDeadline::Clock::now();
        harness.Finish(deadline);
        UNSCOPED_INFO("renderer shutdown does not restart the lease wait budget");
        REQUIRE(OperationDeadline::Clock::now() - start < 1s);
    }
    report.Add("producer slot reuse: published offers drop; claimed and submitted offers block until the shared deadline");
}

void deviceLostScenario(Report& report)
{
    Harness harness(kSourceWidth, kSourceHeight);
    auto&   session = *harness.session;
    session.InjectHostFault(HostOperation::Wait, HostStatus::DeviceLost);
    UNSCOPED_INFO("native completion before device loss");
    REQUIRE(harness.Produce(syntheticFrame(makeGamma(0))).success);
    const auto lease = harness.LastLease();
    const auto shown = harness.Pump();
    UNSCOPED_INFO("a device loss during the copy is reported");
    REQUIRE(shown.outcome == PresentOutcome::DeviceLost);
    UNSCOPED_INFO("a claimed lease fails with the device loss");
    REQUIRE((lease->State() == OutputLeaseState::Failed && lease->Failure() == OutputLeaseFailure::DeviceLost && !lease->AwaitRetirement(0ms)));
    UNSCOPED_INFO("reported loss on a live device conservatively retains ownership");
    REQUIRE((session.Health() == HostHealth::DeviceLost && session.OwnershipRetained()));
    const auto next = harness.Produce(syntheticFrame(makeGamma(1)));
    UNSCOPED_INFO("admission stops after a device loss");
    REQUIRE(!next.success);
    UNSCOPED_INFO("the session stays lost");
    REQUIRE(harness.Pump().outcome == PresentOutcome::DeviceLost);
    session.InjectHostFault(HostOperation::Wait, HostStatus::DeviceLost);
    const auto done = harness.Finish();
    UNSCOPED_INFO("undrained reported loss cannot release a live device's owners");
    REQUIRE((!done.drained && done.retained && done.health == HostHealth::Unproved));
    UNSCOPED_INFO("post-shutdown health and retention preserve the result");
    REQUIRE((session.OwnershipRetained() && session.Health() == done.health));
    report.Add("reported device loss: lease failed, live-device owners retained");
}

void presentResultScenario(Report& report)
{
    Harness    harness(kSourceWidth, kSourceHeight);
    auto&      session = *harness.session;
    const auto gamma   = makeGamma(0);
    UNSCOPED_INFO("frame for present results");
    REQUIRE(harness.Produce(syntheticFrame(gamma)).success);
    session.InjectHostFault(HostOperation::Present, HostStatus::Occluded);
    const auto occluded = harness.Pump();
    UNSCOPED_INFO("an occluded present is reported, not hidden");
    REQUIRE((occluded.outcome == PresentOutcome::Occluded && occluded.newFrame));
    UNSCOPED_INFO("occlusion is not a visible presentation");
    REQUIRE((session.Counters().occluded == 1 && session.Counters().presentedFrames == 0 && session.Health() == HostHealth::Healthy));
    requireRetired(harness.LastLease(), "occluded frame");
    session.InjectHostFault(HostOperation::Present, HostStatus::OutOfDate);
    const auto stale = harness.Repaint();
    UNSCOPED_INFO("an out-of-date present asks for a resize");
    REQUIRE((stale.outcome == PresentOutcome::OutOfDate && session.Counters().outOfDate == 1 && session.Health() == HostHealth::Healthy));
    session.InjectHostFault(HostOperation::Present, HostStatus::Failed);
    const auto failed = harness.Repaint();
    UNSCOPED_INFO("a present failure is terminal without retention");
    REQUIRE((failed.outcome == PresentOutcome::Failed && session.Health() == HostHealth::Failed && !session.OwnershipRetained()));
    UNSCOPED_INFO("no frame is admitted after a present failure");
    REQUIRE(harness.Produce(syntheticFrame(gamma)).success == false);
    harness.Finish();
    report.Add("present results: occluded, out-of-date and failed are distinct");
}

void drainTimeoutScenario(Report& report)
{
    Harness harness(kSourceWidth, kSourceHeight);
    auto&   session = *harness.session;
    UNSCOPED_INFO("frame before the drain");
    REQUIRE(harness.Produce(syntheticFrame(makeGamma(0))).success);
    visibleOrOccluded(harness.Pump());
    session.InjectHostFault(HostOperation::Wait, HostStatus::Timeout);
    const auto done = harness.Finish();
    UNSCOPED_INFO("an unproved drain is reported and retains the owners");
    REQUIRE((!done.drained && done.retained && done.health == HostHealth::Unproved));
    report.Add("drain timeout: reported undrained, ownership retained");
}

void run(const Options& options)
{
    overlaySnapshotChecks();
    if (options.only == "overlay-unit")
    {
        std::cout << "overlay snapshot and lifetime checks passed\n";
        return;
    }
    Report report;
    hostMappingTable();
    if (options.debugLayer)
    {
        g_debugLayer = true;
        NativeRendererD3D12::ConfigureReplayDiagnostics();
    }
    if (options.fixture)
    {
        fixtureScenario(report, options);
    }
    else
    {
        const auto scenario = [&](const char* name, void (*body)(Report&))
        {
            if (options.only.empty() || options.only == name)
            {
                body(report);
            }
        };
        scenario("baseline", baselineScenario);
        scenario("gated", gatedPublicationScenario);
        scenario("quantization", quantizationScenario);
        scenario("nearest", nearestScalingScenario);
        scenario("invalid", invalidOfferScenario);
        scenario("submit-failure", copySubmitFailureScenario);
        scenario("copy-timeout", copyTimeoutScenario);
        scenario("real-copy-timeout", realCopyTimeoutScenario);
        scenario("slot-reuse", producerSlotReuseScenario);
        scenario("resize-zero-race", resizeZeroAreaRaceScenario);
        scenario("device-lost", deviceLostScenario);
        scenario("present-results", presentResultScenario);
        scenario("drain-timeout", drainTimeoutScenario);
        scenario("idle-canvas", idleCanvasScenario);
    }
    UNSCOPED_INFO("the D3D12 debug layer reported " + std::to_string(g_debugErrors.load()) + " errors or warnings");
    REQUIRE(g_debugErrors.load() == 0);
    std::cout << "native presentation d3d12 checks passed" << std::endl;
}

} // namespace

// Runs one presentation configuration. An unavailable GPU, window or shader
// catalog is a skip; any other failure is a defect.
static void runConfiguration(Options options)
{
    if (options.fixture && options.catalog.empty())
    {
        char buffer[4096]{};
        if (GetEnvironmentVariableA("REREVVED_NATIVE_PRESENT_CATALOG", buffer, sizeof(buffer)) != 0)
        {
            options.catalog = buffer;
        }
    }
    if (options.fixture && (options.catalog.empty() || !std::filesystem::is_directory(options.catalog)))
    {
        SKIP("Set REREVVED_NATIVE_PRESENT_CATALOG to a prepared shader catalog directory to run the fixture presentation");
    }
    rerevved::tests::ScopedLogging logging;
    try
    {
        run(options);
    }
    catch (const Unavailable& unavailable)
    {
        SKIP("native presentation d3d12 unavailable: " << unavailable.reason);
    }
}

TEST_CASE("native_presentation_d3d12", "[ctest][release-smoke][native-presentation-d3d12][native_gpu][timeout-180]")
{
    runConfiguration({});
}

TEST_CASE("native_overlay_snapshot", "[ctest][release-smoke][native-presentation-d3d12]")
{
    Options options;
    options.only = "overlay-unit";
    runConfiguration(options);
}

TEST_CASE("native_presentation_d3d12_fixture", "[ctest][release-smoke][native-presentation-d3d12][native_gpu][timeout-180]")
{
    Options options;
    options.fixture = true;
    runConfiguration(options);
}

// Runs a chosen configuration:
//   native-presentation-d3d12.run [--fixture] [--catalog <dir>] [--debug-layer] [--only <scenario>]
TEST_CASE("native_presentation_d3d12_options", "[.][manual][native-presentation-d3d12]")
{
    auto&   args = rerevved::tests::CurrentManualArgv();
    Options options;
    for (int index = 1; index <= args.argc(); ++index)
    {
        const std::string argument = args.argv()[index];
        if (argument == "--fixture")
        {
            options.fixture = true;
        }
        else if (argument == "--catalog" && index < args.argc())
        {
            options.catalog = args.argv()[++index];
        }
        else if (argument == "--debug-layer")
        {
            options.debugLayer = true;
        }
        else if (argument == "--only" && index < args.argc())
        {
            options.only = args.argv()[++index];
        }
        else
        {
            FAIL("usage: native-presentation-d3d12.run [--fixture] [--catalog <dir>] [--debug-layer] [--only <scenario>]");
        }
    }
    runConfiguration(options);
}

REREVVED_MANUAL_HELPER(nativePresentationD3d12Run, "native-presentation-d3d12.run", "native_presentation_d3d12_options")
