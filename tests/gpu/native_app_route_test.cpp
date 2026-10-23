#include "gpu/present/native_app_drain.h"
#include "gpu/present/native_app_route.h"
#include "gpu/present/offer_gate.h"
#include <map>
#include <variant>

#include <catch2/catch_test_macros.hpp>

using namespace rerevved::gpu;

struct Flags
{
    std::map<std::string, std::variant<std::string, bool, std::uint32_t>, std::less<>> values;

    template <typename T>
    T operator()(std::string_view name, T) const
    {
        const auto it = values.find(name);
        return it == values.end() ? T{} : std::get<T>(it->second);
    }
};

TEST_CASE("native_app_route", "[ctest][native-app-route]")
{
    NativeAppTrace           trace;
    std::vector<std::string> events;
    auto                     observe = [&](PresentResult result)
    {
        trace.Observe(result, true, [&](const char* name, std::uint64_t, std::uint64_t)
                      {
                          events.emplace_back(name);
                      });
    };
    PresentResult idle{ PresentOutcome::Presented };
    for (std::uint64_t sequence = 1; sequence <= 100; ++sequence)
    {
        idle.overlaySequence = sequence;
        observe(idle);
    }
    UNSCOPED_INFO("idle ticks deduplicate empty overlay snapshots");
    REQUIRE(events == std::vector<std::string>{ "idle_present", "overlay_cleared" });
    PresentResult shown{ PresentOutcome::Presented, 1 };
    shown.gammaGeneration = 7;
    observe(shown);
    events.clear();
    for (std::uint64_t sequence = 101; sequence <= 600; ++sequence)
    {
        shown.overlaySequence = sequence;
        observe(shown);
        observe(PresentResult{ PresentOutcome::NoFrame });
    }
    UNSCOPED_INFO("static frames and NoFrame gaps produce no trace spam or idle events after a guest frame");
    REQUIRE(events.empty());
    shown.overlaySequence++;
    shown.overlayDraws = 1;
    observe(shown);
    shown.overlaySequence++;
    observe(shown);
    UNSCOPED_INFO("a drawn overlay traces once on a static frame");
    REQUIRE(events == std::vector<std::string>{ "overlay_composed" });
    events.clear();
    shown.frameId++;
    observe(shown);
    observe(shown);
    UNSCOPED_INFO("a drawn overlay traces each advancing frame even with the same overlay snapshot");
    REQUIRE(events == std::vector<std::string>{ "present", "gamma", "overlay_composed" });
    events.clear();
    shown.frameId++;
    shown.overlaySequence++;
    observe(shown);
    shown.overlaySequence++;
    observe(shown);
    UNSCOPED_INFO("new overlay snapshots on the same frame do not repeat composition events");
    REQUIRE(events == std::vector<std::string>{ "present", "gamma", "overlay_composed" });
    events.clear();
    shown.overlaySequence++;
    shown.overlayDraws = 0;
    observe(shown);
    UNSCOPED_INFO("a drawn-to-empty transition traces once");
    REQUIRE(events == std::vector<std::string>{ "overlay_cleared" });
    events.clear();
    shown.frameId++;
    observe(shown);
    shown.gammaGeneration++;
    observe(shown);
    shown.outcome = PresentOutcome::Occluded;
    observe(shown);
    observe(shown);
    UNSCOPED_INFO("frame, gamma and outcome changes each trace once");
    REQUIRE(events == std::vector<std::string>{ "present", "gamma", "present", "gamma", "present", "gamma" });

    Flags flags;
    flags.values["native_present_fixture"] = std::string("catalog");
    UNSCOPED_INFO("native fixture admitted");
    REQUIRE(AdmitNativeAppRoute(true, flags).empty());
    UNSCOPED_INFO("xenos unchanged");
    REQUIRE(AdmitNativeAppRoute(false, Flags{}).empty());
    UNSCOPED_INFO("xenos fixture rejected");
    REQUIRE(!AdmitNativeAppRoute(false, flags).empty());
    UNSCOPED_INFO("native without fixture boots the guest");
    REQUIRE(AdmitNativeAppRoute(true, Flags{}).empty());
#if REREVVED_DEVELOPER
    Flags guestTrace;
    guestTrace.values["native_present_trace"] = std::string("events.toml");
    UNSCOPED_INFO("native guest boot admits a trace");
    REQUIRE(AdmitNativeAppRoute(true, guestTrace).empty());
    guestTrace.values["native_present_script"] = std::string("lifecycle");
    guestTrace.values["window_hidden"]         = true;
    UNSCOPED_INFO("lifecycle script requires the fixture");
    REQUIRE(AdmitNativeAppRoute(true, guestTrace).find("native_present_fixture") != std::string::npos);
#endif
    Flags panel;
    panel.values["native_draw_panel_capture"] = std::string("capture");
    UNSCOPED_INFO("native guest boot admits the panel capture");
    REQUIRE(AdmitNativeAppRoute(true, panel).empty());
    UNSCOPED_INFO("the panel capture is rejected outside renderer=native");
    REQUIRE(AdmitNativeAppRoute(false, panel).find("native_draw_panel_capture") != std::string::npos);
    panel.values["native_present_fixture"] = std::string("catalog");
    UNSCOPED_INFO("the panel capture is rejected with the fixture");
    REQUIRE(AdmitNativeAppRoute(true, panel).find("native_draw_panel_capture") != std::string::npos);
    for (const auto option : { "native_guest_adapter_output", "native_route_trace", "native_scene_capture" })
    {
        auto enabled           = flags;
        enabled.values[option] = std::string("enabled");
        UNSCOPED_INFO(option);
        REQUIRE(AdmitNativeAppRoute(true, enabled).find(option) != std::string::npos);
    }
    for (const auto option : { "native_scene_capture_payloads", "native_scene_capture_effects", "native_scene_capture_targets", "xenos_frame_counter" })
    {
        auto enabled           = flags;
        enabled.values[option] = true;
        UNSCOPED_INFO(option);
        REQUIRE(AdmitNativeAppRoute(true, enabled).find(option) != std::string::npos);
        enabled.values[option] = false;
        UNSCOPED_INFO("false booleans remain admitted");
        REQUIRE(AdmitNativeAppRoute(true, enabled).empty());
    }
#if REREVVED_DEVELOPER
    for (const auto option : { "native_present_trace", "native_present_script" })
    {
        Flags xenos;
        xenos.values[option] = std::string("enabled");
        UNSCOPED_INFO("xenos rejects fixture controls");
        REQUIRE(!AdmitNativeAppRoute(false, xenos).empty());
    }
    flags.values["native_present_script"] = std::string("lifecycle");
    UNSCOPED_INFO("script requires trace and hidden window");
    REQUIRE(!AdmitNativeAppRoute(true, flags).empty());
    flags.values["native_present_trace"] = std::string("events.toml");
    UNSCOPED_INFO("script requires hidden window");
    REQUIRE(!AdmitNativeAppRoute(true, flags).empty());
    flags.values["window_hidden"] = true;
    UNSCOPED_INFO("hidden traced script admitted");
    REQUIRE(AdmitNativeAppRoute(true, flags).empty());
    flags.values.erase("native_present_trace");
    UNSCOPED_INFO("hidden script still requires trace");
    REQUIRE(!AdmitNativeAppRoute(true, flags).empty());
#endif

    NativeAppClose           close;
    std::vector<std::string> order;
    auto                     overlays = [&]
    {
        order.push_back("ShutdownOverlays");
    };
    close.Complete(overlays);
    UNSCOPED_INFO("completion before close cannot tear down overlays");
    REQUIRE(order.empty());
    UNSCOPED_INFO("first close veto starts drain");
    REQUIRE(close.Request() == NativeAppClose::Decision::StartDrain);
    NativeAppDrain drain;
    auto           admission = [&](OperationDeadline&)
    {
        order.push_back("CloseAdmission");
    };
    auto stop = [&]
    {
        order.push_back("request producer stop");
    };
    UNSCOPED_INFO("first close starts drain");
    REQUIRE(drain.Start(admission, stop));
    const auto completion = drain.GetCompletion();
    UNSCOPED_INFO("repeated close during drain vetoes");
    REQUIRE(close.Request() == NativeAppClose::Decision::Veto);
    UNSCOPED_INFO("repeated close cannot restart drain");
    REQUIRE(!drain.Start(admission, stop));
    UNSCOPED_INFO("repeated close preserves deadline");
    REQUIRE(drain.GetCompletion().deadline.Expiry() == completion.deadline.Expiry());
    int  callbacks = 0;
    auto completed = [&](bool result)
    {
        UNSCOPED_INFO("drain result delivered");
        REQUIRE(result);
        ++callbacks;
    };
    drain.Subscribe(completed);
    drain.Subscribe(completed);
    UNSCOPED_INFO("callbacks wait for drain");
    REQUIRE(callbacks == 0);
    UNSCOPED_INFO("shutdown waits for pending close");
    REQUIRE(completion.result.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
    const bool result = drain.Run([&]
                                  {
                                      order.push_back("stop producer");
                                  },
                                  [&](OperationDeadline& deadline)
                                  {
                                      UNSCOPED_INFO("shutdown uses close deadline");
                                      REQUIRE(deadline.Expiry() == completion.deadline.Expiry());
                                      order.push_back("Shutdown");
                                      return true;
                                  });
    drain.Complete(result);
    UNSCOPED_INFO("all pending close callers complete");
    REQUIRE((completion.result.get() && callbacks == 2));
    drain.Subscribe(completed);
    UNSCOPED_INFO("late close callback completes immediately");
    REQUIRE(callbacks == 3);
    drain.Complete(false);
    UNSCOPED_INFO("completed result is immutable");
    REQUIRE((completion.result.get() && callbacks == 3));
    close.Complete(overlays);
    UNSCOPED_INFO("drained close accepted");
    REQUIRE(close.Request() == NativeAppClose::Decision::Accept);
    order.push_back("accept");
    close.Complete(overlays);
    UNSCOPED_INFO("repeated completed close accepted");
    REQUIRE(close.Request() == NativeAppClose::Decision::Accept);
    UNSCOPED_INFO("close operations preserve ordering and run once");
    REQUIRE(order == std::vector<std::string>{ "CloseAdmission", "request producer stop", "stop producer", "Shutdown", "ShutdownOverlays", "accept" });
    NativeAppDrain failedDrain;
    failedDrain.Start([](OperationDeadline&)
                      {
                      },
                      []
                      {
                      });
    failedDrain.Complete(false);
    bool failureDelivered = false;
    failedDrain.Subscribe([&](bool value)
                          {
                              failureDelivered = !value;
                          });
    UNSCOPED_INFO("late close preserves failed drain result");
    REQUIRE((failureDelivered && !failedDrain.GetCompletion().result.get()));

    OfferGate gate;
    gate.gammaGeneration = 99;
    auto first           = gate.offerLocked({}, 7);
    gate.gammaGeneration = 100;
    auto second          = gate.offerLocked({}, 8);
    UNSCOPED_INFO("publication generation overrides mutable gate state");
    REQUIRE((first->GammaGeneration() == 7 && second->GammaGeneration() == 8));
    UNSCOPED_INFO("copied frame retires");
    REQUIRE((first->Claim() && first->MarkCopySubmitted(1) && first->Retire()));
    UNSCOPED_INFO("retirement and repaint identity retain gamma generation");
    REQUIRE(first->GammaGeneration() == 7);
}
