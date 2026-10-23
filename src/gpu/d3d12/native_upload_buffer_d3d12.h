#pragma once

#include <cstdint>

#if defined(_WIN32)

#include <d3d12.h>
#include <wrl/client.h>

namespace rerevved::gpu
{

// An upload-heap buffer mapped for its whole life. Releasing the resource
// ends the mapping; upload heaps allow persistent maps.
struct NativeUploadBuffer
{
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    std::uint8_t*                          mapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS              gpu    = 0;

    explicit operator bool() const noexcept
    {
        return resource && mapped;
    }
};

// A committed upload buffer of bytes in GENERIC_READ, mapped write-only.
// Returns an empty buffer when creation or the map fails.
NativeUploadBuffer CreateNativeUploadBuffer(ID3D12Device* device, std::uint64_t bytes);

} // namespace rerevved::gpu

#endif
