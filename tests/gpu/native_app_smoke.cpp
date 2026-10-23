// Bounded lifecycle smoke of the native presentation session on a hidden
// window: fixture frames, resize, minimize and restore, an overlay that
// composes and clears, and one shared close drain. It drives the real
// NativeAppSession and NativePresentation, not the full app, and reports PASS,
// FAIL or SKIP (exit 0, 1 or 77) on stdout for tests/gpu/native_app_harness.py.
#include "gpu/present/native_app_session.h"
#include "gpu/present/native_immediate_drawer.h"
#include "tools/presentation_fixture.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <Windows.h>

using namespace rerevved::gpu;

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int kSkip = 77;

struct Recorded
{
    std::string   name;
    std::uint64_t frame, value;
};

class Collector final : public NativeAppEventSink
{
public:
    void Event(const char* name, std::uint64_t frame, std::uint64_t value) override
    {
        std::lock_guard lock(mutex_);
        events_.push_back({ name, frame, value });
    }

    std::vector<Recorded> Events() const
    {
        std::lock_guard lock(mutex_);
        return events_;
    }

private:
    mutable std::mutex    mutex_;
    std::vector<Recorded> events_;
};

// Written by the presentation worker's tick, read by the driving thread.
struct Progress
{
    std::mutex              mutex;
    std::condition_variable changed;
    std::uint64_t           newFrames      = 0;
    std::uint64_t           clearedResults = 0;
    bool                    suspended      = false;
    bool                    overlayClosed  = false;
    bool                    failed         = false;
    std::set<std::uint64_t> overlayFrames;
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_GETMINMAXINFO)
    {
        auto* info           = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMaxTrackSize = { 16384, 16384 };
        info->ptMaxSize      = { 16384, 16384 };
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND CreateHiddenWindow(std::uint32_t width, std::uint32_t height)
{
    const wchar_t* className = L"RerevvedNativeAppSmoke";
    WNDCLASSEXW    windowClass{};
    windowClass.cbSize        = sizeof(windowClass);
    windowClass.lpfnWndProc   = WindowProc;
    windowClass.hInstance     = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    RECT rect{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, className, L"ReRevved native app smoke", WS_OVERLAPPEDWINDOW, 0, 0, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);
}

SurfaceExtent ClientExtent(HWND window)
{
    RECT client{};
    GetClientRect(window, &client);
    return { static_cast<std::uint32_t>(client.right - client.left), static_cast<std::uint32_t>(client.bottom - client.top) };
}

bool Fail(const std::string& message)
{
    std::printf("FAIL: %s\n", message.c_str());
    return false;
}

// Waits for the worker to satisfy `ready` while keeping the window's message queue serviced.
template <typename Ready>
bool WaitFor(Progress& progress, Clock::time_point deadline, Ready ready, const char* what)
{
    for (;;)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::unique_lock lock(progress.mutex);
        if (progress.failed)
            return Fail(std::string("presentation failed before ") + what);
        if (ready())
            return true;
        if (Clock::now() >= deadline)
            return Fail(std::string("timed out waiting for ") + what);
        progress.changed.wait_for(lock, std::chrono::milliseconds(5));
    }
}

std::vector<std::uint64_t> FramesOf(const std::vector<Recorded>& events, const char* name, std::size_t from, std::size_t to)
{
    std::vector<std::uint64_t> frames;
    for (std::size_t i = from; i < to; ++i)
        if (events[i].name == name)
            frames.push_back(events[i].frame);
    return frames;
}

bool Strictly(const std::vector<std::uint64_t>& values)
{
    return std::adjacent_find(values.begin(), values.end(), [](auto a, auto b)
                              {
                                  return a >= b;
                              }) == values.end();
}

bool CheckEvents(const std::vector<Recorded>& events)
{
    std::vector<std::string> names;
    for (const auto& event : events)
        names.push_back(event.name);
    const auto count = [&](const char* name)
    {
        return static_cast<std::size_t>(std::count(names.begin(), names.end(), name));
    };
    if (count("startup_failed") || count("producer_failed"))
        return Fail("startup or producer failed");
    const char* const expected[] = { "startup", "published", "present", "script_resize", "resize", "script_minimize", "suspend", "script_restore", "restore", "script_overlay_open", "overlay_composed", "script_overlay_close", "overlay_cleared", "script_close", "close_admission", "producer_stopped", "shutdown" };
    std::size_t       cursor     = 0;
    for (const char* name : expected)
    {
        const auto at = std::find(names.begin() + static_cast<std::ptrdiff_t>(cursor), names.end(), name);
        if (at == names.end())
            return Fail(std::string("missing ordered event ") + name);
        cursor = static_cast<std::size_t>(at - names.begin()) + 1;
    }
    const auto at = [&](const char* name, std::size_t from = 0)
    {
        return static_cast<std::size_t>(std::find(names.begin() + static_cast<std::ptrdiff_t>(from), names.end(), name) - names.begin());
    };
    const auto opened    = at("script_overlay_open");
    const auto closed    = at("script_overlay_close");
    const auto overlay   = FramesOf(events, "overlay_composed", opened, closed);
    const auto windowed  = FramesOf(events, "published", opened, closed);
    const auto published = FramesOf(events, "published", 0, events.size());
    if (overlay.size() < 2 || !Strictly(overlay) || !Strictly(windowed) || !std::includes(windowed.begin(), windowed.end(), overlay.begin(), overlay.end()))
        return Fail("overlays must compose on at least two advancing published frames");
    if (published.size() < 9 || !Strictly(std::vector<std::uint64_t>(published)))
        return Fail("fewer than nine distinct published frames");
    if (std::count(names.begin() + static_cast<std::ptrdiff_t>(opened), names.end(), "overlay_cleared") != 1)
        return Fail("overlay clear was not deduplicated");
    std::size_t presentsAfterClear = 0;
    for (std::size_t i = at("overlay_cleared", closed); i < events.size(); ++i)
        presentsAfterClear += events[i].name == "present";
    if (presentsAfterClear < 2)
        return Fail("missing continued native presentation after overlay clear");
    std::set<std::uint64_t> shown, extents;
    std::set<std::uint64_t> publishedSet(published.begin(), published.end());
    bool                    suspended = false;
    for (const auto& event : events)
    {
        if (event.name == "present")
        {
            // DXGI occlusion is allowed for a hidden window, but is never counted as a shown frame.
            if (event.value > 3)
                return Fail("unexpected present outcome " + std::to_string(event.value));
            suspended |= event.value == 3;
            if (event.value <= 1)
                shown.insert(event.frame);
        }
        else if (event.name == "gamma" && event.frame && event.value != 1)
            return Fail("incorrect gamma generation");
        else if (event.name == "surface")
            extents.insert((event.frame << 32) | event.value);
    }
    if (!suspended)
        return Fail("missing suspended pump");
    if (extents.size() < 2)
        return Fail("swapchain did not resize");
    if (shown != publishedSet)
        return Fail("shown frames differ from published frames");
    if (count("close_admission") != 1)
        return Fail("repeated close did not share one drain");
    for (const auto& event : events)
        if (event.name == "shutdown" && event.value != 1)
            return Fail("shutdown did not drain successfully");
    std::printf("PASS: %zu native fixture frames, resize, suspend/restore, overlay compose/clear and bounded close\n", published.size());
    return true;
}

int Run(const char* catalog)
{
    const auto deadline  = Clock::now() + std::chrono::seconds(35);
    const auto collector = std::make_shared<Collector>();
    HWND       window    = CreateHiddenWindow(1280, 720);
    if (!window)
    {
        std::printf("SKIP: GPU/window unavailable\n");
        return kSkip;
    }
    NativeRenderFrame frame;
    std::string       error;
    if (!LoadPresentationFixture(catalog, frame, error))
    {
        Fail("fixture catalog: " + error);
        return 1;
    }
    auto session = NativeAppSession::Create(window, std::move(frame), collector, error);
    if (!session)
    {
        for (const auto& event : collector->Events())
            if (event.name == "unavailable")
            {
                std::printf("SKIP: GPU/window unavailable\n");
                return kSkip;
            }
        Fail("session startup: " + error);
        return 1;
    }
    auto mailbox = std::make_shared<NativeOverlayMailbox>();
    session->SetOverlayMailbox(mailbox);
    NativeImmediateDrawer drawer(mailbox);
    Progress              progress;
    std::atomic<bool>     closing{ false };
    session->Event("detached", 0, 1);
    session->Start([&progress, &closing, raw = session.get()](PresentResult result)
                   {
                       {
                           std::lock_guard lock(progress.mutex);
                           progress.newFrames += result.newFrame;
                           progress.suspended |= result.outcome == PresentOutcome::Suspended;
                           if (result.overlayDraws)
                               progress.overlayFrames.insert(result.frameId);
                           if (progress.overlayClosed && result.overlaySequence && !result.overlayDraws)
                               ++progress.clearedResults;
                           progress.failed |= result.outcome == PresentOutcome::Failed || result.outcome == PresentOutcome::DeviceLost || result.outcome == PresentOutcome::Timeout;
                       }
                       progress.changed.notify_all();
                       if (!closing.load())
                           raw->Advance();
                   });
    const auto frames = [&](std::uint64_t count)
    {
        return [&progress, count]
        {
            return progress.newFrames >= count;
        };
    };
    bool ok = WaitFor(progress, deadline, frames(1), "the first frame");
    if (ok)
    {
        session->Event("script_resize");
        SetWindowPos(window, nullptr, 0, 0, 960, 640, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        session->RequestExtent(ClientExtent(window));
        ok = WaitFor(progress, deadline, frames(2), "a second frame");
    }
    if (ok)
    {
        session->Event("script_minimize");
        session->SetMinimized(true);
        ok = WaitFor(progress, deadline, [&]
                     {
                         return progress.suspended;
                     },
                     "a suspended pump");
    }
    if (ok)
    {
        session->Event("script_restore");
        session->SetMinimized(false);
        session->RequestExtent(ClientExtent(window));
        session->RequestRepaint();
        ok = WaitFor(progress, deadline, frames(4), "frames after restore");
    }
    if (ok)
    {
        session->Event("script_overlay_open");
        ok = WaitFor(progress, deadline, [&]
                     {
                         return mailbox->Ready();
                     },
                     "overlay admission");
    }
    if (ok)
    {
        rex::ui::AppUIDrawContext      context(256, 128);
        const rex::ui::ImmediateVertex vertices[] = { { 8, 8, 0, 0, 0x80FFFFFF }, { 40, 8, 1, 0, 0x80FFFFFF }, { 40, 40, 1, 1, 0x80FFFFFF }, { 8, 40, 0, 1, 0x80FFFFFF } };
        const std::uint16_t            indices[]  = { 0, 1, 2, 0, 2, 3 };
        drawer.Begin(context, 256, 128);
        drawer.BeginDrawBatch({ vertices, 4, indices, 6 });
        rex::ui::ImmediateDraw draw;
        draw.count = 6;
        drawer.Draw(draw);
        drawer.EndDrawBatch();
        drawer.End();
        session->RequestRepaint();
        ok = WaitFor(progress, deadline, [&]
                     {
                         return progress.overlayFrames.size() >= 2;
                     },
                     "overlay composition on two frames");
    }
    if (ok)
    {
        session->Event("script_overlay_close");
        {
            std::lock_guard lock(progress.mutex);
            progress.overlayClosed = true;
        }
        drawer.Clear();
        session->RequestRepaint();
        ok = WaitFor(progress, deadline, [&]
                     {
                         return progress.clearedResults >= 3;
                     },
                     "the overlay to clear");
    }
    bool drained = false;
    if (ok)
    {
        session->Event("script_close");
        closing.store(true);
        const auto first  = session->Close();
        const auto second = session->Close();
        drained           = first.result.wait_until(first.deadline.Expiry()) == std::future_status::ready && first.result.get() && second.result.get();
        if (!drained)
            ok = Fail("close drain did not complete");
    }
    if (!drained)
    {
        // A timed-out drain leaves workers that may still reference the session.
        (void)new std::shared_ptr<NativeAppSession>(session);
        return 1;
    }
    session.reset();
    return CheckEvents(collector->Events()) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("SKIP: shader catalog was not given\n");
        return kSkip;
    }
    return Run(argv[1]);
}
