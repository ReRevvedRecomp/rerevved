#pragma once

#include "gpu/d3d12/native_renderer_d3d12.h"

#include <cstdlib>
#include <string_view>

namespace rerevved::gpu::stream_queue
{

// With REREVVED_TEST_STREAM_QUEUE=1, every draw stream this process starts
// holds NativeRendererD3D12::kDrawStreamQueueDepth requests, as the title's
// do with native_stream_queue; otherwise each request waits for a drain.
inline const bool kStreamQueueFixture = []
{
    char*       enabled = nullptr;
    std::size_t length  = 0;
    const bool  on      = _dupenv_s(&enabled, &length, "REREVVED_TEST_STREAM_QUEUE") == 0 && enabled && std::string_view(enabled) == "1";
    std::free(enabled);
    if (on)
        NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(NativeRendererD3D12::kDrawStreamQueueDepth);
    return on;
}();

} // namespace rerevved::gpu::stream_queue
