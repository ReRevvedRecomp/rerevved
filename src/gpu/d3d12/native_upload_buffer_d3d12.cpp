#include "native_upload_buffer_d3d12.h"

#if defined(_WIN32)

namespace rerevved::gpu
{

NativeUploadBuffer CreateNativeUploadBuffer(ID3D12Device* device, std::uint64_t bytes)
{
    NativeUploadBuffer buffer;
    if (!device || bytes == 0)
        return buffer;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = bytes;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer.resource))))
        return {};
    void*             mapped = nullptr;
    const D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(buffer.resource->Map(0, &noRead, &mapped)) || !mapped)
        return {};
    buffer.mapped = static_cast<std::uint8_t*>(mapped);
    buffer.gpu    = buffer.resource->GetGPUVirtualAddress();
    return buffer;
}

} // namespace rerevved::gpu

#endif
