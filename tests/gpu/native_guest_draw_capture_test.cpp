#include "gpu/diagnostics/native_guest_draw_capture.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include <rex/hook.h>
#include <toml++/toml.hpp>

REX_EXTERN(sub_826A3568);
REX_EXTERN(sub_826AD150);
REX_EXTERN(sub_826A3000);
REX_EXTERN(sub_826A39F8);
REX_EXTERN(sub_826A7040);
REX_EXTERN(sub_826A7138);
void ObserveNativeGuestGammaTable(PPCRegister& r31, PPCRegister& r1);
void ObserveNativeGuestPwlGamma(PPCRegister& r31);

namespace
{

unsigned originalCalls = 0;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void checkOriginal(void (*hook)(PPCContext&, std::uint8_t*) = sub_826A3568, std::uint64_t caller = 0x1234, std::uint32_t tableAddress = 4)
{
    PPCContext ctx{};
    ctx.r1.u64    = 0x12345678;
    ctx.r3.u64    = 0x1111222233334444ULL;
    ctx.r4.u64    = tableAddress;
    ctx.lr        = caller;
    auto expected = ctx;
    expected.r3.u64 += 1;
    expected.lr ^= 0x100;
    std::uint8_t memory = 0;
    const auto   calls  = originalCalls;
    hook(ctx, &memory);
    require(originalCalls == calls + 1 && memory == 1, "original draw must run exactly once");
    require(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "capture must preserve original context effects");
}

} // namespace

REX_HOOK_RAW(__imp__sub_826A3568)
{
    ++originalCalls;
    ctx.r3.u64 += 1;
    ctx.lr ^= 0x100;
    base[0] += 1;
}

REX_HOOK_RAW(__imp__sub_826AD150)
{
    __imp__sub_826A3568(ctx, base);
}

REX_HOOK_RAW(__imp__sub_826A3000)
{
    __imp__sub_826A3568(ctx, base);
}

REX_HOOK_RAW(__imp__sub_826A39F8)
{
    __imp__sub_826A3568(ctx, base);
}

REX_HOOK_RAW(__imp__sub_826A7040)
{
    __imp__sub_826A3568(ctx, base);
}

REX_HOOK_RAW(__imp__sub_826A7138)
{
    __imp__sub_826A3568(ctx, base);
}

int main()
{
    using namespace rerevved::gpu::diagnostics;
    std::array<std::uint8_t, 1536> gammaBytes{};
    std::array<std::uint32_t, 256> expectedGamma{};
    for (std::size_t channel = 0; channel < 3; ++channel)
        for (std::size_t i = 0; i < 256; ++i)
        {
            const auto value       = static_cast<std::uint32_t>((i * (channel + 1) + 17 * channel) & 1023);
            const auto offset      = channel * 512 + i * 2;
            gammaBytes[offset]     = static_cast<std::uint8_t>(value >> 2);
            gammaBytes[offset + 1] = static_cast<std::uint8_t>(value << 6);
            expectedGamma[i] |= value << ((2 - channel) * 10);
        }
    std::array<std::uint32_t, 256> gamma{};
    std::string                    gammaError;
    require(DecodeNativeGuestGammaTable(gammaBytes, gamma, gammaError) && gamma == expectedGamma,
            "converted guest gamma must retain all three channels and endian order");
    const auto               completeGammaBytes = std::vector<std::uint8_t>(gammaBytes.begin(), gammaBytes.end());
    const auto               completeGamma      = expectedGamma;
    NativeGuestGammaEmission emission;
    std::string              emissionError;
    auto                     pendingBytes = completeGammaBytes;
    require(emission.Begin(1, 0x1000, pendingBytes, emissionError),
            "valid gamma emission must begin pending");
    require(emission.Pending() && emission.Sequence() == 0 && emission.Bytes().empty(),
            "gamma emission must remain unavailable before the original returns");
    require(!emission.Matches(1, completeGamma, completeGammaBytes, emissionError),
            "pending gamma emission must not match a frame");
    pendingBytes[0] ^= 0x40;
    emission.Complete();
    require(!emission.Pending() && emission.Sequence() == 1 && emission.Graphics() == 1 &&
                emission.Address() == 0x1000 && emission.Table() == completeGamma &&
                emission.Bytes().size() == completeGammaBytes.size() &&
                std::equal(emission.Bytes().begin(), emission.Bytes().end(), completeGammaBytes.begin()),
            "gamma emission must own its captured table and bytes");
    require(emission.Matches(1, completeGamma, completeGammaBytes, emissionError),
            "completed gamma emission must match its producer");
    auto changedBytes = completeGammaBytes;
    auto changedTable = completeGamma;
    changedBytes[0]   = 0x40;
    changedBytes[1]   = 0;
    changedTable[0]   = (changedTable[0] & ~(1023U << 20)) | (256U << 20);
    require(emission.Begin(1, 0x2000, changedBytes, emissionError),
            "changed gamma emission must begin after completion");
    require(!emission.Matches(1, completeGamma, completeGammaBytes, emissionError),
            "changed gamma must remain unavailable while pending");
    emission.Complete();
    require(emission.Sequence() == 2 && emission.Address() == 0x2000 &&
                emission.Table() == changedTable &&
                emission.Matches(1, emission.Table(), changedBytes, emissionError),
            "completed changed gamma must replace the prior emission");
    require(!emission.Matches(2, changedTable, changedBytes, emissionError),
            "gamma emission from a foreign device must not match");
    auto wrongTable = changedTable;
    wrongTable[0] ^= 1U;
    require(!emission.Matches(1, wrongTable, changedBytes, emissionError),
            "gamma emission from a foreign producer table must not match");
    auto wrongBytes = changedBytes;
    wrongBytes[2] ^= 1U;
    require(!emission.Matches(1, changedTable, wrongBytes, emissionError),
            "gamma emission from foreign producer bytes must not match");
    NativeGuestGammaEmission emptyEmission;
    require(!emptyEmission.Matches(1, completeGamma, completeGammaBytes, emissionError),
            "empty gamma emission must not match");
    require(!emptyEmission.Begin(0, 0x1000, completeGammaBytes, emissionError),
            "empty gamma device must be rejected");
    require(!DecodeNativeGuestGammaTable(std::span(gammaBytes).first(1534), gamma, gammaError) &&
                gamma == std::array<std::uint32_t, 256>{},
            "incomplete gamma cannot leave a usable stale table");
    gammaBytes.back() |= 1;
    require(!DecodeNativeGuestGammaTable(gammaBytes, gamma, gammaError) &&
                gamma == std::array<std::uint32_t, 256>{},
            "unaligned converted channel must fail without partial output");
    checkOriginal();
    checkOriginal(sub_826AD150);
    checkOriginal(sub_826A3000);
    checkOriginal(sub_826A39F8);
    checkOriginal(sub_826A7040);
    checkOriginal(sub_826A7138);
    const auto  root = std::filesystem::temp_directory_path() /
                       ("rerevved-guest-draw-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::string error;
    require(StartNativeGuestDrawCapture(root, error), "fresh capture should start");
    // The invalid caller is rejected before any guest memory access. Diagnostic
    // failure must still run the original function with unchanged entry state.
    std::ofstream(root / "arm").close();
    checkOriginal(sub_826A3000);
    require(!std::filesystem::exists(root / "result.toml"), "unrelated nonindexed callers are ignored");
    checkOriginal();
    checkOriginal(sub_826AD150);
    checkOriginal(sub_826A3000);
    const auto result = toml::parse_file((root / "result.toml").string());
    require(!result["complete"].value_or(true) &&
                result["error"].value_or(std::string{}) == "unexpected caller at guest menu draw boundary",
            "unsupported caller must produce an explicit failed capture");
    checkOriginal();
    StopNativeGuestDrawCapture();
    checkOriginal();
    std::filesystem::remove(root / "arm");
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    NativeGuestDrawConsumer consume = [](const auto&, const auto&, auto&)
    {
        return true;
    };
    NativeGuestFrameConsumer endFrame = [](const auto&, const auto&, bool, auto&)
    {
        return true;
    };
    unsigned                      stopped = 0;
    NativeGuestDrawCaptureOptions continuous;
    continuous.continuous = true;
    require(!StartNativeGuestDrawCapture(root, error, consume, endFrame, continuous),
            "continuous replay cannot start without a way to release queued work");
    continuous.stopConsumer = [&]
    {
        ++stopped;
    };
    require(!StartNativeGuestDrawCapture(root, error, {}, {}, continuous),
            "continuous replay requires complete frame consumers");
    require(!std::filesystem::exists(root), "invalid continuous setup must not create output");
    require(StartNativeGuestDrawCapture(root, error, consume, endFrame, continuous),
            "continuous replay starts unarmed");
    StopNativeGuestDrawCapture();
    StopNativeGuestDrawCapture();
    const auto idleStop = toml::parse_file((root / "result.toml").string());
    require(stopped == 1 && idleStop["complete"].value_or(false) &&
                idleStop["frames"].value_or(-1) == 0 &&
                idleStop["stop_reason"].value_or(std::string{}) == "shutdown",
            "normal continuous shutdown releases consumers once even before arming");
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    require(StartNativeGuestDrawCapture(root, error, consume, endFrame, continuous),
            "continuous marker session starts");
    std::ofstream(root / "arm").close();
    std::ofstream(root / "stop").close();
    NotifyNativeGuestFrameBoundary(0xFFFFFFFCU, 0, 0);
    const auto markerStop = toml::parse_file((root / "result.toml").string());
    require(stopped == 2 && markerStop["complete"].value_or(false) &&
                markerStop["stop_reason"].value_or(std::string{}) == "stop_marker" &&
                !std::filesystem::exists(root / "frame-0000"),
            "stop marker takes priority over arming and guest memory access");
    checkOriginal();
    StopNativeGuestDrawCapture();
    require(stopped == 2, "marker completion must not repeat shutdown callbacks");
    std::filesystem::remove(root / "arm");
    std::filesystem::remove(root / "stop");
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    require(StartNativeGuestDrawCapture(root, error, consume, endFrame, continuous),
            "continuous failure session starts");
    checkOriginal(sub_826A7138);
    StopNativeGuestDrawCapture();
    require(stopped == 3 && !toml::parse_file((root / "result.toml").string())["complete"].value_or(true),
            "gamma failure clears continuous consumers once and preserves original effects");
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    require(!StartNativeGuestDrawCapture(root, error, consume), "unpaired frame callbacks rejected");
    require(StartNativeGuestDrawCapture(root, error, consume, endFrame), "frame capture starts");
    std::ofstream(root / "arm").close();
    checkOriginal();
    require(!std::filesystem::exists(root / "result.toml"), "draws cannot arm mid-frame");
    // This executable has no guest memory runtime. An overflowing device
    // address must fail before the opening boundary reads its clear words.
    NotifyNativeGuestFrameBoundary(0xFFFFFFFCU, 0, 0);
    const auto invalid = toml::parse_file((root / "result.toml").string());
    require(!invalid["complete"].value_or(true) &&
                invalid["error"].value_or(std::string{}) == "guest draw input exceeds address or allocation bounds",
            "invalid frame device must fail before consumption");
    checkOriginal();
    StopNativeGuestDrawCapture();
    std::filesystem::remove(root / "arm");
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root / "frame-0000");
    std::filesystem::remove(root);
    require(StartNativeGuestDrawCapture(root, error, consume, endFrame), "gamma capture starts before arming");
    PPCRegister gammaDevice{}, gammaStack{};
    gammaDevice.u32 = 1;
    gammaStack.u32  = 0xFFFFFFF0;
    ObserveNativeGuestGammaTable(gammaDevice, gammaStack);
    const auto badGamma = toml::parse_file((root / "result.toml").string());
    require(!badGamma["complete"].value_or(true) &&
                badGamma["error"].value_or(std::string{}) == "guest draw input exceeds address or allocation bounds",
            "gamma producer reads are checked before arming and cannot wrap the stack address");
    StopNativeGuestDrawCapture();
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    require(StartNativeGuestDrawCapture(root, error, consume, endFrame), "gamma emitter capture starts");
    checkOriginal(sub_826A7040, 0x1234, 0xFFFFFFFC);
    const auto badEmission = toml::parse_file((root / "result.toml").string());
    require(!badEmission["complete"].value_or(true) &&
                badEmission["error"].value_or(std::string{}) == "guest draw input exceeds address or allocation bounds",
            "failed gamma input capture must preserve all original emitter effects");
    StopNativeGuestDrawCapture();
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    require(StartNativeGuestDrawCapture(root, error, consume, endFrame), "PWL capture starts");
    checkOriginal(sub_826A7138);
    const auto pwl = toml::parse_file((root / "result.toml").string());
    require(!pwl["complete"].value_or(true) &&
                pwl["error"].value_or(std::string{}) == "guest native frame does not support PWL gamma",
            "PWL must fail closed without reading a 256-entry table");
    StopNativeGuestDrawCapture();
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    ObserveNativeGuestGammaTable(gammaDevice, gammaStack);
    ObserveNativeGuestPwlGamma(gammaDevice);
    require(!std::filesystem::exists(root), "disabled gamma observers cannot create artifacts or access guest memory");
    require(StartNativeGuestDrawCapture(root, error), "label capture starts");
    std::ofstream(root / "arm").close();
    checkOriginal(sub_826A3000, 0x82304258);
    const auto label = toml::parse_file((root / "result.toml").string());
    require(!label["complete"].value_or(true) &&
                label["error"].value_or(std::string{}) == "unsupported guest menu vertex stride",
            "label rejects unsupported stride before reading source geometry");
    StopNativeGuestDrawCapture();
    std::filesystem::remove(root / "arm");
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    require(StartNativeGuestDrawCapture(root, error), "scene capture starts");
    std::ofstream(root / "arm").close();
    checkOriginal(sub_826A39F8);
    const auto scene = toml::parse_file((root / "result.toml").string());
    require(!scene["complete"].value_or(true) &&
                scene["error"].value_or(std::string{}) == "unsupported guest scene base, start or index count",
            "scene rejects unsupported draw before reading bound resources");
    checkOriginal(sub_826A39F8);
    StopNativeGuestDrawCapture();
    std::filesystem::remove(root / "arm");
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root);
    return 0;
}
