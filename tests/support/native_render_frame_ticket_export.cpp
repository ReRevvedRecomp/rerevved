// NativeRenderFrameTicket::Export for builds without the D3D12 renderer. The
// Windows build links the production definition in native_renderer_d3d12.cpp;
// other platforms do not compile that source, and the live driver test reads
// tickets through the renderer-free fake. Keep the body identical to the
// production one.

#include "gpu/d3d12/native_renderer_d3d12.h"

namespace rerevved::gpu
{

std::shared_ptr<const NativeRenderFrameTextureVersion> NativeRenderFrameTicket::Export(std::uint64_t persistentKey) const
{
    std::lock_guard lock(exportMutex);
    const auto      found = exports.find(persistentKey);
    return found == exports.end() ? nullptr : found->second;
}

} // namespace rerevved::gpu
