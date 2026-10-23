#pragma once

#include "native_presentation.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace rerevved::gpu
{

// Trace presentation changes, not the UI tick or overlay publication cadence.
class NativeAppTrace
{
public:
    template <typename Event>
    void Observe(const PresentResult& result, bool idle, Event event)
    {
        seenFrame_ |= result.frameId != 0;
        if (idle && !seenFrame_)
        {
            if (lastOutcome_ != result.outcome)
                event("idle_present", 0, static_cast<std::uint64_t>(result.outcome));
            lastOutcome_ = result.outcome;
        }
        else if (result.outcome != PresentOutcome::NoFrame)
        {
            if (lastFrame_ != result.frameId || lastOutcome_ != result.outcome || lastGamma_ != result.gammaGeneration)
            {
                event("present", result.frameId, static_cast<std::uint64_t>(result.outcome));
                event("gamma", result.frameId, result.gammaGeneration);
            }
            lastFrame_   = result.frameId;
            lastOutcome_ = result.outcome;
            lastGamma_   = result.gammaGeneration;
        }
        if (result.overlaySequence)
        {
            const bool drawn = result.overlayDraws != 0;
            if (lastOverlayDrawn_ != drawn || (drawn && lastOverlayFrame_ != result.frameId))
                event(drawn ? "overlay_composed" : "overlay_cleared", result.frameId, result.overlaySequence);
            lastOverlayDrawn_ = drawn;
            lastOverlayFrame_ = result.frameId;
        }
    }

private:
    std::optional<PresentOutcome> lastOutcome_;
    std::optional<bool>           lastOverlayDrawn_;
    std::uint64_t                 lastFrame_ = 0, lastGamma_ = 0, lastOverlayFrame_ = 0;
    bool                          seenFrame_ = false;
};

template <typename Lookup>
std::string AdmitNativeAppRoute(bool native, Lookup lookup)
{
    const bool fixture = !lookup("native_present_fixture", std::string{}).empty();
#if REREVVED_DEVELOPER
    const bool trace  = !lookup("native_present_trace", std::string{}).empty();
    const auto script = lookup("native_present_script", std::string{});
#else
    constexpr bool    trace = false;
    const std::string script;
#endif
    const bool panel = !lookup("native_draw_panel_capture", std::string{}).empty();
    if (!native)
    {
        if (panel)
            return "native_draw_panel_capture requires renderer=native.";
        return fixture || trace || !script.empty() ? "Native presentation fixture controls require renderer=native." : "";
    }
    if (panel && fixture)
        return "native_draw_panel_capture requires a guest boot without native_present_fixture.";
    for (const auto name : { "native_guest_adapter_output", "native_route_trace", "native_scene_capture" })
        if (!lookup(name, std::string{}).empty())
            return "renderer=native rejects hybrid/live option: " + std::string(name);
    for (const auto name : { "native_scene_capture_payloads", "native_scene_capture_effects", "native_scene_capture_targets", "xenos_frame_counter" })
        if (lookup(name, bool{}))
            return "renderer=native rejects hybrid/live option: " + std::string(name);
#if REREVVED_DEVELOPER
    // Without a fixture, renderer=native boots the guest on title video services.
    if (!script.empty() && !fixture)
        return "native_present_script requires native_present_fixture.";
    if (!script.empty() && (!lookup("window_hidden", bool{}) || !trace))
        return "native_present_script requires window_hidden=true and native_present_trace.";
#endif
    return {};
}

// UI-thread state: repeated requests cannot start a second drain.
class NativeAppClose
{
public:
    enum class Decision
    {
        StartDrain,
        Veto,
        Accept
    };

    Decision Request()
    {
        if (ready_)
            return Decision::Accept;
        if (started_)
            return Decision::Veto;
        started_ = true;
        return Decision::StartDrain;
    }

    template <typename ShutdownOverlays>
    void Complete(ShutdownOverlays shutdownOverlays)
    {
        if (started_ && !ready_)
        {
            shutdownOverlays();
            ready_ = true;
        }
    }

private:
    bool started_ = false;
    bool ready_   = false;
};

} // namespace rerevved::gpu
