#include "research_prompt.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string_view>

namespace
{

using namespace rerevved::research_prompt;

constexpr uint32_t kCaller = 0x7000FE00;
constexpr uint32_t kPrompt = kCaller - kPromptFrameSize;
constexpr uint32_t kShow   = kPrompt - kShowFrameSize;

void testSuppressesOnlyTheArmedShow()
{
    AutomaticPromptGate gate;
    gate.BeginShow(true, kPrompt);
    UNSCOPED_INFO("unarmed prompt stays native");
    CHECK(!gate.SuppressShow(kShow));

    gate.Arm(kCaller);
    gate.BeginShow(false, kPrompt);
    UNSCOPED_INFO("disabled option stays native");
    CHECK(!gate.SuppressShow(kShow));

    gate.BeginShow(true, kPrompt - 0x200);
    UNSCOPED_INFO("nested prompt stays native");
    CHECK(!gate.SuppressShow(kShow - 0x200));

    gate.BeginShow(true, kPrompt);
    UNSCOPED_INFO("nested advisor show stays native");
    CHECK(!gate.SuppressShow(kShow - 0x200));
    UNSCOPED_INFO("armed show is suppressed");
    CHECK(gate.SuppressShow(kShow));
    UNSCOPED_INFO("suppression applies once");
    CHECK(!gate.SuppressShow(kShow));

    gate.BeginShow(true, kPrompt);
    gate.Disarm();
    UNSCOPED_INFO("show after the call stays native");
    CHECK(!gate.SuppressShow(kShow));
}

void testEachArmedCallerIsMatchedByItsOwnFrame()
{
    constexpr uint32_t kOtherCaller = kCaller - 0x300;
    constexpr uint32_t kOtherPrompt = kOtherCaller - kPromptFrameSize;
    constexpr uint32_t kOtherShow   = kOtherPrompt - kShowFrameSize;

    AutomaticPromptGate gate;
    gate.Arm(kCaller);
    gate.Disarm();
    gate.Arm(kOtherCaller);
    gate.BeginShow(true, kPrompt);
    UNSCOPED_INFO("prompt of the previous caller stays native");
    CHECK(!gate.SuppressShow(kShow));
    gate.BeginShow(true, kOtherPrompt);
    UNSCOPED_INFO("re-armed caller is suppressed");
    CHECK(gate.SuppressShow(kOtherShow));
}

} // namespace

TEST_CASE("research_prompt", "[ctest][research-prompt]")
{
    testSuppressesOnlyTheArmedShow();
    testEachArmedCallerIsMatchedByItsOwnFrame();
}
