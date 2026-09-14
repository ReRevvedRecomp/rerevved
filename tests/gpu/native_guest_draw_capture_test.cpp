#include "gpu/diagnostics/native_guest_draw_capture.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include <rex/hook.h>
#include <toml++/toml.hpp>

REX_EXTERN(sub_826A3568);
REX_EXTERN(sub_826AD150);
REX_EXTERN(sub_826A3000);

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

void checkOriginal(void (*hook)(PPCContext&, std::uint8_t*) = sub_826A3568, std::uint64_t caller = 0x1234)
{
    PPCContext ctx{};
    ctx.r1.u64    = 0x12345678;
    ctx.r3.u64    = 0x1111222233334444ULL;
    ctx.r4.u64    = 4;
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

int main()
{
    using namespace rerevved::gpu::diagnostics;
    checkOriginal();
    checkOriginal(sub_826AD150);
    checkOriginal(sub_826A3000);
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
    NativeGuestFrameConsumer endFrame = [](const auto&, bool, auto&)
    {
        return true;
    };
    require(!StartNativeGuestDrawCapture(root, error, consume), "unpaired frame callbacks rejected");
    require(StartNativeGuestDrawCapture(root, error, consume, endFrame), "frame capture starts");
    std::ofstream(root / "arm").close();
    checkOriginal();
    require(!std::filesystem::exists(root / "result.toml"), "draws cannot arm mid-frame");
    NotifyNativeGuestFrameBoundary();
    require(std::filesystem::is_directory(root / "frame-0000"), "swap arms first frame");
    NotifyNativeGuestFrameBoundary();
    const auto empty = toml::parse_file((root / "result.toml").string());
    require(!empty["complete"].value_or(true), "empty frame must fail before consumption");
    checkOriginal();
    StopNativeGuestDrawCapture();
    std::filesystem::remove(root / "arm");
    std::filesystem::remove(root / "result.toml");
    std::filesystem::remove(root / "frame-0000");
    std::filesystem::remove(root);
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
    return 0;
}
