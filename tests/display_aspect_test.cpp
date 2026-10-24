#include "display_aspect.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <rex/cvar.h>

namespace
{

namespace da = rerevved::display_aspect;

bool near(double actual, double expected, double tolerance = 1e-6)
{
    return std::abs(actual - expected) < tolerance;
}

void putFloat(da::FrustumBytes& bytes, size_t offset, float value)
{
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    bytes[offset + 0]   = static_cast<uint8_t>(bits >> 24);
    bytes[offset + 1]   = static_cast<uint8_t>(bits >> 16);
    bytes[offset + 2]   = static_cast<uint8_t>(bits >> 8);
    bytes[offset + 3]   = static_cast<uint8_t>(bits);
}

da::FrustumBytes frustumBytes(float left, float right, float top, float bottom, bool ortho = false)
{
    da::FrustumBytes bytes{};
    putFloat(bytes, 0, left);
    putFloat(bytes, 4, right);
    putFloat(bytes, 8, top);
    putFloat(bytes, 12, bottom);
    putFloat(bytes, 16, 1.0f);
    putFloat(bytes, 20, 1000.0f);
    bytes[24] = ortho ? 1 : 0;
    bytes[25] = 0xAB;
    bytes[26] = 0xCD;
    bytes[27] = 0xEF;
    return bytes;
}

da::Frustum frustum(float left, float right, float top, float bottom)
{
    return da::Frustum{ left, right, top, bottom, 1.0f, 1000.0f };
}

da::Request wideRequest(uint32_t x, uint32_t y)
{
    return da::ResolveRequestedAspect(da::Mode::Custom, std::to_string(x) + ":" + std::to_string(y), {});
}

da::PolicyInputs worldInputs()
{
    da::PolicyInputs in;
    in.request        = wideRequest(21, 9);
    in.keyKnown       = true;
    in.key            = da::kGameplayKey;
    in.movie          = da::MovieState::Inactive;
    in.cameraAccepted = true;
    return in;
}

} // namespace

TEST_CASE("custom aspects parse strictly and reduce", "[ctest][display-aspect][local-fast]")
{
    const auto wide = da::ParseAspect("21:9");
    REQUIRE(wide);
    CHECK(*wide == da::Aspect{ 7, 3 });

    const auto monitor = da::ParseAspect(" 3440:1440 ");
    REQUIRE(monitor);
    CHECK(*monitor == da::Aspect{ 43, 18 });

    for (const char* invalid : { "0:9", "21:0", "21:9x", "21:9:1", "", ":", "21", "21:", ":9", "-21:9", "+21:9", "21.0:9", "21 : 9", "4294967296:9", "99999999999999999999:9" })
    {
        INFO(invalid);
        CHECK_FALSE(da::ParseAspect(invalid));
    }
    CHECK(da::ParseAspect("4294967295:4294967295").value() == da::Aspect{ 1, 1 });
}

TEST_CASE("requests at or below 16:9 and unknown surfaces stay original", "[ctest][display-aspect][local-fast]")
{
    const auto expectOriginal = [](const da::Request& request, da::Reason reason)
    {
        CHECK_FALSE(request.wide);
        CHECK(request.k == 1.0f);
        CHECK(request.aspect == da::kOriginalAspect);
        CHECK(request.reason == reason);
    };
    expectOriginal(da::ResolveRequestedAspect(da::Mode::Custom, "16:10", {}), da::Reason::NarrowAspect);
    expectOriginal(da::ResolveRequestedAspect(da::Mode::Custom, "16:9", {}), da::Reason::NarrowAspect);
    expectOriginal(da::ResolveRequestedAspect(da::Mode::Custom, "1920:1080", {}), da::Reason::NarrowAspect);
    expectOriginal(da::ResolveRequestedAspect(da::Mode::Custom, "21:x", {}), da::Reason::InvalidAspect);
    expectOriginal(da::ResolveRequestedAspect(da::Mode::Auto, "21:9", {}), da::Reason::SurfaceUnknown);
    expectOriginal(da::ResolveRequestedAspect(da::Mode::Auto, "21:9", { 0, 1440 }), da::Reason::SurfaceUnknown);
    expectOriginal(da::ResolveRequestedAspect(da::Mode::Auto, "21:9", { 1920, 1200 }), da::Reason::NarrowAspect);
    expectOriginal(da::ResolveRequestedAspect(da::Mode::Original, "21:9", { 3440, 1440 }), da::Reason::Disabled);
}

TEST_CASE("horizontal scale follows the reduced target aspect", "[ctest][display-aspect][local-fast]")
{
    const auto k21 = wideRequest(21, 9);
    REQUIRE(k21.wide);
    CHECK(k21.aspect == da::Aspect{ 7, 3 });
    CHECK(near(k21.k, 16.0 / 21.0));

    const auto monitor = da::ResolveRequestedAspect(da::Mode::Auto, "21:9", { 3440, 1440 });
    REQUIRE(monitor.wide);
    CHECK(monitor.aspect == da::Aspect{ 43, 18 });
    CHECK(near(monitor.k, 32.0 / 43.0));
    CHECK_FALSE(near(monitor.k, 16.0 / 21.0));

    const auto super = wideRequest(32, 9);
    REQUIRE(super.wide);
    CHECK(near(super.k, 0.5));

    const auto custom = da::ResolveRequestedAspect(da::Mode::Custom, "3440:1440", { 1920, 1080 });
    REQUIRE(custom.wide);
    CHECK(custom.aspect == da::Aspect{ 43, 18 });
}

TEST_CASE("the frustum widens about its own horizontal center", "[ctest][display-aspect][local-fast]")
{
    auto symmetric = da::ScaleFrustumHorizontal(frustum(-0.5f, 0.5f, 0.5f, -0.5f), static_cast<float>(16.0 / 21.0));
    REQUIRE(symmetric.status == da::FrustumStatus::Ok);
    CHECK(near(symmetric.frustum.left, -21.0 / 32.0));
    CHECK(near(symmetric.frustum.right, 21.0 / 32.0));
    CHECK(symmetric.frustum.top == 0.5f);
    CHECK(symmetric.frustum.bottom == -0.5f);
    CHECK(symmetric.frustum.nearPlane == 1.0f);
    CHECK(symmetric.frustum.farPlane == 1000.0f);

    auto offCenter = da::ScaleFrustumHorizontal(frustum(-0.25f, 0.75f, 0.5f, -0.5f), static_cast<float>(16.0 / 21.0));
    REQUIRE(offCenter.status == da::FrustumStatus::Ok);
    CHECK(near(offCenter.frustum.left, -13.0 / 32.0));
    CHECK(near(offCenter.frustum.right, 29.0 / 32.0));
    CHECK(offCenter.frustum.top == 0.5f);
    CHECK(offCenter.frustum.bottom == -0.5f);

    auto super = da::ScaleFrustumHorizontal(frustum(-0.5f, 0.5f, 0.5f, -0.5f), 0.5f);
    REQUIRE(super.status == da::FrustumStatus::Ok);
    CHECK(near(super.frustum.left, -1.0));
    CHECK(near(super.frustum.right, 1.0));
    CHECK(super.frustum.top == 0.5f);
    CHECK(super.frustum.bottom == -0.5f);
}

TEST_CASE("invalid frusta and scales are refused unchanged", "[ctest][display-aspect][local-fast]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(da::ScaleFrustumHorizontal(frustum(-0.5f, 0.5f, 0.5f, -0.5f), 0.0f).status == da::FrustumStatus::InvalidScale);
    CHECK(da::ScaleFrustumHorizontal(frustum(-0.5f, 0.5f, 0.5f, -0.5f), 1.5f).status == da::FrustumStatus::InvalidScale);
    CHECK(da::ScaleFrustumHorizontal(frustum(-0.5f, 0.5f, 0.5f, -0.5f), nan).status == da::FrustumStatus::InvalidScale);
    CHECK(da::ScaleFrustumHorizontal(frustum(nan, 0.5f, 0.5f, -0.5f), 0.5f).status == da::FrustumStatus::NonFinite);
    CHECK(da::ScaleFrustumHorizontal(frustum(0.5f, -0.5f, 0.5f, -0.5f), 0.5f).status == da::FrustumStatus::BadBounds);
    CHECK(da::ScaleFrustumHorizontal(frustum(-0.5f, 0.5f, 0.5f, 0.5f), 0.5f).status == da::FrustumStatus::BadBounds);
    da::Frustum badDepth = frustum(-0.5f, 0.5f, 0.5f, -0.5f);
    badDepth.farPlane    = 0.5f;
    CHECK(da::ScaleFrustumHorizontal(badDepth, 0.5f).status == da::FrustumStatus::BadDepth);
}

TEST_CASE("frustum words change only left and right and never accumulate", "[ctest][display-aspect][local-fast]")
{
    const float      k        = static_cast<float>(16.0 / 21.0);
    const auto       original = frustumBytes(-0.5f, 0.5f, 0.5f, -0.5f);
    da::FrustumBytes first{};
    da::FrustumBytes second{};
    REQUIRE(da::CorrectFrustumBytes(original, k, first) == da::FrustumStatus::Ok);
    // A second correction starts from the same original words, never from the first result.
    REQUIRE(da::CorrectFrustumBytes(original, k, second) == da::FrustumStatus::Ok);
    CHECK(first == second);
    CHECK(first != original);

    // Everything after the right word is byte-identical, including the unused tail.
    CHECK(std::memcmp(first.data() + 8, original.data() + 8, original.size() - 8) == 0);

    da::Frustum decoded;
    bool        ortho = true;
    REQUIRE(da::DecodeFrustum(first, decoded, ortho));
    CHECK_FALSE(ortho);
    CHECK(near(decoded.left, -21.0 / 32.0));
    CHECK(near(decoded.right, 21.0 / 32.0));

    da::FrustumBytes restored = first;
    std::memcpy(restored.data(), original.data(), 8);
    CHECK(restored == original);

    da::FrustumBytes ortho_out{};
    CHECK(da::CorrectFrustumBytes(frustumBytes(-0.5f, 0.5f, 0.5f, -0.5f, true), k, ortho_out) == da::FrustumStatus::Orthographic);
    CHECK(ortho_out == frustumBytes(-0.5f, 0.5f, 0.5f, -0.5f, true));
}

TEST_CASE("the GFx affine row is compressed about the guest center", "[ctest][display-aspect][local-fast]")
{
    const float      k = static_cast<float>(16.0 / 21.0);
    const da::Affine in{ 2.0f, -1.0f, 0.5f };

    const auto full = da::CorrectGfxAffine(in, 0.0f, 1280.0f, k);
    REQUIRE(full.status == da::AffineStatus::Ok);
    CHECK(near(full.affine.a, 32.0 / 21.0));
    CHECK(near(full.affine.b, -16.0 / 21.0));
    CHECK(near(full.affine.tx, 8.0 / 21.0));

    const auto partial = da::CorrectGfxAffine(in, 100.0f, 800.0f, k);
    REQUIRE(partial.status == da::AffineStatus::Ok);
    CHECK(near(partial.affine.a, 32.0 / 21.0));
    CHECK(near(partial.affine.b, -16.0 / 21.0));
    CHECK(near(partial.affine.tx, 13.0 / 28.0));
    CHECK(near(partial.affine.tx - 8.0 / 21.0, 1.0 / 12.0));

    // The input is never modified, so a repeated correction cannot accumulate.
    const auto again = da::CorrectGfxAffine(in, 100.0f, 800.0f, k);
    CHECK(again.affine.tx == partial.affine.tx);

    CHECK(da::CorrectGfxAffine(in, 0.0f, 0.0f, k).status == da::AffineStatus::BadViewport);
    CHECK(da::CorrectGfxAffine(in, 1000.0f, 400.0f, k).status == da::AffineStatus::BadViewport);
    CHECK(da::CorrectGfxAffine(in, -1.0f, 100.0f, k).status == da::AffineStatus::BadViewport);
    CHECK(da::CorrectGfxAffine(in, 0.0f, 1280.0f, 0.0f).status == da::AffineStatus::InvalidScale);
    CHECK(da::CorrectGfxAffine({ std::numeric_limits<float>::infinity(), 0, 0 }, 0.0f, 1280.0f, k).status == da::AffineStatus::NonFinite);
}

TEST_CASE("the scissor rectangle shrinks outward-rounded and stays inside the output", "[ctest][display-aspect][local-fast]")
{
    const float k = static_cast<float>(16.0 / 21.0);

    const auto full = da::CorrectScissor({ 0, 10, 1280, 700 }, k);
    CHECK(full == da::Rect{ 152, 10, 1128, 700 });

    const auto partial = da::CorrectScissor({ 100, 0, 900, 720 }, k);
    CHECK(partial.left == static_cast<int32_t>(std::floor(640.0 + k * (100.0 - 640.0))));
    CHECK(partial.right == static_cast<int32_t>(std::ceil(640.0 + k * (900.0 - 640.0))));
    CHECK(partial.top == 0);
    CHECK(partial.bottom == 720);

    const auto beyond = da::CorrectScissor({ -50, 0, 1400, 720 }, 0.5f);
    CHECK(beyond.left >= 0);
    CHECK(beyond.right <= 1280);

    const da::Rect empty{ 600, 0, 600, 720 };
    CHECK(da::CorrectScissor(empty, k) == empty);
    const da::Rect inverted{ 700, 0, 600, 720 };
    CHECK(da::CorrectScissor(inverted, k) == inverted);
    CHECK(da::CorrectScissor({ 0, 0, 1280, 720 }, 1.0f) == da::Rect{ 0, 0, 1280, 720 });
}

TEST_CASE("a compressed viewport reproduces the affine correction without double compression", "[ctest][display-aspect][local-fast]")
{
    const float k = static_cast<float>(16.0 / 21.0);

    da::ViewportSpan full;
    REQUIRE(da::CompressViewportSpan(0, 1280, k, full));
    CHECK(full == da::ViewportSpan{ 152, 976 });
    da::ViewportSpan status;
    REQUIRE(da::CompressViewportSpan(830, 360, k, status));
    CHECK(status == da::ViewportSpan{ 784, 276 });
    // The compressed span has the rounding of the compressed scissor.
    const da::Rect scissor = da::CorrectScissor({ 830, 0, 1190, 720 }, k);
    CHECK(status.x == scissor.left);
    CHECK(status.x + status.width == scissor.right);

    CHECK_FALSE(da::CompressViewportSpan(0, 0, k, status));
    CHECK_FALSE(da::CompressViewportSpan(-1, 100, k, status));
    CHECK_FALSE(da::CompressViewportSpan(1200, 200, k, status));
    CHECK_FALSE(da::CompressViewportSpan(0, 1280, 0.0f, status));
    CHECK(status == da::ViewportSpan{ 784, 276 });
    da::ViewportSpan same;
    REQUIRE(da::CompressViewportSpan(0, 1280, 1.0f, same));
    CHECK(same == da::ViewportSpan{ 0, 1280 });

    // At NDC u, affine correction in the original viewport should match
    // viewport compression with the original affine, within rounding error.
    const auto oldX = [&](int32_t vx, int32_t vw, double u)
    {
        const auto row = da::CorrectGfxAffine({ 1.0f, 0.0f, 0.0f }, static_cast<float>(vx), static_cast<float>(vw), k);
        return vx + vw / 2.0 * (1.0 + row.affine.a * u + row.affine.tx);
    };
    const auto newX = [&](const da::ViewportSpan& span, double u)
    {
        return span.x + span.width / 2.0 * (1.0 + u);
    };
    const auto idealX = [&](int32_t vx, int32_t vw, double u)
    {
        return 640.0 + k * (vx + vw / 2.0 * (1.0 + u) - 640.0);
    };
    for (const auto& vp : { std::pair{ 0, 1280 }, std::pair{ 830, 360 }, std::pair{ 704, 576 }, std::pair{ 64, 215 }, std::pair{ 421, 600 } })
    {
        da::ViewportSpan span;
        REQUIRE(da::CompressViewportSpan(vp.first, vp.second, k, span));
        for (const double u : { -1.0, -0.25, 0.0, 0.6, 1.0 })
        {
            CHECK(near(oldX(vp.first, vp.second, u), idealX(vp.first, vp.second, u), 1e-3));
            // Outward integer rounding costs less than one guest pixel.
            CHECK(std::fabs(newX(span, u) - idealX(vp.first, vp.second, u)) < 1.0);
            // Geometry inside the original viewport stays inside the compressed one.
            CHECK(newX(span, u) >= span.x);
            CHECK(newX(span, u) <= span.x + span.width);
        }
        // The compressed span covers the exact compressed interval.
        CHECK(span.x <= idealX(vp.first, vp.second, -1.0) + 1e-9);
        CHECK(span.x + span.width >= idealX(vp.first, vp.second, 1.0) - 1e-9);
    }

    // Compressing the request before clipping to the compressed viewport
    // should match clipping first, then compressing.
    for (const da::Rect& request : { da::Rect{ 0, 0, 1280, 720 }, da::Rect{ 900, 10, 1000, 700 }, da::Rect{ 700, 0, 1500, 720 }, da::Rect{ 0, 0, 840, 720 } })
    {
        for (const std::pair<int32_t, int32_t>& vp : { std::pair{ 830, 360 }, std::pair{ 0, 1280 } })
        {
            da::ViewportSpan span;
            REQUIRE(da::CompressViewportSpan(vp.first, vp.second, k, span));
            const int32_t  left  = std::max(vp.first, request.left);
            const int32_t  right = std::min(vp.first + vp.second, request.right);
            const da::Rect old   = da::CorrectScissor({ left, request.top, right, request.bottom }, k);
            const da::Rect now   = da::CorrectScissorInViewport(request, span, k);
            if (right > left)
            {
                CHECK(now.left == old.left);
                CHECK(now.right == old.right);
            }
            CHECK(now.top == request.top);
            CHECK(now.bottom == request.bottom);
            CHECK(now.left >= span.x);
            CHECK(now.right <= span.x + span.width);
        }
    }
}

TEST_CASE("frame policy returns the first failing reason", "[ctest][display-aspect][local-fast]")
{
    const auto expect = [](da::PolicyInputs in, da::Reason reason)
    {
        const auto decision = da::DecideFramePolicy(in);
        INFO(da::ReasonToken(reason));
        CHECK_FALSE(decision.wide);
        CHECK(decision.aspect == da::kOriginalAspect);
        CHECK(decision.k == 1.0f);
        CHECK(decision.reason == reason);
    };

    const auto world = da::DecideFramePolicy(worldInputs());
    CHECK(world.wide);
    CHECK(world.aspect == da::Aspect{ 7, 3 });
    CHECK(near(world.k, 16.0 / 21.0));
    CHECK(world.reason == da::Reason::World);
    CHECK(da::ReasonToken(world.reason) == "world");

    auto in    = worldInputs();
    in.request = da::ResolveRequestedAspect(da::Mode::Original, "", {});
    expect(in, da::Reason::Disabled);

    in         = worldInputs();
    in.request = da::ResolveRequestedAspect(da::Mode::Custom, "bad", {});
    expect(in, da::Reason::InvalidAspect);

    in       = worldInputs();
    in.xenos = false;
    expect(in, da::Reason::NativeRoute);

    in          = worldInputs();
    in.keyKnown = false;
    expect(in, da::Reason::StateUnknown);

    in     = worldInputs();
    in.key = da::kAttractKey;
    expect(in, da::Reason::Attract);

    in     = worldInputs();
    in.key = 3;
    expect(in, da::Reason::Frontend);

    in       = worldInputs();
    in.movie = da::MovieState::Active;
    expect(in, da::Reason::Movie);

    in       = worldInputs();
    in.movie = da::MovieState::Unknown;
    expect(in, da::Reason::MovieUnknown);

    in                = worldInputs();
    in.cameraAccepted = false;
    expect(in, da::Reason::CameraUnknown);

    in                = worldInputs();
    in.cameraAccepted = false;
    in.cameraReason   = da::Reason::CameraOrtho;
    expect(in, da::Reason::CameraOrtho);

    in         = worldInputs();
    in.earlyUi = true;
    expect(in, da::Reason::EarlyUi);

    in            = worldInputs();
    in.earlyMovie = true;
    expect(in, da::Reason::Movie);

    in              = worldInputs();
    in.unclassified = true;
    expect(in, da::Reason::Unclassified);

    in       = worldInputs();
    in.fitOk = false;
    expect(in, da::Reason::FitConflict);

    in                    = worldInputs();
    in.transportAvailable = false;
    expect(in, da::Reason::TransportUnavailable);

    in             = worldInputs();
    in.lateInhibit = da::Reason::LateMovie;
    expect(in, da::Reason::LateMovie);
}

TEST_CASE("probe output stays bounded", "[ctest][display-aspect][local-fast]")
{
    da::LineBudget    budget;
    constexpr int64_t second  = 1000000000LL;
    uint32_t          allowed = 0;
    for (int index = 0; index < 100; ++index)
        allowed += budget.Allow(index * (second / 200), 4) ? 1 : 0;
    CHECK(allowed == 4);
    CHECK(budget.Allow(second + 1, 4));

    da::FrameEmission emission;
    uint32_t          emitted = 0;
    for (uint64_t interval = 0; interval < 1000; ++interval)
        emitted += emission.Decide(interval, false) ? 1 : 0;
    // 120 detailed intervals plus one sample in 60 of the rest.
    CHECK(emitted == 120 + 15);
    CHECK(emission.Lines() == emitted);

    da::FrameEmission anomalous;
    for (uint64_t interval = 0; interval < 130; ++interval)
        anomalous.Decide(interval, false);
    CHECK_FALSE(anomalous.Decide(130, false));
    CHECK(anomalous.Decide(131, true));
    CHECK(anomalous.Decide(250, false));
    CHECK_FALSE(anomalous.Decide(252, false));

    da::FrameEmission capped;
    uint32_t          total = 0;
    for (uint64_t interval = 0; interval < 100000; ++interval)
        total += capped.Decide(interval, true) ? 1 : 0;
    CHECK(total == da::FrameEmission::kLineBudget);

    da::FamilyTable table;
    for (uint32_t index = 0; index < da::FamilyTable::kCapacity; ++index)
    {
        da::FamilyKey key;
        key.hook     = index;
        bool created = false;
        REQUIRE(table.Find(key, created) != nullptr);
        CHECK(created);
    }
    da::FamilyKey existing;
    existing.hook = 7;
    bool  created = true;
    auto* family  = table.Find(existing, created);
    REQUIRE(family != nullptr);
    CHECK_FALSE(created);
    CHECK(table.IndexOf(family) == 7);

    da::FamilyKey extra;
    extra.hook = 1000;
    CHECK(table.Find(extra, created) == nullptr);
    CHECK(table.Count() == da::FamilyTable::kCapacity);
    CHECK(table.Overflow() == 1);
}

TEST_CASE("the feature is inert by default and no frame is wide without owner acceptance", "[ctest][display-aspect][local-fast]")
{
    da::ResetForTesting();
    da::Initialize("title", "sdk");
    CHECK_FALSE(da::Active());
    CHECK_FALSE(da::ProbeEnabled());

    da::LatchInputs inputs;
    inputs.frameOwner     = 0x40000000;
    inputs.incomingCamera = 0x40001000;
    inputs.keyKnown       = true;
    inputs.key            = da::kGameplayKey;
    inputs.cameraReadable = true;
    inputs.cameraVtable   = da::kCameraVtable;

    const auto original = da::LatchFrame(inputs);
    CHECK(original.latched);
    CHECK_FALSE(original.wide);
    CHECK(original.k == 1.0f);
    CHECK(original.reason == da::Reason::Disabled);
    // A later hook of the same interval cannot upgrade a frozen frame.
    CHECK(da::NoteEarlyEvent(da::EarlyEvent::Ui).reason == da::Reason::Disabled);
    da::Shutdown();
    da::ResetForTesting();

    // Without a presentation transport the same request stays original.
    da::Initialize("title", "sdk");
    rex::cvar::SetFlagByName("display_aspect_custom", "21:9");
    rex::cvar::SetFlagByName("display_aspect", "auto");
    const auto unavailable = da::LatchFrame(inputs);
    CHECK_FALSE(unavailable.wide);
    CHECK(unavailable.reason == da::Reason::TransportUnavailable);
    rex::cvar::SetFlagByName("display_aspect", "original");
    rex::cvar::SetFlagByName("display_aspect_custom", "");
    da::Shutdown();
    da::ResetForTesting();

    // A frozen early UI interval is original even when a camera follows.
    da::Initialize("title", "sdk");
    CHECK(da::NoteEarlyEvent(da::EarlyEvent::Ui).reason == da::Reason::EarlyUi);
    CHECK(da::LatchFrame(inputs).reason == da::Reason::EarlyUi);
    da::Shutdown();
    da::ResetForTesting();
}

TEST_CASE("the renderer cache is rebuilt only when the scale differs from the last build", "[ctest][display-aspect][local-fast]")
{
    da::ResetForTesting();
    CHECK_FALSE(da::CacheNeedsRebuild(0x1000, 1.0f, 1));
    CHECK(da::CacheNeedsRebuild(0x1000, 0.5f, 1));
    da::NoteCacheBuilt(0x1000, 0.5f, 1);
    CHECK_FALSE(da::CacheNeedsRebuild(0x1000, 0.5f, 1));
    CHECK(da::CacheNeedsRebuild(0x1000, 1.0f, 1));
    CHECK_FALSE(da::CameraTracked(0x2000));
    CHECK(da::CacheNeedsRebuild(0x2000, 0.5f, 1));
    da::NoteCacheBuilt(0x1000, 1.0f, 1);
    CHECK_FALSE(da::CacheNeedsRebuild(0x1000, 1.0f, 1));
    CHECK_FALSE(da::CameraTracked(0x1000));
    da::ResetForTesting();
}

TEST_CASE("every wide camera is tracked and an untouched table is inert", "[ctest][display-aspect][local-fast]")
{
    da::CameraScaleTable table;
    CHECK_FALSE(table.AnyWide());
    CHECK_FALSE(table.NoteBuilt(0x1000, 1.0f, 1));
    CHECK_FALSE(table.NoteOriginalJoin(1));
    CHECK_FALSE(table.NeedsRebuild(0x1000, 1.0f, 1));
    CHECK_FALSE(table.AnyWide());

    CHECK(table.NoteBuilt(0x1000, 0.75f, 1));
    CHECK_FALSE(table.NoteBuilt(0x2000, 0.75f, 1));
    CHECK(table.NeedsRebuild(0x1000, 1.0f, 2));
    CHECK(table.NeedsRebuild(0x2000, 0.5f, 2));
    CHECK_FALSE(table.NeedsRebuild(0x2000, 0.75f, 2));
    CHECK_FALSE(table.NoteBuilt(0x1000, 0.5f, 3));
    CHECK(table.NeedsRebuild(0x2000, 0.5f, 3));
    CHECK_FALSE(table.NoteBuilt(0x1000, 1.0f, 4));
    CHECK(table.NoteBuilt(0x2000, 1.0f, 4));
    CHECK_FALSE(table.AnyWide());
}

TEST_CASE("an untracked wide build keeps every camera rebuilding until Original sweeps", "[ctest][display-aspect][local-fast]")
{
    da::CameraScaleTable table;
    for (uint32_t index = 0; index < da::CameraScaleTable::kCapacity; ++index)
        table.NoteBuilt(0x10000 + index, 0.75f, 10);
    CHECK_FALSE(table.Untracked());
    // The 65th wide camera cannot be tracked.
    table.NoteBuilt(0x90000, 0.75f, 10);
    CHECK(table.Untracked());

    // Back to Original: every tracked camera drains, yet the untracked one
    // still forces a rebuild and the state keeps hooks active.
    for (uint32_t index = 0; index < da::CameraScaleTable::kCapacity; ++index)
        table.NoteBuilt(0x10000 + index, 1.0f, 11);
    CHECK(table.AnyWide());
    CHECK(table.NeedsRebuild(0x90000, 1.0f, 11));
    CHECK_FALSE(table.NoteOriginalJoin(11));
    // Same interval does not count twice.
    CHECK_FALSE(table.NoteOriginalJoin(11));
    CHECK(table.Untracked());
    CHECK(table.NoteOriginalJoin(12));
    CHECK_FALSE(table.Untracked());
    CHECK_FALSE(table.AnyWide());
    CHECK_FALSE(table.NeedsRebuild(0x90000, 1.0f, 12));
}

TEST_CASE("a wide build between Original sweeps restarts them", "[ctest][display-aspect][local-fast]")
{
    da::CameraScaleTable table;
    for (uint32_t index = 0; index < da::CameraScaleTable::kCapacity; ++index)
        table.NoteBuilt(0x10000 + index, 0.75f, 1);
    table.NoteBuilt(0x90000, 0.75f, 1);
    CHECK_FALSE(table.NoteOriginalJoin(2));
    table.NoteBuilt(0x10000, 0.75f, 3);
    CHECK_FALSE(table.NoteOriginalJoin(4));
    CHECK(table.Untracked());
}

TEST_CASE("entries not seen for a bounded time are retired conservatively", "[ctest][display-aspect][local-fast]")
{
    da::CameraScaleTable table;
    for (uint32_t index = 0; index < da::CameraScaleTable::kCapacity; ++index)
        table.NoteBuilt(0x10000 + index, 0.75f, 1);
    // A seen camera keeps its slot; the rest age out when a new wide camera needs room.
    CHECK_FALSE(table.NeedsRebuild(0x10000, 0.75f, 500));
    const uint64_t later = 1 + da::CameraScaleTable::kMaxAge + 1;
    table.NoteBuilt(0x90000, 0.75f, later);
    CHECK(table.Tracked(0x90000));
    CHECK(table.Tracked(0x10000));
    CHECK_FALSE(table.Tracked(0x10001));
    // The retired addresses are unproven, so the state is conservative.
    CHECK(table.Untracked());
    CHECK(table.NeedsRebuild(0x10001, 1.0f, later));

    // Within the age limit no entry retires, so the next wide camera is untracked.
    da::CameraScaleTable fresh;
    for (uint32_t index = 0; index < da::CameraScaleTable::kCapacity; ++index)
        fresh.NoteBuilt(0x10000 + index, 0.75f, 1);
    fresh.NoteBuilt(0x90000, 0.75f, 2);
    CHECK(fresh.Untracked());
    CHECK_FALSE(fresh.Tracked(0x90000));
}

TEST_CASE("a changed frustum without a setter is flagged once", "[ctest][display-aspect][local-fast]")
{
    da::ResetForTesting();
    const auto a = frustumBytes(-0.5f, 0.5f, 0.5f, -0.5f);
    const auto b = frustumBytes(-0.4f, 0.4f, 0.5f, -0.5f);
    CHECK_FALSE(da::BaselineChangedWithoutSetter(0x1000, a));
    CHECK_FALSE(da::BaselineChangedWithoutSetter(0x1000, a));
    CHECK(da::BaselineChangedWithoutSetter(0x1000, b));
    da::NoteSetter(0x1000);
    CHECK_FALSE(da::BaselineChangedWithoutSetter(0x1000, a));
    da::ResetForTesting();
}

TEST_CASE("a frustum with top at or below bottom is refused", "[ctest][display-aspect][local-fast]")
{
    CHECK(da::ScaleFrustumHorizontal(frustum(-0.5f, 0.5f, -0.5f, 0.5f), 0.5f).status == da::FrustumStatus::BadBounds);
    CHECK(da::ScaleFrustumHorizontal(frustum(-0.5f, 0.5f, 0.5f, 0.5f), 0.5f).status == da::FrustumStatus::BadBounds);
    da::FrustumBytes corrected{};
    CHECK(da::CorrectFrustumBytes(frustumBytes(-0.5f, 0.5f, -0.5f, 0.5f), 0.5f, corrected) == da::FrustumStatus::BadBounds);
    CHECK(corrected == frustumBytes(-0.5f, 0.5f, -0.5f, 0.5f));
}

TEST_CASE("the hypothetical scale follows a wide custom value in every mode", "[ctest][display-aspect][local-fast]")
{
    const float k21 = static_cast<float>(16.0 / 21.0);
    CHECK(da::HypotheticalScale(da::Mode::Original, "32:9", {}) == 0.5f);
    CHECK(da::HypotheticalScale(da::Mode::Custom, "32:9", {}) == 0.5f);
    CHECK(da::HypotheticalScale(da::Mode::Auto, "32:9", {}) == 0.5f);
    CHECK(near(da::HypotheticalScale(da::Mode::Auto, "32:9", { 3440, 1440 }), 32.0 / 43.0));
    CHECK(near(da::HypotheticalScale(da::Mode::Original, "16:9", {}), k21));
    CHECK(near(da::HypotheticalScale(da::Mode::Original, "bad", {}), k21));
    CHECK(near(da::HypotheticalScale(da::Mode::Original, "16:10", {}), k21));
}

TEST_CASE("a full bounded stack refuses a push and keeps its contents", "[ctest][display-aspect][local-fast]")
{
    da::BoundedStack<uint32_t, 3> stack;
    CHECK(stack.Push(1));
    CHECK(stack.Push(2));
    CHECK(stack.Push(3));
    CHECK_FALSE(stack.Push(4));
    CHECK(stack.Count() == 3);
    CHECK(*stack.Top() == 3);
    stack.Pop();
    CHECK(stack.Push(5));
    CHECK(stack.At(0) == 1);
    CHECK(stack.At(2) == 5);
    stack.Pop();
    stack.Pop();
    stack.Pop();
    stack.Pop();
    CHECK(stack.Count() == 0);
    CHECK(stack.Top() == nullptr);
}

TEST_CASE("the request ledger separates mismatch from eviction", "[ctest][display-aspect][local-fast]")
{
    da::RequestLedger ledger;
    uint64_t          interval = 99;

    ledger.Record(1, 10, 7, 3);
    ledger.Record(2, 11, 0, 0);
    CHECK(ledger.Classify(1, 7, 3, true, interval) == da::RequestLedger::Match::Ok);
    CHECK(interval == 10);
    CHECK(ledger.Classify(2, 0, 0, false, interval) == da::RequestLedger::Match::Ok);
    CHECK(interval == 11);
    CHECK(ledger.Classify(1, 21, 9, true, interval) == da::RequestLedger::Match::Mismatch);
    CHECK(ledger.Classify(2, 0, 0, true, interval) == da::RequestLedger::Match::Mismatch);
    CHECK(ledger.Classify(77, 7, 3, true, interval) == da::RequestLedger::Match::Mismatch);

    // Past its capacity the oldest requests are evicted, not mismatched.
    for (uint64_t sequence = 3; sequence < 3 + da::RequestLedger::kCapacity; ++sequence)
        ledger.Record(sequence, sequence, 7, 3);
    CHECK(ledger.Classify(1, 7, 3, true, interval) == da::RequestLedger::Match::Evicted);
    CHECK(ledger.Classify(2, 0, 0, false, interval) == da::RequestLedger::Match::Evicted);
    CHECK(ledger.Classify(3, 7, 3, true, interval) == da::RequestLedger::Match::Ok);
    CHECK(ledger.Classify(3, 21, 9, true, interval) == da::RequestLedger::Match::Mismatch);
    CHECK(ledger.Classify(100000, 7, 3, true, interval) == da::RequestLedger::Match::Mismatch);
    CHECK(ledger.Classify(0, 0, 0, false, interval) == da::RequestLedger::Match::Mismatch);
}

TEST_CASE("the census-accepted camera vtables, frame caller and hooks", "[ctest][display-aspect][local-fast]")
{
    for (const uint32_t vtable : { 0x8200AA28u, 0x82123738u, 0x8216A3F8u })
    {
        CHECK(da::CameraVtableAccepted(vtable));
        for (const uint32_t hook : { 0x8234CF18u, 0x8234DF90u, 0x823528D8u, 0x8250BA00u, 0x82C71A00u, 0x82C49D08u })
            CHECK(da::CameraFamilyAccepted(0x82C7E108, hook, vtable));
        // Outside the world frame, or from the constructor hook, nothing is accepted.
        CHECK_FALSE(da::CameraFamilyAccepted(0, 0x8250BA00, vtable));
        CHECK_FALSE(da::CameraFamilyAccepted(0x82000000, 0x8250BA00, vtable));
        CHECK_FALSE(da::CameraFamilyAccepted(0x82C7E108, 0x8234EB00, vtable));
        CHECK_FALSE(da::CameraFamilyAccepted(0x82C7E108, 0x82C5DF60, vtable));
    }
    CHECK_FALSE(da::CameraVtableAccepted(0));
    CHECK_FALSE(da::CameraVtableAccepted(0x82164050));
    CHECK_FALSE(da::CameraFamilyAccepted(0x82C7E108, 0x8250BA00, 0x82164050));
    CHECK(da::CameraCallerAccepted(0x82C7E108));
    CHECK_FALSE(da::CameraCallerAccepted(0));
}

TEST_CASE("a camera is widened only for a symmetric 16:9 frustum", "[ctest][display-aspect][local-fast]")
{
    CHECK(da::IsSixteenByNineFrustum(frustum(-0.503f, 0.503f, 0.283f, -0.283f)));
    CHECK(da::IsSixteenByNineFrustum(frustum(-16.0f, 16.0f, 9.0f, -9.0f)));
    // Render-to-texture square, screen-space and shadow-like orthographic shapes.
    CHECK_FALSE(da::IsSixteenByNineFrustum(frustum(-0.5f, 0.5f, 0.5f, -0.5f)));
    CHECK_FALSE(da::IsSixteenByNineFrustum(frustum(0.0f, 1.0f, 1.0f, 0.0f)));
    CHECK_FALSE(da::IsSixteenByNineFrustum(frustum(-4000.0f, 4000.0f, 4000.0f, -4000.0f)));
    // Already widened, off-center and non-finite frustums.
    CHECK_FALSE(da::IsSixteenByNineFrustum(frustum(-0.6f, 0.6f, 0.283f, -0.283f)));
    CHECK_FALSE(da::IsSixteenByNineFrustum(frustum(-0.2f, 0.8f, 0.283f, -0.283f)));
    CHECK_FALSE(da::IsSixteenByNineFrustum(frustum(-0.503f, 0.503f, 0.4f, -0.1f)));
    CHECK_FALSE(da::IsSixteenByNineFrustum(frustum(-0.503f, std::numeric_limits<float>::quiet_NaN(), 0.283f, -0.283f)));
    CHECK_FALSE(da::IsSixteenByNineFrustum(da::Frustum{}));
    // Uninitialized garbage depth fails validation.
    CHECK_FALSE(da::IsSixteenByNineFrustum(da::Frustum{ -0.503f, 0.503f, 0.283f, -0.283f, 0.0f, 0.0f }));
}

TEST_CASE("the GFx scope is accepted only inside the world frame", "[ctest][display-aspect][local-fast]")
{
    CHECK(da::GfxScopeAccepted(0x8222E3F8, 0x82C7E108));
    CHECK_FALSE(da::GfxScopeAccepted(0x8222E3F8, 0));
    CHECK_FALSE(da::GfxScopeAccepted(0x8222E3F8, 0x82000000));
    CHECK_FALSE(da::GfxScopeAccepted(0x82000000, 0x82C7E108));
    CHECK_FALSE(da::GfxScopeAccepted(0, 0));
}

TEST_CASE("movie state is derived from gameplay and recent movie hooks", "[ctest][display-aspect][local-fast]")
{
    CHECK(da::DeriveMovieState(true, da::kGameplayKey, false, false) == da::MovieState::Inactive);
    CHECK(da::DeriveMovieState(true, da::kGameplayKey, true, false) == da::MovieState::Active);
    CHECK(da::DeriveMovieState(true, da::kGameplayKey, false, true) == da::MovieState::Active);
    CHECK(da::DeriveMovieState(true, 0, false, false) == da::MovieState::Unknown);
    CHECK(da::DeriveMovieState(true, da::kAttractKey, false, false) == da::MovieState::Unknown);
    CHECK(da::DeriveMovieState(false, da::kGameplayKey, false, false) == da::MovieState::Unknown);
}

TEST_CASE("original mode stays inert and the latch camera acceptance matches the census", "[ctest][display-aspect][local-fast]")
{
    da::LatchInputs inputs;
    inputs.frameOwner     = 0x40000000;
    inputs.incomingCamera = 0x40001000;
    inputs.frameCaller    = 0x82C7E108;
    inputs.keyKnown       = true;
    inputs.key            = da::kGameplayKey;
    inputs.cameraReadable = true;
    inputs.cameraVtable   = 0x82123738;
    inputs.frustum        = frustum(-0.503f, 0.503f, 0.283f, -0.283f);

    da::ResetForTesting();
    da::Initialize("title", "sdk");
    CHECK_FALSE(da::Active());
    const auto original = da::LatchFrame(inputs);
    CHECK_FALSE(original.wide);
    CHECK(original.reason == da::Reason::Disabled);
    da::Shutdown();
    da::ResetForTesting();

    // The camera acceptance the latch computes for this frame, and for the
    // same frame with a square frustum or an unaccepted caller.
    const auto accepts = [](uint32_t caller, uint32_t vtable, const da::Frustum& shape)
    {
        return da::CameraCallerAccepted(caller) && da::CameraVtableAccepted(vtable) && da::IsSixteenByNineFrustum(shape);
    };
    CHECK(accepts(inputs.frameCaller, inputs.cameraVtable, inputs.frustum));
    CHECK_FALSE(accepts(inputs.frameCaller, inputs.cameraVtable, frustum(-0.5f, 0.5f, 0.5f, -0.5f)));
    CHECK_FALSE(accepts(0x82000000, inputs.cameraVtable, inputs.frustum));
    CHECK_FALSE(accepts(inputs.frameCaller, 0x82164050, inputs.frustum));
}

TEST_CASE("a settings change racing a snapshot keeps the feature active", "[ctest][display-aspect][local-fast]")
{
    da::ChangeGate    gate;
    std::atomic<bool> active{ false };

    // The change lands after the snapshot began reading: the stale snapshot
    // computes "inactive" but the notification survives.
    gate.Consume();
    gate.Notify(active);
    gate.Publish(active, false);
    CHECK(active.load());

    // Consuming the pending change lets an inactive snapshot clear the flag.
    CHECK(gate.Consume());
    gate.Publish(active, false);
    CHECK_FALSE(active.load());
    CHECK_FALSE(gate.Consume());

    gate.Notify(active);
    CHECK(gate.Consume());
    gate.Publish(active, true);
    CHECK(active.load());

    gate.Reset();
    gate.Publish(active, false);
    CHECK_FALSE(active.load());
}
