#include "cvar_scope.hpp"
#include "gotw_hooks.h"
#include "gotw_settings.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <spdlog/sinks/ostream_sink.h>

REXCVAR_DECLARE(bool, gotw_custom_week);
REXCVAR_DECLARE(std::string, gotw_week);
REXCVAR_DECLARE(bool, gotw_custom_civilization);
REXCVAR_DECLARE(int32_t, gotw_civilization);
REXCVAR_DECLARE(bool, gotw_standard_rules);
REXCVAR_DECLARE(bool, gotw_trace);

namespace
{

namespace session = rerevved::gameplay_session;
namespace host    = rerevved::host_settings;

struct RestoreNetwork
{
    std::shared_ptr<const host::HostSettings> previous = session::ForNetworkGame();

    RestoreNetwork()
    {
        session::ResetOnLoad();
    }

    ~RestoreNetwork()
    {
        session::ResetOnLoad();
        if (previous)
        {
            session::Publish({ host::SnapshotState::Received, *previous, {} }, true);
        }
    }
};

host::GameSnapshot suppliedSnapshot;
uint32_t           captureCalls    = 0;
bool               captureThrows   = false;
bool               capturedNetwork = false;

host::GameSnapshot captureWeekly(bool network)
{
    ++captureCalls;
    capturedNetwork = network;
    if (captureThrows)
    {
        throw std::bad_alloc{};
    }
    return suppliedSnapshot;
}

host::GameSnapshot authenticatedWeekly(uint32_t week, uint8_t digest)
{
    host::HostSettings settings;
    settings.gotwCustomWeek     = true;
    settings.gotwWeek           = week;
    settings.gotwSeed           = rerevved::gotw::SeedForWeek(week);
    settings.rulesDigestPresent = true;
    settings.rulesDigest.fill(digest);
    return host::ResolveForGame(true, { rex::session_data::State::kReceived, host::Encode(settings), true }, settings);
}

// Attaches a log sink for one test and detaches it however the test ends, so
// a failed assertion cannot leave the logger pointing at a destroyed stream.
class SinkScope
{
public:
    explicit SinkScope(spdlog::sink_ptr sink)
    : sink_(std::move(sink))
    {
        rex::AddSink(sink_);
    }

    ~SinkScope()
    {
        rex::RemoveSink(sink_);
    }

    SinkScope(const SinkScope&)            = delete;
    SinkScope& operator=(const SinkScope&) = delete;

private:
    spdlog::sink_ptr sink_;
};

// One logging lifetime per case, shut down even when the case fails. Logging
// shutdown replaces the process crash filter that Catch2 installed for the
// case, so the filter is put back to avoid Catch2's disengage warning.
class LoggingScope
{
public:
    LoggingScope()
    {
#ifdef _WIN32
        filter_ = SetUnhandledExceptionFilter(nullptr);
        SetUnhandledExceptionFilter(filter_);
#endif
        rex::InitLogging(rex::LogConfig{});
    }

    ~LoggingScope()
    {
        rex::ShutdownLogging();
#ifdef _WIN32
        SetUnhandledExceptionFilter(filter_);
#endif
    }

    LoggingScope(const LoggingScope&)            = delete;
    LoggingScope& operator=(const LoggingScope&) = delete;

private:
#ifdef _WIN32
    LPTOP_LEVEL_EXCEPTION_FILTER filter_ = nullptr;
#endif
};

constexpr uint32_t kSetup         = 0x83000000;
uint32_t           liveSetup      = kSetup;
uint32_t           liveMode       = 2;
bool               readableGlobal = true;
bool               readableMode   = true;

bool readSetup(uint64_t address, uint32_t& value) noexcept
{
    if (address == 0x8314EFC8 && readableGlobal)
    {
        value = liveSetup;
        return true;
    }
    if (address == uint64_t{ liveSetup } + 4 && readableMode)
    {
        value = liveMode;
        return true;
    }
    return false;
}

void controls(bool week)
{
    REXCVAR_SET(gotw_custom_week, week);
    REXCVAR_SET(gotw_week, "1084");
}

void civilizationControls(bool custom, int32_t civilization)
{
    REXCVAR_SET(gotw_custom_civilization, custom);
    REXCVAR_SET(gotw_civilization, civilization);
}

constexpr uint64_t derivedBits(uint32_t derived)
{
    return 0xABCD000000000000ull | derived;
}

// Runs the hook before the writer's store with the derived civilization and returns r10.
uint64_t runOverride(uint32_t derived, uint32_t writerSetup, uint32_t count)
{
    PPCRegister civilization{}, setup{}, liveCount{};
    civilization.u64 = derivedBits(derived);
    setup.u64        = writerSetup;
    liveCount.u64    = count;
    rerevved::gotw::detail::OverrideCivilization(civilization, setup, liveCount);
    REQUIRE((setup.u64 == writerSetup && liveCount.u64 == count));
    return civilization.u64;
}

void testValidation()
{
    constexpr std::array<uint32_t, 23> excluded = { 5, 7, 13, 19, 28, 34, 54, 66, 82, 88, 94, 115, 124, 130, 137, 148, 160, 161, 212, 226, 255, 265, 296 };
    uint32_t                           ordinal  = 0;
    for (uint32_t value = 1; value <= 300; ++value)
    {
        bool valid = true;
        for (const auto missing : excluded)
        {
            valid = valid && value != missing;
        }
        if (valid)
        {
            UNSCOPED_INFO("MapList retains the full shipped order");
            REQUIRE(rerevved::gotw::SeedForWeek(ordinal) == value);
            UNSCOPED_INFO("MapList repeats every 277 weeks");
            REQUIRE(rerevved::gotw::SeedForWeek(ordinal + 277) == value);
            ++ordinal;
        }
    }
    UNSCOPED_INFO("all shipped seeds were tested");
    REQUIRE(ordinal == 277);
    UNSCOPED_INFO("replay uses the native ordinal");
    REQUIRE((rerevved::gotw::SeedForWeek(1084) == 276 && rerevved::gotw::SeedForWeek(1085) == 277));
    UNSCOPED_INFO("valid week is accepted");
    REQUIRE(rex::cvar::SetFlagByName("gotw_week", "276"));
    for (const auto* text : { "", "0", "0000", "2147483648", "4294967296", "18446744073709551616", "-1", "+1", "1.5", "0x10", "1x", " 1", "1 " })
    {
        UNSCOPED_INFO("invalid input is rejected");
        REQUIRE(!rex::cvar::SetFlagByName("gotw_week", text));
        UNSCOPED_INFO("rejection keeps the previous value");
        REQUIRE(rex::cvar::GetFlagByName("gotw_week") == "276");
    }
    uint32_t week = 99;
    UNSCOPED_INFO("minimum week is accepted");
    REQUIRE((rerevved::gotw::ParseWeek("1", week) && week == 1));
    UNSCOPED_INFO("maximum week is accepted");
    REQUIRE((rerevved::gotw::ParseWeek("2147483647", week) && week == 0x7FFFFFFFu));
    for (const auto* text : { "0", "2147483648", "4294967295" })
    {
        week = 99;
        UNSCOPED_INFO("invalid Week leaves the caller's previous value intact");
        REQUIRE((!rerevved::gotw::ParseWeek(text, week) && week == 99));
        UNSCOPED_INFO("registry rejects sentinel and out-of-domain weeks");
        REQUIRE(!rex::cvar::SetFlagByName("gotw_week", text));
    }
}

void testWeekSelection()
{
    UNSCOPED_INFO("custom Week defaults off");
    REQUIRE(!REXCVAR_GET(gotw_custom_week));
    for (const bool customWeek : { false, true })
    {
        controls(customWeek);
        PPCRegister receiver{}, week{}, seed{};
        receiver.u64 = kSetup;
        week.u64     = 1085;
        rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
        const auto policy       = rerevved::gotw::ForGame();
        const auto expectedWeek = customWeek ? 1084u : 1085u;
        const auto expectedSeed = customWeek ? 276u : 0u;
        UNSCOPED_INFO("Week selects its shipped seed and preserves r3");
        REQUIRE((receiver.u64 == kSetup && week.u64 == expectedWeek && seed.u64 == expectedSeed));
        UNSCOPED_INFO("accepted launch retains native and effective values");
        REQUIRE((policy.nativeWeek == 1085 && policy.week == expectedWeek && policy.seed == expectedSeed && policy.setup == kSetup));
        controls(!customWeek);
        seed.u64 = 0;
        rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
        UNSCOPED_INFO("late hook uses the accepted selection despite pending edits");
        REQUIRE((week.u64 == expectedWeek && seed.u64 == expectedSeed));
        receiver.u64 = 0xABCD000000000000ull | kSetup;
        week.u64     = 0xFFFFFFFF00000000ull | expectedWeek;
        seed.u64     = 0;
        rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
        UNSCOPED_INFO("late hook matches the low words of stale registers");
        REQUIRE((week.u32 == expectedWeek && seed.u64 == expectedSeed));
        receiver.u64 = kSetup;
        for (const int32_t mode : { -1, 0, 1, 2, 3, 4 })
        {
            PPCRegister nativeMode{};
            nativeMode.s64 = mode;
            UNSCOPED_INFO("custom Week isolates only mode-2 outcomes");
            REQUIRE(GuardGotwOutcome(nativeMode) == (mode == 2 && customWeek));
            UNSCOPED_INFO("outcome guard preserves its register");
            REQUIRE(nativeMode.s64 == mode);
        }
        UNSCOPED_INFO("custom Week isolates caches and submission");
        REQUIRE((GuardGotwScoreCache() == customWeek && GuardGotwWeekCache() == customWeek && GuardGotwSubmission() == customWeek));
    }
    controls(false);
    PPCRegister receiver{}, week{}, seed{};
    receiver.u64 = kSetup;
    week.u64     = 0xFFFFFFFF0000043Dull;
    seed.u64     = 0x1234567800000000ull;
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("unchecked Week preserves all incoming register bits");
    REQUIRE((receiver.u64 == kSetup && week.u64 == 0xFFFFFFFF0000043Dull && seed.u64 == 0x1234567800000000ull));
}

void testCivilizationLatch()
{
    controls(false);
    UNSCOPED_INFO("ordinary civilizations are 0 through 15");
    REQUIRE((rerevved::gotw::IsValidCivilization(0) && rerevved::gotw::IsValidCivilization(15) &&
             !rerevved::gotw::IsValidCivilization(16) && !rerevved::gotw::IsValidCivilization(0xFFFFFFFFu)));
    UNSCOPED_INFO("custom Civilization defaults off");
    REQUIRE((!REXCVAR_GET(gotw_custom_civilization) && REXCVAR_GET(gotw_civilization) == 0));

    host::HostSettings settings;
    settings.gotwCustomCivilization = true;
    settings.gotwCivilization       = 7;
    auto policy                     = rerevved::gotw::LatchForGame(1085, kSetup, settings);
    UNSCOPED_INFO("Civilization alone keeps the native week and seed");
    REQUIRE((policy.customCivilization && policy.civilization == 7 && !policy.customWeek && policy.week == 1085 && policy.seed == 0));
    UNSCOPED_INFO("a custom civilization isolates like a custom week");
    REQUIRE((policy.IsCustom() && rerevved::gotw::IsolateWeeklyResult(2) && !rerevved::gotw::IsolateWeeklyResult(1)));

    settings.gotwCustomWeek = true;
    settings.gotwWeek       = 1084;
    policy                  = rerevved::gotw::LatchForGame(1085, kSetup, settings);
    UNSCOPED_INFO("Week and Civilization combine");
    REQUIRE((policy.customCivilization && policy.civilization == 7 && policy.customWeek && policy.week == 1084 && policy.seed == 276));

    settings.gotwCustomWeek   = false;
    settings.gotwWeek         = 0;
    settings.gotwCivilization = 16;
    policy                    = rerevved::gotw::LatchForGame(1085, kSetup, settings);
    UNSCOPED_INFO("an invalid civilization falls back to native");
    REQUIRE((!policy.customCivilization && policy.civilization == 0 && !policy.IsCustom() && !rerevved::gotw::IsolateWeeklyResult(2)));

    settings.gotwCustomCivilization = false;
    settings.gotwCivilization       = 7;
    policy                          = rerevved::gotw::LatchForGame(1085, kSetup, settings);
    UNSCOPED_INFO("an unchecked Civilization stays native");
    REQUIRE((!policy.customCivilization && !policy.IsCustom()));

    civilizationControls(true, 3);
    policy = rerevved::gotw::LatchForGame(1085, kSetup);
    UNSCOPED_INFO("direct preference selection latches the checked civilization");
    REQUIRE((policy.customCivilization && policy.civilization == 3 && !policy.customWeek && policy.IsCustom()));
    civilizationControls(false, 3);
    policy = rerevved::gotw::LatchForGame(1085, kSetup);
    UNSCOPED_INFO("direct preference selection ignores an unchecked civilization");
    REQUIRE((!policy.customCivilization && !policy.IsCustom()));

    civilizationControls(true, 11);
    auto local = host::LocalSettings();
    UNSCOPED_INFO("local settings sample the checked civilization");
    REQUIRE((local.gotwCustomCivilization && local.gotwCivilization == 11));
    civilizationControls(false, 11);
    local = host::LocalSettings();
    UNSCOPED_INFO("local settings drop an unchecked civilization");
    REQUIRE((!local.gotwCustomCivilization && local.gotwCivilization == 0));
    civilizationControls(false, 0);
}

void testCivilizationOverride()
{
    static_assert(noexcept(OverrideGotwCivilization(std::declval<PPCRegister&>(), std::declval<PPCRegister&>(), std::declval<PPCRegister&>())));
    controls(false);
    civilizationControls(true, 7);
    PPCRegister receiver{}, week{}, seed{};
    receiver.u64 = kSetup;
    week.u64     = 0xFFFFFFFF0000043Dull;
    seed.u64     = 0x1234567800000000ull;
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("Civilization alone leaves the writer's week and seed arguments untouched");
    REQUIRE((receiver.u64 == kSetup && week.u64 == 0xFFFFFFFF0000043Dull && seed.u64 == 0x1234567800000000ull));
    UNSCOPED_INFO("an armed launch replaces the derived civilization once");
    REQUIRE(runOverride(3, kSetup, 16) == 7);
    UNSCOPED_INFO("the override is one-shot");
    REQUIRE(runOverride(3, kSetup, 16) == derivedBits(3));
    UNSCOPED_INFO("the override does not disturb the latched selection");
    REQUIRE((rerevved::gotw::ForGame().customCivilization && rerevved::gotw::ForGame().civilization == 7));

    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("a different writer setup keeps the derived civilization");
    REQUIRE(runOverride(3, kSetup + 0x1000, 16) == derivedBits(3));
    UNSCOPED_INFO("a rejected call still consumes the arm");
    REQUIRE(runOverride(3, kSetup, 16) == derivedBits(3));

    civilizationControls(true, 12);
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("a civilization at or beyond the live count keeps the derived value");
    REQUIRE(runOverride(3, kSetup, 12) == derivedBits(3));
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("the highest civilization below the live count applies");
    REQUIRE(runOverride(3, kSetup, 13) == 12);

    civilizationControls(true, 0);
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("Roman is a valid choice when another civilization was derived");
    REQUIRE(runOverride(9, kSetup, 16) == 0);

    UNSCOPED_INFO("an unarmed writer call keeps the derived civilization");
    REQUIRE(runOverride(5, kSetup, 16) == derivedBits(5));

    civilizationControls(true, 7);
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    civilizationControls(false, 7);
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("a later native launch clears a stale arm");
    REQUIRE(runOverride(3, kSetup, 16) == derivedBits(3));

    civilizationControls(true, 7);
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    rerevved::gotw::LatchForGame(1085, kSetup);
    UNSCOPED_INFO("an arm from an older launch is rejected");
    REQUIRE(runOverride(3, kSetup, 16) == derivedBits(3));

    liveMode = 6;
    civilizationControls(true, 7);
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    liveMode = 2;
    UNSCOPED_INFO("a rejected nonweekly setup arms nothing");
    REQUIRE(runOverride(3, kSetup, 16) == derivedBits(3));

    controls(true);
    civilizationControls(true, 5);
    week.u64 = 1085;
    seed.u64 = 0;
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("Week and Civilization both apply at the early call");
    REQUIRE((week.u64 == 1084 && seed.u64 == 276 && runOverride(3, kSetup, 16) == 5));
    controls(false);
    civilizationControls(false, 0);
}

void testCivilizationThreadIsolation()
{
    controls(false);
    civilizationControls(true, 7);
    PPCRegister receiver{}, week{}, seed{};
    receiver.u64 = kSetup;
    week.u64     = 1085;
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    // Another guest thread's writer call, matching or not, cannot take this thread's arm.
    uint64_t    unrelated = 0, matching = 0;
    std::thread other([&]
                      {
                          PPCRegister civilization{}, setup{}, count{};
                          civilization.u64 = derivedBits(3);
                          setup.u64        = kSetup + 0x1000;
                          count.u64        = 16;
                          rerevved::gotw::detail::OverrideCivilization(civilization, setup, count);
                          unrelated        = civilization.u64;
                          civilization.u64 = derivedBits(3);
                          setup.u64        = kSetup;
                          rerevved::gotw::detail::OverrideCivilization(civilization, setup, count);
                          matching = civilization.u64;
                      });
    other.join();
    UNSCOPED_INFO("a writer call on another thread keeps its derived civilization");
    REQUIRE((unrelated == derivedBits(3) && matching == derivedBits(3)));
    UNSCOPED_INFO("the arming thread still receives its override");
    REQUIRE(runOverride(3, kSetup, 16) == 7);
    civilizationControls(false, 0);
}

void testCivilizationLateReapply()
{
    controls(false);
    civilizationControls(true, 9);
    PPCRegister receiver{}, week{}, seed{};
    receiver.u64 = kSetup;
    week.u64     = 1085;
    seed.u64     = 0;
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("the early writer call consumes its arm");
    REQUIRE(runOverride(2, kSetup, 16) == 9);
    civilizationControls(false, 0);

    rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("late Civilization leaves the native week and seed alone");
    REQUIRE((week.u64 == 1085 && seed.u64 == 0));
    UNSCOPED_INFO("the late writer call applies the accepted civilization despite pending edits");
    REQUIRE(runOverride(2, kSetup, 16) == 9);
    UNSCOPED_INFO("the late override is one-shot");
    REQUIRE(runOverride(2, kSetup, 16) == derivedBits(2));

    for (int failure = 0; failure != 6; ++failure)
    {
        readableGlobal = failure != 0;
        readableMode   = failure != 1;
        liveSetup      = failure == 2 ? kSetup + 0x1000 : kSetup;
        receiver.u64   = failure == 3 ? kSetup + 0x1000 : kSetup;
        week.u64       = failure == 4 ? 1084 : 1085;
        rerevved::gotw::detail::ReapplySeed(receiver, week, seed, failure == 5 ? nullptr : readSetup);
        UNSCOPED_INFO("an unreadable or unrelated late setup arms nothing");
        REQUIRE(runOverride(2, kSetup, 16) == derivedBits(2));
    }
    readableGlobal = readableMode = true;
    liveSetup                     = kSetup;
    receiver.u64                  = kSetup;
    week.u64                      = 1085;
    liveMode                      = 1;
    rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
    liveMode = 2;
    UNSCOPED_INFO("late Civilization requires setup mode 2");
    REQUIRE(runOverride(2, kSetup, 16) == derivedBits(2));

    controls(true);
    civilizationControls(true, 4);
    week.u64 = 1085;
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    REQUIRE(runOverride(2, kSetup, 16) == 4);
    controls(false);
    civilizationControls(false, 0);
    seed.u64 = 0;
    rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
    UNSCOPED_INFO("late Week and Civilization both reapply");
    REQUIRE((week.u64 == 1084 && seed.u64 == 276 && runOverride(2, kSetup, 16) == 4));
}

void testCivilizationLogging()
{
    std::ostringstream output;
    auto               sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    sink->set_pattern("%v");
    const SinkScope sinkScope(sink);
    controls(false);
    civilizationControls(true, 7);
    PPCRegister receiver{}, week{}, seed{};
    receiver.u64 = kSetup;
    week.u64     = 1085;
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    REQUIRE(runOverride(3, kSetup, 16) == 7);
    REQUIRE(runOverride(3, kSetup, 16) == derivedBits(3));
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    REQUIRE(runOverride(3, kSetup, 5) == derivedBits(3));
    rex::FlushLogging();
    std::istringstream lines(output.str());
    std::string        line;
    UNSCOPED_INFO("an applied override logs the derived and chosen civilizations once");
    REQUIRE((static_cast<bool>(std::getline(lines, line)) && line.find("civilization site=early derived=3 chosen=7") != std::string::npos));
    UNSCOPED_INFO("a rejected override logs a warning and nothing else");
    REQUIRE((static_cast<bool>(std::getline(lines, line)) && line.find("chosen=7 rejected") != std::string::npos));
    UNSCOPED_INFO("unarmed calls log nothing");
    REQUIRE(!std::getline(lines, line));
    civilizationControls(false, 0);
}

void testStandardRules()
{
    UNSCOPED_INFO("Standard Rules defaults checked");
    REQUIRE(REXCVAR_GET(gotw_standard_rules));
    for (const bool checked : { false, true })
    {
        for (const int32_t mode : { -1, 0, 1, 2, 3, 4, 5, 6 })
        {
            REXCVAR_SET(gotw_standard_rules, checked);
            const bool expected = checked && mode == 2;
            UNSCOPED_INFO("only a checked GOTW freezes standard rules");
            REQUIRE(rerevved::gotw::FreezeStandardRules(mode) == expected);
            REXCVAR_SET(gotw_standard_rules, !checked);
            UNSCOPED_INFO("later checkbox edits cannot change the AI-count decision");
            REQUIRE(rerevved::gotw::StandardRulesForGame() == expected);
        }
    }
    REXCVAR_SET(gotw_standard_rules, true);
    rerevved::gotw::FreezeStandardRules(2);
    UNSCOPED_INFO("ordinary new game clears a previous GOTW decision");
    REQUIRE((!rerevved::gotw::FreezeStandardRules(1) && !rerevved::gotw::StandardRulesForGame()));
}

void testLaunchFallback()
{
    for (const auto* invalidWeek : { "0", "2147483648", "bad" })
    {
        controls(true);
        REXCVAR_SET(gotw_week, invalidWeek);
        PPCRegister receiver{}, week{}, seed{};
        receiver.u64 = kSetup;
        week.u64     = 1085;
        rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
        UNSCOPED_INFO("invalid checked Week falls back to native launch");
        REQUIRE((week.u64 == 1085 && seed.u64 == 0 && !rerevved::gotw::ForGame().IsCustom()));
        UNSCOPED_INFO("native fallback clears custom isolation");
        REQUIRE((!GuardGotwScoreCache() && !GuardGotwSubmission()));
        rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
        UNSCOPED_INFO("fallback remains native at the late writer");
        REQUIRE((week.u64 == 1085 && seed.u64 == 0));
    }
}

void testFallbackLogging()
{
    std::ostringstream output;
    auto               sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    sink->set_pattern("%v");
    const SinkScope sinkScope(sink);
    controls(true);
    REXCVAR_SET(gotw_week, "0");
    PPCRegister receiver{}, week{}, seed{};
    receiver.u64 = kSetup;
    week.u64     = 1085;
    rerevved::gotw::LatchForGame(week.u32, receiver.u32);
    rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
    rex::FlushLogging();
    std::istringstream lines(output.str());
    std::string        line;
    UNSCOPED_INFO("direct preference selection logs an invalid checked week");
    REQUIRE((static_cast<bool>(std::getline(lines, line)) && line.find("invalid custom week '0'") != std::string::npos));
    UNSCOPED_INFO("late reapplication does not repeat fallback logs");
    REQUIRE(!std::getline(lines, line));
}

void testLateLaunchIdentity()
{
    controls(true);
    PPCRegister receiver{}, week{}, seed{};
    receiver.u64 = kSetup;
    week.u64     = 1085;
    rerevved::gotw::detail::ApplySeed(receiver, week, seed, readSetup);
    for (const uint32_t mode : { 0u, 1u, 2u, 3u })
    {
        liveMode = mode;
        seed.u64 = 0x1234567800000000ull;
        rerevved::gotw::detail::ReapplySeed(receiver, week, seed, readSetup);
        UNSCOPED_INFO("late substitution requires setup mode 2");
        REQUIRE(seed.u64 == (mode == 2 ? 276ull : 0x1234567800000000ull));
    }
    liveMode = 2;
    for (int failure = 0; failure != 6; ++failure)
    {
        readableGlobal       = failure != 0;
        readableMode         = failure != 1;
        liveSetup            = failure == 2 ? kSetup + 0x1000 : kSetup;
        receiver.u64         = failure == 3 ? kSetup + 0x1000 : kSetup;
        week.u64             = failure == 4 ? 1085 : 1084;
        seed.u64             = 0x1234567800000000ull;
        const auto savedWeek = week.u64;
        rerevved::gotw::detail::ReapplySeed(receiver, week, seed, failure == 5 ? nullptr : readSetup);
        UNSCOPED_INFO("unreadable or unrelated setup and command preserve arguments");
        REQUIRE((seed.u64 == 0x1234567800000000ull && week.u64 == savedWeek));
    }
    readableGlobal = readableMode = true;
    liveSetup                     = kSetup;
    receiver.u64                  = kSetup;
    week.u64                      = 1084;
    seed.u64                      = 0;
    ReapplyGotwSeed(receiver, week, seed);
    UNSCOPED_INFO("runtime wrapper leaves unavailable guest memory untouched");
    REQUIRE(seed.u64 == 0);
}

void testDeferredStoreBack()
{
    auto                               alive = std::make_shared<std::atomic<bool>>(true);
    std::vector<std::function<void()>> pending;
    rerevved::gotw::SetSeedDispatcher([&](std::function<void()> task)
                                      {
                                          pending.push_back(std::move(task));
                                      },
                                      alive);
    controls(false);
    rerevved::gotw::LatchForGame(1085, kSetup);
    rerevved::gotw::RecordAutomaticWeek(1085);
    rerevved::gotw::RecordAutomaticSeed(277);
    UNSCOPED_INFO("Week queues without writing config on the guest thread");
    REQUIRE((pending.size() == 1 && REXCVAR_GET(gotw_week) == "1084"));
    REXCVAR_SET(gotw_custom_week, true);
    pending[0]();
    UNSCOPED_INFO("checking Week before delivery protects the field");
    REQUIRE(REXCVAR_GET(gotw_week) == "1084");
    REXCVAR_SET(gotw_custom_week, false);
    pending[0]();
    UNSCOPED_INFO("Week store-back uses the UI dispatcher");
    REQUIRE(REXCVAR_GET(gotw_week) == "1085");
    const auto count = pending.size();
    rerevved::gotw::RecordAutomaticWeek(0);
    UNSCOPED_INFO("invalid native observation cannot write the field");
    REQUIRE(pending.size() == count);
    controls(true);
    rerevved::gotw::LatchForGame(1085, kSetup);
    rerevved::gotw::RecordAutomaticSeed(277);
    UNSCOPED_INFO("native seed observation cannot replace a replay's derived seed");
    REQUIRE(rerevved::gotw::ForGame().seed == 276);
    controls(false);
    rerevved::gotw::LatchForGame(1085, kSetup);
    rerevved::gotw::RecordAutomaticWeek(1085);
    alive->store(false);
    pending.back()();
    UNSCOPED_INFO("dead alive-token cancels delivery");
    REQUIRE(REXCVAR_GET(gotw_week) == "1084");
    alive->store(true);
    rerevved::gotw::LatchForGame(1085, kSetup);
    pending.back()();
    UNSCOPED_INFO("a new launch invalidates old callbacks");
    REQUIRE(REXCVAR_GET(gotw_week) == "1084");
    rerevved::gotw::RecordAutomaticWeek(1085);
    alive.reset();
    pending.back()();
    UNSCOPED_INFO("expired alive-token cancels delivery");
    REQUIRE(REXCVAR_GET(gotw_week) == "1084");
    alive = std::make_shared<std::atomic<bool>>(true);
    rerevved::gotw::SetSeedDispatcher([](std::function<void()>)
                                      {
                                          throw 1;
                                      },
                                      alive);
    rerevved::gotw::RecordAutomaticWeek(1085);
    rerevved::gotw::SetSeedDispatcher({});
    rerevved::gotw::RecordAutomaticWeek(1085);
    UNSCOPED_INFO("unavailable dispatcher causes no guest-thread config writes");
    REQUIRE(REXCVAR_GET(gotw_week) == "1084");
}

void testTraceObservers()
{
    static_assert(noexcept(ApplyGotwSeed(std::declval<PPCRegister&>(), std::declval<PPCRegister&>(), std::declval<PPCRegister&>())));
    static_assert(noexcept(ReapplyGotwSeed(std::declval<PPCRegister&>(), std::declval<PPCRegister&>(), std::declval<PPCRegister&>())));
    static_assert(noexcept(LogGotwSeed(std::declval<PPCRegister&>(), std::declval<PPCRegister&>())));
    static_assert(noexcept(rerevved::gotw::RecordAutomaticSeed(1)));
    static_assert(noexcept(rerevved::gotw::RecordAutomaticWeek(1)));
    UNSCOPED_INFO("observer defaults off");
    REQUIRE((!REXCVAR_GET(gotw_trace) && !rerevved::gotw::TraceEnabled()));
    UNSCOPED_INFO("disabled or zero sequence never logs");
    REQUIRE((!rerevved::gotw::ShouldTraceEvent(false, 1) && !rerevved::gotw::ShouldTraceEvent(true, 0)));
    UNSCOPED_INFO("enabled events within the cap log");
    REQUIRE((rerevved::gotw::ShouldTraceEvent(true, 1) && rerevved::gotw::ShouldTraceEvent(true, rerevved::gotw::kTraceEventCap)));
    UNSCOPED_INFO("events beyond the cap do not log");
    REQUIRE(!rerevved::gotw::ShouldTraceEvent(true, rerevved::gotw::kTraceEventCap + 1));
    PPCRegister r3{}, r4{}, r5{}, setup{};
    r3.u64    = 3;
    r4.u64    = 4;
    r5.u64    = 5;
    setup.u64 = 31;
    TraceGotwWriterEntry(0x82D86860, r3, r4, r5);
    TraceGotwWriterExit(0x82DADCE4, r3, r4, r5, setup);
    TraceGotwGeneratorSeed(setup);
    TraceGotwPrng(r5);
    TraceGotwProcedural();
    TraceGotwNamedLoad(r3);
    UNSCOPED_INFO("disabled observers preserve every supplied register");
    REQUIRE((r3.u64 == 3 && r4.u64 == 4 && r5.u64 == 5 && setup.u64 == 31));
}

void testTraceCap()
{
    std::ostringstream output;
    auto               sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    sink->set_pattern("%v");
    const SinkScope sinkScope(sink);
    TraceGotwProcedural();
    REXCVAR_SET(gotw_trace, true);
    for (uint64_t i = 0; i < rerevved::gotw::kTraceEventCap + 8; ++i)
    {
        TraceGotwProcedural();
    }
    rex::FlushLogging();
    std::istringstream lines(output.str());
    std::string        line;
    const auto         readLine = [&]()
    {
        const bool read = static_cast<bool>(std::getline(lines, line));
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        return read;
    };
    for (uint64_t sequence = 1; sequence <= rerevved::gotw::kTraceEventCap; ++sequence)
    {
        UNSCOPED_INFO("all 512 trace events are logged");
        REQUIRE(readLine());
        UNSCOPED_INFO("observer logs events 1 through 512 exactly once in order");
        REQUIRE(line == "GOTW trace seq=" + std::to_string(sequence) + " site=82D0F100 procedural");
    }
    UNSCOPED_INFO("trace cap emits a truncation marker");
    REQUIRE(readLine());
    UNSCOPED_INFO("truncation follows event 512 exactly once");
    REQUIRE(line == "GOTW trace seq=513 truncated cap=512; complete ordering is inconclusive");
    UNSCOPED_INFO("observer emits nothing after the truncation marker");
    REQUIRE(!std::getline(lines, line));
    const auto capped = output.str();
    REXCVAR_SET(gotw_trace, false);
    TraceGotwProcedural();
    REXCVAR_SET(gotw_trace, true);
    TraceGotwProcedural();
    rex::FlushLogging();
    UNSCOPED_INFO("trace cap remains exhausted after toggling the setting");
    REQUIRE(output.str() == capped);
    REXCVAR_SET(gotw_trace, false);
}

} // namespace

TEST_CASE("gotw_settings", "[ctest][gotw-settings][local-fast]")
{
    const RestoreNetwork restoreNetwork;
    REREVVED_RESTORE_CVAR(gotw_custom_week);
    REREVVED_RESTORE_CVAR(gotw_week);
    REREVVED_RESTORE_CVAR(gotw_standard_rules);
    REREVVED_RESTORE_CVAR(gotw_custom_civilization);
    REREVVED_RESTORE_CVAR(gotw_civilization);
    REREVVED_RESTORE_CVAR(gotw_trace);
    // Category IDs survive shutdown, while the SDK's category registry does not.
    // Keep one logging lifetime and attach each test's sink only while it runs.
    const LoggingScope logging;
    testWeekSelection();
    testStandardRules();
    testValidation();
    testLaunchFallback();
    testFallbackLogging();
    testLateLaunchIdentity();
    testCivilizationLatch();
    testCivilizationOverride();
    testCivilizationThreadIsolation();
    testCivilizationLateReapply();
    testCivilizationLogging();
    testDeferredStoreBack();
    testTraceObservers();
    testTraceCap();
}

TEST_CASE("local weekly seed clears previous host policy", "[ctest][gotw-settings]")
{
    const RestoreNetwork restore;
    REREVVED_RESTORE_CVAR(gotw_custom_week);
    REREVVED_RESTORE_CVAR(gotw_week);
    const auto previous = authenticatedWeekly(1085, 0xA5);
    session::Publish(previous, true);
    REQUIRE(session::ForNetworkGame());
    controls(true);
    PPCRegister setup{}, week{}, seed{};
    setup.u32 = kSetup;
    week.u32  = 99;
    rerevved::gotw::detail::ApplySeed(setup, week, seed, readSetup);
    CHECK(week.u32 == 1084);
    CHECK(seed.u32 == 276);
    CHECK_FALSE(session::ForNetworkGame());
    const auto outer = session::Prepare(false, kSetup, 2);
    REQUIRE(outer.settings);
    CHECK(outer.state == host::SnapshotState::Local);
    CHECK(outer.settings->gotwWeek == 1084);
    CHECK(outer.settings->rulesDigest == host::LocalSettings().rulesDigest);
}

TEST_CASE("local weekly seed and outer setup share one snapshot", "[ctest][gotw-settings]")
{
    const RestoreNetwork restore;
    REREVVED_RESTORE_CVAR(gotw_custom_week);
    REREVVED_RESTORE_CVAR(gotw_week);
    REREVVED_RESTORE_CVAR(gotw_standard_rules);
    REXCVAR_SET(gotw_standard_rules, true);
    controls(true);
    suppliedSnapshot = host::ResolveForGame(false, {}, host::LocalSettings());
    suppliedSnapshot.settings->rulesDigest.fill(0xA5);
    REXCVAR_SET(gotw_week, "1085");
    captureCalls  = 0;
    captureThrows = false;
    PPCRegister setup{}, week{}, seed{};
    setup.u32 = kSetup;
    week.u32  = 99;
    rerevved::gotw::detail::ApplySeed(setup, week, seed, readSetup, captureWeekly);
    CHECK(week.u32 == 1084);
    CHECK(seed.u32 == 276);
    CHECK(REXCVAR_GET(gotw_week) == "1085");
    CHECK_FALSE(capturedNetwork);
    CHECK_FALSE(session::ForNetworkGame());
    suppliedSnapshot                             = host::ResolveForGame(false, {}, host::LocalSettings());
    suppliedSnapshot.settings->gotwStandardRules = false;
    const auto outer                             = session::detail::Prepare(false, kSetup, 2, captureWeekly);
    REQUIRE(outer.settings);
    CHECK(captureCalls == 1);
    CHECK(outer.state == host::SnapshotState::Local);
    CHECK(outer.settings->gotwWeek == 1084);
    CHECK(outer.settings->gotwSeed == 276);
    CHECK(outer.settings->gotwStandardRules);
    CHECK(outer.settings->rulesDigest[0] == 0xA5);
    seed.u32 = 0;
    rerevved::gotw::detail::ReapplySeed(setup, week, seed, readSetup);
    CHECK(seed.u32 == 276);
}

TEST_CASE("failed weekly capture cannot admit later preferences", "[ctest][gotw-settings]")
{
    const RestoreNetwork restore;
    REREVVED_RESTORE_CVAR(gotw_custom_week);
    REREVVED_RESTORE_CVAR(gotw_week);
    controls(false);
    const auto before = rerevved::gotw::LatchForGame(99, kSetup);
    session::Publish(authenticatedWeekly(1085, 0xA5), true);
    captureCalls  = 0;
    captureThrows = true;
    PPCRegister setup{}, week{}, seed{};
    setup.u32 = kSetup;
    week.u32  = 99;
    seed.u32  = 777;
    rerevved::gotw::detail::ApplySeed(setup, week, seed, readSetup, captureWeekly);
    captureThrows = false;
    CHECK(week.u32 == 99);
    CHECK(seed.u32 == 777);
    CHECK(rerevved::gotw::ForGame().launch == before.launch);
    CHECK_FALSE(session::ForNetworkGame());
    controls(true);
    suppliedSnapshot = host::ResolveForGame(false, {}, host::LocalSettings());
    const auto outer = session::detail::Prepare(false, kSetup, 2, captureWeekly);
    CHECK_FALSE(outer.settings);
    CHECK_FALSE(outer.diagnostic.empty());
    CHECK(captureCalls == 1);
    rerevved::gotw::detail::ApplySeed(setup, week, seed, readSetup, captureWeekly);
    CHECK(week.u32 == 1084);
    CHECK(seed.u32 == 276);
    CHECK(captureCalls == 2);
}

TEST_CASE("unreadable weekly mode stays rejected", "[ctest][gotw-settings]")
{
    const RestoreNetwork restore;
    REREVVED_RESTORE_CVAR(gotw_custom_week);
    REREVVED_RESTORE_CVAR(gotw_week);
    controls(true);
    const auto  before = rerevved::gotw::ForGame();
    PPCRegister setup{}, week{}, seed{};
    setup.u32 = kSetup;
    week.u32  = 99;
    seed.u32  = 777;
    // There is no Runtime in this fixture, so the public checked reader fails.
    ApplyGotwSeed(setup, week, seed);
    CHECK(week.u32 == 99);
    CHECK(seed.u32 == 777);
    CHECK(rerevved::gotw::ForGame().launch == before.launch);
    suppliedSnapshot = host::ResolveForGame(false, {}, host::LocalSettings());
    captureCalls     = 0;
    CHECK_FALSE(session::detail::Prepare(false, kSetup, 2, captureWeekly).settings);
    CHECK(captureCalls == 0);
}

TEST_CASE("weekly seed rejects a current nonweekly mode", "[ctest][gotw-settings]")
{
    const RestoreNetwork restore;
    const auto           before = rerevved::gotw::ForGame();
    PPCRegister          setup{}, week{}, seed{};
    setup.u32 = kSetup;
    week.u32  = 99;
    seed.u32  = 777;
    liveMode  = 6;
    rerevved::gotw::detail::ApplySeed(setup, week, seed, readSetup, captureWeekly);
    liveMode = 2;
    CHECK(week.u32 == 99);
    CHECK(seed.u32 == 777);
    CHECK(rerevved::gotw::ForGame().launch == before.launch);
    CHECK_FALSE(session::detail::Prepare(true, kSetup, 6, captureWeekly).settings);
}
