#include "gpu/d3d12/native_frame_replay.h"

#include <cstdlib>
#include <iostream>

using namespace rerevved::gpu;

namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "native_frame_replay_test: " << message << '\n';
        std::exit(1);
    }
}

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

int main()
{
    auto        frame = makeFrame();
    std::string error;
    require(ValidateNativeFrameReplayRecipe(frame, error), "native clear frame admission");
    frame.halves[0][0].depth.enabled      = true;
    frame.halves[0][0].depth.writeEnabled = true;
    require(ValidateNativeFrameReplayRecipe(frame, error), "native far depth clear admission");
    frame.halves[0][0].initialSample0.resize(640 * 720 * 4);
    frame.halves[0][0].initialSample1.resize(640 * 720 * 4);
    require(!ValidateNativeFrameReplayRecipe(frame, error), "captured color attachments rejected");
    frame = makeFrame();
    frame.halves[0][0].depth.initialSamples.resize(640 * 720 * 8);
    require(!ValidateNativeFrameReplayRecipe(frame, error), "captured depth attachments rejected");
    frame = makeFrame();
    frame.halves[1].clear();
    require(!ValidateNativeFrameReplayRecipe(frame, error), "missing half rejected");
    frame                    = makeFrame();
    frame.halves[0][0].width = 1280;
    require(!ValidateNativeFrameReplayRecipe(frame, error), "foreign resolve extent rejected");
    frame                         = makeFrame();
    frame.halves[1][0].indices[2] = 3;
    require(!ValidateNativeFrameReplayRecipe(frame, error), "draw geometry validation retained");
    frame = makeFrame();
    frame.halves[0].resize(1024, frame.halves[0][0]);
    require(!ValidateNativeFrameReplayRecipe(frame, error), "aggregate draw bound enforced");
    return 0;
}
