#include "gpu/d3d12/native_frame_replay.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>
#include <vector>

using namespace rerevved::gpu;

namespace
{

NativeFrameReplayRecipe makeFrame()
{
    NativeDrawReplayRecipe draw;
    draw.schemaVersion    = 2;
    draw.width            = 640;
    draw.height           = 720;
    draw.sampleCount      = 4;
    draw.sampleMask       = 15;
    draw.vertexShaderHash = 1;
    draw.pixelShaderHash  = 2;
    draw.vertexShaderDxil.resize(4);
    draw.pixelShaderDxil.resize(4);
    draw.vertexAttributeCount = 2;
    draw.vertexStrideBytes    = 32;
    draw.vertexData.resize(3 * 32);
    draw.indices    = { 0, 1, 2 };
    draw.indexCount = 3;
    draw.vertexConstants.resize(256 * 16);
    draw.pixelConstants.resize(224 * 16);
    draw.sharedConstants.resize(336);
    draw.viewport   = { 0, 0, 1280, 720, 0, 1 };
    draw.scissor    = { 0, 0, 640, 720 };
    draw.clearColor = { 0, 0, 0, 0 };
    NativeFrameReplayRecipe frame;
    frame.halves[0].push_back(draw);
    draw.viewport.x = -640;
    frame.halves[1].push_back(draw);
    return frame;
}

} // namespace

TEST_CASE("native_frame_replay_recipe", "[ctest][native-frame-replay]")
{
    auto        frame = makeFrame();
    std::string error;
    UNSCOPED_INFO("native clear frame admission");
    REQUIRE(ValidateNativeFrameReplayRecipe(frame, error));
    {
        // Fifty draws and the other half share 12 MiB of indices, which fit
        // the 512 MiB frame bound only when charged once.
        auto shared        = makeFrame();
        auto draw          = shared.halves[0][0];
        draw.sharedIndices = std::make_shared<const NativeDrawReplayIndices>(std::vector<std::uint32_t>(3U << 20));
        draw.indices.clear();
        draw.indexCount = static_cast<std::uint32_t>(draw.sharedIndices->Values().size());
        shared.halves[0].assign(50, draw);
        shared.halves[1][0].sharedIndices = draw.sharedIndices;
        shared.halves[1][0].indices.clear();
        shared.halves[1][0].indexCount = draw.indexCount;
        UNSCOPED_INFO("frame replay charges a shared index buffer once across draws and halves");
        REQUIRE(ValidateNativeFrameReplayRecipe(shared, error));
    }
    frame.halves[0][0].clearColor[3] = 255;
    frame.halves[1][0].clearColor[3] = 255;
    UNSCOPED_INFO("opaque boot clear admission");
    REQUIRE(ValidateNativeFrameReplayRecipe(frame, error));
    frame.halves[0][0].clearColor[3] = 128;
    UNSCOPED_INFO("unsupported clear alpha rejected");
    REQUIRE(!ValidateNativeFrameReplayRecipe(frame, error));
    frame.halves[0][0].clearColor[3] = 255;
    frame.halves[0].push_back(frame.halves[0][0]);
    UNSCOPED_INFO("clear alpha belongs to the first draw only");
    REQUIRE(!ValidateNativeFrameReplayRecipe(frame, error));
    frame                                 = makeFrame();
    frame.halves[0][0].depth.enabled      = true;
    frame.halves[0][0].depth.writeEnabled = true;
    UNSCOPED_INFO("native far depth clear admission");
    REQUIRE(ValidateNativeFrameReplayRecipe(frame, error));
    frame.halves[0][0].initialSample0.resize(640 * 720 * 4);
    frame.halves[0][0].initialSample1.resize(640 * 720 * 4);
    UNSCOPED_INFO("captured color attachments rejected");
    REQUIRE(!ValidateNativeFrameReplayRecipe(frame, error));
    frame = makeFrame();
    frame.halves[0][0].depth.initialSamples.resize(640 * 720 * 8);
    UNSCOPED_INFO("captured depth attachments rejected");
    REQUIRE(!ValidateNativeFrameReplayRecipe(frame, error));
    frame = makeFrame();
    frame.halves[1].clear();
    UNSCOPED_INFO("missing half rejected");
    REQUIRE(!ValidateNativeFrameReplayRecipe(frame, error));
    frame                    = makeFrame();
    frame.halves[0][0].width = 1280;
    UNSCOPED_INFO("foreign resolve extent rejected");
    REQUIRE(!ValidateNativeFrameReplayRecipe(frame, error));
    frame                         = makeFrame();
    frame.halves[1][0].indices[2] = 3;
    UNSCOPED_INFO("draw geometry validation retained");
    REQUIRE(!ValidateNativeFrameReplayRecipe(frame, error));
    frame = makeFrame();
    frame.halves[0].resize(1024, frame.halves[0][0]);
    UNSCOPED_INFO("aggregate draw bound enforced");
    REQUIRE(!ValidateNativeFrameReplayRecipe(frame, error));
    NativeDrawFramesRecipe frames;
    auto                   ui = makeFrame().halves[0][0];
    ui.width                  = 1280;
    ui.scissor.right          = 1280;
    frames.frames             = { { ui, ui }, { ui }, { ui } };
    UNSCOPED_INFO("consecutive full UI frames admitted");
    REQUIRE(ValidateNativeDrawFramesRecipe(frames, error));
    auto singleFrame = frames.frames[0];
    UNSCOPED_INFO("single full UI frame admitted without copying");
    REQUIRE(ValidateNativeDrawFrame(singleFrame, error));
    auto wideHalf = makeFrame().halves[0];
    wideHalf.resize(257, wideHalf.front());
    auto wideFrame      = makeFrame();
    wideFrame.halves[0] = std::move(wideHalf);
    UNSCOPED_INFO("wide frame half retains its aggregate draw bound");
    REQUIRE(ValidateNativeFrameReplayRecipe(wideFrame, error));
    auto sharedFrame = makeFrame();
    auto payload     = std::make_shared<const std::vector<std::uint8_t>>(1024U * 1024U * 4U, 255);
    for (auto& half : sharedFrame.halves)
    {
        auto& texture = half.front().textures[0];
        texture.width = texture.height = 1024;
        texture.sharedBytes            = payload;
        half.front().textureMask       = 1;
        half.resize(128, half.front());
    }
    payload.reset();
    UNSCOPED_INFO("repeated gameplay draws retain and charge one shared texture payload across both halves");
    REQUIRE(ValidateNativeFrameReplayRecipe(sharedFrame, error));
    singleFrame[1].initialSample0.resize(1280 * 720 * 4);
    UNSCOPED_INFO("single frame captured color attachments rejected");
    REQUIRE(!ValidateNativeDrawFrame(singleFrame, error));
    frames.frames[0][0].clearColor[3] = 255;
    UNSCOPED_INFO("guest opaque initial clear admitted");
    REQUIRE(ValidateNativeDrawFramesRecipe(frames, error));
    frames.frames[0][1].clearColor[3] = 255;
    UNSCOPED_INFO("later guest draw cannot supply clear alpha");
    REQUIRE(!ValidateNativeDrawFramesRecipe(frames, error));
    frames.frames[0][1].clearColor[3] = 0;
    frames.frames[0][0].clearColor[3] = 128;
    UNSCOPED_INFO("unsupported guest clear alpha rejected");
    REQUIRE(!ValidateNativeDrawFramesRecipe(frames, error));
    frames.frames[0][0].clearColor[3] = 0;
    frames.frames[1].clear();
    UNSCOPED_INFO("empty intermediate UI frame rejected");
    REQUIRE(!ValidateNativeDrawFramesRecipe(frames, error));
    frames.frames[1] = { ui };
    frames.frames[2][0].initialSample0.resize(1280 * 720 * 4);
    UNSCOPED_INFO("UI frame cannot begin from oracle pixels");
    REQUIRE(!ValidateNativeDrawFramesRecipe(frames, error));
    frames.frames[2][0]               = ui;
    frames.frames[0][0].depth.enabled = true;
    UNSCOPED_INFO("scene depth retained before UI draws");
    REQUIRE(ValidateNativeDrawFramesRecipe(frames, error));
    frames.frames[0][0].depth.initialClear = 0.0F;
    UNSCOPED_INFO("menu frame requires a native far clear");
    REQUIRE(!ValidateNativeDrawFramesRecipe(frames, error));
    frames.frames[0][0].depth.initialClear = 1.0F;
    frames.frames[0][0].depth.initialSamples.resize(1280 * 720 * 8);
    UNSCOPED_INFO("menu frame cannot import captured depth");
    REQUIRE(!ValidateNativeDrawFramesRecipe(frames, error));
    frames.frames[0][0] = ui;
    frames.frames[1].resize(257, ui);
    UNSCOPED_INFO("UI per-frame draw bound enforced");
    REQUIRE(!ValidateNativeDrawFramesRecipe(frames, error));
}
