#include "gpu/d3d12/native_gameplay_frame.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

using namespace rerevved::gpu;
using namespace rex::graphics::diagnostic;

namespace
{

DrawCaptureFrameSnapshot frameBoundary()
{
    DrawCaptureFrameSnapshot snapshot;
    snapshot.start_frame      = 40;
    snapshot.end_frame        = 41;
    snapshot.start_submission = 100;
    snapshot.end_submission   = 102;
    auto& swap                = snapshot.final_swap;
    swap.frontbuffer_ptr      = 0x100000;
    swap.output_width         = 1280;
    swap.output_height        = 720;
    swap.output_format        = 24;
    swap.gamma.gamma_256.resize(256);
    swap.gamma.is_8bpc = true;
    DrawCaptureFrameEvent event;
    event.kind             = DrawCaptureFrameEvent::Kind::kSwap;
    event.id               = 1;
    event.frame            = 40;
    event.submission       = 102;
    event.success          = true;
    event.execution_status = DrawCaptureExecutionStatus::kIssued;
    event.frontbuffer_ptr  = swap.frontbuffer_ptr;
    event.draw.draw.registers.resize(0x5003);
    snapshot.events.push_back(std::move(event));
    return snapshot;
}

DrawCaptureFrameDraw completeDrawInputs()
{
    DrawCaptureFrameDraw capture;
    capture.acquisition               = DrawCaptureAvailability::kComplete;
    capture.texture_bindings_complete = true;
    capture.draw.index.present        = true;
    capture.draw.index.availability   = DrawCaptureAvailability::kComplete;
    capture.draw.index.bytes          = { 0, 0 };
    DrawCaptureVertexFetch fetch;
    fetch.availability = DrawCaptureAvailability::kComplete;
    fetch.bytes        = { 0, 0, 0, 0 };
    capture.draw.vertex_fetches.push_back(std::move(fetch));
    return capture;
}

} // namespace

TEST_CASE("native_gameplay_frame", "[ctest][native-gameplay-frame]")
{
    NativeDrawReplaySampler sampler;
    std::string             samplerError;
    UNSCOPED_INFO("captured anisotropic sampler decodes");
    REQUIRE(DecodeNativeGameplaySampler(63616, sampler, samplerError));
    UNSCOPED_INFO("captured sampler preserves the 4x anisotropy enum and linear filters");
    REQUIRE((sampler.anisotropy == 3 && sampler.minLinear && sampler.magLinear && sampler.mipLinear));
    UNSCOPED_INFO("disabled base-map sampler retains legacy max LOD");
    REQUIRE((DecodeNativeGameplaySampler(1U << 21, sampler, samplerError) && sampler.anisotropy == 0 &&
             sampler.maxLod == 0.25F));
    UNSCOPED_INFO("anisotropic base-map sampler clamps max LOD to min LOD");
    REQUIRE((DecodeNativeGameplaySampler((1U << 14) | (1U << 21), sampler, samplerError) &&
             sampler.anisotropy == 1 && sampler.maxLod == 0.0F));
    UNSCOPED_INFO("unsupported captured anisotropy enum is rejected");
    REQUIRE((!DecodeNativeGameplaySampler(6U << 14, sampler, samplerError) &&
             samplerError.find("anisotropy") != std::string::npos));

    NativeFrameReplayRecipe     recipe;
    NativeGameplayFrameCoverage coverage;
    std::string                 error;
    auto                        snapshot = frameBoundary();
    // A completed Xenos image alone cannot satisfy the native scene contract.
    snapshot.final_swap.output_bytes.resize(1280 * 720 * 4, 255);
    UNSCOPED_INFO("final presented pixels cannot replace native geometry");
    REQUIRE((!BuildNativeGameplayFrameRecipe(snapshot, {}, recipe, coverage, error) &&
             error.find("incomplete native gameplay scene") != std::string::npos));
    UNSCOPED_INFO("failure clears partial recipes");
    REQUIRE((recipe.halves[0].empty() && recipe.halves[1].empty()));
    snapshot.events.back().frame = 41;
    UNSCOPED_INFO("events outside the completed frame are rejected");
    REQUIRE((!BuildNativeGameplayFrameRecipe(snapshot, {}, recipe, coverage, error) &&
             error.find("ownership boundary") != std::string::npos));
    snapshot = frameBoundary();
    snapshot.events.insert(snapshot.events.begin(), snapshot.events.front());
    UNSCOPED_INFO("duplicate event identities are rejected");
    REQUIRE((!BuildNativeGameplayFrameRecipe(snapshot, {}, recipe, coverage, error) &&
             error.find("event order") != std::string::npos));
    snapshot                           = frameBoundary();
    snapshot.final_swap.gamma.use_fxaa = true;
    UNSCOPED_INFO("unsupported final image processing is not silently omitted");
    REQUIRE((!BuildNativeGameplayFrameRecipe(snapshot, {}, recipe, coverage, error) &&
             error.find("presentation state") != std::string::npos));
    snapshot                                 = frameBoundary();
    snapshot.events.front().execution_status = DrawCaptureExecutionStatus::kUnfinished;
    UNSCOPED_INFO("unfinished original execution cannot be accepted from a success flag alone");
    REQUIRE((!BuildNativeGameplayFrameRecipe(snapshot, {}, recipe, coverage, error) &&
             error.find("completion") != std::string::npos));
    snapshot                = frameBoundary();
    snapshot.schema_version = 1;
    UNSCOPED_INFO("frame schema without acquisition metadata is rejected");
    REQUIRE(!BuildNativeGameplayFrameRecipe(snapshot, {}, recipe, coverage, error));

    auto draw = completeDrawInputs();
    UNSCOPED_INFO("complete vertex fetch payload is accepted");
    REQUIRE(ValidateNativeGameplayVertexFetch(draw.draw.vertex_fetches.front()));
    UNSCOPED_INFO(error.c_str());
    REQUIRE(ValidateNativeGameplayDrawCaptureInputs(draw, {}, error));
    draw.draw.index.availability = DrawCaptureAvailability::kPartial;
    UNSCOPED_INFO("partial index payload is rejected");
    REQUIRE((!ValidateNativeGameplayDrawCaptureInputs(draw, {}, error) &&
             error.find("index payload") != std::string::npos));
    draw                                          = completeDrawInputs();
    draw.draw.vertex_fetches.front().availability = DrawCaptureAvailability::kUnavailable;
    UNSCOPED_INFO("unavailable vertex fetch payload is rejected");
    REQUIRE(!ValidateNativeGameplayVertexFetch(draw.draw.vertex_fetches.front()));
    UNSCOPED_INFO("unavailable vertex fetch payload is rejected");
    REQUIRE((!ValidateNativeGameplayDrawCaptureInputs(draw, {}, error) &&
             error.find("vertex fetch payload") != std::string::npos));
    draw = completeDrawInputs();
    draw.draw.vertex_fetches.front().bytes.clear();
    UNSCOPED_INFO("empty vertex fetch payload is rejected");
    REQUIRE(!ValidateNativeGameplayVertexFetch(draw.draw.vertex_fetches.front()));
    draw = completeDrawInputs();
    DrawCaptureTextureUse active;
    active.fetch_constant = 3;
    active.required       = true;
    active.data_available = true;
    active.availability   = DrawCaptureAvailability::kPartial;
    draw.texture_uses.push_back(active);
    const std::array<std::uint32_t, 1> slots = { 3 };
    UNSCOPED_INFO("partial required texture use is rejected");
    REQUIRE((!ValidateNativeGameplayDrawCaptureInputs(draw, slots, error) &&
             error.find("texture use") != std::string::npos));
    draw.texture_uses.front().availability = DrawCaptureAvailability::kComplete;
    DrawCaptureTextureUse inactive         = draw.texture_uses.front();
    inactive.required                      = false;
    inactive.data_available                = true;
    inactive.availability                  = DrawCaptureAvailability::kUnavailable;
    inactive.texture_index                 = DrawCaptureTextureUse::kUnavailableTexture;
    inactive.component_mapping             = 0;
    draw.texture_uses.push_back(inactive);
    UNSCOPED_INFO("inactive malformed texture companion is ignored");
    REQUIRE(ValidateNativeGameplayDrawCaptureInputs(draw, slots, error));
}
