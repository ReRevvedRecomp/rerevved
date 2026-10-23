#include "native_renderer_d3d12.h"
#include "native_resource_pool.h"
#include "native_sample_coverage.h"
#include "native_upload_buffer_d3d12.h"
#include "native_upload_pool.h"
#include "native_vertex_residency.h"

#include "gpu/diagnostics/native_renderer_guest_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <compare>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

#if defined(_WIN32)
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <wrl/client.h>

#endif

REXCVAR_DEFINE_BOOL(native_vertex_residency, false, "ReRevved", "Bring-up only for renderer=native: keep immutable shared vertex owners in persistent default-heap chunks and bind them on later draws instead of copying their bytes to upload memory every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_vertex_residency_exact, false, "ReRevved", "With native_vertex_residency: compare the CPU bytes of every resident span with a snapshot before each hit and refuse a changed owner; no effect without native_vertex_residency")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rerevved::gpu
{

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

namespace
{

constexpr std::uint32_t kFrameCount         = 2;
constexpr DWORD         kFenceWaitTimeoutMs = 5000;

#if REREVVED_DEVELOPER
using DevClockPoint = std::chrono::steady_clock::time_point;

inline DevClockPoint DevNow() noexcept
{
    return std::chrono::steady_clock::now();
}
#else
// Timing points of developer-only accounting: shipping reads no clock.
struct DevClockPoint
{
};

inline DevClockPoint DevNow() noexcept
{
    return {};
}
#endif

bool logFailure(const char* operation, HRESULT result)
{
    REXLOG_ERROR("Native D3D12 {} failed: HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result));
    return false;
}

void enableDred()
{
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    const HRESULT                                    result = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
    if (FAILED(result))
    {
        REXLOG_WARN("Native D3D12 DRED settings unavailable: HRESULT 0x{:08X}",
                    static_cast<std::uint32_t>(result));
        return;
    }
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    REXLOG_INFO("Native D3D12 DRED enabled");
}

bool enableReplayDebugLayer()
{
    ComPtr<ID3D12Debug> debug;
    const HRESULT       result = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (FAILED(result))
    {
        REXLOG_WARN("Native D3D12 replay debug layer unavailable: HRESULT 0x{:08X}",
                    static_cast<std::uint32_t>(result));
        return false;
    }
    debug->EnableDebugLayer();
    REXLOG_INFO("Native D3D12 replay debug layer enabled");
    return true;
}

bool compileReplayShader(const char*       source,
                         const char*       entryPoint,
                         const char*       profile,
                         ComPtr<ID3DBlob>& bytecode)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT    result = D3DCompile(source,
                                         std::strlen(source),
                                         "native_draw_replay_helper",
                                         nullptr,
                                         nullptr,
                                         entryPoint,
                                         profile,
                                         D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                         0,
                                         &bytecode,
                                         &errors);
    if (FAILED(result))
    {
        if (errors)
        {
            REXLOG_ERROR("Native D3D12 replay helper shader failed: {}",
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
        return logFailure("replay helper shader compilation", result);
    }
    return true;
}

bool equivalentWindowsPath(const std::filesystem::path& left,
                           const std::filesystem::path& right)
{
    const auto leftString  = left.wstring();
    const auto rightString = right.wstring();
    return leftString.size() == rightString.size() &&
           std::equal(leftString.begin(),
                      leftString.end(),
                      rightString.begin(),
                      [](wchar_t leftCharacter, wchar_t rightCharacter)
                      {
                          return std::towlower(leftCharacter) == std::towlower(rightCharacter);
                      });
}

D3D12_BLEND mapBlendFactor(NativeDrawReplayBlendFactor factor)
{
    switch (factor)
    {
        case NativeDrawReplayBlendFactor::Zero:
            return D3D12_BLEND_ZERO;
        case NativeDrawReplayBlendFactor::One:
            return D3D12_BLEND_ONE;
        case NativeDrawReplayBlendFactor::SourceColor:
            return D3D12_BLEND_SRC_COLOR;
        case NativeDrawReplayBlendFactor::InverseSourceColor:
            return D3D12_BLEND_INV_SRC_COLOR;
        case NativeDrawReplayBlendFactor::SourceAlpha:
            return D3D12_BLEND_SRC_ALPHA;
        case NativeDrawReplayBlendFactor::InverseSourceAlpha:
            return D3D12_BLEND_INV_SRC_ALPHA;
        case NativeDrawReplayBlendFactor::DestinationAlpha:
            return D3D12_BLEND_DEST_ALPHA;
        case NativeDrawReplayBlendFactor::InverseDestinationAlpha:
            return D3D12_BLEND_INV_DEST_ALPHA;
        case NativeDrawReplayBlendFactor::DestinationColor:
            return D3D12_BLEND_DEST_COLOR;
        case NativeDrawReplayBlendFactor::InverseDestinationColor:
            return D3D12_BLEND_INV_DEST_COLOR;
        case NativeDrawReplayBlendFactor::SourceAlphaSaturated:
            return D3D12_BLEND_SRC_ALPHA_SAT;
        case NativeDrawReplayBlendFactor::BlendFactor:
            return D3D12_BLEND_BLEND_FACTOR;
        case NativeDrawReplayBlendFactor::InverseBlendFactor:
            return D3D12_BLEND_INV_BLEND_FACTOR;
    }
    return D3D12_BLEND_ONE;
}

D3D12_BLEND_OP mapBlendOp(NativeDrawReplayBlendOp op)
{
    switch (op)
    {
        case NativeDrawReplayBlendOp::Add:
            return D3D12_BLEND_OP_ADD;
        case NativeDrawReplayBlendOp::Subtract:
            return D3D12_BLEND_OP_SUBTRACT;
        case NativeDrawReplayBlendOp::ReverseSubtract:
            return D3D12_BLEND_OP_REV_SUBTRACT;
        case NativeDrawReplayBlendOp::Minimum:
            return D3D12_BLEND_OP_MIN;
        case NativeDrawReplayBlendOp::Maximum:
            return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

DXGI_FORMAT mapTextureFormat(NativeDrawReplayTextureFormat format)
{
    switch (format)
    {
        case NativeDrawReplayTextureFormat::Rgba8:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case NativeDrawReplayTextureFormat::R8:
            return DXGI_FORMAT_R8_UNORM;
        case NativeDrawReplayTextureFormat::Bc1:
            return DXGI_FORMAT_BC1_UNORM;
        case NativeDrawReplayTextureFormat::Bc2:
            return DXGI_FORMAT_BC2_UNORM;
        case NativeDrawReplayTextureFormat::Bc3:
            return DXGI_FORMAT_BC3_UNORM;
        case NativeDrawReplayTextureFormat::Rgba16Float:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case NativeDrawReplayTextureFormat::Rg16Float:
            return DXGI_FORMAT_R16G16_FLOAT;
        case NativeDrawReplayTextureFormat::R32Float:
            return DXGI_FORMAT_R32_FLOAT;
        case NativeDrawReplayTextureFormat::Rgb10a2:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case NativeDrawReplayTextureFormat::R32Uint:
            return DXGI_FORMAT_R32_UINT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT mapTargetFormat(NativeDrawReplayTargetFormat format)
{
    switch (format)
    {
        case NativeDrawReplayTargetFormat::Rgba8:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case NativeDrawReplayTargetFormat::Rgb10a2:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case NativeDrawReplayTargetFormat::Rg16Snorm:
            return DXGI_FORMAT_R16G16_SNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

constexpr std::uint64_t kMaxReplayTextureFootprintBytes = 512ULL * 1024ULL * 1024ULL;
// GPU bytes the version store keeps: about twice a world frame's decoded
// texture set (109-119 MiB over 192-229 textures in the world harvests).
constexpr std::uint64_t kVersionTextureCapBytes = 256ULL * 1024ULL * 1024ULL;

D3D12_RESOURCE_DESC replayTextureDescription(const NativeDrawReplayTextureLayout& texture)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = texture.width;
    desc.Height           = texture.height;
    desc.DepthOrArraySize = texture.dimension == NativeDrawReplayTextureDimension::TextureCube ? 6 : 1;
    desc.MipLevels        = static_cast<UINT16>(texture.mipLevels);
    desc.Format           = mapTextureFormat(texture.format);
    desc.SampleDesc.Count = 1;
    return desc;
}

D3D12_RESOURCE_DESC replayTextureDescription(const NativeDrawReplayTexture& texture)
{
    return replayTextureDescription(NativeDrawReplayTextureLayout{ texture.width, texture.height, texture.format, texture.mipLevels, texture.dimension });
}

struct ReplayTexturePreparation
{
    D3D12_RESOURCE_DESC                             description{};
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT>                               rows;
    std::vector<UINT64>                             rowBytes;
    UINT64                                          totalBytes  = 0;
    UINT64                                          packedBytes = 0;

    std::size_t StorageBytes() const noexcept
    {
        return sizeof(*this) + footprints.capacity() * sizeof(footprints[0]) +
               rows.capacity() * sizeof(rows[0]) + rowBytes.capacity() * sizeof(rowBytes[0]);
    }
};

std::shared_ptr<const ReplayTexturePreparation> prepareReplayTexture(ID3D12Device* device, const NativeDrawReplayTexture& texture)
{
    auto preparation         = std::make_shared<ReplayTexturePreparation>();
    preparation->description = replayTextureDescription(texture);
    const auto count         = static_cast<UINT>(preparation->description.DepthOrArraySize) * preparation->description.MipLevels;
    preparation->footprints.resize(count);
    preparation->rows.resize(count);
    preparation->rowBytes.resize(count);
    device->GetCopyableFootprints(&preparation->description, 0, count, 0, preparation->footprints.data(), preparation->rows.data(), preparation->rowBytes.data(), &preparation->totalBytes);
    for (UINT subresource = 0; subresource < count; ++subresource)
        preparation->packedBytes += preparation->rowBytes[subresource] * preparation->rows[subresource];
    return preparation;
}

constexpr const char* kReplayCopyShaderSource = R"hlsl(
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput copy_vs(uint vertex_id : SV_VertexID) {
    VSOutput output;
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0),
    };
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = positions[vertex_id] * 0.5 + 0.5;
    return output;
}

Texture2D<float4> source_texture : register(t0);
cbuffer CopyConstants : register(b0) {
    uint sample_a;
    uint sample_b;
    uint sample_count;
    uint padding;
};

float4 copy_ps(VSOutput input) : SV_Target0 {
    const int2 pixel = int2(input.position.xy);
    return source_texture.Load(int3(pixel, 0));
}
)hlsl";

constexpr const char* kReplayResolveShaderSource = R"hlsl(
struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput copy_vs(uint vertex_id : SV_VertexID) {
    VSOutput output;
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0),
    };
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = positions[vertex_id] * 0.5 + 0.5;
    return output;
}

Texture2DMS<float4> source_texture : register(t0);
cbuffer CopyConstants : register(b0) {
    uint sample_a;
    uint sample_b;
    uint sample_count;
    uint padding;
};

float4 copy_ps(VSOutput input) : SV_Target0 {
    const int2 pixel = int2(input.position.xy);
    if (sample_a == sample_b) {
        return source_texture.Load(pixel, sample_a);
    }
    return 0.5 * (source_texture.Load(pixel, sample_a) + source_texture.Load(pixel, sample_b));
}
)hlsl";

constexpr const char* kFrameGammaShaderSource = R"hlsl(
#ifdef SINGLE_SAMPLE
Texture2D<float4> source_texture : register(t0);
#else
Texture2DMS<float4> source_texture : register(t0);
#endif
cbuffer GammaTable : register(b0) { uint4 ramp[64]; };
uint entry(uint index) { return ramp[index >> 2][index & 3]; }
float4 gamma_ps(float4 position : SV_Position) : SV_Target0 {
    int2 xy = int2(position.xy);
#ifdef SINGLE_SAMPLE
    uint3 index = uint3(round(source_texture.Load(int3(xy, 0)).rgb * 255.0));
#else
    uint3 a = uint3(round(source_texture.Load(xy, 0).rgb * 255.0));
    uint3 b = uint3(round(source_texture.Load(xy, 3).rgb * 255.0));
    uint3 index = (a + b + 1) >> 1;
#endif
    uint3 color = uint3((entry(index.r) >> 20) & 1023,
                       (entry(index.g) >> 10) & 1023, entry(index.b) & 1023);
    return float4(float3(color) / 1023.0, 1.0);
}
)hlsl";

constexpr const char* kReplayDepthCopyShaderSource = R"hlsl(
Texture2D<float4> source_texture : register(t0);
float copy_depth(float4 position : SV_Position) : SV_Depth {
    const uint3 bytes = uint3(round(source_texture.Load(int3(int2(position.xy), 0)).yzw * 255.0));
    const uint depth = bytes.x | (bytes.y << 8) | (bytes.z << 16);
    // A rounded reciprocal of 0xFFFFFF adds one D24 unit near the far plane.
    // Split the exact power-of-two scaling from its small correction.
    precise float scaled = float(depth) * (1.0 / 16777216.0);
    precise float correction = scaled * (1.0 / 16777215.0);
    precise float normalized = scaled + correction;
    return normalized;
}
)hlsl";

constexpr const char* kFrameResolveShaderSource = R"hlsl(
#ifdef SINGLE_SAMPLE
Texture2D<float4> source_texture : register(t0);
#else
Texture2DMS<float4> source_texture : register(t0);
#endif
cbuffer ResolveConstants : register(b0) {
    uint sample_a; uint sample_b; float value_scale; uint swap_red_blue;
    int offset_x; int offset_y; uint round_unorm8; uint fixed16;
};
// k_16_16 to format 31 as the SDK's resolve_full_32bpp: the signed code times
// 0x3A800100 (32 / 32767), clamped below at -1.0 before samples average.
float2 fixed16_value(float4 loaded) {
    precise float2 value = max(float2(int2(round(loaded.rg * 32767.0))) * asfloat(0x3A800100), -1.0);
    return value;
}
float4 resolve_frame(float4 position : SV_Position) : SV_Target0 {
    int2 xy = int2(position.xy) + int2(offset_x, offset_y);
#ifdef SINGLE_SAMPLE
    float4 value = source_texture.Load(int3(xy, 0));
    float4 other = value;
#else
    float4 value = source_texture.Load(xy, sample_a);
    float4 other = source_texture.Load(xy, sample_b);
#endif
    if (fixed16)
    {
        precise float2 halves = fixed16_value(value);
        if (sample_b != sample_a)
            halves = (halves + fixed16_value(other)) * 0.5;
        // The SDK packs with f32tof16; f16tof32 is exact, so the float16
        // target stores those halves rather than its own rounding.
        return float4(f16tof32(f32tof16(halves)), 0.0, 0.0);
    }
#ifndef SINGLE_SAMPLE
    if (sample_b != sample_a)
    {
        // Preserve the exercised RGBA8 two-sample half-up resolve. A plain
        // UNORM store would round halfway values to even instead.
        if (round_unorm8)
            value = float4((uint4(round(value * 255.0)) + uint4(round(other * 255.0)) + 1) >> 1) / 255.0;
        else
            value = 0.5 * (value + other);
    }
#endif
    value *= value_scale;
    return swap_red_blue ? value.bgra : value;
}
)hlsl";

// Guest EDRAM resolve decoding/rounding map.
// Format/path         | Exact arithmetic or source
// SDK source          | e19035b
//   src/graphics/shaders/bytecode/d3d12_5_1/resolve_full_32bpp_cs.h.
// Shader provenance   | resolve_full_32bpp; Xenia resolve.xesli and
//   pixel_formats.xesli.
// Color unpack        | Float reciprocals 0x3B808081, 0x3A802008 and
//   0x3EAAAAAB; host UNORM loads recover exact integers.
// Color average       | Add two samples, scale by 0.5, saturate, truncate value
//   * max + 0.5.
// k_16_16, EDRAM 4    | Unpack signed code with 0x3A800100 (32 / 32767), clamp
//   below at -1.0; SNORM loads recover the code.
// k_16_16 destination | Average and store f32tof16 halves, destination format
//   31.
// k_24_8 depth view   | DecodeNativeTexture/convertDepthUnorm uses
//   xenos::UNorm24To32 on bits 8..31 of each word.
// Depth conversion    | n + (n >> 23), scaled by 2^-24; sum <= 2^24 converts
//   exactly and the power-of-two scale is exact.
constexpr const char* kSurfaceDepthShaderSource = R"hlsl(
Texture2D<uint> words : register(t0);
cbuffer GuestWordConstants : register(b0) {
    uint sample_a; uint sample_b; uint packing; uint padding;
    int offset_x; int offset_y;
};
float convert_depth(float4 position : SV_Position) : SV_Target0 {
    uint depth = words.Load(int3(int2(position.xy) - int2(offset_x, offset_y), 0)) >> 8;
    precise float value = float(depth + (depth >> 23)) * asfloat(0x33800000);
    return value;
}
)hlsl";

constexpr const char* kFrameGuestWordShaderSource = R"hlsl(
#ifdef SINGLE_SAMPLE
#define LOAD(source, xy, sample) source.Load(int3(xy, 0))
#define TEXTURE(type) Texture2D<type>
#else
#define LOAD(source, xy, sample) source.Load(xy, sample)
#define TEXTURE(type) Texture2DMS<type>
#endif
#ifdef DEPTH
TEXTURE(float) source_depth : register(t0);
TEXTURE(uint2) source_stencil : register(t1);
#else
TEXTURE(float4) source_color : register(t0);
#endif
cbuffer GuestWordConstants : register(b0) {
    uint sample_a; uint sample_b; uint packing; uint padding;
    int offset_x; int offset_y;
};
uint resolve_guest_words(float4 position : SV_Position) : SV_Target0 {
    int2 xy = int2(position.xy) + int2(offset_x, offset_y);
#ifdef DEPTH
    // As read_depth: the nearest 24-bit unit, exact at integer-valued floats.
    uint depth = uint(round(saturate(LOAD(source_depth, xy, sample_a)) * 16777215.0));
    return (depth << 8) | LOAD(source_stencil, xy, sample_a).y;
#else
    if (packing == 4)
    {
        precise float2 value = max(float2(int2(round(LOAD(source_color, xy, sample_a).rg * 32767.0))) * asfloat(0x3A800100), -1.0);
        if (sample_b != sample_a)
        {
            precise float2 other = max(float2(int2(round(LOAD(source_color, xy, sample_b).rg * 32767.0))) * asfloat(0x3A800100), -1.0);
            value = (value + other) * 0.5;
        }
        uint2 halves = f32tof16(value);
        return halves.r | (halves.g << 16);
    }
    bool wide = packing == 2;
    float4 maximum = wide ? float4(1023.0, 1023.0, 1023.0, 3.0) : float4(255.0, 255.0, 255.0, 255.0);
    float4 unpack = wide ? float4(asfloat(0x3A802008), asfloat(0x3A802008), asfloat(0x3A802008), asfloat(0x3EAAAAAB))
                         : asfloat(0x3B808081).xxxx;
    precise float4 value = float4(uint4(round(LOAD(source_color, xy, sample_a) * maximum))) * unpack;
    if (sample_b != sample_a)
    {
        precise float4 other = float4(uint4(round(LOAD(source_color, xy, sample_b) * maximum))) * unpack;
        value = (value + other) * 0.5;
    }
    // precise keeps the multiply and add unfused, as the CPU model computes them.
    precise float4 scaled = saturate(value) * maximum + 0.5;
    uint4 word = uint4(scaled);
    return wide ? word.r | (word.g << 10) | (word.b << 20) | (word.a << 30)
                : word.r | (word.g << 8) | (word.b << 16) | (word.a << 24);
#endif
}
)hlsl";

constexpr const char* kReplayDepthReadShaderSource = R"hlsl(
#ifdef SINGLE_SAMPLE
Texture2D<float> source_depth : register(t0);
#else
Texture2DMS<float> source_depth : register(t0);
#endif
cbuffer CopyConstants : register(b0) {
    uint sample_a; uint sample_b; uint sample_count; uint padding;
};
float4 read_depth(float4 position : SV_Position) : SV_Target0 {
#ifdef SINGLE_SAMPLE
    const float value = source_depth.Load(int3(int2(position.xy), 0));
#else
    const float value = source_depth.Load(int2(position.xy), sample_a);
#endif
    const uint depth = uint(round(saturate(value) * 16777215.0));
    return float4(0, depth & 255, (depth >> 8) & 255, depth >> 16) / 255.0;
}
)hlsl";

bool createReplayUploadBuffer(ID3D12Device*           device,
                              std::size_t             size,
                              ComPtr<ID3D12Resource>& resource)
{
    if (size == 0)
    {
        return false;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width            = size;
    desc.Height           = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels        = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT result        = device->CreateCommittedResource(&heap,
                                                            D3D12_HEAP_FLAG_NONE,
                                                            &desc,
                                                            D3D12_RESOURCE_STATE_GENERIC_READ,
                                                            nullptr,
                                                            IID_PPV_ARGS(&resource));
    return SUCCEEDED(result);
}

bool createReplayUploadBuffer(ID3D12Device*           device,
                              const void*             data,
                              std::size_t             size,
                              ComPtr<ID3D12Resource>& resource)
{
    if (!createReplayUploadBuffer(device, size, resource))
    {
        return false;
    }
    void*         mapped = nullptr;
    const HRESULT result = resource->Map(0, nullptr, &mapped);
    if (FAILED(result))
    {
        resource.Reset();
        return false;
    }
    std::memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);
    return true;
}

std::uint32_t alignConstantBytes(std::size_t size)
{
    return static_cast<std::uint32_t>((size + 255U) & ~std::size_t(255U));
}

template <typename T>
void appendReplayCacheKeyScalar(std::vector<std::uint8_t>& key, const T& value)
{
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    key.insert(key.end(), bytes, bytes + sizeof(value));
}

void appendReplayCacheKeyBytes(std::vector<std::uint8_t>&    key,
                               std::span<const std::uint8_t> bytes)
{
    appendReplayCacheKeyScalar(key, static_cast<std::uint64_t>(bytes.size()));
    key.insert(key.end(), bytes.begin(), bytes.end());
}

// Whether a key holds exactly these shader runs and state suffix, compared in
// place so a cache hit builds no key copy of the shader bytes.
bool replayGuestPipelineKeyMatches(std::span<const std::uint8_t> key,
                                   std::span<const std::uint8_t> vertex,
                                   std::span<const std::uint8_t> pixel,
                                   std::span<const std::uint8_t> state)
{
    if (key.size() != 2 * sizeof(std::uint64_t) + vertex.size() + pixel.size() + state.size() ||
        !std::equal(state.begin(), state.end(), key.end() - state.size()))
        return false;
    std::size_t offset = 0;
    for (const auto bytes : { vertex, pixel })
    {
        const auto size = static_cast<std::uint64_t>(bytes.size());
        if (std::memcmp(key.data() + offset, &size, sizeof(size)) != 0 ||
            !std::equal(bytes.begin(), bytes.end(), key.begin() + offset + sizeof(size)))
            return false;
        offset += sizeof(size) + bytes.size();
    }
    return true;
}

std::size_t replayGuestPipelineKeyCapacity(const NativeDrawReplayRecipe& recipe)
{
    return recipe.vertexShaderBytes().size() + recipe.pixelShaderBytes().size() +
           2 * sizeof(std::uint64_t) + 5 * sizeof(std::uint32_t) +
           sizeof(NativeDrawReplayBlendState) + sizeof(NativeDrawReplayRasterizer) +
           sizeof(NativeDrawReplayDepthState) + sizeof(NativeDrawReplayStencilState) +
           (recipe.extraTargetCount ? 1 + sizeof(recipe.extraTargets) : 0);
}

struct ReplayHelperPipelineCacheEntry
{
    std::uint32_t                              sampleCount = 0;
    DXGI_FORMAT                                colorFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12PipelineState>                copy;
    ComPtr<ID3D12PipelineState>                copySample3;
    ComPtr<ID3D12PipelineState>                resolve;
    ComPtr<ID3D12PipelineState>                depthRead;
    std::array<ComPtr<ID3D12PipelineState>, 2> depthCopy;
    ComPtr<ID3D12PipelineState>                gamma;

    void detach()
    {
        (void)copy.Detach();
        (void)copySample3.Detach();
        (void)resolve.Detach();
        (void)depthRead.Detach();
        for (auto& pipeline : depthCopy)
            (void)pipeline.Detach();
        (void)gamma.Detach();
    }
};

struct ReplayAttachmentKey
{
    // Replay attachments have one mip/layer, quality zero and fixed usage flags.
    std::uint32_t width, height, samples;
    DXGI_FORMAT   format;
    // Color slots 1..3 whose format differs from slot 0, as in an MRT pass.
    // Unknown means slot 0's format, so single-format keys are unchanged.
    std::array<DXGI_FORMAT, 3> extraFormats{};
    // With identityAttachments, presented passes keep attachments apart from
    // off-screen passes of the same layout, so they can resume after them.
    bool presented                                    = false;
    bool operator==(const ReplayAttachmentKey&) const = default;
};

ReplayAttachmentKey replayAttachmentKey(ID3D12Resource* resource)
{
    const auto desc = resource->GetDesc();
    return { static_cast<std::uint32_t>(desc.Width), desc.Height, desc.SampleDesc.Count, desc.Format };
}

ReplayAttachmentKey replayColorKey(const NativeRenderFramePass::Target& target)
{
    ReplayAttachmentKey key{ target.width, target.height, target.sampleCount, mapTargetFormat(target.format) };
    for (std::uint32_t slot = 1; slot < 4; ++slot)
        if (const auto format = mapTargetFormat(NativeRenderFrameSlotFormat(target, slot)); format != key.format)
            key.extraFormats[slot - 1] = format;
    return key;
}

// The key of the four live color slots; slot 0 must exist.
ReplayAttachmentKey replayColorKey(const std::array<ComPtr<ID3D12Resource>, 4>& resources)
{
    auto key = replayAttachmentKey(resources[0].Get());
    for (std::size_t slot = 1; slot < resources.size(); ++slot)
        if (resources[slot])
            if (const auto format = resources[slot]->GetDesc().Format; format != key.format)
                key.extraFormats[slot - 1] = format;
    return key;
}

struct ReplayColorAttachments
{
    ReplayAttachmentKey                   key;
    std::array<ComPtr<ID3D12Resource>, 4> resources;
};

struct ReplayDepthAttachment
{
    ReplayAttachmentKey    key;
    ComPtr<ID3D12Resource> resource;
    bool                   initialized = false; // as last parked, for a resume
};

// A resolve destination copied into a readback buffer by a recorded prefix.
// The renderer keeps its own reference through the frame's retirement.
struct D3D12RawReadback final : NativeRawReadback
{
    ComPtr<ID3D12Resource>             buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64                             bytes = 0;

    std::uint64_t Bytes() const noexcept override
    {
        return bytes;
    }

    bool Read(std::vector<std::uint8_t>& texels, std::uint32_t& rowPitch, std::string& error) const override
    {
        void*             mapped = nullptr;
        const D3D12_RANGE range{ 0, static_cast<SIZE_T>(bytes) };
        if (FAILED(buffer->Map(0, &range, &mapped)))
        {
            error = "raw_readback_map_failed";
            return false;
        }
        rowPitch = footprint.Footprint.Width * 4;
        texels.resize(std::size_t(rowPitch) * footprint.Footprint.Height);
        for (UINT y = 0; y < footprint.Footprint.Height; ++y)
            std::memcpy(texels.data() + std::size_t(y) * rowPitch,
                        static_cast<const std::uint8_t*>(mapped) + footprint.Offset + std::size_t(y) * footprint.Footprint.RowPitch,
                        rowPitch);
        const D3D12_RANGE written{ 0, 0 };
        buffer->Unmap(0, &written);
        return true;
    }
};

struct ReplayGuestPipelineCacheEntry
{
    std::vector<std::uint8_t>                            key;
    NativeDrawReplayPipelineStateKey                     state;
    std::shared_ptr<const NativeDrawReplayShaderVersion> shaderVersion;
    ComPtr<ID3D12PipelineState>                          pipeline;
    std::uint64_t                                        lastUsed = 0; // frame recording that last bound it
    // The queue signals at least this value after the command list that last
    // bound it, so a completed fence at or above it means the GPU is done.
    std::uint64_t lastUsedFence = 0;

    // The key and the shader version the entry keeps, which may be shared.
    std::uint64_t OwnedBytes() const noexcept
    {
        std::uint64_t bytes = key.capacity();
        if (shaderVersion)
        {
            bytes += sizeof(NativeDrawReplayShaderVersion);
            for (const auto& bytecode : { shaderVersion->VertexBytecode(), shaderVersion->PixelBytecode() })
                bytes += sizeof(*bytecode) + bytecode->capacity();
        }
        return bytes;
    }
};

constexpr std::size_t kMaxReplayHelperPipelineEntries = 8;
constexpr std::size_t kMaxReplayGuestPipelineEntries  = 32;
// A guest frame records each off-screen resolve as its own terminal frame,
// often after the queue drained, so a pipeline survives this many frame
// recordings without use. Storage pressure still evicts unpinned entries.
constexpr std::uint64_t kFramePipelineRetention = 64;

// Two independent draw slots bound retained storage while sharing one command list.
// A slot is rewritten only after the preceding batch fence has completed.
constexpr std::size_t kReplayBatchDraws = 2;

// A draw slot's views: three color helpers precede two bounded texture-register
// ranges, and three depth helpers follow. The two ranges use distinct register
// spaces but share one descriptor table, so each texture descriptor is duplicated.
constexpr std::size_t kDrawViewRegion = 2 * kNativeDrawReplayTextureSlots + 6;

// Frame chunks alternate owners. An owner's draws take regions of its one view
// heap, so a chunk ends at this many draws or at the command bound.
constexpr std::size_t kFrameCommandBatches = 2;
constexpr std::size_t kFrameCommandDraws   = 1024;
constexpr std::size_t kFrameBatchDraws     = kFrameCommandDraws * kFrameCommandBatches;
static_assert(kFrameCommandDraws <= NativeRenderBatchCommands);
static_assert(kFrameCommandDraws * kDrawViewRegion <= D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1);

// Keep aggregate shader-visible sampler storage within one heap's budget,
// including the standalone replay slots used for final presentation.
constexpr std::size_t kFrameSamplerEntries = (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE - kReplayBatchDraws * kNativeDrawReplayTextureSlots) / kFrameCommandBatches;

// The host bytes an owner's sampler tables reach when its heap is full.
constexpr std::size_t kFrameSamplerTableBytes = kFrameSamplerEntries * (sizeof(D3D12_SAMPLER_DESC) + sizeof(std::pair<const std::uint64_t, std::uint32_t>) + 4 * sizeof(void*));

// An owner's sampler heap of deduplicated draw tables. Entries are append-only
// while the tables live, so a recorded table stays valid through the owner's
// fence; prepareFrameCommandBatch starts over only after that wait.
struct FrameSamplerTables
{
    std::vector<D3D12_SAMPLER_DESC>                       entries;
    std::unordered_multimap<std::uint64_t, std::uint32_t> tables; // a table's byte hash to its first entry
    std::uint32_t                                         lastOffset = 0, lastCount = 0;

    void clear()
    {
        entries.clear();
        tables.clear();
        lastOffset = lastCount = 0;
    }
};

struct FrameCommandBatch
{
    ComPtr<ID3D12CommandAllocator>              allocator;
    ComPtr<ID3D12GraphicsCommandList>           commandList;
    std::uint64_t                               fenceValue = 0;
    std::vector<ReplayHelperPipelineCacheEntry> helpers;
    // Bound once per list: every draw slot's view region and the sampler tables.
    ComPtr<ID3D12DescriptorHeap> viewHeap, samplerHeap;
    FrameSamplerTables           samplers;
};

struct FrameGpuOutputSlot
{
    ComPtr<ID3D12Resource>                                resource;
    std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket> ticket;
    std::shared_ptr<NativeOutputLease>                    lease;
    std::uint64_t                                         fenceValue  = 0;
    bool                                                  initialized = false;
};

using UploadPool  = FencedUploadPool<NativeUploadBuffer>;
using UploadLease = UploadPool::Lease;

// Leaks a lease's buffer for a device loss that must not free queue-visible storage.
void detachUpload(UploadLease& lease) noexcept
{
    lease.Detach([](NativeUploadBuffer& buffer) noexcept
                 {
                     (void)buffer.resource.Detach();
                 });
}

// The allocation a default-heap texture or attachment was created with.
// GetDesc reports the placement alignment, which requests leave 0, and not
// the castable formats a guest-word texture was created with.
struct PooledTextureKey
{
    std::uint32_t dimension = 0, height = 0, format = 0, samples = 0, quality = 0, layout = 0, flags = 0;
    std::uint64_t width            = 0;
    std::uint16_t depthOrArraySize = 0, mipLevels = 0;
    bool          castable = false;

    auto operator<=>(const PooledTextureKey&) const = default;
};

PooledTextureKey pooledTextureKey(const D3D12_RESOURCE_DESC& desc, bool castable = false) noexcept
{
    return { static_cast<std::uint32_t>(desc.Dimension), desc.Height, static_cast<std::uint32_t>(desc.Format), desc.SampleDesc.Count, desc.SampleDesc.Quality, static_cast<std::uint32_t>(desc.Layout), static_cast<std::uint32_t>(desc.Flags), desc.Width, desc.DepthOrArraySize, desc.MipLevels, castable };
}

// Idle CPU textures and resolve destinations rest in shader-read state,
// color attachments as render targets and depth attachments in depth-write
// state.
using TexturePool    = FencedResourcePool<PooledTextureKey, ComPtr<ID3D12Resource>>;
using TextureReturns = ResourceReturnQueue<PooledTextureKey, ComPtr<ID3D12Resource>>;

void detachPooled(TexturePool& pool) noexcept
{
    pool.DetachAll([](ComPtr<ID3D12Resource>& resource) noexcept
                   {
                       (void)resource.Detach();
                   });
}

struct ReplayDrawResources
{
    ComPtr<ID3D12Resource>                                            vertexBuffer;
    ComPtr<ID3D12Resource>                                            indexBuffer;
    ComPtr<ID3D12Resource>                                            vertexConstants;
    ComPtr<ID3D12Resource>                                            pixelConstants;
    ComPtr<ID3D12Resource>                                            sharedConstants;
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 5>                          uploadAddresses{};
    std::array<ComPtr<ID3D12Resource>, kNativeDrawReplayTextureSlots> textures;
    std::array<UploadLease, kNativeDrawReplayTextureSlots>            textureUploads;
    // A standalone slot's own heaps; a frame slot has neither and uses its
    // owner's view region and sampler tables.
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> samplerHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE  viewCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE  viewGpu{};
    // Cache eviction must not release a PSO referenced by an unsubmitted draw.
    ComPtr<ID3D12PipelineState> pipeline;

    // The texture descriptors last written to these heaps, so a repeated
    // binding skips rewriting them. Resource serials are never reused, unlike
    // addresses; serial 0 marks a descriptor that must be rewritten.
    struct TextureDescriptor
    {
        std::uint64_t                   serial = 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    };

    std::array<TextureDescriptor, kNativeDrawReplayTextureSlots>  textureDescriptors{};
    std::uint32_t                                                 textureDescriptorCount = 0;
    std::array<D3D12_SAMPLER_DESC, kNativeDrawReplayTextureSlots> samplerDescriptors{};
    std::uint64_t                                                 samplerDescriptorMask = 0;

    void forgetDescriptors()
    {
        textureDescriptors.fill({});
        textureDescriptorCount = 0;
        samplerDescriptorMask  = 0;
    }

    void reset()
    {
        forgetDescriptors();
        vertexBuffer.Reset();
        indexBuffer.Reset();
        vertexConstants.Reset();
        pixelConstants.Reset();
        sharedConstants.Reset();
        for (auto& texture : textures)
        {
            texture.Reset();
        }
        for (auto& upload : textureUploads)
        {
            upload.reset();
        }
        srvHeap.Reset();
        samplerHeap.Reset();
        viewCpu = {};
        viewGpu = {};
        pipeline.Reset();
    }

    void detach()
    {
        forgetDescriptors();
        (void)vertexBuffer.Detach();
        (void)indexBuffer.Detach();
        (void)vertexConstants.Detach();
        (void)pixelConstants.Detach();
        (void)sharedConstants.Detach();
        for (auto& texture : textures)
        {
            (void)texture.Detach();
        }
        for (auto& upload : textureUploads)
        {
            detachUpload(upload);
        }
        (void)srvHeap.Detach();
        (void)samplerHeap.Detach();
        viewCpu = {};
        viewGpu = {};
        (void)pipeline.Detach();
    }
};

// A frame list binds its owner's heaps once after reset; a standalone slot
// binds its own.
void bindDrawHeaps(ID3D12GraphicsCommandList* commandList, const ReplayDrawResources& draw)
{
    if (!draw.srvHeap)
        return;
    ID3D12DescriptorHeap* heaps[] = { draw.srvHeap.Get(), draw.samplerHeap.Get() };
    commandList->SetDescriptorHeaps(2, heaps);
}

bool awaitGpuOutputTicket(const std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>& ticket, bool requireCopy, std::string& error, std::uint64_t* value = nullptr)
{
    if (!ticket)
    {
        if (!requireCopy)
            return true;
        error = "native GPU output has no admitted ticket";
        return false;
    }
    if (!ticket->AwaitRetirement(kFenceWaitTimeoutMs))
    {
        error = "native GPU output consumer copy did not retire";
        return false;
    }
    if (requireCopy && ticket->status() != rex::ui::d3d12::GpuGuestOutputTicket::Status::kCompleted)
    {
        error = "native GPU output was canceled before consumer copying";
        return false;
    }
    if (value)
        *value = ticket->copy_fence_value();
    return true;
}

// A title lease retires only by an actual consumer copy fence, or by
// cancellation before any consumer use.
bool awaitGpuOutputLease(const std::shared_ptr<NativeOutputLease>& lease, bool requireCopy, std::string& error, std::uint64_t* value = nullptr)
{
    if (!lease)
    {
        if (!requireCopy)
            return true;
        error = "native GPU output has no admitted lease";
        return false;
    }
    if (!lease->AwaitRetirement(std::chrono::milliseconds(kFenceWaitTimeoutMs)))
    {
        error = "native GPU output consumer copy did not retire";
        return false;
    }
    if (requireCopy && lease->State() != OutputLeaseState::Retired)
    {
        error = "native GPU output was canceled before consumer copying";
        return false;
    }
    if (value)
        *value = lease->CopySubmissionId();
    return true;
}

std::uint64_t resourceStorageBytes(ID3D12Device* device, ID3D12Resource* resource)
{
    const auto desc = resource->GetDesc();
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? desc.Width : device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
}

struct ReplayOutputBundleKey
{
    std::uint32_t width         = 0;
    std::uint32_t height        = 0;
    DXGI_FORMAT   colorFormat   = DXGI_FORMAT_UNKNOWN;
    std::uint32_t sampleCount   = 0;
    std::uint32_t planeCount    = 0;
    std::uint64_t readbackBytes = 0;
    bool          includesGamma = false;

    bool operator==(const ReplayOutputBundleKey&) const = default;
};

} // namespace
#endif

struct NativeFrameResourceOwner
{
#if defined(_WIN32)
    ComPtr<ID3D12Resource> resource;
    std::uint64_t          resourceStorageBytes = 0;
    // Created with RGBA8, R10G10B10A2 and R16G16_FLOAT castable views of its
    // R32_UINT words.
    bool castable = false;
    // Set where the owner creates its resource, which it never replaces; draw
    // slots keep views of the same serial. Zero rewrites them each draw.
    std::uint64_t serial = 0;
    // Set on resolve destinations: the last holder hands the storage back to
    // the draw stream's pool. A refused post releases it here, as before.
    std::shared_ptr<TextureReturns> returns;

    ~NativeFrameResourceOwner()
    {
        if (returns && resource)
            (void)returns->Post(pooledTextureKey(resource->GetDesc(), castable), resource, resourceStorageBytes);
    }
#endif
};

bool frameUsesTexture(const NativeRenderFrame& frame, std::uint64_t id)
{
    if (frame.outputTextureId == id)
        return true;
    for (const auto& pass : frame.passes)
        for (std::size_t index = 0; index < pass.DrawCount(); ++index)
            for (const auto& texture : pass.Draw(index).textures)
                if (texture.frameTextureId == id)
                    return true;
    return false;
}

struct NativeRenderFrameTextureVersion::Impl
{
    NativeRenderFrameTexture                             descriptor;
    std::weak_ptr<const void>                            generation;
    ID3D12CommandQueue*                                  queue      = nullptr;
    std::uint64_t                                        fenceValue = 0;
    std::weak_ptr<NativeFrameResourceOwner>              resource;
    std::weak_ptr<const NativeRenderFrameTextureVersion> canonical;
};

std::shared_ptr<const NativeRenderFrameTextureVersion> NativeRenderFrameTicket::Export(std::uint64_t persistentKey) const
{
    std::lock_guard lock(exportMutex);
    const auto      found = exports.find(persistentKey);
    return found == exports.end() ? nullptr : found->second;
}

struct NativeRendererD3D12::Impl
{
    detail::OwnedHeapBudget                             ownedHeap;
    std::thread                                         rendererThread;
    std::mutex                                          stateMutex;
    std::condition_variable                             stateCv;
    bool                                                stopRequested           = false;
    bool                                                initializationDone      = false;
    bool                                                initializationSucceeded = false;
    bool                                                resizePending           = false;
    std::uint32_t                                       requestedWidth          = 0;
    std::uint32_t                                       requestedHeight         = 0;
    bool                                                runtimeFailed           = false;
    std::function<void()>                               requestDeferredQuit;
    std::atomic<bool>                                   initialized{ false };
    std::atomic<bool>                                   gpuObjectsAbandoned{ false };
    bool                                                drawStreamStarted       = false;
    bool                                                drawStreamPending       = false;
    bool                                                drawStreamInFlight      = false;
    bool                                                frameRecordingCancelled = false;
    std::string                                         initializationError;
    std::vector<NativeDrawReplayRecipe>                 drawStreamDraws;
    std::filesystem::path                               drawStreamSampleOutputPath;
    std::optional<NativeDrawStreamOutput>               drawStreamOutput;
    std::optional<NativeRenderFrame>                    drawStreamRenderFrame;
    bool                                                drawStreamPrefixRequest = false;
    bool                                                drawStreamHasPrefix     = false;
    std::optional<std::promise<NativeDrawReplayResult>> drawStreamPromise;

    struct TerminalRequest
    {
        std::shared_ptr<NativeRenderFrameTicket> ticket;
        std::promise<NativeDrawReplayResult>     recording;
        std::promise<NativeDrawReplayResult>     completion;
        std::atomic<bool>                        recordingSettled{ false }, completionSettled{ false }, submitted{ false };

        void SetRecording(NativeDrawReplayResult result)
        {
            if (!recordingSettled.exchange(true))
                recording.set_value(std::move(result));
        }

        void SetCompletion(NativeDrawReplayResult result)
        {
            if (!completionSettled.exchange(true))
                completion.set_value(std::move(result));
        }
    };

    std::shared_ptr<TerminalRequest> drawStreamTerminal;

    static constexpr std::uint64_t kOwnedByteBound      = 512ULL * 1024 * 1024;
    static constexpr std::size_t   kDirectHeldTerminals = 2;

    // Requests queued behind the slot when the stream holds more than one.
    // The worker admits each at intake, in order; drawStreamPending covers
    // the slot and this queue.
    struct QueuedRequest
    {
        NativeRenderFrame                                   frame;
        std::optional<std::promise<NativeDrawReplayResult>> promise;
        std::shared_ptr<TerminalRequest>                    terminal;
        bool                                                prefix = false, allowAppend = false;
        // native_direct_stream: the bytes charged when the request queued.
        std::uint64_t directBytes = 0;
    };

    std::deque<QueuedRequest> drawStreamQueue;
    // Requests the stream holds at once, counting the one recording.
    std::atomic<std::size_t> drawStreamQueueDepth{ 1 };
    // Guest admissions under way; queued enqueues wait until none remains.
    std::size_t guestAdmissions = 0;
    // The prefix chain's state once every taken and queued request succeeds.
    // Queued enqueues test it; the worker rebuilds it after refusing one.
    bool queuedPrefixOpen = false;
#if REREVVED_DEVELOPER
    // Caller time blocked before the stream accepted a request, and blocked
    // on a synchronous request's own result.
    std::atomic<std::uint64_t> streamQueueWaits{ 0 }, streamQueueWaitNanoseconds{ 0 }, streamFutureWaits{ 0 }, streamFutureWaitNanoseconds{ 0 };
#endif

    bool drawStreamSlotFilled() const noexcept
    {
        return drawStreamPromise.has_value() || drawStreamTerminal != nullptr;
    }

    std::size_t drawStreamHeld() const noexcept
    {
        return std::size_t(drawStreamSlotFilled()) + drawStreamQueue.size() + std::size_t(drawStreamInFlight);
    }

    void rebuildQueuedPrefixOpen()
    {
        queuedPrefixOpen = drawStreamHasPrefix;
        for (const auto& queued : drawStreamQueue)
            queuedPrefixOpen = queued.prefix;
    }

    // Waits until ready; developer builds also time a wait that blocked.
    template <class Ready>
    void waitCounted(std::unique_lock<std::mutex>& lock, Ready ready)
    {
        if (ready())
            return;
#if REREVVED_DEVELOPER
        const auto start = std::chrono::steady_clock::now();
#endif
        stateCv.wait(lock, ready);
#if REREVVED_DEVELOPER
        streamQueueWaits.fetch_add(1, std::memory_order_relaxed);
        streamQueueWaitNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
#endif
    }

    template <class Future>
    decltype(auto) getCounted(Future& future)
    {
#if REREVVED_DEVELOPER
        if (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            const auto start = std::chrono::steady_clock::now();
            future.wait();
            streamFutureWaits.fetch_add(1, std::memory_order_relaxed);
            streamFutureWaitNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        }
#endif
        return future.get();
    }

    // The terminal frames the stream holds: queued, in the slot or recording.
    std::size_t directHeldTerminals() const noexcept
    {
        std::size_t held = std::size_t(drawStreamInFlight && directInFlightTerminal) + std::size_t(drawStreamSlotFilled() && !drawStreamPrefixRequest);
        for (const auto& queued : drawStreamQueue)
            held += std::size_t(!queued.prefix);
        return held;
    }

    // Wait for queue room and no admission; direct requests also need byte
    // budget. Set tooLarge if no held charge can release first: a packet's own
    // chain cannot end before that packet.
    bool awaitQueueRoom(std::unique_lock<std::mutex>& lock, bool packet = false, std::uint64_t directBytes = 0, bool* tooLarge = nullptr)
    {
        const auto unavailable = [&]
        {
            return !drawStreamStarted || stopRequested || runtimeFailed || gpuObjectsAbandoned.load(std::memory_order_acquire);
        };
        const auto slot = [&]
        {
            return directStream ? packet || directHeldTerminals() < kDirectHeldTerminals : drawStreamHeld() < drawStreamQueueDepth.load(std::memory_order_relaxed);
        };
        const auto stuck = [&]
        {
            return directStream && !directFits(directBytes) && !directReleasePending();
        };
        waitCounted(lock, [&]
                    {
                        return (guestAdmissions == 0 && slot() && (!directStream || directFits(directBytes) || stuck())) || unavailable();
                    });
        if (unavailable() || !initialized.load(std::memory_order_acquire))
            return false;
        if (stuck())
        {
            if (tooLarge)
                *tooLarge = true;
            return false;
        }
        return true;
    }

    // Charges a queued direct request's bytes once awaitQueueRoom found room.
    void reserveDirect(std::uint64_t bytes) noexcept
    {
        if (!directStream)
            return;
        directChargeBytes.fetch_add(bytes, std::memory_order_relaxed);
        publishDirectCharge();
    }

    // A direct request refused at enqueue breaks the chain it belongs to, as
    // a refusal at intake does, so the stream stops.
    void refuseDirect(bool breaksChain)
    {
        if (!breaksChain)
            return;
        runtimeFailed = true;
        stopRequested = true;
        cancelQueuedRequests();
    }

    // Settles every queued request once the stream has failed.
    void cancelQueuedRequests()
    {
        NativeDrawReplayResult canceled;
        canceled.error = "native draw stream stopped before frame submission";
#if defined(_WIN32)
        canceled.submittedFenceValue = replayFenceValue;
#endif
        std::uint64_t reserved = 0;
        for (auto& queued : drawStreamQueue)
        {
            reserved += queued.directBytes;
            if (queued.promise)
                queued.promise->set_value(canceled);
            if (queued.terminal)
            {
                queued.terminal->SetRecording(canceled);
                queued.terminal->SetCompletion(canceled);
            }
        }
        drawStreamQueue.clear();
        drawStreamPending = drawStreamSlotFilled();
        if (reserved)
        {
            directChargeBytes.fetch_sub(reserved, std::memory_order_relaxed);
            publishDirectCharge();
        }
    }

    std::shared_ptr<TerminalRequest> recordingTerminalRequest;
    std::shared_ptr<TerminalRequest> activeTerminalRequest;
    std::function<void()>            beforeRequestTestHook;
    // ownedAdmissionBytes and its high-water mark, for statistics readers.
    std::atomic<std::uint64_t> ownedChargePublished{ 0 }, ownedChargePeak{ 0 };

    void publishOwnedCharge() noexcept
    {
        ownedChargePublished.store(ownedAdmissionBytes, std::memory_order_relaxed);
        if (ownedAdmissionBytes > ownedChargePeak.load(std::memory_order_relaxed))
            ownedChargePeak.store(ownedAdmissionBytes, std::memory_order_relaxed);
    }

    // Published only by the worker before CPU request capacity is released.
    std::uint64_t                   ownedAdmissionBytes = 0, ownedAdmissionRevision = 0, ownedCapacityRequest = 0;
    std::optional<detail::OwnerSet> ownedAdmissionOwners;
    std::uint64_t                   ownedAdmissionFullCount = 0, ownedScanCeiling = 0, ownedLastScanFullCount = 0, ownedMaintenanceWork = 0;
    bool                            ownedRefreshRequested = false;
    std::string                     ownedCapacityError;
    bool                            ownedAdmissionBusy = false, ownedAdmissionSnapshotValid = false;
    std::atomic<std::uint64_t>      ownedStateWalks{ 0 }, ownedJoinWalks{ 0 }; // all owned walks, and those in joins
    std::atomic<std::uint64_t>      replayHelperShaderCompilations{ 0 };
    std::atomic<std::uint64_t>      replayRootSignatureCreations{ 0 };
    std::atomic<std::uint64_t>      replayPipelineCreations{ 0 };
    std::atomic<std::uint64_t>      replayDescriptorHeapCreations{ 0 };
    std::atomic<std::uint64_t>      replayImmutableConstantCreations{ 0 };
    std::atomic<std::uint64_t>      replayOutputBundleCreations{ 0 };
    std::atomic<std::uint64_t>      replayOutputBundleReuses{ 0 };
    std::atomic<std::uint64_t>      replayDrawUploadCreations{ 0 };
    std::atomic<std::uint64_t>      replayDrawUploadReuses{ 0 };
    std::atomic<std::uint64_t>      replayTextureCreations{ 0 };
    std::atomic<std::uint64_t>      replayTextureReuses{ 0 };
    std::atomic<std::uint64_t>      replayTextureUploadCreations{ 0 };
    std::atomic<std::uint64_t>      replayTextureUploadReuses{ 0 };
    std::atomic<std::uint64_t>      replayTexturePayloadReuses{ 0 };
    std::atomic<std::uint64_t>      replayTexturePreparationCreations{ 0 };
    std::atomic<std::uint64_t>      replayTextureVersionReuses{ 0 };
    std::atomic<std::uint64_t>      replayDescriptorReuses{ 0 };
    std::atomic<std::uint64_t>      replayTextureContentReuses{ 0 };
    std::atomic<std::uint64_t>      replaySamplerTables{ 0 };
    std::atomic<std::uint64_t>      replaySamplerTableHits{ 0 };
    std::atomic<std::uint64_t>      replaySamplerTableFlushes{ 0 };
    std::atomic<std::uint64_t>      frameCpuTexturePayloadBytes{ 0 };
    std::atomic<std::uint64_t>      replayColorTargetCreations{ 0 };
    std::atomic<std::uint64_t>      replayColorTargetReuses{ 0 };
    std::atomic<std::uint64_t>      replayDepthTargetCreations{ 0 };
    std::atomic<std::uint64_t>      replayDepthTargetReuses{ 0 };
    std::atomic<std::uint64_t>      frameAttachmentReuses{ 0 };
    std::atomic<std::uint64_t>      frameAttachmentByteBound{ 0 };
    // Operation counts behind ReplayCounts, kept in every build.
    std::atomic<std::uint64_t> replayRecordedDraws{ 0 };
    std::atomic<std::uint64_t> replayQueueSubmissions{ 0 };
    std::atomic<std::uint64_t> replayChunkSubmissions{ 0 };
#if REREVVED_DEVELOPER
    // Observation behind ReplayTimingStats; shipping keeps none of it.
    std::atomic<std::uint64_t> replayRecordingNanoseconds{ 0 };
    std::atomic<std::uint64_t> replayTextureUploadNanoseconds{ 0 };
    std::atomic<std::uint64_t> replayDrawUploadNanoseconds{ 0 };
    std::atomic<std::uint64_t> replayFenceWaitNanoseconds{ 0 };
    std::atomic<std::uint64_t> replayPipelineNanoseconds{ 0 };
    std::atomic<std::uint64_t> surfaceConversions{ 0 }, surfaceConversionNanoseconds{ 0 };
    std::atomic<bool>          replayPipelineTiming{ false };
    // The stream thread's busy time, and resident geometry bytes by kind.
    std::atomic<std::uint64_t>                streamBusyNanoseconds{ 0 };
    std::array<std::atomic<std::uint64_t>, 2> replayResidentGeometryBytes{};
#endif
    // The device creates castable resources and casts across the 32-bit
    // formats (D3D12_OPTIONS12), so packed guest words can be sampled typed.
    std::atomic<bool> castableGuestWords{ false };
    // Owner cache buckets are reserved by the stream thread; expired owners
    // leave full tables before eviction.
    using UploadedVertexOwners                              = std::unordered_map<const void*, std::weak_ptr<const std::vector<std::uint8_t>>>;
    static constexpr std::size_t kUploadedVertexOwners      = 4096;
    static constexpr std::size_t kUploadedVertexOwnerBucket = 2 * sizeof(void*);
    // A node, and the control block an expired owner leaves retained.
    static constexpr std::size_t kUploadedVertexOwnerBytes = sizeof(UploadedVertexOwners::value_type) + 4 * sizeof(void*) + sizeof(std::vector<std::uint8_t>) + 4 * sizeof(void*);
    UploadedVertexOwners         uploadedVertexOwners      = []
    {
        UploadedVertexOwners owners;
        owners.reserve(kUploadedVertexOwners);
        return owners;
    }();
#if REREVVED_DEVELOPER && defined(_WIN32)
    // While timing is on, the constant block hashes of the current command
    // chunk, open addressed in place so measuring never allocates; and each
    // stage's block hash in the chunk's previous draw. Stream thread only.
    static constexpr std::size_t                   kChunkConstantSlots = 8 * kFrameCommandDraws;
    std::array<std::uint64_t, kChunkConstantSlots> chunkConstantHashes{};
    std::size_t                                    chunkConstantCount = 0;
    std::array<std::uint64_t, 3>                   chunkPreviousConstants{};
    std::array<std::atomic<std::uint64_t>, 3>      replayConstantRepeatPreviousBytes{}, replayConstantRepeatChunkBytes{};
    std::atomic<std::uint64_t>                     replayTextureDescriptorReuses{ 0 }, replayTextureSrvWrites{ 0 }, replayRtvWrites{ 0 };
    static_assert(std::has_single_bit(kChunkConstantSlots));
#endif

    // native_vertex_residency, sampled when the device initializes; null when
    // off. Renderer thread only, except the atomics.
    enum ResidencySeam
    {
        SeamInit,
        SeamDetach,
        SeamShutdown,
        SeamFailure,
        SeamCount
    };

    std::unique_ptr<vertex_residency::VertexResidency> vertexResidency;
#if defined(_WIN32)
    std::array<ComPtr<ID3D12Resource>, 4>    residencyChunks;
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4> residencyChunkAddresses{};
#endif
    std::uint64_t residencyGeneration = 0;
#if REREVVED_DEVELOPER
    // Totals of helpers already released, so the published counters stay
    // cumulative across a restart.
    vertex_residency::Counters   residencyRetiredTotals;
    static constexpr std::size_t kResidencyFallbackReasons = vertex_residency::kReasonCount;
    // Outcomes of successful recorded draws, published with the timed phases.
    std::atomic<std::uint64_t>                                        residentHits{ 0 }, residentBytesAvoided{ 0 }, residentMisses{ 0 }, residentCopyBytes{ 0 }, residentFallbacks{ 0 };
    std::array<std::atomic<std::uint64_t>, kResidencyFallbackReasons> residentFallbackReasons{};
    // Operational events and gauges for ReplayTimingStats.
    std::atomic<std::uint64_t> residentEvictions{ 0 }, residentExpiries{ 0 }, residentClears{ 0 }, residentUploadFailures{ 0 }, residentInitFailures{ 0 };
    std::atomic<std::uint64_t> residentFootprint{ 0 }, residentEntries{ 0 }, residentPayloadBytes{ 0 }, residentRetiredBytes{ 0 };
#endif
    // Post-copy transitions recorded, and live entries left by each clear seam.
    std::atomic<std::uint64_t>                        residencyBarriers{ 0 }, residencyAdmissions{ 0 }, residencyPreBarriers{ 0 };
    std::array<std::atomic<std::uint64_t>, SeamCount> residencySeamEntries{ ~std::uint64_t(0), ~std::uint64_t(0), ~std::uint64_t(0), ~std::uint64_t(0) };
    // Faults 1-2 fail metadata/chunk setup; 3-5 fail staging/close/signal, 6
    // scheduling, 7 clears keys, 8 throws on the next draw.
    std::atomic<int> residencyInitFault{ 0 }, residencyStagingFault{ 0 }, residencyCloseFault{ 0 }, residencySignalFault{ 0 }, residencySchedulingFault{ 0 }, residencyClearFault{ 0 }, residencyThrowFault{ 0 };

    // Per-stage prior constant block holds recipe bytes and arena address while
    // recording. Arena epoch changes whenever storage may be rewritten/swapped,
    // invalidating every older block.
#if defined(_WIN32)
    struct UploadedConstants
    {
        const std::vector<std::uint8_t>* source  = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS        address = 0;
        std::uint64_t                    epoch   = 0;
    };

    std::array<UploadedConstants, 3> uploadedConstants{};
#endif
    std::uint64_t frameUploadEpoch = 1;

#if REREVVED_DEVELOPER && defined(_WIN32)
    void resetChunkConstants() noexcept
    {
        if (chunkConstantCount)
            chunkConstantHashes.fill(0);
        chunkConstantCount = 0;
        chunkPreviousConstants.fill(0);
    }
#else
    void resetChunkConstants() noexcept
    {
    }
#endif

#if REREVVED_DEVELOPER && defined(_WIN32)
    // Whether the chunk already held this nonzero hash. The table stops
    // taking hashes at three quarters, so a probe always reaches a hole.
    bool noteChunkConstant(std::uint64_t hash) noexcept
    {
        for (std::size_t slot = hash & (kChunkConstantSlots - 1);; slot = (slot + 1) & (kChunkConstantSlots - 1))
        {
            if (chunkConstantHashes[slot] == hash)
                return true;
            if (!chunkConstantHashes[slot])
            {
                if (4 * (chunkConstantCount + 1) <= 3 * kChunkConstantSlots)
                {
                    chunkConstantHashes[slot] = hash;
                    ++chunkConstantCount;
                }
                return false;
            }
        }
    }
#endif

    // Every context's FrameCpuTexture::RetainedOwnedBytes, kept with the
    // payload count; and the guest pipeline caches' entry bytes.
    std::atomic<std::uint64_t> frameCpuTextureOwnedBytes{ 0 }, pipelineCacheBytes{ 0 };

    // native_direct_stream tracks held, synchronous, open-chain, deleter-held,
    // and worker structure charges separately.
    bool                       directStream = false, directInFlightTerminal = false;
    std::atomic<std::uint64_t> directChargeBytes{ 0 }, directDeleterBytes{ 0 }, directStructureBytes{ 0 };
    std::uint64_t              directAdmittedBytes = 0, directChainBytes = 0;

    // Persistent charges include CPU textures, pipeline bytecode, and
    // structures; cache/request overlap may overcount.
    std::uint64_t directPersistentBytes() const noexcept
    {
        return detail::OwnedAdd(detail::OwnedAdd(frameCpuTextureOwnedBytes.load(std::memory_order_relaxed), pipelineCacheBytes.load(std::memory_order_relaxed)),
                                directStructureBytes.load(std::memory_order_relaxed));
    }

    std::uint64_t directOwnedBytes() const noexcept
    {
        return detail::OwnedAdd(detail::OwnedAdd(directChargeBytes.load(std::memory_order_relaxed), directPersistentBytes()),
                                ownedHeap.live.load(std::memory_order_relaxed));
    }

    bool directFits(std::uint64_t bytes) const noexcept
    {
        const auto owned = directOwnedBytes();
        return owned <= kOwnedByteBound && bytes <= kOwnedByteBound - owned;
    }

    // No request holds the slot or stages an admission, as a synchronous
    // caller requires before it fills the slot.
    bool directSyncIdle() const noexcept
    {
#if defined(_WIN32)
        return !drawStreamPending && !drawStreamInFlight && !ownedAdmissionBusy && !admissionImports;
#else
        return !drawStreamPending && !drawStreamInFlight && !ownedAdmissionBusy;
#endif
    }

    // A held terminal or a retired frame the deleter has not destroyed yet
    // will release its charge.
    bool directReleasePending() const noexcept
    {
        return directHeldTerminals() || directDeleterBytes.load(std::memory_order_relaxed);
    }

    void publishDirectCharge() noexcept
    {
        ownedAdmissionBytes = detail::OwnedAdd(directChargeBytes.load(std::memory_order_relaxed), directPersistentBytes());
        publishOwnedCharge();
    }

    // The deleter destroyed frames whose charges total bytes.
    void releaseDirect(std::uint64_t bytes)
    {
        {
            std::lock_guard lock(stateMutex);
            directChargeBytes.fetch_sub(bytes, std::memory_order_relaxed);
            directDeleterBytes.fetch_sub(bytes, std::memory_order_relaxed);
            publishDirectCharge();
        }
        stateCv.notify_all();
    }

    // Recording time per draw phase, summed over draws, indexed by
    // RecordingPhase in kRecordingPhases order.
    enum RecordingPhase : std::size_t
    {
        PhaseSetup,
        PhaseTextures,
        PhaseTextureCompare,
        PhaseTextureUpload,
        PhaseTextureRetain,
        PhaseSamplers,
        PhaseDrawUpload,
        PhaseViews,
        PhasePipeline,
        PhaseDraw,
        PhaseTail,
    };

#if REREVVED_DEVELOPER
    std::array<std::atomic<std::uint64_t>, NativeDrawReplayTimingStats::kRecordingPhases.size()> replayRecordingPhaseNanoseconds{};
    std::array<std::atomic<std::uint64_t>, NativeDrawReplayTimingStats::kDrawUploadKinds.size()> replayDrawUploadBytes{};
    std::atomic<std::uint64_t>                                                                   replayTexturePrepareNanoseconds{ 0 };
    // Held by a timed draw while it publishes its phases, upload bytes and
    // texture outcomes with its recording total and by ReplayTimingStats, so a snapshot holds
    // all of them or none.
    std::mutex replayTimingMutex;
#endif

    enum TextureOutcome : std::size_t
    {
        TextureRepeat,
        TextureMatch,
        TextureFrame,
        TextureNew,
        TextureChanged,
        TextureUnretained,
        TextureUncached,
    };

#if REREVVED_DEVELOPER
    std::array<std::atomic<std::uint64_t>, NativeDrawReplayTimingStats::kTextureOutcomes.size()> replayTextureOutcomes{}, replayTextureOutcomeBytes{};

    // The renderer thread's latest draw timing decision. Untimed preparation
    // follows it without loading replayPipelineTiming; timed preparation
    // reloads it, so a disable stops timing at the next preparation.
    bool recordingTimed = false;

#if defined(_WIN32)
    // Frame CPU texture preparation, timed while recording phases are.
    bool prepareFrameCpuTexturesTimed(const NativeRenderFrame& frame, std::string& error, bool keepUnused, bool preserveRecorded)
    {
        if (!recordingTimed || !(recordingTimed = replayPipelineTiming.load(std::memory_order_relaxed)))
            return prepareFrameCpuTextures(frame, error, keepUnused, preserveRecorded);
        const auto start    = std::chrono::steady_clock::now();
        const bool prepared = prepareFrameCpuTextures(frame, error, keepUnused, preserveRecorded);
        replayTexturePrepareNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
        return prepared;
    }
#endif
#elif defined(_WIN32)
    bool prepareFrameCpuTexturesTimed(const NativeRenderFrame& frame, std::string& error, bool keepUnused, bool preserveRecorded)
    {
        return prepareFrameCpuTextures(frame, error, keepUnused, preserveRecorded);
    }
#endif

    void notifyStream()
    {
        stateCv.notify_all();
#if defined(_WIN32)
        std::lock_guard eventLock(requestEventMutex);
        if (requestEvent)
            SetEvent(requestEvent);
#endif
    }

#if defined(_WIN32)
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device>  device;
    // Set with castableGuestWords: creates guest-word resolve destinations castable.
    ComPtr<ID3D12Device10>                                                    castingDevice;
    ComPtr<ID3D12CommandQueue>                                                queue;
    ComPtr<IDXGISwapChain3>                                                   swapChain;
    ComPtr<ID3D12DescriptorHeap>                                              rtvHeap;
    std::array<ComPtr<ID3D12Resource>, kFrameCount>                           backBuffers;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount>                   allocators;
    ComPtr<ID3D12GraphicsCommandList>                                         commandList;
    ComPtr<ID3D12Fence>                                                       fence;
    ComPtr<ID3D12Device>                                                      sharedStreamDevice;
    NativeGpuOutputPublisher                                                  streamOutputPublisher;
    NativeTitleOutputPublisher                                                streamTitlePublisher;
    std::atomic<std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>>        gpuOutputTicket;
    std::atomic<std::shared_ptr<NativeOutputLease>>                           gpuOutputLease;
    std::array<FrameGpuOutputSlot, NativeRendererD3D12::MaxPendingGpuOutputs> frameGpuOutputs;
    std::size_t                                                               frameGpuOutputIndex = 0;
    FrameGpuOutputSlot*                                                       recordingGpuOutput  = nullptr;
    ComPtr<ID3D12InfoQueue>                                                   replayInfoQueue;
    std::array<std::uint64_t, kFrameCount>                                    fenceValues{};
    std::uint64_t                                                             nextFenceValue = 1;
    HANDLE                                                                    fenceEvent     = nullptr;
    HANDLE                                                                    terminalEvent  = nullptr;
    HANDLE                                                                    requestEvent   = nullptr;
    std::mutex                                                                requestEventMutex;
    std::uint32_t                                                             rtvStride = 0;
    std::uint32_t                                                             width     = 0;
    std::uint32_t                                                             height    = 0;

    // The renderer owns GPU objects through actual completion. Descriptor slots
    // recycle at an owner fence; frame uploads and resources retire together.
    // Unknown live-device completion detaches every outstanding owner.
    ComPtr<ID3D12RootSignature>                                                  replayCopyRootSignature;
    std::array<ComPtr<ID3D12RootSignature>, kNativeDrawReplayTextureSlots + 1>   replayDrawRootSignatures;
    ComPtr<ID3DBlob>                                                             replayCopyVertexShader;
    ComPtr<ID3DBlob>                                                             replayCopyPixelShader;
    ComPtr<ID3DBlob>                                                             replayResolvePixelShader;
    ComPtr<ID3DBlob>                                                             replayGammaShader;
    ComPtr<ID3DBlob>                                                             replaySingleSampleGammaShader;
    ComPtr<ID3DBlob>                                                             replayDepthReadShader;
    ComPtr<ID3DBlob>                                                             replaySingleSampleDepthReadShader;
    ComPtr<ID3DBlob>                                                             replayDepthCopyShader;
    ComPtr<ID3DBlob>                                                             frameResolveShader;
    ComPtr<ID3DBlob>                                                             frameResolveSingleShader;
    std::map<std::pair<std::uint32_t, DXGI_FORMAT>, ComPtr<ID3D12PipelineState>> frameResolvePipelines;
    // Guest-word resolves: two SRVs (depth and stencil planes) and constants.
    ComPtr<ID3D12RootSignature>                                           frameGuestWordRootSignature;
    std::array<ComPtr<ID3DBlob>, 4>                                       frameGuestWordShaders;
    std::map<std::pair<std::uint32_t, bool>, ComPtr<ID3D12PipelineState>> frameGuestWordPipelines;
    ComPtr<ID3DBlob>                                                      surfaceDepthShader;
    ComPtr<ID3D12PipelineState>                                           surfaceDepthPipeline;
    std::map<std::uint64_t, ComPtr<ID3D12Resource>>                       frameTextures;

    struct RetainedFrameTexture
    {
        NativeRenderFrameTexture                               descriptor;
        ComPtr<ID3D12Resource>                                 resource;
        std::shared_ptr<NativeFrameResourceOwner>              owner;
        std::shared_ptr<const NativeRenderFrameTextureVersion> version;
    };

    struct PinnedFrameTexture
    {
        NativeRenderFrameTexture                  descriptor;
        std::shared_ptr<NativeFrameResourceOwner> owner;
    };

    using FrameImports = std::map<std::uint64_t, PinnedFrameTexture>;
    FrameImports        drawStreamImports, recordingImports;
    const FrameImports* admissionImports              = nullptr;
    const FrameImports* ownedAdmissionSnapshotImports = nullptr;

    // Held passes move without copying, so their draw storage, whose
    // addresses key inline payloads, stays where the packet recorded it.
    static_assert(std::is_nothrow_move_constructible_v<NativeRenderFramePass>);

    struct RecordedRenderPrefix
    {
        explicit RecordedRenderPrefix(detail::OwnedHeapBudget* budget)
        : validation(NativeRenderFramePrefixState(budget))
        {
        }

        NativeRenderFramePrefixState validation;
        // The chain's recorded pass count.
        std::size_t passCount = 0;
        // The recorded packets' passes, held until a tail terminal joins them
        // in front of its own or the chain's terminal retires.
        std::vector<NativeRenderFramePass>    passes;
        std::vector<NativeRenderFrameTexture> textures;
        bool                                  identityAttachments = false, typedGuestWords = false;
        // A tail terminal took the held passes.
        bool joined = false;
    };

    std::optional<RecordedRenderPrefix> recordedRenderPrefix;

    // native_direct_stream: retired frames and chains are destroyed on this
    // thread, off the stream's recording path. Drain joins it.
    struct RetiredDeleter
    {
        struct Retired
        {
            std::optional<NativeRenderFrame>    frame;
            std::optional<RecordedRenderPrefix> prefix;
            std::uint64_t                       charge = 0; // released once both are destroyed
        };

        Impl*                   renderer = nullptr;
        std::mutex              mutex;
        std::condition_variable ready;
        std::deque<Retired>     retired;
        bool                    stopping = false;
        std::thread             thread;

        ~RetiredDeleter()
        {
            Drain();
        }

        // A thread that fails to start, or a full queue, leaves value to its
        // caller's unwinding, which destroys the frame before its charge goes.
        void Push(Retired value)
        {
            {
                std::lock_guard lock(mutex);
                if (!thread.joinable())
                    thread = std::thread([this]
                                         {
                                             Run();
                                         });
                retired.push_back(std::move(value));
            }
            ready.notify_one();
        }

        void Drain()
        {
            {
                std::lock_guard lock(mutex);
                if (!thread.joinable())
                    return;
                stopping = true;
            }
            ready.notify_one();
            thread.join();
            std::lock_guard lock(mutex);
            stopping = false;
        }

        void Run()
        {
            std::unique_lock lock(mutex);
            for (;;)
            {
                ready.wait(lock, [&]
                           {
                               return stopping || !retired.empty();
                           });
                if (retired.empty())
                    return;
                auto batch = std::move(retired);
                retired.clear();
                lock.unlock();
                std::uint64_t charge = 0;
                for (const auto& value : batch)
                    charge += value.charge;
                batch.clear();
                if (charge)
                    renderer->releaseDirect(charge);
                lock.lock();
            }
        }
    };

    RetiredDeleter                                retiredDeleter{ this };
    std::map<std::uint64_t, RetainedFrameTexture> retainedFrameTextures;

    // Surface store keys keep strong owners beyond frames. Replaced/released
    // owners retire after the recorded queue fence. Its mutex is a leaf:
    // admission imports under stateMutex.
    struct StoredSurface
    {
        NativeRenderFrameTexture                  descriptor;
        std::shared_ptr<NativeFrameResourceOwner> owner;
    };

    mutable std::mutex                                                              surfaceMutex;
    std::map<std::uint64_t, StoredSurface>                                          surfaceStore;
    std::deque<std::pair<std::uint64_t, std::shared_ptr<NativeFrameResourceOwner>>> surfaceRetires;
    // Set under surfaceMutex while surfaceRetires is not empty, so frame
    // retirement without stored surfaces takes neither the mutex nor the fence.
    std::atomic<bool> surfacesRetiring{ false };
    // Set once surfaceStore first held a surface: without the registry,
    // accounting and imports never take surfaceMutex.
    std::atomic<bool>                                                  surfacesUsed{ false };
    void                                                               storeFrameSurfaces(const NativeRenderFrame& frame);
    void                                                               retireSurfaces();
    std::map<std::uint64_t, std::shared_ptr<NativeFrameResourceOwner>> frameTextureOwners;
    std::map<std::uint32_t, ComPtr<ID3D12Resource>>                    frameColorSnapshots;
    std::vector<ReplayHelperPipelineCacheEntry>                        replayHelperPipelineCache;
    std::vector<ReplayGuestPipelineCacheEntry>                         replayGuestPipelineCache;
    std::vector<ReplayGuestPipelineCacheEntry>                         frameGuestPipelineCache;
    std::uint64_t                                                      frameRecordings = 0;
    // Texture uploads and frame arenas lease persistently mapped buffers.
    UploadPool uploadPool{ [this](std::uint64_t bytes)
                           {
                               return CreateNativeUploadBuffer(device.Get(), bytes);
                           } };
    // Default-heap CPU textures and frame attachments return here where they
    // were released, and serve later creations of the same allocation.
    TexturePool texturePool{ kTexturePoolIdleCapBytes };
    TexturePool attachmentPool{ kAttachmentPoolIdleCapBytes };
#if REREVVED_DEVELOPER
    // Attachment pool misses by shape, against the render frames recorded.
    PoolMissLog<PooledTextureKey>                                                                   attachmentMisses;
    std::atomic<std::uint64_t>                                                                      attachmentMissFrames{ 0 };
    std::array<std::atomic<std::uint64_t>, NativeDrawReplayCacheStats::kAttachmentTakeSites.size()> attachmentSiteMisses{};
    std::atomic<std::uint64_t>                                                                      attachmentCreations{ 0 }, attachmentCreateNanoseconds{ 0 };

    // One attachment creation call after a pool miss, started at started.
    void countAttachmentCreate(DevClockPoint started) noexcept
    {
        attachmentCreations.fetch_add(1, std::memory_order_relaxed);
        attachmentCreateNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()),
                                              std::memory_order_relaxed);
    }
#else
    void countAttachmentCreate(DevClockPoint) noexcept
    {
    }
#endif

    // Resolve destinations come back from their owners' last holders, on any
    // thread; only this thread drains them into resolvePool.
    TexturePool                     resolvePool{ kResolvePoolIdleCapBytes };
    std::shared_ptr<TextureReturns> resolveReturns = std::make_shared<TextureReturns>();

    // Everything recorded so far ends with the next signal; a destination
    // another holder still references is released instead.
    void drainResolveReturns() noexcept
    {
        resolveReturns->Drain(resolvePool, nextFenceValue, [](ComPtr<ID3D12Resource>& resource) noexcept
                              {
                                  resource->AddRef();
                                  return resource->Release() == 1;
                              });
    }

    // An idle resolve destination of desc's allocation and its size, or none.
    ComPtr<ID3D12Resource> takeResolvePooled(const D3D12_RESOURCE_DESC& desc, bool castable, std::uint64_t& bytes) noexcept
    {
        drainResolveReturns();
        const auto completed = fence ? fence->GetCompletedValue() : 0;
        auto       taken     = resolvePool.Take(pooledTextureKey(desc, castable), completed == UINT64_MAX ? 0 : completed);
        bytes                = taken.bytes;
        return std::move(taken.resource);
    }

    // open binds a batch; staged retains its upload through submission, while
    // unsubmitted entries release on next preparation.
    struct VersionTexture
    {
        std::weak_ptr<const NativeDrawReplayTextureVersion> version;
        ComPtr<ID3D12Resource>                              resource;
        // The recording that last bound it, so a repeated binding counts as one.
        std::uint64_t bytes = 0, lastUseFence = 0, lastUse = 0, recording = 0;
        bool          staged = false, open = false;
        // Assigned at insert; draw slots keep views of the same serial.
        std::uint64_t serial = 0;
    };

    // Evicted storage the texture pool refused, released once its fence completes.
    struct VersionTextureRelease
    {
        std::uint64_t          fence = 0;
        ComPtr<ID3D12Resource> resource;
        std::uint64_t          bytes = 0;
    };

    using VersionTextures = std::map<const NativeDrawReplayTextureVersion*, VersionTexture>;
    VersionTextures                                    versionTextures;
    std::vector<const NativeDrawReplayTextureVersion*> versionTexturesOpen;
    std::vector<VersionTextureRelease>                 versionTextureReleases;
    std::uint64_t                                      versionTextureBytes = 0, versionTextureCap = kVersionTextureCapBytes, versionTextureClock = 0;
    // Tests turn the store off to drive the per-context cache with canonical versions.
    bool                       versionTextureStore = true;
    std::atomic<std::uint64_t> versionTextureHits{ 0 }, versionTextureUploads{ 0 }, versionTextureEvictions{ 0 }, versionTextureHeldBytes{ 0 };

    // The immutable version a frame draw's CPU texture slot samples whole,
    // which the store serves, or null for any other slot.
    static const std::shared_ptr<const NativeDrawReplayTextureVersion>* storedVersion(const NativeDrawReplayTexture& texture, const NativeDrawReplayTextureBinding* binding) noexcept
    {
        const auto& version = binding ? binding->textureVersion : texture.textureVersion;
        if (!version || !version->MatchesOwner(version) || texture.textureVersion.get() != version.get() || texture.textureVersion.owner_before(version) ||
            version.owner_before(texture.textureVersion))
            return nullptr;
        const auto  layout  = binding ? binding->ResourceLayout() : NativeDrawReplayTextureLayout{ texture.width, texture.height, texture.format, texture.mipLevels, texture.dimension };
        const auto  payload = texture.payloadBytes();
        const auto& kept    = *version->Payload();
        if (!(version->Layout() == layout) || payload.data() != kept.data() || payload.size() != kept.size())
            return nullptr;
        return &version;
    }

    void noteVersionTextureBytes() noexcept
    {
        versionTextureHeldBytes.store(versionTextureBytes, std::memory_order_relaxed);
    }

    // Returns an entry's storage to the texture pool, pending its last use's
    // fence; storage the pool refuses waits for that fence here.
    void evictVersionTexture(VersionTextures::iterator found) noexcept
    {
        auto& entry = found->second;
        versionTextureBytes -= entry.bytes;
        if (entry.resource && !texturePool.Return(pooledTextureKey(entry.resource->GetDesc()), std::move(entry.resource), entry.bytes, entry.lastUseFence))
        {
            // Reserved first, so a failed allocation leaves the resource here
            // until its fence completes, or leaked when that wait fails.
            try
            {
                versionTextureReleases.reserve(versionTextureReleases.size() + 1);
                versionTextureReleases.push_back({ entry.lastUseFence, std::move(entry.resource), entry.bytes });
            }
            catch (const std::bad_alloc&)
            {
                if (!waitForFence(entry.lastUseFence))
                    (void)entry.resource.Detach();
            }
        }
        versionTextures.erase(found);
        versionTextureEvictions.fetch_add(1, std::memory_order_relaxed);
        noteVersionTextureBytes();
    }

    // The entry holding exactly `version`, or none; one an expired version
    // left at the same address leaves first.
    VersionTexture* findVersionTexture(const std::shared_ptr<const NativeDrawReplayTextureVersion>& version) noexcept
    {
        const auto found = versionTextures.find(version.get());
        if (found == versionTextures.end())
            return nullptr;
        auto& entry = found->second;
        if (!entry.version.expired() && !entry.version.owner_before(version) && !version.owner_before(entry.version))
            return &entry;
        if (!entry.open)
            evictVersionTexture(found);
        return nullptr;
    }

    // Marks an entry used by the open batch; throws, failing the recording,
    // when the list cannot grow.
    void useVersionTexture(const NativeDrawReplayTextureVersion* key, VersionTexture& entry)
    {
        entry.lastUse = ++versionTextureClock;
        if (!entry.open)
        {
            versionTexturesOpen.push_back(key);
            entry.open = true;
        }
    }

    // The open batch executed: its entries' last use ends with `fenceValue`.
    void publishVersionTextures(std::uint64_t fenceValue) noexcept
    {
        for (const auto* key : versionTexturesOpen)
            if (const auto found = versionTextures.find(key); found != versionTextures.end())
            {
                found->second.open = found->second.staged = false;
                found->second.lastUseFence                = fenceValue;
            }
        versionTexturesOpen.clear();
    }

    // A recording ended without submitting its open batch, which never runs.
    void discardVersionTextures() noexcept
    {
        for (const auto* key : versionTexturesOpen)
            if (const auto found = versionTextures.find(key); found != versionTextures.end())
            {
                if (found->second.staged)
                {
                    versionTextureBytes -= found->second.bytes;
                    versionTextures.erase(found);
                }
                else
                    found->second.open = false;
            }
        versionTexturesOpen.clear();
        noteVersionTextureBytes();
    }

    // Evicts the least recently used entries no open batch uses until the
    // store holds at most `cap` bytes.
    void trimVersionTextures(std::uint64_t cap) noexcept
    {
        while (versionTextureBytes > cap)
        {
            auto oldest = versionTextures.end();
            for (auto entry = versionTextures.begin(); entry != versionTextures.end(); ++entry)
                if (!entry->second.open && (oldest == versionTextures.end() || entry->second.lastUse < oldest->second.lastUse))
                    oldest = entry;
            if (oldest == versionTextures.end())
                return;
            evictVersionTexture(oldest);
        }
    }

    // Releases evicted storage the pool refused once its fence completed.
    void releaseVersionTextures() noexcept
    {
        const auto completed = fence ? fence->GetCompletedValue() : 0;
        if (completed != UINT64_MAX)
            std::erase_if(versionTextureReleases, [&](const VersionTextureRelease& release)
                          {
                              return release.fence <= completed;
                          });
    }

    // Before a recording: refused storage whose fence completed is released,
    // entries of expired versions leave, then the cap applies.
    void sweepVersionTextures() noexcept
    {
        releaseVersionTextures();
        for (auto entry = versionTextures.begin(); entry != versionTextures.end();)
            if (!entry->second.open && entry->second.version.expired())
                evictVersionTexture(entry++);
            else
                ++entry;
        trimVersionTextures(versionTextureCap);
    }

    // Recorded work not yet signaled, or still in flight, ends with the next signal.
    std::uint64_t pooledReturnFence() const noexcept
    {
        return replaySubmissionInFlight ? nextFenceValue : 0;
    }

    // Hands a resource to the pool; bytes 0 measures it. A pending return the
    // pool refuses stays with the caller.
    bool holdPooled(TexturePool& pool, ComPtr<ID3D12Resource>& resource, std::uint64_t bytes = 0) noexcept
    {
        if (!resource)
            return true;
        const auto desc = resource->GetDesc();
        if (!bytes)
            bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        const auto key = pooledTextureKey(desc);
#if REREVVED_DEVELOPER
        if (&pool == &attachmentPool)
            attachmentMisses.Returned(key, attachmentMissFrames.load(std::memory_order_relaxed));
#endif
        return pool.Return(key, std::move(resource), bytes, pooledReturnFence());
    }

    // Returns a resource where its owner released it before pooling, and
    // releases one the pool refuses there, as before.
    void returnPooled(TexturePool& pool, ComPtr<ID3D12Resource>& resource, std::uint64_t bytes = 0) noexcept
    {
        if (!holdPooled(pool, resource, bytes))
            resource.Reset();
    }

    // An idle resource of desc's allocation and its size, or none. Attachment
    // misses enter the miss log under their take site, a clear pass's or a
    // draw replay's.
    ComPtr<ID3D12Resource> takePooled(TexturePool& pool, const D3D12_RESOURCE_DESC& desc, std::uint64_t* bytes = nullptr, bool clearPass = false) noexcept
    {
        const auto completed = fence ? fence->GetCompletedValue() : 0;
        const auto key       = pooledTextureKey(desc);
        auto       taken     = pool.Take(key, completed == UINT64_MAX ? 0 : completed);
        if (bytes)
            *bytes = taken.bytes;
#if REREVVED_DEVELOPER
        if (!taken.resource && &pool == &attachmentPool)
        {
            attachmentMisses.Missed(key, attachmentMissFrames.load(std::memory_order_relaxed));
            const std::size_t site = (clearPass ? 0 : 2) + ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? 1 : 0);
            attachmentSiteMisses[site].fetch_add(1, std::memory_order_relaxed);
            // A log failure drops the report; it must not leave this noexcept take.
            if (pool.ReportDue())
                try
                {
                    REXLOG_INFO("Native attachment pool: {}", pool.Report());
                }
                catch (...)
                {
                }
        }
#else
        static_cast<void>(clearPass);
#endif
        return std::move(taken.resource);
    }

    std::array<ReplayDrawResources, kReplayBatchDraws> replayDrawSlots;
    // Descriptor ranges recycle at their owner fence. Upload extents remain
    // immutable across all chunks until the complete frame retires.
    std::vector<ReplayDrawResources>                    frameDrawSlots;
    std::array<FrameCommandBatch, kFrameCommandBatches> frameCommandBatches;
    UploadLease                                         frameUploadArena;
    std::vector<ComPtr<ID3D12Resource>>                 frameRetiredUploads;
    std::vector<UploadLease>                            frameRetiredUploadLeases;
    std::uint8_t*                                       frameUploadMapped             = nullptr;
    std::size_t                                         frameUploadCursor             = 0;
    bool                                                frameUploadPrepared           = false;
    std::uint64_t                                       frameCpuTextureFootprintBytes = 0;
    std::uint64_t                                       frameCpuTexturePayloadBudget  = 0;
    using FrameCpuTextureLayout                                                       = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, NativeDrawReplayTextureFormat, NativeDrawReplayTextureDimension>;
    using FrameCpuTextureKey                                                          = std::pair<FrameCpuTextureLayout, std::uintptr_t>;

    static FrameCpuTextureKey frameCpuTextureKey(const NativeDrawReplayTexture& texture, const NativeDrawReplayTextureBinding* binding = nullptr)
    {
        const auto* identity = texture.sharedBytes ? texture.sharedBytes.get() : &texture.bytes;
        const auto  layout   = binding ? binding->ResourceLayout() : NativeDrawReplayTextureLayout{ texture.width, texture.height, texture.format, texture.mipLevels, texture.dimension };
        return { { layout.width, layout.height, layout.mipLevels, layout.format, layout.dimension },
                 reinterpret_cast<std::uintptr_t>(identity) };
    }

    struct FrameCpuTexture
    {
        ComPtr<ID3D12Resource>                                resource;
        UploadLease                                           upload;
        std::uint64_t                                         resourceStorageBytes = 0;
        std::uint64_t                                         resourceSerial       = 0;
        std::shared_ptr<const ReplayTexturePreparation>       preparation;
        std::shared_ptr<const NativeDrawReplayTextureVersion> uploadedVersion;
        std::shared_ptr<const NativeDrawReplayTextureVersion> reservedRecordingVersion;
        bool                                                  reservationConflict = false;
        std::uint64_t                                         reservationToken    = 0;
        bool                                                  recorded = false, recordedShared = false;
        // The context's preceding texture preparation held this payload key;
        // read only by the texture outcome counters.
        bool                            priorKey = false;
        std::unique_ptr<std::uint8_t[]> uploadedBytes;
        std::size_t                     uploadedSize = 0;

        std::uint64_t RetainedPayloadBytes() const noexcept
        {
            return uploadedSize + (uploadedVersion ? uploadedVersion->MetadataBytes() + uploadedVersion->Payload()->size() : 0);
        }

        // The payload with its vector and the device preparation, which may
        // be shared.
        std::uint64_t RetainedOwnedBytes() const noexcept
        {
            return RetainedPayloadBytes() + (uploadedVersion ? sizeof(std::vector<std::uint8_t>) : 0) + (preparation ? preparation->StorageBytes() : 0);
        }
    };

    // Takes textures about to be destroyed out of the payload and owned
    // counts, from the active context or a stored one.
    template <class Context>
    void uncountCpuTextures(const Context& context, bool retiredOnly) noexcept
    {
        const auto uncount = [&](const FrameCpuTexture& texture)
        {
            frameCpuTexturePayloadBytes.fetch_sub(texture.RetainedPayloadBytes(), std::memory_order_relaxed);
            frameCpuTextureOwnedBytes.fetch_sub(texture.RetainedOwnedBytes(), std::memory_order_relaxed);
        };
        if (!retiredOnly)
            for (const auto& [key, texture] : context.frameCpuTextures)
                uncount(texture);
        for (const auto& texture : context.frameRetiredCpuTextures)
            uncount(texture);
    }

    std::map<FrameCpuTextureKey, FrameCpuTexture> frameCpuTextures;
    std::vector<FrameCpuTexture>                  frameRetiredCpuTextures;
    ComPtr<ID3D12Resource>                        replayGammaConstants;

    // Whether a texture's retained snapshot of its resource bytes equals a payload.
    static bool retainsPayload(const FrameCpuTexture& texture, std::span<const std::uint8_t> payload)
    {
        if (texture.uploadedVersion)
        {
            const auto& kept = *texture.uploadedVersion->Payload();
            return kept.size() == payload.size() && (kept.data() == payload.data() || std::equal(payload.begin(), payload.end(), kept.begin()));
        }
        return texture.uploadedBytes && texture.uploadedSize == payload.size() && std::equal(payload.begin(), payload.end(), texture.uploadedBytes.get());
    }

    ComPtr<ID3D12Resource>                replayGammaTarget;
    ComPtr<ID3D12Resource>                replayInitialSample0;
    ComPtr<ID3D12Resource>                replayInitialSample1;
    ComPtr<ID3D12Resource>                replayInitialSample0Upload;
    ComPtr<ID3D12Resource>                replayInitialSample1Upload;
    std::array<ComPtr<ID3D12Resource>, 4> replayColorTargets;
    ComPtr<ID3D12Resource>                replayResolvedTarget;
    ComPtr<ID3D12Resource>                replayReadback;
    ComPtr<ID3D12Resource>                replayDepthTarget;
    bool                                  replayDepthInitialized = false;
    bool                                  frameAttachmentCaching = false;
    std::vector<ReplayColorAttachments>   frameColorAttachmentCache;
    std::vector<ReplayDepthAttachment>    frameDepthAttachmentCache;
    // The namespace of the active color and depth attachments.
    bool                                  activeColorPresented = false, activeDepthPresented = false;
    ComPtr<ID3D12DescriptorHeap>          replayDsvHeap;
    ID3D12Resource*                       replayDsvResource = nullptr; // the depth replayDsvHeap's view names
    std::array<ComPtr<ID3D12Resource>, 2> replayInitialDepth;
    std::array<ComPtr<ID3D12Resource>, 2> replayInitialDepthUpload;
    ComPtr<ID3D12DescriptorHeap>          replayRtvHeap;
    ComPtr<ID3D12Resource>                replayCopyConstants;
    ComPtr<ID3D12Resource>                replayResolveConstants;
    ComPtr<ID3D12Resource>                replayResolveConstantsSample3;
    ReplayOutputBundleKey                 replayOutputBundleKey;
    bool                                  replayOutputBundleValid  = false;
    std::uint64_t                         replayFenceValue         = 0;
    bool                                  replaySubmissionInFlight = false;

    // Names each texture resource a draw slot's views may hold: frame CPU
    // textures, stored versions and frame texture owners draw from it, so a
    // serial stays unique across both frame contexts and all three kinds.
    std::uint64_t textureResourceSerial = 0;

    struct FrameContext
    {
        FrameContext()                                                                               = default;
        FrameContext(FrameContext&&)                                                                 = default;
        FrameContext&                                                      operator=(FrameContext&&) = default;
        std::map<std::uint64_t, ComPtr<ID3D12Resource>>                    frameTextures;
        std::map<std::uint64_t, std::shared_ptr<NativeFrameResourceOwner>> frameTextureOwners;
        std::optional<RecordedRenderPrefix>                                recordedRenderPrefix;
        std::map<std::uint32_t, ComPtr<ID3D12Resource>>                    frameColorSnapshots;
        UploadLease                                                        frameUploadArena;
        std::vector<ComPtr<ID3D12Resource>>                                frameRetiredUploads;
        std::vector<UploadLease>                                           frameRetiredUploadLeases;
        std::uint8_t*                                                      frameUploadMapped             = nullptr;
        std::size_t                                                        frameUploadCursor             = 0;
        bool                                                               frameUploadPrepared           = false;
        std::uint64_t                                                      frameCpuTextureFootprintBytes = 0, frameCpuTexturePayloadBudget = 0;
        std::map<FrameCpuTextureKey, FrameCpuTexture>                      frameCpuTextures;
        std::vector<FrameCpuTexture>                                       frameRetiredCpuTextures;
        ComPtr<ID3D12Resource>                                             replayGammaConstants, replayGammaTarget;
        ComPtr<ID3D12Resource>                                             replayInitialSample0, replayInitialSample1, replayInitialSample0Upload, replayInitialSample1Upload;
        std::array<ComPtr<ID3D12Resource>, 4>                              replayColorTargets;
        ComPtr<ID3D12Resource>                                             replayResolvedTarget, replayReadback, replayDepthTarget;
        bool                                                               replayDepthInitialized = false, frameAttachmentCaching = false;
        std::vector<ReplayColorAttachments>                                frameColorAttachmentCache;
        std::vector<ReplayDepthAttachment>                                 frameDepthAttachmentCache;
        bool                                                               activeColorPresented = false, activeDepthPresented = false;
        ComPtr<ID3D12DescriptorHeap>                                       replayDsvHeap, replayRtvHeap;
        ID3D12Resource*                                                    replayDsvResource = nullptr;
        std::array<ComPtr<ID3D12Resource>, 2>                              replayInitialDepth, replayInitialDepthUpload;
        ComPtr<ID3D12Resource>                                             replayCopyConstants, replayResolveConstants, replayResolveConstantsSample3;
        ReplayOutputBundleKey                                              replayOutputBundleKey;
        bool                                                               replayOutputBundleValid = false, replaySubmissionInFlight = false;
        std::uint64_t                                                      replayFenceValue = 0;
    };

    std::array<FrameContext, 2> frameContexts;
    std::size_t                 activeFrameContext = 0, nextCommandOwner = 0;
    std::shared_ptr<const void> rendererGeneration = std::make_shared<int>(0);

    struct SubmittedFrame
    {
        std::size_t                           context    = 0;
        std::uint64_t                         fenceValue = 0;
        std::vector<NativeRenderFrameTexture> textures;
        std::shared_ptr<TerminalRequest>      request;
        NativeDrawReplayResult                result;
        FrameGpuOutputSlot*                   output = nullptr;
        // A raw readback copied by this frame, mapped at retirement.
        ComPtr<ID3D12Resource>             rawReadback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT rawFootprint{};
        UINT64                             rawBytes = 0;
    };

    std::deque<SubmittedFrame> submittedFrames;
    bool                       deferFrameCompletion   = false;
    bool                       detachingFrameContexts = false;
    // During a detach: retired-upload references it leaked, in every context.
    std::vector<ID3D12Resource*> detachedRetiredUploads;
    FrameGpuOutputSlot*          submittedGpuOutput = nullptr;
    // The deferred raw readback executeRenderFrame recorded, for the submission.
    ComPtr<ID3D12Resource>             submittedRawReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT submittedRawFootprint{};
    UINT64                             submittedRawBytes = 0;

    void switchFrameContext(std::size_t index);
    bool retireSubmittedFrames(bool wait, std::string& error, bool cancelable = false);
    // Copies a completed raw readback into result.rawOutput.
    static bool mapRawReadback(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, UINT64 bytes, NativeDrawReplayResult& result);
    void        pruneFramePipelines();
    bool        acquireFrameContext(std::string& error);
    enum class StorageDomain
    {
        Upload,
        Texture,
        Attachment,
        CpuTexture,
        Owned
    };
    const NativeRenderFrame*                recordingFrameLease             = nullptr;
    const NativeRenderFrame*                recordingCpuReservationLease    = nullptr;
    std::size_t                             recordingCpuReservationContext  = 0;
    std::uint64_t                           recordingCpuReservationToken    = 0;
    std::uint64_t                           ownedRecordingReservation       = 0;
    std::uint64_t                           ownedRawOwnerOccurrences        = 0;
    std::size_t                             ownedRecordingLegacyKeyCapacity = 0;
    void                                    visitOwnedBytes(detail::OwnedVisitor visitor, bool stateLocked = false);
    std::pair<std::uint64_t, std::uint64_t> censusOwnedBytes(bool stateLocked = false);
    bool                                    ensureOwnerHeap(std::uint64_t work, std::string& error, bool stateLocked = false, bool retainGrowth = true);
    void                                    cancelRecordingCpuReservation();
    void                                    reserveRecordingCpuTextures(const NativeRenderFrame& frame);
    bool                                    recordingCpuVersionReserved(const FrameCpuTexture& texture, const std::shared_ptr<const NativeDrawReplayTextureVersion>& version) const;
    std::uint64_t                           retainedStorageBytes(StorageDomain domain, detail::OwnerSet* uniqueOwners = nullptr, bool stateLocked = false, bool cpuCandidates = false);
    bool                                    reserveStorage(StorageDomain domain, std::uint64_t bytes, std::string& error);
    void                                    refreshOwnedAdmission();
    void                                    serviceOwnedCapacity(std::uint64_t requested);
    bool                                    awaitOwnedSnapshot(std::unique_lock<std::mutex>& lock, std::string& error, bool onWorker);
    bool                                    admitTerminalInputs(const NativeRenderFrame& frame, std::unique_lock<std::mutex>& lock, std::string& error, bool onWorker);
    // onWorker admits a queued request at worker intake: the stream is quiet
    // by construction, and capacity and refreshes run inline.
    bool awaitOwnedAdmission(const NativeRenderFrame& frame, FrameImports& imports, std::unique_lock<std::mutex>& lock, std::string& error, bool terminalTicket = false, bool onWorker = false, bool refusePrefix = false);

    struct FrameOwnerForecast
    {
        std::uint64_t occurrences = 0, cpuKeys = 0, metadata = 0, prefixGrowth = 0, prefixPeak = 0;
        std::size_t   legacyKeyCapacity = 0;
    };

    FrameOwnerForecast                        frameMetadataReservation(const NativeRenderFrame& frame, bool requestOnly = false) const;
    bool                                      importFrameTexture(const NativeRenderFrameTexture& texture, std::string& error);
    std::shared_ptr<NativeFrameResourceOwner> resolveImportOwner(const NativeRenderFrameTexture& texture, std::string& error);
    bool                                      pinFrameImports(const NativeRenderFrame& frame, FrameImports& imports, std::string& error);
    bool                                      admitDirect(const NativeRenderFrame& frame, FrameImports& imports, std::unique_lock<std::mutex>& lock, std::string& error, bool charge, bool refusePrefix);
    std::uint64_t                             directFrameBytes(const NativeRenderFrame& frame);
    std::uint64_t                             directDrawBytes(const std::vector<NativeDrawReplayRecipe>& draws);
    bool                                      awaitDirectFit(std::uint64_t bytes, std::unique_lock<std::mutex>& lock, std::string& error);
    void                                      chargeDirectAdmission(std::uint64_t bytes);
    void                                      refreshDirectStructure();
    void                                      exportFrameTextures(const NativeRenderFrame& frame, const std::shared_ptr<TerminalRequest>& request);

    bool waitForFence(std::uint64_t value, bool cancelable = false)
    {
        const auto completedValue = fence->GetCompletedValue();
        if (completedValue == std::numeric_limits<std::uint64_t>::max())
        {
            return logDeviceRemoval("fence status", DXGI_ERROR_DEVICE_REMOVED);
        }
        if (value == 0 || completedValue >= value)
        {
            return true;
        }
        const HRESULT result = fence->SetEventOnCompletion(value, fenceEvent);
        if (FAILED(result))
        {
            return logDeviceRemoval("fence event", result);
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFenceWaitTimeoutMs);
        for (;;)
        {
            if (cancelable)
            {
                std::lock_guard lock(stateMutex);
                if (stopRequested)
                {
                    frameRecordingCancelled = true;
                    return false;
                }
            }
            // A canceled older registration can signal this shared event.
            // Only the requested actual fence value proves this owner's drain.
            const auto actual = fence->GetCompletedValue();
            if (actual == std::numeric_limits<std::uint64_t>::max())
                return logDeviceRemoval("fence completion", DXGI_ERROR_DEVICE_REMOVED);
            if (actual >= value)
                return true;
            const auto  remaining  = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            HANDLE      events[]   = { fenceEvent, requestEvent };
            const DWORD waitResult = remaining <= 0 ? WAIT_TIMEOUT : cancelable && requestEvent ? WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(remaining))
                                                                                                : WaitForSingleObject(fenceEvent, static_cast<DWORD>(remaining));
            if (waitResult == WAIT_TIMEOUT)
            {
                REXLOG_ERROR("Native D3D12 fence wait timed out after {} ms", kFenceWaitTimeoutMs);
                const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
                if (FAILED(reason))
                    REXLOG_ERROR("Native D3D12 device removed after fence timeout: HRESULT 0x{:08X}", static_cast<std::uint32_t>(reason));
                return false;
            }
            if (waitResult != WAIT_OBJECT_0 && !(cancelable && requestEvent && waitResult == WAIT_OBJECT_0 + 1))
            {
                REXLOG_ERROR("Native D3D12 fence wait failed: Win32 error {}", GetLastError());
                return false;
            }
        }
    }

    bool waitForGpu()
    {
        const std::uint64_t value  = nextFenceValue++;
        const HRESULT       result = queue->Signal(fence.Get(), value);
        if (FAILED(result))
        {
            return logDeviceRemoval("queue signal", result);
        }
        return waitForFence(value);
    }

    void releaseBackBuffers()
    {
        for (auto& buffer : backBuffers)
        {
            buffer.Reset();
        }
    }

    bool createBackBuffers()
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE heapStart =
            rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t index = 0; index < kFrameCount; ++index)
        {
            const HRESULT result =
                swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffers[index]));
            if (FAILED(result))
            {
                return logFailure("swap-chain buffer", result);
            }
            D3D12_CPU_DESCRIPTOR_HANDLE handle = heapStart;
            handle.ptr += static_cast<SIZE_T>(index) * rtvStride;
            device->CreateRenderTargetView(backBuffers[index].Get(), nullptr, handle);
        }
        return true;
    }

    bool logDeviceRemoval(const char* operation, HRESULT result)
    {
        REXLOG_ERROR("Native D3D12 {} failed: HRESULT 0x{:08X}", operation, static_cast<std::uint32_t>(result));
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
        if (FAILED(reason))
        {
            REXLOG_ERROR("Native D3D12 device removed: HRESULT 0x{:08X}",
                         static_cast<std::uint32_t>(reason));
        }
        return false;
    }

    void drainReplayInfoQueue(const char* operation)
    {
        if (!replayInfoQueue)
        {
            return;
        }

        constexpr UINT64 kMaxReportedMessages = 64;
        constexpr SIZE_T kMaxMessageBytes     = 64U * 1024U;
        const UINT64     messageCount         = replayInfoQueue->GetNumStoredMessages();
        const UINT64     reportCount          = std::min(messageCount, kMaxReportedMessages);
        for (UINT64 index = 0; index < reportCount; ++index)
        {
            SIZE_T  messageBytes = 0;
            HRESULT result       = replayInfoQueue->GetMessage(index, nullptr, &messageBytes);
            if (FAILED(result) || messageBytes < sizeof(D3D12_MESSAGE) ||
                messageBytes > kMaxMessageBytes)
            {
                REXLOG_ERROR("Native D3D12 replay InfoQueue [{}] message {} could not be sized: "
                             "HRESULT 0x{:08X}, bytes {}",
                             operation,
                             index,
                             static_cast<std::uint32_t>(result),
                             messageBytes);
                continue;
            }

            std::vector<std::uint8_t> storage(messageBytes);
            auto*                     message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            result                            = replayInfoQueue->GetMessage(index, message, &messageBytes);
            if (FAILED(result))
            {
                REXLOG_ERROR("Native D3D12 replay InfoQueue [{}] message {} could not be read: "
                             "HRESULT 0x{:08X}",
                             operation,
                             index,
                             static_cast<std::uint32_t>(result));
                continue;
            }
            REXLOG_ERROR("Native D3D12 replay InfoQueue [{}] severity={} id={} {}",
                         operation,
                         static_cast<std::uint32_t>(message->Severity),
                         static_cast<std::uint32_t>(message->ID),
                         message->pDescription ? message->pDescription : "<no description>");
        }
        if (messageCount > reportCount)
        {
            REXLOG_ERROR("Native D3D12 replay InfoQueue [{}] omitted {} additional messages",
                         operation,
                         messageCount - reportCount);
        }
        replayInfoQueue->ClearStoredMessages();
    }

    bool                            compileCachedReplayShader(const char*       source,
                                                              const char*       entryPoint,
                                                              const char*       profile,
                                                              ComPtr<ID3DBlob>& bytecode);
    bool                            ensureReplayDescriptorHeaps(ReplayDrawResources& draw);
    bool                            clearRenderFramePass(const NativeRenderFramePass& pass, FrameCommandBatch& owner, std::string& error);
    bool                            copyFrameColorBits(std::uint32_t sourcePass, bool restore, FrameCommandBatch& owner, std::string& error);
    ReplayHelperPipelineCacheEntry* getReplayHelperPipelines(
        std::uint32_t sampleCount,
        DXGI_FORMAT   colorFormat,
        std::string&  error);
    bool                      ensureReplayDrawRootSignature(std::uint32_t textureCount,
                                                            std::string&  error);
    std::vector<std::uint8_t> makeReplayGuestPipelineKey(const NativeDrawReplayRecipe& recipe,
                                                         DXGI_FORMAT                   colorFormat,
                                                         std::uint32_t                 textureCount) const;
    void                      clearReplayOutputBundle();
    void                      detachReplayOutputBundle();
    void                      clearReplayCache();
    void                      detachReplayCache();

    // The key's suffix after the two shader byte runs.
    void appendReplayGuestPipelineState(std::vector<std::uint8_t>&    key,
                                        const NativeDrawReplayRecipe& recipe,
                                        DXGI_FORMAT                   colorFormat,
                                        std::uint32_t                 textureCount) const;

    bool                   initializeOnRendererThread(std::uintptr_t nativeWindow,
                                                      std::uint32_t  initialWidth,
                                                      std::uint32_t  initialHeight);
    bool                   initializeDeviceAndCommandObjects(bool replayDiagnostics);
    bool                   initializeHeadlessOnRendererThread(std::uint32_t initialWidth,
                                                              std::uint32_t initialHeight);
    bool                   resizeOnRendererThread(std::uint32_t width, std::uint32_t height);
    bool                   presentOnRendererThread();
    NativeDrawReplayResult executeOffscreenReplayOnRendererThread(
        const NativeDrawReplayRecipe& recipe, bool preserveTargets = false, bool readback = true, const std::array<std::uint32_t, 256>* gamma = nullptr, std::vector<std::uint8_t>* outputBytes = nullptr, std::size_t batchSlot = 0, bool finishBatch = true, ReplayDrawResources* frameDraw = nullptr, FrameCommandBatch* frameBatch = nullptr, const NativeDrawReplayAdmission* admission = nullptr, bool recordingRecipeCharged = false);
    bool prepareFrameCommandBatch(FrameCommandBatch& batch, std::string& error);
    bool submitFrameCommandBatch(FrameCommandBatch& batch, std::string& error);
    bool completeFrameCommands(std::string& error);
    bool prepareFrameUploads(const NativeRenderFrame& frame, std::string& error, bool terminal = true, std::size_t firstPass = 0);
    bool readReplayOutput(const NativeDrawReplayRecipe& recipe, bool gamma, std::vector<std::uint8_t>& output, std::string& error);
    void detachReplayGpuObjects();
    bool ensureFrameGuestWordPipeline(std::uint32_t sampleCount, bool depth, std::string& error);
    bool resolveFrameTexture(const NativeRenderFrameResolve& resolve,
                             const NativeRenderFrameTexture& destination,
                             ReplayDrawResources&            resources,
                             FrameCommandBatch&              owner,
                             std::string&                    error);
    bool prepareFrameCpuTextures(const NativeRenderFrame& frame, std::string& error, bool keepUnused = false, bool preserveRecorded = false);
    bool prepareFrameAttachmentCache(const NativeRenderFrame& frame, std::string& error, bool keepUnused = false);
    void activateFrameColorAttachments(const ReplayAttachmentKey& key);

    void initializeVertexResidency();
    void releaseVertexResidency();
    void clearVertexResidency(ResidencySeam seam);
#if REREVVED_DEVELOPER
    void publishVertexResidency();
#else
    void publishVertexResidency() noexcept
    {
    }
#endif
    void          beginResidencyPacket();
    void          endResidencyPacket(bool success);
    std::uint64_t residencyCompleted() const;

    // Frame owner batches: when a chunk ends, a frame draw slot's view region,
    // and a deduplicated sampler table in the owner's heap.
    bool                 frameBatchFull(const FrameCommandBatch& owner, std::size_t ownerSlots, std::size_t ownerCommands, std::size_t commands, bool descriptors);
    ReplayDrawResources& frameDrawSlot(FrameCommandBatch& owner, std::size_t ownerIndex, std::size_t slot);
    bool                 frameSamplerTable(FrameCommandBatch& owner, const D3D12_SAMPLER_DESC* table, std::uint32_t count, D3D12_GPU_DESCRIPTOR_HANDLE& handle);

    // A resume restores the depth initialization the entry was parked with.
    void activateFrameDepthAttachment(const ReplayAttachmentKey& key, bool restoreInitialized = false);
    bool recordRawReadback(std::uint64_t textureId, FrameCommandBatch& owner, std::string& error);
    // Copies the just-resolved `tile` into the stored texture `surface`
    // assembles, creating that texture at its first tile.
    bool recordSurfaceAssembly(const NativeRenderFrameSurface& surface, const NativeRenderFrameTexture& tile, FrameCommandBatch& owner, std::string& error);
    bool ensureFrameGuestWordRootSignature(std::string& error);
    // Writes the R32Float conversion of the just-resolved depth words `tile`
    // into the stored texture `surface.depthKey` names, at the tile's place,
    // creating that texture at its first tile.
    bool recordSurfaceConversion(const NativeRenderFrameSurface& surface, const NativeRenderFrameTexture& tile, ReplayDrawResources& resources, FrameCommandBatch& owner, std::string& error);
    // The views castable guest-word storage is created with.
    static constexpr std::array<DXGI_FORMAT, 4> kGuestWordCastFormats{ DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16_FLOAT };
    // Set while a frame recording runs: its identityAttachments setting, the
    // current pass's namespace, and the prefix readbacks it recorded.
    bool                                                                           frameIdentityAttachments = false;
    bool                                                                           framePassPresented       = false;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const D3D12RawReadback>>> recordingRawReadbacks;
    // Readbacks handed to callers, counted as attachment storage while held.
    mutable std::mutex                                 rawReadbackMutex;
    std::vector<std::weak_ptr<const D3D12RawReadback>> liveRawReadbacks;

    ReplayAttachmentKey frameKey(ReplayAttachmentKey key) const
    {
        key.presented = framePassPresented;
        return key;
    }

    ReplayAttachmentKey currentColorKey() const
    {
        auto key      = replayColorKey(replayColorTargets);
        key.presented = activeColorPresented;
        return key;
    }

    ReplayAttachmentKey currentDepthKey() const
    {
        auto key      = replayAttachmentKey(replayDepthTarget.Get());
        key.presented = activeDepthPresented;
        return key;
    }

    NativeDrawReplayResult recordRenderFramePrefix(const NativeRenderFrame& prefix);
    void                   holdRecordedPrefixPasses(NativeRenderFrame& packet, NativeDrawReplayResult& result);
    bool                   joinRecordedPrefix(NativeRenderFrame& tail, std::unique_lock<std::mutex>& lock, std::string& error);
    bool                   matchesRenderFramePrefix(const NativeRenderFrame& frame, std::string& error);
    bool                   recordRenderFramePasses(const NativeRenderFrame& frame, std::size_t firstPass, FrameCommandBatch*& owner, std::size_t& ownerIndex, std::size_t& ownerSlots, std::size_t& ownerCommands, std::string& error);
    NativeDrawReplayResult executeRenderFrame(const NativeRenderFrame& frame);
    bool                   awaitGpuOutput(bool requireCopy, std::string& error, std::uint64_t* value = nullptr);
    bool                   awaitFrameGpuOutputs(std::string& error);
    void                   shutdownOnRendererThread();
#endif
};

NativeRendererD3D12::NativeRendererD3D12()
: impl(std::make_unique<Impl>())
{
}

NativeRendererD3D12::~NativeRendererD3D12()
{
    Shutdown();
}

#if defined(_WIN32)

bool NativeRendererD3D12::Impl::compileCachedReplayShader(const char*       source,
                                                          const char*       entryPoint,
                                                          const char*       profile,
                                                          ComPtr<ID3DBlob>& bytecode)
{
    if (bytecode)
    {
        return true;
    }
    ++replayHelperShaderCompilations;
    return compileReplayShader(source, entryPoint, profile, bytecode);
}

bool NativeRendererD3D12::Impl::ensureReplayDescriptorHeaps(ReplayDrawResources& draw)
{
    if (!replayRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 6;
        HRESULT result      = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&replayRtvHeap));
        if (FAILED(result))
        {
            return false;
        }
        ++replayDescriptorHeapCreations;
    }
    // A frame slot's region lives in its owner's view heap.
    if (draw.viewCpu.ptr && !draw.srvHeap)
        return true;
    if (!draw.srvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kDrawViewRegion;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT result      = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&draw.srvHeap));
        if (FAILED(result))
        {
            return false;
        }
        draw.forgetDescriptors();
        draw.viewCpu = draw.srvHeap->GetCPUDescriptorHandleForHeapStart();
        draw.viewGpu = draw.srvHeap->GetGPUDescriptorHandleForHeapStart();
        ++replayDescriptorHeapCreations;
    }
    if (!draw.samplerHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        desc.NumDescriptors = kNativeDrawReplayTextureSlots;
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT result      = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&draw.samplerHeap));
        if (FAILED(result))
        {
            return false;
        }
        draw.samplerDescriptorMask = 0;
        ++replayDescriptorHeapCreations;
    }
    return true;
}

ReplayHelperPipelineCacheEntry* NativeRendererD3D12::Impl::getReplayHelperPipelines(
    std::uint32_t sampleCount,
    DXGI_FORMAT   colorFormat,
    std::string&  error)
{
    for (auto& entry : replayHelperPipelineCache)
    {
        if (entry.sampleCount == sampleCount && entry.colorFormat == colorFormat)
        {
            return &entry;
        }
    }

    if (replayHelperPipelineCache.size() >= kMaxReplayHelperPipelineEntries)
    {
        replayHelperPipelineCache.erase(replayHelperPipelineCache.begin());
    }

    if (!replayCopyRootSignature)
    {
        D3D12_DESCRIPTOR_RANGE1 copyRange{};
        copyRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        copyRange.NumDescriptors     = 1;
        copyRange.BaseShaderRegister = 0;
        copyRange.RegisterSpace      = 0;
        // Helper draws sample attachments written in this same command list.
        // Rebind before sampling; keep descriptors fixed through batch completion.
        copyRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
        D3D12_ROOT_PARAMETER1 copyParameters[2]{};
        copyParameters[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        copyParameters[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        copyParameters[0].DescriptorTable.NumDescriptorRanges = 1;
        copyParameters[0].DescriptorTable.pDescriptorRanges   = &copyRange;
        copyParameters[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
        copyParameters[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        copyParameters[1].Descriptor.ShaderRegister           = 0;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
        rootDesc.Desc_1_1.NumParameters = 2;
        rootDesc.Desc_1_1.pParameters   = copyParameters;
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        HRESULT          result = D3D12SerializeVersionedRootSignature(&rootDesc, &serialized, &errors);
        if (FAILED(result))
        {
            error = "could not serialize replay copy root signature";
            return nullptr;
        }
        result = device->CreateRootSignature(0,
                                             serialized->GetBufferPointer(),
                                             serialized->GetBufferSize(),
                                             IID_PPV_ARGS(&replayCopyRootSignature));
        if (FAILED(result))
        {
            error = "could not create replay copy root signature";
            return nullptr;
        }
        ++replayRootSignatureCreations;
    }

    if (!compileCachedReplayShader(kReplayCopyShaderSource,
                                   "copy_vs",
                                   "vs_5_1",
                                   replayCopyVertexShader) ||
        !compileCachedReplayShader(kReplayCopyShaderSource,
                                   "copy_ps",
                                   "ps_5_1",
                                   replayCopyPixelShader) ||
        !compileCachedReplayShader(kReplayResolveShaderSource,
                                   "copy_ps",
                                   "ps_5_1",
                                   replayResolvePixelShader) ||
        !compileCachedReplayShader(kFrameGammaShaderSource,
                                   "gamma_ps",
                                   "ps_5_1",
                                   replayGammaShader) ||
        !compileCachedReplayShader(kReplayDepthReadShaderSource,
                                   "read_depth",
                                   "ps_5_1",
                                   replayDepthReadShader) ||
        !compileCachedReplayShader(kReplayDepthCopyShaderSource,
                                   "copy_depth",
                                   "ps_5_1",
                                   replayDepthCopyShader))
    {
        error = "could not compile replay helper shaders";
        return nullptr;
    }

    ReplayHelperPipelineCacheEntry newEntry;
    auto*                          resolveShader   = replayResolvePixelShader.Get();
    auto*                          gammaShader     = replayGammaShader.Get();
    auto*                          depthReadShader = replayDepthReadShader.Get();
    if (sampleCount == 1)
    {
        const auto gammaSource = std::string("#define SINGLE_SAMPLE\n") + kFrameGammaShaderSource;
        const auto depthSource = std::string("#define SINGLE_SAMPLE\n") + kReplayDepthReadShaderSource;
        if (!compileCachedReplayShader(gammaSource.c_str(), "gamma_ps", "ps_5_1", replaySingleSampleGammaShader) ||
            !compileCachedReplayShader(depthSource.c_str(), "read_depth", "ps_5_1", replaySingleSampleDepthReadShader))
        {
            error = "could not compile single-sample replay helper shaders";
            return nullptr;
        }
        resolveShader   = replayCopyPixelShader.Get();
        gammaShader     = replaySingleSampleGammaShader.Get();
        depthReadShader = replaySingleSampleDepthReadShader.Get();
    }
    newEntry.sampleCount = sampleCount;
    newEntry.colorFormat = colorFormat;
    D3D12_BLEND_DESC blend{};
    for (auto& targetBlend : blend.RenderTarget)
    {
        targetBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    D3D12_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode          = D3D12_FILL_MODE_SOLID;
    rasterizer.CullMode          = D3D12_CULL_MODE_NONE;
    rasterizer.DepthClipEnable   = TRUE;
    rasterizer.MultisampleEnable = TRUE;
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable   = FALSE;
    depth.StencilEnable = FALSE;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC copyPso{};
    copyPso.pRootSignature        = replayCopyRootSignature.Get();
    copyPso.VS                    = { replayCopyVertexShader->GetBufferPointer(), replayCopyVertexShader->GetBufferSize() };
    copyPso.PS                    = { replayCopyPixelShader->GetBufferPointer(), replayCopyPixelShader->GetBufferSize() };
    copyPso.BlendState            = blend;
    copyPso.RasterizerState       = rasterizer;
    copyPso.DepthStencilState     = depth;
    copyPso.SampleMask            = 1;
    copyPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    copyPso.NumRenderTargets      = 4;
    copyPso.SampleDesc            = { sampleCount, 0 };
    for (std::size_t index = 0; index < 4; ++index)
    {
        copyPso.RTVFormats[index] = colorFormat;
    }
    const auto createPipeline = [this](const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                       ComPtr<ID3D12PipelineState>&              pipeline)
    {
        const HRESULT result = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (SUCCEEDED(result))
        {
            ++replayPipelineCreations;
            return true;
        }
        return false;
    };
    if (!createPipeline(copyPso, newEntry.copy))
    {
        error = "could not create replay initial-sample pipeline";
        return nullptr;
    }
    copyPso.SampleMask = sampleCount == 1 ? 1 : 1U << 3;
    if (!createPipeline(copyPso, newEntry.copySample3))
    {
        error = "could not create replay sample-3 pipeline";
        return nullptr;
    }
    copyPso.PS               = { resolveShader->GetBufferPointer(), resolveShader->GetBufferSize() };
    copyPso.SampleMask       = D3D12_DEFAULT_SAMPLE_MASK;
    copyPso.NumRenderTargets = 1;
    copyPso.SampleDesc       = { 1, 0 };
    copyPso.RTVFormats[0]    = colorFormat;
    for (std::size_t index = 1; index < std::size(copyPso.RTVFormats); ++index)
    {
        copyPso.RTVFormats[index] = DXGI_FORMAT_UNKNOWN;
    }
    if (!createPipeline(copyPso, newEntry.resolve))
    {
        error = "could not create replay resolve pipeline";
        return nullptr;
    }
    auto depthReadPso = copyPso;
    depthReadPso.PS   = { depthReadShader->GetBufferPointer(), depthReadShader->GetBufferSize() };
    if (!createPipeline(depthReadPso, newEntry.depthRead))
    {
        error = "could not create replay depth readback pipeline";
        return nullptr;
    }
    auto depthCopyPso             = copyPso;
    depthCopyPso.PS               = { replayDepthCopyShader->GetBufferPointer(), replayDepthCopyShader->GetBufferSize() };
    depthCopyPso.NumRenderTargets = 0;
    std::fill(std::begin(depthCopyPso.RTVFormats), std::end(depthCopyPso.RTVFormats), DXGI_FORMAT_UNKNOWN);
    depthCopyPso.SampleDesc                       = { sampleCount, 0 };
    depthCopyPso.DSVFormat                        = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthCopyPso.DepthStencilState.DepthEnable    = TRUE;
    depthCopyPso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depthCopyPso.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
    depthCopyPso.SampleMask                       = 1;
    if (!createPipeline(depthCopyPso, newEntry.depthCopy[0]))
    {
        error = "could not create replay depth initialization pipeline";
        return nullptr;
    }
    depthCopyPso.SampleMask = sampleCount == 1 ? 1 : 1U << 3;
    if (!createPipeline(depthCopyPso, newEntry.depthCopy[1]))
    {
        error = "could not create replay depth initialization pipeline";
        return nullptr;
    }
    auto gammaPso          = copyPso;
    gammaPso.PS            = { gammaShader->GetBufferPointer(), gammaShader->GetBufferSize() };
    gammaPso.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!createPipeline(gammaPso, newEntry.gamma))
    {
        error = "could not create native frame gamma pipeline";
        return nullptr;
    }

    replayHelperPipelineCache.push_back(std::move(newEntry));
    return &replayHelperPipelineCache.back();
}

bool NativeRendererD3D12::Impl::ensureReplayDrawRootSignature(std::uint32_t textureCount,
                                                              std::string&  error)
{
    if (textureCount >= replayDrawRootSignatures.size())
    {
        error = "replay texture count is outside the root signature cache";
        return false;
    }
    if (replayDrawRootSignatures[textureCount])
    {
        return true;
    }
    D3D12_ROOT_PARAMETER1 parameters[5]{};
    parameters[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].ShaderVisibility          = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].Descriptor.RegisterSpace  = 4;
    parameters[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].Descriptor.RegisterSpace  = 4;
    parameters[2].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[2].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    parameters[2].Descriptor.ShaderRegister = 2;
    parameters[2].Descriptor.RegisterSpace  = 4;
    D3D12_DESCRIPTOR_RANGE1 textureRanges[2]{};
    textureRanges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRanges[0].NumDescriptors                    = textureCount;
    textureRanges[0].BaseShaderRegister                = 0;
    textureRanges[0].RegisterSpace                     = 0;
    textureRanges[0].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    textureRanges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRanges[1].NumDescriptors                    = textureCount;
    textureRanges[1].BaseShaderRegister                = 0;
    textureRanges[1].RegisterSpace                     = 2;
    textureRanges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    textureRanges[1].Flags                             = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    D3D12_DESCRIPTOR_RANGE1 samplerRange{};
    samplerRange.RangeType         = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samplerRange.NumDescriptors    = textureCount;
    samplerRange.RegisterSpace     = 3;
    parameters[3].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[3].DescriptorTable  = { 2, textureRanges };
    parameters[4].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[4].DescriptorTable  = { 1, &samplerRange };
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootDesc.Desc_1_1.NumParameters = textureCount ? 5 : 3;
    rootDesc.Desc_1_1.pParameters   = parameters;
    rootDesc.Desc_1_1.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    HRESULT          result = D3D12SerializeVersionedRootSignature(&rootDesc, &serialized, &errors);
    if (FAILED(result))
    {
        error = "could not serialize replay draw root signature";
        return false;
    }
    result = device->CreateRootSignature(0,
                                         serialized->GetBufferPointer(),
                                         serialized->GetBufferSize(),
                                         IID_PPV_ARGS(&replayDrawRootSignatures[textureCount]));
    if (FAILED(result))
    {
        error = "could not create replay draw root signature";
        return false;
    }
    ++replayRootSignatureCreations;
    return true;
}

std::vector<std::uint8_t> NativeRendererD3D12::Impl::makeReplayGuestPipelineKey(
    const NativeDrawReplayRecipe& recipe,
    DXGI_FORMAT                   colorFormat,
    std::uint32_t                 textureCount) const
{
    std::vector<std::uint8_t> key;
    key.reserve(replayGuestPipelineKeyCapacity(recipe));
    appendReplayCacheKeyBytes(key, recipe.vertexShaderBytes());
    appendReplayCacheKeyBytes(key, recipe.pixelShaderBytes());
    appendReplayGuestPipelineState(key, recipe, colorFormat, textureCount);
    return key;
}

void NativeRendererD3D12::Impl::appendReplayGuestPipelineState(
    std::vector<std::uint8_t>&    key,
    const NativeDrawReplayRecipe& recipe,
    DXGI_FORMAT                   colorFormat,
    std::uint32_t                 textureCount) const
{
    appendReplayCacheKeyScalar(key, recipe.sampleMask);
    appendReplayCacheKeyScalar(key, recipe.sampleCount);
    appendReplayCacheKeyScalar(key, recipe.vertexAttributeCount);
    appendReplayCacheKeyScalar(key, textureCount);
    appendReplayCacheKeyScalar(key, static_cast<std::uint32_t>(colorFormat));
    const auto appendBool = [&key](bool value)
    {
        appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(value ? 1 : 0));
    };
    appendBool(recipe.blend.enabled);
    appendBool(recipe.blend.alphaToCoverage);
    appendBool(recipe.blend.logicOpEnabled);
    appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(recipe.blend.sourceColor));
    appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(recipe.blend.destinationColor));
    appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(recipe.blend.colorOp));
    appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(recipe.blend.sourceAlpha));
    appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(recipe.blend.destinationAlpha));
    appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(recipe.blend.alphaOp));
    appendReplayCacheKeyScalar(key, recipe.blend.writeMask);
    appendReplayCacheKeyScalar(key, recipe.rasterizer.cull);
    appendBool(recipe.rasterizer.frontCounterClockwise);
    appendBool(recipe.rasterizer.depthClip);
    appendBool(recipe.depth.enabled);
    appendBool(recipe.depth.writeEnabled);
    appendReplayCacheKeyScalar(key, recipe.depth.compare);
    appendBool(recipe.stencil.enabled);
    appendReplayCacheKeyScalar(key, recipe.stencil.readMask);
    appendReplayCacheKeyScalar(key, recipe.stencil.writeMask);
    for (const auto& face : { recipe.stencil.front, recipe.stencil.back })
    {
        appendReplayCacheKeyScalar(key, face.compare);
        appendReplayCacheKeyScalar(key, face.fail);
        appendReplayCacheKeyScalar(key, face.depthFail);
        appendReplayCacheKeyScalar(key, face.pass);
    }
    if (recipe.extraTargetCount)
    {
        appendReplayCacheKeyScalar(key, recipe.extraTargetCount);
        for (const auto& extra : recipe.extraTargets)
        {
            appendReplayCacheKeyScalar(key, static_cast<std::uint32_t>(mapTargetFormat(extra.format)));
            appendBool(extra.blend.enabled);
            appendBool(extra.blend.alphaToCoverage);
            appendBool(extra.blend.logicOpEnabled);
            appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(extra.blend.sourceColor));
            appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(extra.blend.destinationColor));
            appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(extra.blend.colorOp));
            appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(extra.blend.sourceAlpha));
            appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(extra.blend.destinationAlpha));
            appendReplayCacheKeyScalar(key, static_cast<std::uint8_t>(extra.blend.alphaOp));
            appendReplayCacheKeyScalar(key, extra.blend.writeMask);
        }
    }
}

void NativeRendererD3D12::Impl::clearReplayOutputBundle()
{
    replayGammaConstants.Reset();
    replayGammaTarget.Reset();
    replayResolvedTarget.Reset();
    replayReadback.Reset();
    replayOutputBundleKey   = {};
    replayOutputBundleValid = false;
}

void NativeRendererD3D12::Impl::detachReplayOutputBundle()
{
    (void)replayGammaConstants.Detach();
    (void)replayGammaTarget.Detach();
    (void)replayResolvedTarget.Detach();
    (void)replayReadback.Detach();
    replayOutputBundleKey   = {};
    replayOutputBundleValid = false;
}

void NativeRendererD3D12::Impl::clearReplayCache()
{
    replayCopyRootSignature.Reset();
    for (auto& root : replayDrawRootSignatures)
    {
        root.Reset();
    }
    replayCopyVertexShader.Reset();
    replayCopyPixelShader.Reset();
    replayResolvePixelShader.Reset();
    replayGammaShader.Reset();
    replaySingleSampleGammaShader.Reset();
    replayDepthReadShader.Reset();
    replaySingleSampleDepthReadShader.Reset();
    replayDepthCopyShader.Reset();
    replayHelperPipelineCache.clear();
    replayGuestPipelineCache.clear();
    frameGuestPipelineCache.clear();
    pipelineCacheBytes.store(0, std::memory_order_relaxed);
    replayRtvHeap.Reset();
    replayDsvHeap.Reset();
    replayDsvResource = nullptr;
    replayCopyConstants.Reset();
    clearReplayOutputBundle();
    replayResolveConstants.Reset();
    replayResolveConstantsSample3.Reset();
}

void NativeRendererD3D12::Impl::detachReplayCache()
{
    (void)replayCopyRootSignature.Detach();
    for (auto& root : replayDrawRootSignatures)
    {
        (void)root.Detach();
    }
    (void)replayCopyVertexShader.Detach();
    (void)replayCopyPixelShader.Detach();
    (void)replayResolvePixelShader.Detach();
    (void)replayGammaShader.Detach();
    (void)replaySingleSampleGammaShader.Detach();
    (void)replayDepthReadShader.Detach();
    (void)replaySingleSampleDepthReadShader.Detach();
    (void)replayDepthCopyShader.Detach();
    for (auto& entry : replayHelperPipelineCache)
    {
        entry.detach();
    }
    for (auto& entry : replayGuestPipelineCache)
    {
        (void)entry.pipeline.Detach();
    }
    for (auto& entry : frameGuestPipelineCache)
    {
        (void)entry.pipeline.Detach();
    }
    (void)replayRtvHeap.Detach();
    (void)replayCopyConstants.Detach();
    detachReplayOutputBundle();
    (void)replayResolveConstants.Detach();
    (void)replayResolveConstantsSample3.Detach();
}

// The queue's completed fence value, or 0 where it proves nothing.
std::uint64_t NativeRendererD3D12::Impl::residencyCompleted() const
{
    const auto completed = fence ? fence->GetCompletedValue() : 0;
    return completed == UINT64_MAX ? 0 : completed;
}

#if REREVVED_DEVELOPER
// Publishes the gauge group and the cumulative event totals together, under
// the lock ReplayTimingStats reads them with.
void NativeRendererD3D12::Impl::publishVertexResidency()
{
    std::lock_guard timingLock(replayTimingMutex);
    auto            counters = residencyRetiredTotals;
    if (!vertexResidency)
    {
        for (auto* gauge : { &residentFootprint, &residentEntries, &residentPayloadBytes, &residentRetiredBytes })
            gauge->store(0, std::memory_order_relaxed);
        residentEvictions.store(counters.evictions, std::memory_order_relaxed);
        residentExpiries.store(counters.expiries, std::memory_order_relaxed);
        residentClears.store(counters.clears, std::memory_order_relaxed);
        return;
    }
    counters.evictions += vertexResidency->counters().evictions;
    counters.expiries += vertexResidency->counters().expiries;
    counters.clears += vertexResidency->counters().clears;
    residentFootprint.store(vertexResidency->FootprintBytes(), std::memory_order_relaxed);
    residentEntries.store(vertexResidency->Entries(), std::memory_order_relaxed);
    residentPayloadBytes.store(vertexResidency->PayloadBytes(), std::memory_order_relaxed);
    residentRetiredBytes.store(vertexResidency->RetiredBytes(), std::memory_order_relaxed);
    residentEvictions.store(counters.evictions, std::memory_order_relaxed);
    residentExpiries.store(counters.expiries, std::memory_order_relaxed);
    residentClears.store(counters.clears, std::memory_order_relaxed);
}
#endif

void NativeRendererD3D12::Impl::releaseVertexResidency()
{
#if REREVVED_DEVELOPER
    if (vertexResidency)
    {
        residencyRetiredTotals.evictions += vertexResidency->counters().evictions;
        residencyRetiredTotals.expiries += vertexResidency->counters().expiries;
        residencyRetiredTotals.clears += vertexResidency->counters().clears;
    }
#endif
    vertexResidency.reset();
    for (auto& chunk : residencyChunks)
        chunk.Reset();
    residencyChunkAddresses.fill(0);
    publishVertexResidency();
}

// Removes every live key at a seam, and records what that seam left.
void NativeRendererD3D12::Impl::clearVertexResidency(ResidencySeam seam)
{
    if (vertexResidency)
        vertexResidency->ClearLive();
    residencySeamEntries[seam].store(vertexResidency ? vertexResidency->Entries() : 0, std::memory_order_relaxed);
    publishVertexResidency();
}

void NativeRendererD3D12::Impl::beginResidencyPacket()
{
    if (vertexResidency)
    {
        // Test seam: drop every live key as an eviction would, before the packet.
        if (residencyClearFault.exchange(0, std::memory_order_relaxed))
            vertexResidency->ClearLive();
        vertexResidency->BeginPacket(residencyCompleted());
    }
}

// A packet that recorded stays resident. One that failed leaves nothing
// addressable; its chunks retire through their pins and fences.
void NativeRendererD3D12::Impl::endResidencyPacket(bool success)
{
    if (!vertexResidency)
        return;
    if (success)
        vertexResidency->EndPacket(residencyCompleted());
    else
    {
        vertexResidency->FailPacket();
        residencySeamEntries[SeamFailure].store(vertexResidency->Entries(), std::memory_order_relaxed);
    }
    publishVertexResidency();
}

// Creates the residency table and its fixed chunks for a new device. A
// refusal leaves residency off and counts an initialization failure.
void NativeRendererD3D12::Impl::initializeVertexResidency()
{
    clearVertexResidency(SeamInit);
    releaseVertexResidency();
    if (!REXCVAR_GET(native_vertex_residency))
        return;
    const auto fault = residencyInitFault.exchange(0, std::memory_order_relaxed);
    const auto fail  = [&](const char* what)
    {
        releaseVertexResidency();
#if REREVVED_DEVELOPER
        residentInitFailures.fetch_add(1, std::memory_order_relaxed);
#endif
        REXLOG_WARN("Native vertex residency is off: {}", what);
    };
    try
    {
        if (fault == 1)
            throw std::bad_alloc{};
        vertexResidency = std::make_unique<vertex_residency::VertexResidency>(REXCVAR_GET(native_vertex_residency_exact) ? vertex_residency::Mode::Exact : vertex_residency::Mode::Owner);
        vertexResidency->SetGeneration(++residencyGeneration);
    }
    catch (const std::bad_alloc&)
    {
        return fail("metadata allocation failed");
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width     = vertexResidency->limits().chunkBytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout                                                                  = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (std::uint32_t index = 0; index < vertexResidency->chunkCount(); ++index)
    {
        // Default-heap buffers promote from COMMON at their first copy or read.
        if (fault == 2 || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&residencyChunks[index]))))
            return fail("chunk creation failed");
        residencyChunks[index]->SetName(L"ReRevved native resident vertex chunk");
        residencyChunkAddresses[index] = residencyChunks[index]->GetGPUVirtualAddress();
    }
    publishVertexResidency();
}

bool NativeRendererD3D12::Impl::initializeDeviceAndCommandObjects(bool replayDiagnostics)
{
    clearReplayCache();
    HRESULT result = S_OK;
    if (sharedStreamDevice)
    {
        device = sharedStreamDevice;
        if (FAILED(device->GetDeviceRemovedReason()))
            return logFailure("borrowed device health", device->GetDeviceRemovedReason());
    }
    else
    {
        if (!replayDiagnostics)
        {
            enableDred();
        }
        result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(result))
        {
            return logFailure("factory creation", result);
        }

        for (std::uint32_t index = 0;; ++index)
        {
            ComPtr<IDXGIAdapter1> candidate;
            result = factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate));
            if (result == DXGI_ERROR_NOT_FOUND)
            {
                break;
            }
            if (FAILED(result))
            {
                return logFailure("adapter enumeration", result);
            }

            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(candidate->GetDesc1(&description)) ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            {
                continue;
            }

            ComPtr<ID3D12Device> candidateDevice;
            result = D3D12CreateDevice(candidate.Get(),
                                       D3D_FEATURE_LEVEL_11_0,
                                       IID_PPV_ARGS(&candidateDevice));
            if (SUCCEEDED(result))
            {
                adapter = std::move(candidate);
                device  = std::move(candidateDevice);
                device->SetName(L"ReRevved native renderer device");
                REXLOG_INFO("Native D3D12 adapter selected: vendor={:04X} device={:04X}",
                            description.VendorId,
                            description.DeviceId);
                break;
            }
            logFailure("adapter device creation", result);
        }
        if (!device)
        {
            REXLOG_ERROR("Native D3D12 found no compatible hardware adapter");
            return false;
        }
    }

    // Castable creation takes an initial layout, so it also needs enhanced
    // barriers; the textures interoperate with legacy barriers from COMMON.
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    castingDevice.Reset();
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12, sizeof(options12))) &&
        options12.EnhancedBarriersSupported && options12.RelaxedFormatCastingSupported)
        (void)device.As(&castingDevice);
    castableGuestWords.store(castingDevice != nullptr, std::memory_order_relaxed);

    replayInfoQueue.Reset();
    if (replayDiagnostics)
    {
        result = device.As(&replayInfoQueue);
        if (FAILED(result))
        {
            REXLOG_WARN("Native D3D12 replay InfoQueue unavailable: HRESULT 0x{:08X}",
                        static_cast<std::uint32_t>(result));
        }
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    result         = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    if (FAILED(result))
    {
        return logFailure("command queue creation", result);
    }
    queue->SetName(L"ReRevved native renderer direct queue");

    for (std::uint32_t index = 0; index < kFrameCount; ++index)
    {
        auto& allocator = allocators[index];
        result          = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        if (FAILED(result))
        {
            return logFailure("command allocator creation", result);
        }
        allocator->SetName(index == 0 ? L"ReRevved native frame allocator 0"
                                      : L"ReRevved native frame allocator 1");
    }
    result = device->CreateCommandList(0,
                                       D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocators[0].Get(),
                                       nullptr,
                                       IID_PPV_ARGS(&commandList));
    if (FAILED(result))
    {
        return logFailure("command list creation", result);
    }
    commandList->SetName(L"ReRevved native frame command list");
    result = commandList->Close();
    if (FAILED(result))
    {
        return logFailure("initial command list close", result);
    }
    result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(result))
    {
        return logFailure("fence creation", result);
    }
    fence->SetName(L"ReRevved native frame fence");
    fenceEvent    = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    terminalEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        std::lock_guard eventLock(requestEventMutex);
        requestEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (!fenceEvent || !terminalEvent || !requestEvent)
    {
        REXLOG_ERROR("Native D3D12 fence event creation failed: Win32 error {}",
                     GetLastError());
        return false;
    }
    initializeVertexResidency();
    return true;
}

bool NativeRendererD3D12::Impl::initializeOnRendererThread(
    std::uintptr_t nativeWindow,
    std::uint32_t  initialWidth,
    std::uint32_t  initialHeight)
{
    const HWND hwnd = reinterpret_cast<HWND>(nativeWindow);
    if (!hwnd || initialWidth == 0 || initialHeight == 0)
    {
        REXLOG_ERROR("Native D3D12 surface has no drawable HWND extent");
        return false;
    }
    width  = initialWidth;
    height = initialHeight;

    const auto failInitialization = [this]()
    {
        shutdownOnRendererThread();
        return false;
    };

    if (!initializeDeviceAndCommandObjects(false))
    {
        return failInitialization();
    }

    HRESULT result = S_OK;

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width       = width;
    swapChainDesc.Height      = height;
    swapChainDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc  = { 1, 0 };
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> newSwapChain;
    result = factory->CreateSwapChainForHwnd(
        queue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &newSwapChain);
    if (FAILED(result))
    {
        logFailure("swap chain creation", result);
        return failInitialization();
    }
    result = newSwapChain.As(&swapChain);
    if (FAILED(result))
    {
        logFailure("swap chain interface", result);
        return failInitialization();
    }
    result = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(result))
    {
        logFailure("window association", result);
        return failInitialization();
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrameCount;
    result                 = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(result))
    {
        logFailure("RTV heap creation", result);
        return failInitialization();
    }
    rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!createBackBuffers())
    {
        return failInitialization();
    }

    initialized.store(true, std::memory_order_release);
    REXLOG_INFO("Native D3D12 initialized: {}x{} frames={}", width, height, kFrameCount);
    if (!presentOnRendererThread())
    {
        shutdownOnRendererThread();
        return false;
    }
    REXLOG_INFO("Native D3D12 diagnostic frame presented");
    return true;
}

bool NativeRendererD3D12::Impl::initializeHeadlessOnRendererThread(
    std::uint32_t initialWidth,
    std::uint32_t initialHeight)
{
    if (initialWidth == 0 || initialHeight == 0)
    {
        REXLOG_ERROR("Native D3D12 headless replay has no target extent");
        return false;
    }
    width  = initialWidth;
    height = initialHeight;
    if (!initializeDeviceAndCommandObjects(true))
    {
        shutdownOnRendererThread();
        return false;
    }
    initialized.store(true, std::memory_order_release);
    REXLOG_INFO("Native D3D12 headless replay initialized: {}x{}", width, height);
    return true;
}

bool NativeRendererD3D12::Impl::resizeOnRendererThread(std::uint32_t newWidth,
                                                       std::uint32_t newHeight)
{
    if (newWidth == 0 || newHeight == 0 || !initialized.load(std::memory_order_acquire))
    {
        return newWidth == 0 || newHeight == 0;
    }
    if (newWidth == width && newHeight == height)
    {
        return true;
    }
    if (!waitForGpu())
    {
        return false;
    }

    releaseBackBuffers();
    fenceValues.fill(0);
    const HRESULT result = swapChain->ResizeBuffers(
        kFrameCount, newWidth, newHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(result))
    {
        logDeviceRemoval("swap chain resize", result);
        return false;
    }
    width  = newWidth;
    height = newHeight;
    if (!createBackBuffers())
    {
        return false;
    }
    REXLOG_INFO("Native D3D12 resized: {}x{}", newWidth, newHeight);
    return presentOnRendererThread();
}

bool NativeRendererD3D12::Impl::presentOnRendererThread()
{
    if (!initialized.load(std::memory_order_acquire))
    {
        return false;
    }

    const std::uint32_t frameIndex = swapChain->GetCurrentBackBufferIndex();
    if (!waitForFence(fenceValues[frameIndex]))
    {
        return false;
    }

    HRESULT result = allocators[frameIndex]->Reset();
    if (FAILED(result))
    {
        logFailure("command allocator reset", result);
        return false;
    }
    result = commandList->Reset(allocators[frameIndex].Get(), nullptr);
    if (FAILED(result))
    {
        logFailure("command list reset", result);
        return false;
    }

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource   = backBuffers[frameIndex].Get();
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(frameIndex) * rtvStride;
    constexpr float clearColor[4] = { 0.015F, 0.02F, 0.04F, 1.0F };
    commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &toPresent);

    result = commandList->Close();
    if (FAILED(result))
    {
        logFailure("command list close", result);
        return false;
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, lists);

    const std::uint64_t fenceValue = nextFenceValue++;
    result                         = queue->Signal(fence.Get(), fenceValue);
    if (FAILED(result))
    {
        logDeviceRemoval("frame signal", result);
        return false;
    }
    fenceValues[frameIndex] = fenceValue;

    result = swapChain->Present(1, 0);
    if (FAILED(result))
    {
        logDeviceRemoval("present", result);
        return false;
    }
    return true;
}

NativeDrawReplayResult NativeRendererD3D12::Impl::executeOffscreenReplayOnRendererThread(
    const NativeDrawReplayRecipe& recipe, bool preserveTargets, bool readback, const std::array<std::uint32_t, 256>* gamma, std::vector<std::uint8_t>* outputBytes, std::size_t batchSlot, bool finishBatch, ReplayDrawResources* frameDraw, FrameCommandBatch* frameBatch, const NativeDrawReplayAdmission* admission, bool recordingRecipeCharged)
{
    NativeDrawReplayResult result;
    result.outputPath = recipe.outputPath;
    const auto fail   = [&result](std::string message)
    {
        result.error = std::move(message);
        return result;
    };
    if (!initialized.load(std::memory_order_acquire) || !device || !queue || !fence)
    {
        return fail("headless D3D12 device is not initialized");
    }
    if (!frameDraw && !directStream)
    {
        const auto [physical, current] = censusOwnedBytes();
        const auto incoming            = std::max(ownedRawOwnerOccurrences, detail::OwnedAdd(6, detail::OwnedMultiply(3, recipe.textures.size())));
        const auto future              = detail::OwnedAdd(current, detail::OwnedAdd(incoming, 6));
        const auto storage             = detail::ForecastOwnerSet(future);
        const auto snapshot            = ownedAdmissionOwners ? detail::OwnerSetBytes(ownedAdmissionOwners->size(), ownedAdmissionOwners->bucket_count()) : 0;
        if (!ensureOwnerHeap(detail::OwnedAdd(storage.peak, storage.bytes > snapshot ? storage.bytes - snapshot : 0), result.error))
            return result;
        std::lock_guard lock(stateMutex);
        ownedScanCeiling = std::max(ownedScanCeiling, future);
    }
    auto& commandList = frameBatch ? frameBatch->commandList : this->commandList;
    auto& allocator   = frameBatch ? frameBatch->allocator : allocators[0];

    if (!frameAttachmentCaching)
    {
        frameColorAttachmentCache.clear();
        frameDepthAttachmentCache.clear();
        frameAttachmentByteBound.store(0, std::memory_order_relaxed);
    }
    auto& draw = frameDraw ? *frameDraw : replayDrawSlots[batchSlot];
#if REREVVED_DEVELOPER
    const auto recordingStart = std::chrono::steady_clock::now();
    auto       phaseStart     = recordingStart;
    const bool phaseTiming    = replayPipelineTiming.load(std::memory_order_relaxed);
    recordingTimed            = phaseTiming;
    // Phases publish together with the total, so a recorded draw's phases sum
    // to it; a failed draw publishes neither. A boundary may reuse a clock
    // read the recording already takes.
    std::array<std::uint64_t, NativeDrawReplayTimingStats::kRecordingPhases.size()> phaseNanoseconds{};
    std::array<std::uint64_t, NativeDrawReplayTimingStats::kDrawUploadKinds.size()> drawUploadBytes{};
    // Timed repeat, resident and view write counts publish with them too.
    std::array<std::uint64_t, NativeDrawReplayTimingStats::kConstantStages.size()> repeatPreviousBytes{}, repeatChunkBytes{};
    std::array<std::uint64_t, 2>                                                   residentBytes{};
    std::uint64_t                                                                  textureViewReuses = 0, textureSrvWrites = 0, drawRtvWrites = 0;
    const auto                                                                     phaseEnd = [&](std::size_t phase, std::chrono::steady_clock::time_point now = {})
    {
        if (!phaseTiming)
            return;
        if (now == std::chrono::steady_clock::time_point{})
            now = std::chrono::steady_clock::now();
        phaseNanoseconds[phase] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - phaseStart).count());
        phaseStart = now;
    };
#else
    constexpr bool phaseTiming = false;
    const auto     phaseEnd    = [](std::size_t, DevClockPoint = {})
    {
    };
#endif
    replayInitialSample0.Reset();
    replayInitialSample1.Reset();
    replayInitialSample0Upload.Reset();
    replayInitialSample1Upload.Reset();
    if (!preserveTargets)
    {
        // Allocation lifetime spans frames; depth contents start at first use.
        // A cached attachment of another key parks with its own state first.
        if (frameAttachmentCaching)
            activateFrameDepthAttachment(frameKey({ recipe.width, recipe.height, recipe.sampleCount, DXGI_FORMAT_R24G8_TYPELESS }));
        replayDepthInitialized = false;
    }
    for (auto& object : replayInitialDepth)
    {
        object.Reset();
    }
    for (auto& object : replayInitialDepthUpload)
    {
        object.Reset();
    }
    HRESULT hr = S_OK;
    if (!ensureReplayDescriptorHeaps(draw))
    {
        return fail("could not create replay descriptor heaps");
    }
    const std::uint32_t replayRtvStride =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    const UINT          textureCount = std::popcount(recipe.textureMask);
    const UINT          depthSrvBase = 3 + 2 * textureCount;
    const std::uint32_t replaySrvStride =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::uint64_t replayTextureFootprintBytes = 0;
    std::string   helperError;
    auto*         helper = getReplayHelperPipelines(recipe.sampleCount,
                                                    mapTargetFormat(recipe.targetFormat),
                                                    helperError);
    if (!helper)
    {
        return fail(std::move(helperError));
    }

    if (frameBatch && std::none_of(frameBatch->helpers.begin(), frameBatch->helpers.end(), [&](const auto& entry)
                                   {
                                       return entry.sampleCount == helper->sampleCount && entry.colorFormat == helper->colorFormat;
                                   }))
        frameBatch->helpers.push_back(*helper);

    const DXGI_FORMAT colorFormat   = mapTargetFormat(recipe.targetFormat);
    const auto        drawTarget    = NativeRenderFrameDrawTarget(recipe);
    const auto        createTexture = [&](D3D12_RESOURCE_FLAGS     flags,
                                          std::uint32_t            sampleCount,
                                          D3D12_RESOURCE_STATES    initialState,
                                          const D3D12_CLEAR_VALUE* clearValue,
                                          ComPtr<ID3D12Resource>&  resource,
                                          DXGI_FORMAT              format     = DXGI_FORMAT_UNKNOWN,
                                          bool                     attachment = false)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = recipe.width;
        desc.Height           = recipe.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = format == DXGI_FORMAT_UNKNOWN ? colorFormat : format;
        desc.SampleDesc       = { sampleCount, 0 };
        desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags            = flags;
        if (!reserveStorage(StorageDomain::Attachment, device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes, result.error))
            return E_OUTOFMEMORY;
        const auto    started = DevNow();
        const HRESULT created = device->CreateCommittedResource(&heap,
                                                                D3D12_HEAP_FLAG_NONE,
                                                                &desc,
                                                                initialState,
                                                                clearValue,
                                                                IID_PPV_ARGS(&resource));
        if (attachment)
            countAttachmentCreate(started);
        return created;
    };
    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = colorFormat;
    for (std::size_t component = 0; component < 4; ++component)
    {
        clearValue.Color[component] = recipe.clearColor[component] / 255.0F;
    }
    if (frameAttachmentCaching && !preserveTargets)
        activateFrameColorAttachments(frameKey(replayColorKey(drawTarget)));
    for (std::uint32_t slot = 0; slot < replayColorTargets.size(); ++slot)
    {
        auto&      target     = replayColorTargets[slot];
        const auto slotFormat = mapTargetFormat(NativeRenderFrameSlotFormat(drawTarget, slot));
        if (preserveTargets)
        {
            continue;
        }
        if (target)
        {
            const auto desc = target->GetDesc();
            if (desc.Width == recipe.width && desc.Height == recipe.height &&
                desc.Format == slotFormat && desc.SampleDesc.Count == recipe.sampleCount)
            {
                ++replayColorTargetReuses;
                continue;
            }
            returnPooled(attachmentPool, target);
        }
        D3D12_RESOURCE_DESC slotDesc{};
        slotDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        slotDesc.Width            = recipe.width;
        slotDesc.Height           = recipe.height;
        slotDesc.DepthOrArraySize = slotDesc.MipLevels = 1;
        slotDesc.Format                                = slotFormat;
        slotDesc.SampleDesc                            = { recipe.sampleCount, 0 };
        slotDesc.Flags                                 = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if ((target = takePooled(attachmentPool, slotDesc)))
        {
            ++replayColorTargetReuses;
            continue;
        }
        // Clear colors vary per frame and do not constrain allocation reuse.
        hr = createTexture(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                           recipe.sampleCount,
                           D3D12_RESOURCE_STATE_RENDER_TARGET,
                           nullptr,
                           target,
                           slotFormat,
                           true);
        if (FAILED(hr))
        {
            return fail("could not create replay multisample render target");
        }
        ++replayColorTargetCreations;
    }
    const std::uint64_t outputPlaneBytes =
        static_cast<std::uint64_t>(recipe.width) * recipe.height * 4ULL;
    const std::uint32_t outputRowPitch = (recipe.width * 4U + 255U) & ~255U;
    const std::uint64_t outputPlaneStride =
        (static_cast<std::uint64_t>(outputRowPitch) * recipe.height + 511ULL) & ~511ULL;
    const std::uint32_t outputPlaneCount   = (recipe.depth.enabled ? 4U : 2U) + (gamma ? 1U : 0U);
    const std::uint64_t readbackBytes      = outputPlaneStride * outputPlaneCount;
    bool                reusedOutputBundle = false;
    if (readback || gamma)
    {
        const ReplayOutputBundleKey outputKey{ recipe.width,
                                               recipe.height,
                                               colorFormat,
                                               recipe.sampleCount,
                                               outputPlaneCount,
                                               readbackBytes,
                                               gamma != nullptr };
        reusedOutputBundle = replayOutputBundleValid && replayOutputBundleKey == outputKey;
        if (reusedOutputBundle)
        {
            ++replayOutputBundleReuses;
        }
        else
        {
            clearReplayOutputBundle();
            hr = createTexture(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                               1,
                               D3D12_RESOURCE_STATE_RENDER_TARGET,
                               &clearValue,
                               replayResolvedTarget);
            if (FAILED(hr))
            {
                return fail("could not create replay resolved target");
            }

            D3D12_HEAP_PROPERTIES readbackHeap{};
            readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC readbackDesc{};
            readbackDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            readbackDesc.Width            = readbackBytes;
            readbackDesc.Height           = 1;
            readbackDesc.DepthOrArraySize = 1;
            readbackDesc.MipLevels        = 1;
            readbackDesc.SampleDesc.Count = 1;
            readbackDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (!reserveStorage(StorageDomain::Attachment, readbackBytes, result.error))
                return result;
            hr = device->CreateCommittedResource(&readbackHeap,
                                                 D3D12_HEAP_FLAG_NONE,
                                                 &readbackDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr,
                                                 IID_PPV_ARGS(&replayReadback));
            if (FAILED(hr))
            {
                return fail("could not create replay readback buffer");
            }

            if (gamma)
            {
                if (!reserveStorage(StorageDomain::Upload, sizeof(*gamma), result.error))
                    return result;
                if (!createReplayUploadBuffer(device.Get(),
                                              gamma->data(),
                                              sizeof(*gamma),
                                              replayGammaConstants))
                {
                    return fail("could not prepare native frame gamma pass");
                }
            }
            replayOutputBundleKey   = outputKey;
            replayOutputBundleValid = true;
            ++replayOutputBundleCreations;
        }
        if (gamma && reusedOutputBundle)
        {
            void* mapped = nullptr;
            hr           = replayGammaConstants->Map(0, nullptr, &mapped);
            if (FAILED(hr))
            {
                return fail("could not update native frame gamma pass");
            }
            std::memcpy(mapped, gamma->data(), sizeof(*gamma));
            replayGammaConstants->Unmap(0, nullptr);
        }
        if (gamma)
        {
            if (recordingGpuOutput)
                replayGammaTarget = recordingGpuOutput->resource;
            if (!replayGammaTarget)
            {
                auto gammaDesc   = replayResolvedTarget->GetDesc();
                gammaDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                if (!reserveStorage(StorageDomain::Attachment, device->GetResourceAllocationInfo(0, 1, &gammaDesc).SizeInBytes, result.error))
                    return result;
                D3D12_HEAP_PROPERTIES gammaHeap{};
                gammaHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
                hr             = device->CreateCommittedResource(&gammaHeap,
                                                                 D3D12_HEAP_FLAG_NONE,
                                                                 &gammaDesc,
                                                                 D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                 nullptr,
                                                                 IID_PPV_ARGS(&replayGammaTarget));
                if (FAILED(hr))
                {
                    return fail("could not create native frame gamma target");
                }
                if (recordingGpuOutput)
                    recordingGpuOutput->resource = replayGammaTarget;
            }
        }
    }

    if (!frameBatch && batchSlot == 0)
    {
        hr = allocator->Reset();
        if (FAILED(hr))
        {
            return fail("could not reset replay command allocator");
        }
        hr = commandList->Reset(allocator.Get(), nullptr);
        if (FAILED(hr))
        {
            return fail("could not reset replay command list");
        }
    }

    const auto makeRowUpload = [&](const std::vector<std::uint8_t>& source,
                                   ComPtr<ID3D12Resource>&          texture,
                                   ComPtr<ID3D12Resource>&          upload)
    {
        const UINT                rowPitch = (recipe.width * 4U + 255U) & ~255U;
        std::vector<std::uint8_t> rows(static_cast<std::size_t>(rowPitch) * recipe.height, 0);
        for (std::uint32_t row = 0; row < recipe.height; ++row)
        {
            std::memcpy(rows.data() + static_cast<std::size_t>(row) * rowPitch,
                        source.data() + static_cast<std::size_t>(row) * recipe.width * 4U,
                        static_cast<std::size_t>(recipe.width) * 4U);
        }
        if (!createReplayUploadBuffer(device.Get(), rows.data(), rows.size(), upload))
        {
            return false;
        }
        hr = createTexture(D3D12_RESOURCE_FLAG_NONE,
                           1,
                           D3D12_RESOURCE_STATE_COPY_DEST,
                           nullptr,
                           texture);
        if (FAILED(hr))
        {
            return false;
        }
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource        = texture.Get();
        destination.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource                          = upload.Get();
        sourceLocation.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint.Offset             = 0;
        sourceLocation.PlacedFootprint.Footprint.Format   = colorFormat;
        sourceLocation.PlacedFootprint.Footprint.Width    = recipe.width;
        sourceLocation.PlacedFootprint.Footprint.Height   = recipe.height;
        sourceLocation.PlacedFootprint.Footprint.Depth    = 1;
        sourceLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
        return true;
    };

    const bool initializeFromSamples = !preserveTargets && !recipe.initialSample0.empty();
    if (initializeFromSamples && (!makeRowUpload(recipe.initialSample0,
                                                 replayInitialSample0,
                                                 replayInitialSample0Upload) ||
                                  !makeRowUpload(recipe.initialSample1,
                                                 replayInitialSample1,
                                                 replayInitialSample1Upload)))
    {
        return fail("could not create replay initial sample textures");
    }

    phaseEnd(PhaseSetup);
#if REREVVED_DEVELOPER
    // Published with the phases, only for a recorded draw.
    std::array<std::uint64_t, NativeDrawReplayTimingStats::kTextureOutcomes.size()> outcomeCounts{}, outcomeBytes{};
    const auto                                                                      countTexture = [&](TextureOutcome outcome, std::uint64_t bytes)
    {
        if (!phaseTiming)
            return;
        ++outcomeCounts[outcome];
        outcomeBytes[outcome] += bytes;
    };
#else
    const auto countTexture = [](TextureOutcome, std::uint64_t)
    {
    };
#endif
    // The cube range starts after textureCount views, and depth helpers follow it.
    if (draw.textureDescriptorCount != textureCount)
    {
        draw.textureDescriptors.fill({});
        draw.textureDescriptorCount = textureCount;
    }
    for (UINT slot = 0; slot < textureCount; ++slot)
    {
        const auto&      texture        = recipe.textures[slot];
        const auto*      binding        = admission ? &admission->TextureBindings()[slot] : nullptr;
        const auto       frameTextureId = binding ? binding->frameTextureId : texture.frameTextureId;
        const auto&      version        = binding ? binding->textureVersion : texture.textureVersion;
        const auto*      storeVersion   = frameDraw && !frameTextureId && versionTextureStore ? storedVersion(texture, binding) : nullptr;
        FrameCpuTexture* cached         = nullptr;
        if (frameDraw && !frameTextureId && !storeVersion)
        {
            cached = &frameCpuTextures.at(frameCpuTextureKey(texture, binding));
        }
        // A stored version's upload, storage and lease, until the store takes them.
        ComPtr<ID3D12Resource> storeTexture;
        UploadLease            storeUpload;
        std::uint64_t          storeBytes          = 0;
        auto&                  replayTexture       = storeVersion ? storeTexture : cached ? cached->resource
                                                                                          : draw.textures[slot];
        auto&                  replayTextureUpload = storeVersion ? storeUpload : cached ? cached->upload
                                                                                         : draw.textureUploads[slot];
        const auto             payload             = texture.payloadBytes();
        const auto             desc                = cached ? cached->preparation->description : binding ? replayTextureDescription(binding->ResourceLayout())
                                                                                                         : replayTextureDescription(texture);
        const auto             retainPayload       = [&]()
        {
            if (!cached)
                return;
            phaseEnd(PhaseTextures);
            frameCpuTexturePayloadBytes.fetch_sub(cached->RetainedPayloadBytes(), std::memory_order_relaxed);
            frameCpuTextureOwnedBytes.fetch_sub(cached->RetainedOwnedBytes(), std::memory_order_relaxed);
            // Counts what the entry holds on every exit, including an
            // allocation failure part way through.
            struct Recount
            {
                Impl&                  renderer;
                const FrameCpuTexture& texture;

                ~Recount()
                {
                    renderer.frameCpuTexturePayloadBytes.fetch_add(texture.RetainedPayloadBytes(), std::memory_order_relaxed);
                    renderer.frameCpuTextureOwnedBytes.fetch_add(texture.RetainedOwnedBytes(), std::memory_order_relaxed);
                }
            } recount{ *this, *cached };
            auto       reusableBytes = std::move(cached->uploadedBytes);
            const auto reusableSize  = cached->uploadedSize;
            cached->uploadedSize     = 0;
            cached->uploadedVersion.reset();
            const bool reservedCpu  = recordingCpuVersionReserved(*cached, version);
            const auto retained     = reservedCpu ? 0 : retainedStorageBytes(StorageDomain::CpuTexture);
            const auto cpuAvailable = reservedCpu ? kMaxReplayTextureFootprintBytes : retained < kMaxReplayTextureFootprintBytes ? kMaxReplayTextureFootprintBytes - retained
                                                                                                                                 : 0;
            const auto versionBytes = version ? version->MetadataBytes() + version->Payload()->size() : 0;
            // The exact recording recipe's immutable version and payload were
            // charged with its strong frame lease. Another cache ref adds no bytes.
            bool          sharedVersion  = reservedCpu || (version && recordingRecipeCharged && recordingFrameLease);
            std::uint64_t ownedAvailable = 0;
            if (directStream)
            {
                const auto owned = directOwnedBytes();
                ownedAvailable   = owned < kMaxReplayTextureFootprintBytes ? kMaxReplayTextureFootprintBytes - owned : 0;
            }
            else if (!sharedVersion || versionBytes > cpuAvailable)
            {
                std::string metadataError;
                if (!ensureOwnerHeap(detail::ForecastOwnerSet(ownedScanCeiling).peak, metadataError))
                    throw std::bad_alloc{};
                detail::OwnerSet retainedOwners(detail::OwnedAllocator<const void*>{ &ownedHeap });
                const auto       owned = retainedStorageBytes(StorageDomain::Owned, &retainedOwners);
                ownedAvailable         = owned < kMaxReplayTextureFootprintBytes ? kMaxReplayTextureFootprintBytes - owned : 0;
                sharedVersion          = version && retainedOwners.contains(version.get()) && retainedOwners.contains(version->Payload().get());
            }
            const auto available = std::min(cpuAvailable, ownedAvailable);
            if (version && versionBytes <= cpuAvailable && (sharedVersion || versionBytes <= ownedAvailable))
            {
                reusableBytes.reset();
                cached->uploadedVersion = version;
            }
            else if (payload.size() <= available)
            {
                if (reusableBytes && reusableSize == payload.size())
                    cached->uploadedBytes = std::move(reusableBytes);
                else
                {
                    reusableBytes.reset();
                    cached->uploadedBytes = std::make_unique_for_overwrite<std::uint8_t[]>(payload.size());
                }
                cached->uploadedSize = payload.size();
                std::memcpy(cached->uploadedBytes.get(), payload.data(), payload.size());
            }
            phaseEnd(PhaseTextureRetain);
        };
        ID3D12Resource* textureResource = nullptr;
        std::uint64_t   viewSerial      = 0;
        if (frameTextureId)
        {
            const auto found = frameTextures.find(frameTextureId);
            if (found == frameTextures.end())
                return fail("draw references a frame texture with no native producer");
            const auto produced = found->second->GetDesc();
            // A typed view of packed words needs storage created castable.
            const auto owner = frameTextureOwners.find(frameTextureId);
            const bool typed = produced.Format == DXGI_FORMAT_R32_UINT &&
                               (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || desc.Format == DXGI_FORMAT_R16G16_FLOAT) &&
                               owner != frameTextureOwners.end() && owner->second && owner->second->castable;
            if (produced.Width != desc.Width || produced.Height != desc.Height || (produced.Format != desc.Format && !typed) ||
                produced.MipLevels != 1 || produced.DepthOrArraySize != 1 || produced.SampleDesc.Count != 1)
                return fail("draw frame texture reference has incompatible storage");
            textureResource = found->second.Get();
            if (owner != frameTextureOwners.end() && owner->second && owner->second->resource.Get() == textureResource)
                viewSerial = owner->second->serial;
            countTexture(TextureFrame, 0);
        }
        else if (auto* stored = storeVersion ? findVersionTexture(*storeVersion) : nullptr)
        {
            useVersionTexture(storeVersion->get(), *stored);
            textureResource = stored->resource.Get();
            viewSerial      = stored->serial;
            versionTextureHits.fetch_add(1, std::memory_order_relaxed);
            replayTextureReuses.fetch_add(1, std::memory_order_relaxed);
            if (std::exchange(stored->recording, frameRecordings) == frameRecordings)
                countTexture(TextureRepeat, 0);
            else
            {
                replayTextureVersionReuses.fetch_add(1, std::memory_order_relaxed);
                countTexture(TextureMatch, 0);
            }
        }
        else if (cached && cached->recorded)
        {
            textureResource = cached->resource.Get();
            replayTextureReuses.fetch_add(1, std::memory_order_relaxed);
            countTexture(TextureRepeat, 0);
        }
        else if (cached && cached->resource && version && cached->uploadedVersion == version)
        {
            countTexture(TextureMatch, 0);
            textureResource        = cached->resource.Get();
            cached->recorded       = true;
            cached->recordedShared = true;
            replayTextureReuses.fetch_add(1, std::memory_order_relaxed);
            replayTextureVersionReuses.fetch_add(1, std::memory_order_relaxed);
        }
        else if (cached && cached->resource && [&]
                 {
                     phaseEnd(PhaseTextures);
                     const bool equal = (cached->uploadedVersion && cached->uploadedVersion->Payload()->size() == payload.size() &&
                                         std::equal(payload.begin(), payload.end(), cached->uploadedVersion->Payload()->begin())) ||
                                        (cached->uploadedBytes && cached->uploadedSize == payload.size() &&
                                         std::equal(payload.begin(), payload.end(), cached->uploadedBytes.get()));
                     phaseEnd(PhaseTextureCompare);
                     return equal;
                 }())
        {
            // Changed versions and external shared owners still require exact
            // bytes from the preceding completed resource's private snapshot.
            countTexture(TextureMatch, 0);
            textureResource        = cached->resource.Get();
            cached->recorded       = true;
            cached->recordedShared = bool(texture.sharedBytes);
            replayTextureReuses.fetch_add(1, std::memory_order_relaxed);
            replayTexturePayloadReuses.fetch_add(1, std::memory_order_relaxed);
            if (version)
                retainPayload();
        }
        else
        {
            countTexture(storeVersion                                                             ? TextureNew
                         : !cached                                                                ? TextureUncached
                         : cached->resource && !cached->uploadedVersion && !cached->uploadedBytes ? TextureUnretained
                         : cached->priorKey                                                       ? TextureChanged
                                                                                                  : TextureNew,
                         payload.size());
            // A stored version prepares its own layout, which its binding names.
            NativeDrawReplayTexture described;
            if (storeVersion)
            {
                const auto layout   = (*storeVersion)->Layout();
                described.width     = layout.width;
                described.height    = layout.height;
                described.format    = layout.format;
                described.mipLevels = layout.mipLevels;
                described.dimension = layout.dimension;
            }
            const auto preparation = cached ? cached->preparation : prepareReplayTexture(device.Get(), storeVersion ? described : texture);
            if (!cached)
                replayTexturePreparationCreations.fetch_add(1, std::memory_order_relaxed);
            const auto& footprints       = preparation->footprints;
            const auto& rows             = preparation->rows;
            const auto& rowBytes         = preparation->rowBytes;
            const auto  subresourceCount = static_cast<UINT>(footprints.size());
            const auto  totalBytes       = preparation->totalBytes;
            if (preparation->packedBytes != payload.size() || totalBytes > 128ULL * 1024 * 1024 ||
                replayTextureFootprintBytes > kMaxReplayTextureFootprintBytes - totalBytes)
            {
                return fail("texture upload footprint does not match the decoded payload");
            }
            if (version)
            {
                const auto sourceLayout = version->Subresources();
                if (sourceLayout.size() != footprints.size())
                    return fail("texture version has incompatible native subresources");
                std::size_t sourceOffset = 0;
                for (UINT subresource = 0; subresource < subresourceCount; ++subresource)
                {
                    const auto& source = sourceLayout[subresource];
                    if (source.arraySlice != subresource / desc.MipLevels || source.mipLevel != subresource % desc.MipLevels ||
                        source.sourceOffset != sourceOffset || source.rowBytes != rowBytes[subresource] || source.rowCount != rows[subresource])
                        return fail("texture version source layout does not match native footprints");
                    sourceOffset += source.rowBytes * source.rowCount;
                }
            }
            replayTextureFootprintBytes += totalBytes;
            if (cached || storeVersion)
            {
                if (totalBytes > kMaxReplayTextureFootprintBytes - frameCpuTextureFootprintBytes)
                    return fail("frame CPU texture uploads exceed the footprint bound");
                frameCpuTextureFootprintBytes += totalBytes;
            }
            // One pair per slot stays bounded by recipe and footprint admission.
            // A retained texture ended its prior completed draw in shader-read state.
            const auto previous      = replayTexture ? replayTexture->GetDesc() : D3D12_RESOURCE_DESC{};
            const bool reusedTexture = replayTexture && previous.Width == desc.Width &&
                                       previous.Dimension == desc.Dimension &&
                                       previous.Height == desc.Height &&
                                       previous.MipLevels == desc.MipLevels &&
                                       previous.DepthOrArraySize == desc.DepthOrArraySize &&
                                       previous.Format == desc.Format;
            // An idle pooled texture of the same allocation rests in shader-read
            // state, like a retained one; its charge is already in the census.
            bool pooledTexture = false;
            if (!reusedTexture)
            {
                if (cached || storeVersion)
                {
                    if (cached)
                    {
                        returnPooled(texturePool, replayTexture, cached->resourceStorageBytes);
                        cached->resourceStorageBytes = 0;
                    }
                    std::uint64_t pooledBytes = 0;
                    replayTexture             = takePooled(texturePool, desc, &pooledBytes);
                    pooledTexture             = bool(replayTexture);
                    storeBytes                = pooledBytes;
                    if (pooledTexture && cached)
                    {
                        cached->resourceStorageBytes = pooledBytes;
                        cached->resourceSerial       = ++textureResourceSerial;
                    }
                }
                else
                    replayTexture.Reset();
            }
            if (!reusedTexture && !pooledTexture)
            {
                if (!reserveStorage(StorageDomain::Texture, device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes, result.error))
                    return result;
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                hr        = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&replayTexture));
                if (FAILED(hr))
                {
                    return fail("could not create replay texture");
                }
                if (cached)
                {
                    cached->resourceStorageBytes = resourceStorageBytes(device.Get(), replayTexture.Get());
                    cached->resourceSerial       = ++textureResourceSerial;
                }
                if (storeVersion)
                    storeBytes = resourceStorageBytes(device.Get(), replayTexture.Get());
                replayTextureCreations.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                replayTextureReuses.fetch_add(1, std::memory_order_relaxed);
            }
            const auto uploadStart = DevNow();
            phaseEnd(PhaseTextures, uploadStart);
            const bool reusedUpload = replayTextureUpload && replayTextureUpload.bytes() >= totalBytes;
            bool       pooledUpload = false;
            if (!reusedUpload)
            {
                replayTextureUpload.reset();
                // Both domains charge the lease. An idle buffer is already
                // charged as Upload, so only a creation reserves Upload.
                std::string admitError;
                replayTextureUpload = uploadPool.Acquire(totalBytes, 0, [&](std::uint64_t classBytes)
                                                         {
                                                             return reserveStorage(StorageDomain::Upload, classBytes, admitError);
                                                         });
                if (!replayTextureUpload)
                {
                    if (!admitError.empty())
                    {
                        result.error = std::move(admitError);
                        return result;
                    }
                    return fail("could not create replay texture upload");
                }
                // CpuTexture is checked against the buffer actually leased: an
                // idle one may span up to twice the request's class. A frame
                // texture's lease is already in that census.
                if (!reserveStorage(StorageDomain::CpuTexture, cached || storeVersion ? 0 : replayTextureUpload.bytes(), result.error))
                {
                    replayTextureUpload.reset();
                    return result;
                }
                pooledUpload = replayTextureUpload.reused();
            }
            // Refresh the complete footprint, including padding, after this slot's fence.
            auto* uploadBytes = replayTextureUpload.buffer().mapped;
            std::memset(uploadBytes, 0, static_cast<std::size_t>(totalBytes));
            std::size_t sourceOffset = 0;
            for (UINT subresource = 0; subresource < subresourceCount; ++subresource)
            {
                for (UINT row = 0; row < rows[subresource]; ++row)
                {
                    const auto rowStart = static_cast<std::size_t>(footprints[subresource].Offset) +
                                          static_cast<std::size_t>(row) * footprints[subresource].Footprint.RowPitch;
                    std::memcpy(uploadBytes + rowStart,
                                payload.data() + sourceOffset,
                                static_cast<std::size_t>(rowBytes[subresource]));
                    sourceOffset += static_cast<std::size_t>(rowBytes[subresource]);
                }
            }
            (reusedUpload || pooledUpload ? replayTextureUploadReuses : replayTextureUploadCreations).fetch_add(1, std::memory_order_relaxed);
            const auto uploadEnd = DevNow();
#if REREVVED_DEVELOPER
            replayTextureUploadNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(uploadEnd - uploadStart).count()), std::memory_order_relaxed);
#endif
            phaseEnd(PhaseTextureUpload, uploadEnd);
            const auto             shaderReadState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = replayTexture.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (reusedTexture || pooledTexture)
            {
                barrier.Transition.StateBefore = shaderReadState;
                barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                commandList->ResourceBarrier(1, &barrier);
            }
            D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
            sourceLocation.pResource = replayTextureUpload.buffer().resource.Get();
            sourceLocation.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = replayTexture.Get();
            destination.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            for (UINT subresource = 0; subresource < subresourceCount; ++subresource)
            {
                sourceLocation.PlacedFootprint = footprints[subresource];
                destination.SubresourceIndex   = subresource;
                commandList->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
            }
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter  = shaderReadState;
            commandList->ResourceBarrier(1, &barrier);
            textureResource = replayTexture.Get();
            if (cached)
            {
                cached->recorded       = true;
                cached->recordedShared = bool(texture.sharedBytes);
                retainPayload();
            }
            else if (storeVersion)
            {
                // The lease outlives the copy with the frame; the entry is
                // staged until its batch submits. A throw fails the recording.
                frameRetiredUploadLeases.push_back(std::move(storeUpload));
                const auto* key = storeVersion->get();
                versionTexturesOpen.push_back(key);
                VersionTextures::iterator entry;
                bool                      inserted = false;
                try
                {
                    std::tie(entry, inserted) = versionTextures.try_emplace(key);
                }
                catch (...)
                {
                    versionTexturesOpen.pop_back();
                    throw;
                }
                // An open entry an expired version left at this address keeps
                // its key; this upload's storage then lives with the frame.
                if (!inserted)
                {
                    versionTexturesOpen.pop_back();
                    frameRetiredUploads.push_back(std::move(storeTexture));
                }
                else
                {
                    entry->second = VersionTexture{ *storeVersion, std::move(storeTexture), storeBytes, 0, ++versionTextureClock, frameRecordings, true, true, ++textureResourceSerial };
                    viewSerial    = entry->second.serial;
                    versionTextureBytes += storeBytes;
                }
                versionTextureUploads.fetch_add(1, std::memory_order_relaxed);
                trimVersionTextures(versionTextureCap);
                noteVersionTextureBytes();
            }
        }

        // Zeroed whole so a repeated binding compares equal bytewise.
        D3D12_SHADER_RESOURCE_VIEW_DESC srv;
        std::memset(&srv, 0, sizeof(srv));
        srv.Format                        = desc.Format;
        srv.Texture2D.MipLevels           = desc.MipLevels;
        srv.Texture2D.ResourceMinLODClamp = 0.0F;
        const auto& swizzle               = binding ? binding->swizzle : texture.swizzle;
        srv.Shader4ComponentMapping       = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
            swizzle[0], swizzle[1], swizzle[2], swizzle[3]);
        if (desc.DepthOrArraySize == 6)
        {
            srv.ViewDimension                      = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MostDetailedMip     = 0;
            srv.Texture2DArray.MipLevels           = desc.MipLevels;
            srv.Texture2DArray.FirstArraySlice     = 0;
            srv.Texture2DArray.ArraySize           = 6;
            srv.Texture2DArray.PlaneSlice          = 0;
            srv.Texture2DArray.ResourceMinLODClamp = 0.0F;
        }
        else
        {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        }
        // Cached, stored and owned frame textures carry serials; other
        // resources rewrite their views each draw.
        auto&      written = draw.textureDescriptors[slot];
        const auto serial  = cached ? cached->resourceSerial : viewSerial;
        if (serial && written.serial == serial && std::memcmp(&written.srv, &srv, sizeof(srv)) == 0)
        {
            replayDescriptorReuses.fetch_add(1, std::memory_order_relaxed);
#if REREVVED_DEVELOPER
            ++textureViewReuses;
#endif
            continue;
        }
#if REREVVED_DEVELOPER
        textureSrvWrites += 2;
#endif
        written.serial     = serial;
        written.srv        = srv;
        auto textureHandle = draw.viewCpu;
        textureHandle.ptr += static_cast<SIZE_T>(3 + slot) * replaySrvStride;
        device->CreateShaderResourceView(textureResource, &srv, textureHandle);

        auto cubeHandle = draw.viewCpu;
        cubeHandle.ptr += static_cast<SIZE_T>(3 + textureCount + slot) * replaySrvStride;
        D3D12_SHADER_RESOURCE_VIEW_DESC cubeSrv = srv;
        cubeSrv.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURECUBE;
        cubeSrv.TextureCube.MostDetailedMip     = 0;
        cubeSrv.TextureCube.MipLevels           = desc.MipLevels;
        cubeSrv.TextureCube.ResourceMinLODClamp = 0.0F;
        if (desc.DepthOrArraySize == 6)
        {
            device->CreateShaderResourceView(textureResource, &cubeSrv, cubeHandle);
        }
        else
        {
            // A null cube SRV keeps the space-2 range bounded and valid for
            // 2D recipes; the shader never samples that unused dimension.
            device->CreateShaderResourceView(nullptr, &cubeSrv, cubeHandle);
        }
    }
    phaseEnd(PhaseTextures);
    std::array<D3D12_SAMPLER_DESC, kNativeDrawReplayTextureSlots> samplers;
    for (UINT slot = 0; slot < textureCount; ++slot)
    {
        const auto& state   = admission ? admission->TextureBindings()[slot].sampler : recipe.samplers.empty() ? recipe.sampler
                                                                                                               : recipe.samplers[slot];
        auto&       sampler = samplers[slot];
        // Zeroed whole so equal tables compare equal bytewise.
        std::memset(&sampler, 0, sizeof(sampler));
        if (state.anisotropy != 0)
        {
            sampler.Filter        = D3D12_FILTER_ANISOTROPIC;
            sampler.MaxAnisotropy = 1U << (state.anisotropy - 1);
        }
        else
        {
            sampler.Filter = D3D12_ENCODE_BASIC_FILTER(
                state.minLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                state.magLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                state.mipLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
                D3D12_FILTER_REDUCTION_TYPE_STANDARD);
            sampler.MaxAnisotropy = 1;
        }
        sampler.AddressU       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(state.address[0]);
        sampler.AddressV       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(state.address[1]);
        sampler.AddressW       = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(state.address[2]);
        sampler.MipLODBias     = state.mipBias;
        sampler.MinLOD         = state.minLod;
        sampler.MaxLOD         = state.maxLod;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        std::copy(state.border.begin(), state.border.end(), sampler.BorderColor);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE samplerTable{};
    if (frameBatch && textureCount && !frameSamplerTable(*frameBatch, samplers.data(), textureCount, samplerTable))
        return fail("native frame sampler tables exceed their owner heap");
    if (!frameBatch && textureCount)
    {
        const auto samplerStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        for (UINT slot = 0; slot < textureCount; ++slot)
        {
            const auto bit = std::uint64_t(1) << slot;
            if ((draw.samplerDescriptorMask & bit) && std::memcmp(&draw.samplerDescriptors[slot], &samplers[slot], sizeof(samplers[slot])) == 0)
            {
                replayDescriptorReuses.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            draw.samplerDescriptors[slot] = samplers[slot];
            draw.samplerDescriptorMask |= bit;
            auto handle = draw.samplerHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(slot) * samplerStride;
            device->CreateSampler(&samplers[slot], handle);
        }
        samplerTable = draw.samplerHeap->GetGPUDescriptorHandleForHeapStart();
    }

    // Upload extents stay aligned through last use; arena or slot rewrites wait
    // for their fence.
#if REREVVED_DEVELOPER
    const auto countDrawUpload = [&](std::size_t inputSlot, std::size_t bytes)
    {
        drawUploadBytes[inputSlot < 3 ? 0 : inputSlot - 2] += bytes;
    };
#else
    const auto countDrawUpload = [](std::size_t, std::size_t)
    {
    };
#endif
    const auto updateDrawUpload = [&](std::size_t size, const auto& write, ComPtr<ID3D12Resource>& resource, std::size_t inputSlot)
    {
        if (frameDraw)
        {
            const auto capacity = (size + 255U) & ~std::size_t(255U);
            const auto limit    = frameUploadArena ? frameUploadArena.bytes() : 0;
            if (!frameUploadMapped || size > capacity || frameUploadCursor > limit ||
                capacity > limit - frameUploadCursor)
                return false;
            auto* mapped = frameUploadMapped + frameUploadCursor;
            write(mapped);
            // Geometry views expose only current bytes; root CBVs have no size.
            if (inputSlot < 3)
                std::memset(mapped + size, 0, capacity - size);
            countDrawUpload(inputSlot, inputSlot < 3 ? capacity : size);
            draw.uploadAddresses[inputSlot] = frameUploadArena.buffer().gpu + frameUploadCursor;
            frameUploadCursor += capacity;
            return true;
        }
        const bool reused = resource && resource->GetDesc().Width >= size;
        if (!reused)
        {
            resource.Reset();
            if (!reserveStorage(StorageDomain::Upload, size, result.error) || !createReplayUploadBuffer(device.Get(), size, resource))
                return false;
        }
        const auto        capacity = static_cast<std::size_t>(resource->GetDesc().Width);
        const D3D12_RANGE noRead{ 0, 0 };
        void*             mapped = nullptr;
        if (FAILED(resource->Map(0, &noRead, &mapped)))
            return false;
        write(static_cast<std::uint8_t*>(mapped));
        // Root CBVs have no view size. Clear retained tail bytes when shrinking
        // so a smaller recipe cannot observe a previous draw's constants.
        if (inputSlot < 3)
            std::memset(static_cast<std::uint8_t*>(mapped) + size, 0, capacity - size);
        countDrawUpload(inputSlot, inputSlot < 3 ? capacity : size);
        const D3D12_RANGE written{ 0, capacity };
        resource->Unmap(0, &written);
        draw.uploadAddresses[inputSlot] = resource->GetGPUVirtualAddress();
        (reused ? replayDrawUploadReuses : replayDrawUploadCreations).fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    phaseEnd(PhaseSamplers);
#if REREVVED_DEVELOPER
    const auto drawUploadStart = std::chrono::steady_clock::now();
#endif
    const auto makeConstantBuffer = [&](const std::vector<std::uint8_t>& bytes,
                                        ComPtr<ID3D12Resource>&          resource,
                                        std::size_t                      inputSlot)
    {
        const std::uint32_t size = alignConstantBytes(bytes.size());
        // A frame draw whose block equals the previous frame draw's, byte for
        // byte, binds that upload; its zeroed tail matches for an equal size.
        auto& uploaded = uploadedConstants[inputSlot];
        if (frameDraw && uploaded.source && uploaded.epoch == frameUploadEpoch && uploaded.source->size() == bytes.size() &&
            (bytes.empty() || std::memcmp(uploaded.source->data(), bytes.data(), bytes.size()) == 0))
        {
            draw.uploadAddresses[inputSlot] = uploaded.address;
            uploaded.source                 = &bytes;
            return true;
        }
        if (!updateDrawUpload(size, [&](std::uint8_t* mapped)
                              {
                                  std::memcpy(mapped, bytes.data(), bytes.size());
                                  std::memset(mapped + bytes.size(), 0, size - bytes.size());
                              },
                              resource,
                              inputSlot))
            return false;
        if (frameDraw)
            uploaded = { &bytes, draw.uploadAddresses[inputSlot], frameUploadEpoch };
        return true;
    };
#if REREVVED_DEVELOPER
    bool residentGeometry = false;
    if (phaseTiming && frameDraw && recipe.sharedVertexData)
    {
        const auto& owner = recipe.sharedVertexData;
        // Erasing never rehashes, and a quarter of the table frees each time
        // it fills, so an insert stays within the reserved buckets.
        if (uploadedVertexOwners.size() >= kUploadedVertexOwners && !uploadedVertexOwners.contains(owner.get()))
        {
            std::erase_if(uploadedVertexOwners, [](const auto& seen)
                          {
                              return seen.second.expired();
                          });
            for (auto seen = uploadedVertexOwners.begin(); uploadedVertexOwners.size() > kUploadedVertexOwners * 3 / 4;)
                seen = uploadedVertexOwners.erase(seen);
        }
        auto [entry, inserted] = uploadedVertexOwners.try_emplace(owner.get(), owner);
        const bool sameOwner   = !inserted && !entry->second.owner_before(owner) && !owner.owner_before(entry->second);
        residentGeometry       = sameOwner;
        if (!inserted && !sameOwner)
            entry->second = owner;
    }
#endif
    if (!makeConstantBuffer(recipe.vertexConstants, draw.vertexConstants, 0) ||
        !makeConstantBuffer(recipe.pixelConstants, draw.pixelConstants, 1) ||
        !makeConstantBuffer(recipe.sharedConstants, draw.sharedConstants, 2))
    {
        return fail("could not update replay stage constant buffers");
    }
#if REREVVED_DEVELOPER
    if (phaseTiming && frameDraw && frameBatch)
    {
        const std::array<const std::vector<std::uint8_t>*, 3> blocks{ &recipe.vertexConstants, &recipe.pixelConstants, &recipe.sharedConstants };
        for (std::size_t stage = 0; stage < blocks.size(); ++stage)
        {
            // Salted by stage and kept nonzero, the table's empty mark.
            const auto& bytes = *blocks[stage];
            const auto  hash  = (XXH3_64bits(bytes.data(), bytes.size()) ^ ((stage + 1) * 0x9E3779B97F4A7C15ULL)) | 1;
            const auto  size  = alignConstantBytes(bytes.size());
            if (chunkPreviousConstants[stage] == hash)
                repeatPreviousBytes[stage] += size;
            if (noteChunkConstant(hash))
                repeatChunkBytes[stage] += size;
            chunkPreviousConstants[stage] = hash;
        }
    }
#endif

    // The draw's residency outcome publishes with its recording, but the
    // table's own events are real even when the draw fails afterwards.
    struct ResidencyDraw
    {
        Impl&         renderer;
        bool          hit = false, miss = false, copied = false, published = false;
        std::uint64_t bytes    = 0;
        int           fallback = -1;

        ~ResidencyDraw()
        {
#if REREVVED_DEVELOPER
            if (copied && !published)
                renderer.residentUploadFailures.fetch_add(1, std::memory_order_relaxed);
#endif
            if (renderer.vertexResidency)
                renderer.publishVertexResidency();
        }
    } residency{ *this };

    vertex_residency::Decision residencyPlan;
    const auto                 vertexBatch = frameBatch ? static_cast<std::uint32_t>(frameBatch - frameCommandBatches.data()) : 0;
    if (vertexResidency)
    {
        vertex_residency::Request request;
        request.owner      = recipe.sharedVertexData ? &recipe.sharedVertexData : nullptr;
        request.length     = recipe.vertexBytes().size();
        request.generation = residencyGeneration;
        request.batch      = vertexBatch;
        request.frameDraw  = frameDraw != nullptr;
        request.frameBatch = frameBatch != nullptr;
        residencyPlan      = vertexResidency->Plan(request, [this]
                                                   {
                                                  return residencyCompleted();
                                                   });
        if (residencyPlan.kind != vertex_residency::Decision::Kind::Fallback && residencySchedulingFault.exchange(0, std::memory_order_relaxed))
        {
            vertexResidency->Abandon(residencyPlan);
            residencyPlan        = {};
            residencyPlan.kind   = vertex_residency::Decision::Kind::Fallback;
            residencyPlan.reason = vertex_residency::Reason::Scheduling;
        }
        if (residencyPlan.kind == vertex_residency::Decision::Kind::Hit)
        {
            draw.uploadAddresses[3] = residencyChunkAddresses[residencyPlan.where.chunk] + residencyPlan.where.offset;
            residency.hit           = true;
            residency.bytes         = residencyPlan.length;
            if (residencyThrowFault.exchange(0, std::memory_order_relaxed))
                throw std::runtime_error("native vertex residency test fault");
        }
        else if (residencyPlan.kind == vertex_residency::Decision::Kind::Fallback)
            residency.fallback = static_cast<int>(residencyPlan.reason);
    }
    if (!residency.hit)
    {
        // The staging write is the ordinary upload; a miss then copies it.
        const auto stagingOffset   = frameUploadCursor;
        auto*      stagingResource = frameUploadArena ? frameUploadArena.buffer().resource.Get() : nullptr;
        const bool staged          = !(residencyPlan.kind == vertex_residency::Decision::Kind::Miss && residencyStagingFault.exchange(0, std::memory_order_relaxed)) &&
                                     updateDrawUpload(recipe.vertexBytes().size(), [&](std::uint8_t* mapped)
                                                      {
                                                 std::memcpy(mapped, recipe.vertexBytes().data(), recipe.vertexBytes().size());
                                                      },
                                                      draw.vertexBuffer,
                                                      3);
        if (!staged)
        {
            if (residencyPlan.kind == vertex_residency::Decision::Kind::Miss)
            {
                vertexResidency->Abandon(residencyPlan);
#if REREVVED_DEVELOPER
                residentUploadFailures.fetch_add(1, std::memory_order_relaxed);
#endif
            }
            return fail("could not update replay vertex buffer");
        }
        if (residencyPlan.kind == vertex_residency::Decision::Kind::Miss)
        {
            ID3D12Resource* chunk      = residencyChunks[residencyPlan.where.chunk].Get();
            const auto      transition = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, bool postCopy = false)
            {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = { chunk, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
                commandList->ResourceBarrier(1, &barrier);
                if (postCopy)
                    residencyBarriers.fetch_add(1, std::memory_order_relaxed);
            };
            // COMMON promotes to a copy destination; a chunk this list already
            // read or wrote must transition first.
            if (vertexResidency->NeedsPreCopyBarrier(vertexBatch, residencyPlan.where.chunk))
            {
                transition(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
                residencyPreBarriers.fetch_add(1, std::memory_order_relaxed);
            }
            commandList->CopyBufferRegion(chunk, residencyPlan.where.offset, stagingResource, stagingOffset, residencyPlan.length);
            transition(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, true);
            residency.copied = true;
            if (vertexResidency->Commit(residencyPlan, vertexBatch))
            {
                residencyAdmissions.fetch_add(1, std::memory_order_relaxed);
                draw.uploadAddresses[3] = residencyChunkAddresses[residencyPlan.where.chunk] + residencyPlan.where.offset;
                residency.miss          = true;
                residency.bytes         = residencyPlan.length;
            }
        }
    }
    const auto indexByteCount = recipe.indexSpan().size() * sizeof(std::uint32_t);
    if (!updateDrawUpload(indexByteCount, [&](std::uint8_t* mapped)
                          {
                              if constexpr (std::endian::native == std::endian::little)
                                  std::memcpy(mapped, recipe.indexSpan().data(), indexByteCount);
                              else
                                  for (std::size_t index = 0; index < recipe.indexSpan().size(); ++index)
                                  {
                                      const std::uint32_t value = recipe.indexSpan()[index];
                                      mapped[index * 4 + 0]     = static_cast<std::uint8_t>(value);
                                      mapped[index * 4 + 1]     = static_cast<std::uint8_t>(value >> 8);
                                      mapped[index * 4 + 2]     = static_cast<std::uint8_t>(value >> 16);
                                      mapped[index * 4 + 3]     = static_cast<std::uint8_t>(value >> 24);
                                  }
                          },
                          draw.indexBuffer,
                          4))
    {
        return fail("could not update replay index buffer");
    }

#if REREVVED_DEVELOPER
    if (residentGeometry)
        residentBytes = { recipe.vertexBytes().size(), indexByteCount };
    replayDrawUploadNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - drawUploadStart).count()), std::memory_order_relaxed);
#endif
    phaseEnd(PhaseDrawUpload);

    struct CopyConstants
    {
        std::uint32_t sampleA;
        std::uint32_t sampleB;
        std::uint32_t sampleCount;
        std::uint32_t padding;
    };

    const CopyConstants           initConstants{ 0, 0, 0, 0 };
    const CopyConstants           resolveConstants{ 0, 0, 0, 0 };
    const CopyConstants           resolveSample3Constants{ 3, 3, 0, 0 };
    std::array<std::uint8_t, 256> initConstantBytes{};
    std::array<std::uint8_t, 256> resolveConstantBytes{};
    std::array<std::uint8_t, 256> resolveSample3ConstantBytes{};
    std::memcpy(initConstantBytes.data(), &initConstants, sizeof(initConstants));
    std::memcpy(resolveConstantBytes.data(), &resolveConstants, sizeof(resolveConstants));
    std::memcpy(resolveSample3ConstantBytes.data(),
                &resolveSample3Constants,
                sizeof(resolveSample3Constants));
    if (!replayCopyConstants)
    {
        if (!reserveStorage(StorageDomain::Upload, initConstantBytes.size(), result.error) || !createReplayUploadBuffer(device.Get(),
                                                                                                                        initConstantBytes.data(),
                                                                                                                        initConstantBytes.size(),
                                                                                                                        replayCopyConstants))
        {
            return fail("could not create replay copy constants");
        }
        ++replayImmutableConstantCreations;
    }
    if (readback && !replayResolveConstants)
    {
        if (!reserveStorage(StorageDomain::Upload, resolveConstantBytes.size(), result.error) || !createReplayUploadBuffer(device.Get(),
                                                                                                                           resolveConstantBytes.data(),
                                                                                                                           resolveConstantBytes.size(),
                                                                                                                           replayResolveConstants))
        {
            return fail("could not create replay resolve constants");
        }
        ++replayImmutableConstantCreations;
    }
    if (readback && !replayResolveConstantsSample3)
    {
        if (!reserveStorage(StorageDomain::Upload, resolveSample3ConstantBytes.size(), result.error) || !createReplayUploadBuffer(device.Get(),
                                                                                                                                  resolveSample3ConstantBytes.data(),
                                                                                                                                  resolveSample3ConstantBytes.size(),
                                                                                                                                  replayResolveConstantsSample3))
        {
            return fail("could not create replay sample-3 constants");
        }
        ++replayImmutableConstantCreations;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvStart =
        replayRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (std::size_t index = 0; index < replayColorTargets.size(); ++index)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvStart;
        handle.ptr += static_cast<SIZE_T>(index) * replayRtvStride;
        device->CreateRenderTargetView(replayColorTargets[index].Get(), nullptr, handle);
    }
#if REREVVED_DEVELOPER
    drawRtvWrites += replayColorTargets.size();
#endif
    D3D12_CPU_DESCRIPTOR_HANDLE resolvedRtv = rtvStart;
    resolvedRtv.ptr += static_cast<SIZE_T>(4) * replayRtvStride;
    if (readback)
    {
        device->CreateRenderTargetView(replayResolvedTarget.Get(), nullptr, resolvedRtv);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC initialSrv{};
    initialSrv.Format                  = colorFormat;
    initialSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    initialSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    initialSrv.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE srvStart =
        draw.viewCpu;
    // Helper views are written only for the helper passes that bind them.
    if (initializeFromSamples)
    {
        device->CreateShaderResourceView(replayInitialSample0.Get(), &initialSrv, srvStart);
        D3D12_CPU_DESCRIPTOR_HANDLE sample1Srv = srvStart;
        sample1Srv.ptr += static_cast<SIZE_T>(replaySrvStride);
        device->CreateShaderResourceView(replayInitialSample1.Get(), &initialSrv, sample1Srv);
    }
    if (readback || gamma)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC msaaSrv{};
        msaaSrv.Format        = colorFormat;
        msaaSrv.ViewDimension = recipe.sampleCount == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS;
        if (recipe.sampleCount == 1)
            msaaSrv.Texture2D.MipLevels = 1;
        msaaSrv.Shader4ComponentMapping       = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        D3D12_CPU_DESCRIPTOR_HANDLE targetSrv = srvStart;
        targetSrv.ptr += static_cast<SIZE_T>(2) * replaySrvStride;
        device->CreateShaderResourceView(replayColorTargets[0].Get(), &msaaSrv, targetSrv);
    }

    const auto transitionTexture = [&](ID3D12Resource*       resource,
                                       D3D12_RESOURCE_STATES before,
                                       D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter  = after;
        commandList->ResourceBarrier(1, &barrier);
    };
    if (initializeFromSamples)
    {
        transitionTexture(replayInitialSample0.Get(),
                          D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transitionTexture(replayInitialSample1.Get(),
                          D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    D3D12_VIEWPORT fullTargetViewport{ 0.0F,
                                       0.0F,
                                       static_cast<float>(recipe.width),
                                       static_cast<float>(recipe.height),
                                       0.0F,
                                       1.0F };
    D3D12_RECT     fullTargetScissor{ 0,
                                      0,
                                      static_cast<LONG>(recipe.width),
                                      static_cast<LONG>(recipe.height) };
    D3D12_VIEWPORT replayViewport{ recipe.viewport.x,
                                   recipe.viewport.y,
                                   recipe.viewport.width,
                                   recipe.viewport.height,
                                   recipe.viewport.minDepth,
                                   recipe.viewport.maxDepth };
    D3D12_RECT     replayScissor{ recipe.scissor.left,
                                  recipe.scissor.top,
                                  recipe.scissor.right,
                                  recipe.scissor.bottom };
    commandList->RSSetViewports(1, &fullTargetViewport);
    commandList->RSSetScissorRects(1, &fullTargetScissor);
    commandList->SetGraphicsRootSignature(replayCopyRootSignature.Get());
    bindDrawHeaps(commandList.Get(), draw);
    D3D12_CPU_DESCRIPTOR_HANDLE targetRtvs[4];
    for (std::size_t index = 0; index < 4; ++index)
    {
        targetRtvs[index] = rtvStart;
        targetRtvs[index].ptr += static_cast<SIZE_T>(index) * replayRtvStride;
        if (!preserveTargets)
            commandList->ClearRenderTargetView(targetRtvs[index], clearValue.Color, 0, nullptr);
    }
    if (initializeFromSamples)
    {
        commandList->SetPipelineState(helper->copy.Get());
        commandList->SetGraphicsRootDescriptorTable(0, draw.viewGpu);
        commandList->SetGraphicsRootConstantBufferView(1, replayCopyConstants->GetGPUVirtualAddress());
        commandList->OMSetRenderTargets(4, targetRtvs, FALSE, nullptr);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->DrawInstanced(3, 1, 0, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE sample1Gpu = draw.viewGpu;
        sample1Gpu.ptr += static_cast<UINT64>(replaySrvStride);
        commandList->SetPipelineState(helper->copySample3.Get());
        commandList->SetGraphicsRootDescriptorTable(0, sample1Gpu);
        commandList->DrawInstanced(3, 1, 0, 0);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
    if (recipe.depth.enabled || recipe.stencil.enabled)
    {
        const bool initializeDepth = !replayDepthInitialized;
        if (initializeDepth)
        {
            if (!replayDsvHeap)
            {
                D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
                heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
                heapDesc.NumDescriptors = 1;
                hr                      = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&replayDsvHeap));
                if (FAILED(hr))
                    return fail("could not create replay DSV heap");
                ++replayDescriptorHeapCreations;
            }
            D3D12_RESOURCE_DESC desc = replayColorTargets[0]->GetDesc();
            desc.Format              = DXGI_FORMAT_R24G8_TYPELESS;
            desc.Flags               = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            if (frameAttachmentCaching)
                activateFrameDepthAttachment(frameKey({ recipe.width, recipe.height, recipe.sampleCount, desc.Format }));
            if (replayDepthTarget)
            {
                const auto previous = replayDepthTarget->GetDesc();
                if (previous.Width != desc.Width || previous.Height != desc.Height ||
                    previous.Format != desc.Format || previous.SampleDesc.Count != desc.SampleDesc.Count)
                {
                    returnPooled(attachmentPool, replayDepthTarget);
                }
            }
            if (!replayDepthTarget)
                replayDepthTarget = takePooled(attachmentPool, desc);
            if (replayDepthTarget)
            {
                ++replayDepthTargetReuses;
            }
            else
            {
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (!reserveStorage(StorageDomain::Attachment, device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes, result.error))
                    return result;
                const auto started = DevNow();
                hr                 = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, nullptr, IID_PPV_ARGS(&replayDepthTarget));
                countAttachmentCreate(started);
                if (FAILED(hr))
                {
                    return fail("could not create replay D24 depth target");
                }
                ++replayDepthTargetCreations;
            }
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dsv.ViewDimension = recipe.sampleCount == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DMS;
            depthDsv          = replayDsvHeap->GetCPUDescriptorHandleForHeapStart();
            device->CreateDepthStencilView(replayDepthTarget.Get(), &dsv, depthDsv);
            replayDsvResource = replayDepthTarget.Get();
            commandList->ClearDepthStencilView(depthDsv,
                                               D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                               recipe.depth.initialClear,
                                               0,
                                               0,
                                               nullptr);
            replayDepthInitialized = true;
        }
        else if (replayDsvResource != replayDepthTarget.Get())
        {
            // A resume activates a preserved attachment without initializing
            // it, so the shared view must name the active depth again.
            if (!replayDsvHeap || !replayDepthTarget)
                return fail("replay depth is initialized without a depth target");
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
            dsv.ViewDimension = recipe.sampleCount == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DMS;
            device->CreateDepthStencilView(replayDepthTarget.Get(), &dsv, replayDsvHeap->GetCPUDescriptorHandleForHeapStart());
            replayDsvResource = replayDepthTarget.Get();
        }
        depthDsv = replayDsvHeap->GetCPUDescriptorHandleForHeapStart();
        if (readback && recipe.depth.enabled)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv{};
            depthSrv.Format        = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            depthSrv.ViewDimension = recipe.sampleCount == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS;
            if (recipe.sampleCount == 1)
                depthSrv.Texture2D.MipLevels = 1;
            depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            auto handle                      = srvStart;
            handle.ptr += static_cast<SIZE_T>(depthSrvBase + 2) * replaySrvStride;
            device->CreateShaderResourceView(replayDepthTarget.Get(), &depthSrv, handle);
        }

        if (initializeDepth && !recipe.depth.initialSamples.empty())
        {
            if (!helper->depthCopy[0] || !helper->depthCopy[1])
                return fail("could not create replay depth initialization pipeline");
            commandList->OMSetRenderTargets(0, nullptr, FALSE, &depthDsv);
            for (std::size_t plane = 0; plane < 2; ++plane)
            {
                const auto                      first = recipe.depth.initialSamples.begin() + plane * outputPlaneBytes;
                const std::vector<std::uint8_t> bytes(first, first + outputPlaneBytes);
                if (!makeRowUpload(bytes, replayInitialDepth[plane], replayInitialDepthUpload[plane]))
                    return fail("could not upload replay initial depth samples");
                transitionTexture(replayInitialDepth[plane].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                auto handle = srvStart;
                handle.ptr += (depthSrvBase + plane) * replaySrvStride;
                device->CreateShaderResourceView(replayInitialDepth[plane].Get(), &initialSrv, handle);
                auto gpuHandle = draw.viewGpu;
                gpuHandle.ptr += (depthSrvBase + plane) * replaySrvStride;
                commandList->SetGraphicsRootDescriptorTable(0, gpuHandle);
                commandList->SetPipelineState(helper->depthCopy[plane].Get());
                commandList->DrawInstanced(3, 1, 0, 0);
            }
        }
    }

    phaseEnd(PhaseViews);
    if (!ensureReplayDrawRootSignature(textureCount, helperError))
    {
        return fail(std::move(helperError));
    }

    const auto*               preparedState = admission && recipe.shaderVersion ? &admission->PipelineStateKey() : nullptr;
    std::vector<std::uint8_t> guestPipelineKey;
    if (!preparedState)
    {
        const auto keyCapacity = replayGuestPipelineKeyCapacity(recipe);
        // The packet keeps its peak temporary key charged even after a miss
        // moves a key into the cache. Synthetic helpers may exceed that peak.
        const bool reservedKey = frameDraw && recordingFrameLease && keyCapacity <= ownedRecordingLegacyKeyCapacity;
        if (!reservedKey && !reserveStorage(StorageDomain::Owned, keyCapacity, result.error))
            return result;
    }
    std::vector<std::uint8_t> guestPipelineState;
    if (!preparedState)
        appendReplayGuestPipelineState(guestPipelineState, recipe, colorFormat, textureCount);
    ID3D12PipelineState* guestPipeline  = nullptr;
    auto&                guestPipelines = frameDraw ? frameGuestPipelineCache : replayGuestPipelineCache;
    for (auto& entry : guestPipelines)
    {
        bool matches = false;
        if (preparedState)
        {
            if (entry.shaderVersion && entry.state == *preparedState)
            {
                const auto& vertex       = recipe.shaderVersion->VertexBytecode();
                const auto& pixel        = recipe.shaderVersion->PixelBytecode();
                const auto& cachedVertex = entry.shaderVersion->VertexBytecode();
                const auto& cachedPixel  = entry.shaderVersion->PixelBytecode();
                matches                  = (vertex == cachedVertex && pixel == cachedPixel) ||
                                           (*vertex == *cachedVertex && *pixel == *cachedPixel);
            }
        }
        else
            matches = !entry.shaderVersion && replayGuestPipelineKeyMatches(entry.key, recipe.vertexShaderBytes(), recipe.pixelShaderBytes(), guestPipelineState);
        if (matches)
        {
            guestPipeline       = entry.pipeline.Get();
            entry.lastUsed      = frameRecordings;
            entry.lastUsedFence = nextFenceValue;
            break;
        }
    }
    if (!guestPipeline)
    {
        if (!preparedState)
            guestPipelineKey = makeReplayGuestPipelineKey(recipe, colorFormat, textureCount);
        std::uint64_t ownerGrowth = 0;
        if (recipe.shaderVersion && !(recordingRecipeCharged && recordingFrameLease) && !directStream)
        {
            if (!ensureOwnerHeap(detail::ForecastOwnerSet(ownedScanCeiling).peak, result.error))
                return result;
            detail::OwnerSet owners(detail::OwnedAllocator<const void*>{ &ownedHeap });
            retainedStorageBytes(StorageDomain::Owned, &owners);
            detail::OwnedCollector collector{ owners, nullptr, ownedScanCeiling };
            collector(recipe.shaderVersion.get(), sizeof(NativeDrawReplayShaderVersion));
            for (const auto& bytecode : { recipe.shaderVersion->VertexBytecode(), recipe.shaderVersion->PixelBytecode() })
                collector(bytecode.get(), sizeof(*bytecode) + bytecode->capacity());
            ownerGrowth = collector.bytes;
        }
        const auto growth = guestPipelines.size() == guestPipelines.capacity()
                                ? (guestPipelines.size() + 1) * sizeof(ReplayGuestPipelineCacheEntry)
                                : 0;
        if (!reserveStorage(StorageDomain::Owned, detail::OwnedAdd(ownerGrowth, detail::OwnedAdd(guestPipelineKey.capacity(), growth)), result.error))
            return result;
        if (guestPipelines.size() == guestPipelines.capacity())
            guestPipelines.reserve(guestPipelines.size() + 1);
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements(recipe.vertexAttributeCount);
        for (std::uint32_t attribute = 0; attribute < recipe.vertexAttributeCount; ++attribute)
        {
            inputElements[attribute] = { "TEXCOORD", attribute, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, attribute * 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
        }
        D3D12_BLEND_DESC drawBlend{};
        drawBlend.AlphaToCoverageEnable  = recipe.blend.alphaToCoverage ? TRUE : FALSE;
        drawBlend.IndependentBlendEnable = recipe.extraTargetCount ? TRUE : FALSE;
        // A single-target draw repeats RT0's blend; an MRT draw blends RT1..
        // from its extras and leaves the scratch slots unwritten.
        NativeDrawReplayBlendState unusedBlend;
        unusedBlend.writeMask = 0;
        for (std::size_t index = 0; index < std::size(drawBlend.RenderTarget); ++index)
        {
            const auto& blend                 = !recipe.extraTargetCount || index == 0 ? recipe.blend
                                                : index <= recipe.extraTargetCount     ? recipe.extraTargets[index - 1].blend
                                                                                       : unusedBlend;
            auto&       targetBlend           = drawBlend.RenderTarget[index];
            targetBlend.BlendEnable           = blend.enabled ? TRUE : FALSE;
            targetBlend.SrcBlend              = mapBlendFactor(blend.sourceColor);
            targetBlend.DestBlend             = mapBlendFactor(blend.destinationColor);
            targetBlend.BlendOp               = mapBlendOp(blend.colorOp);
            targetBlend.SrcBlendAlpha         = mapBlendFactor(blend.sourceAlpha);
            targetBlend.DestBlendAlpha        = mapBlendFactor(blend.destinationAlpha);
            targetBlend.BlendOpAlpha          = mapBlendOp(blend.alphaOp);
            targetBlend.RenderTargetWriteMask = blend.writeMask;
        }
        D3D12_RASTERIZER_DESC drawRaster{};
        drawRaster.FillMode              = D3D12_FILL_MODE_SOLID;
        drawRaster.CullMode              = static_cast<D3D12_CULL_MODE>(recipe.rasterizer.cull + 1);
        drawRaster.FrontCounterClockwise = recipe.rasterizer.frontCounterClockwise;
        drawRaster.DepthClipEnable       = recipe.rasterizer.depthClip ? TRUE : FALSE;
        drawRaster.MultisampleEnable     = TRUE;
        D3D12_DEPTH_STENCIL_DESC drawDepth{};
        drawDepth.DepthEnable      = recipe.depth.enabled;
        drawDepth.DepthWriteMask   = recipe.depth.writeEnabled ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        drawDepth.DepthFunc        = static_cast<D3D12_COMPARISON_FUNC>(recipe.depth.compare);
        drawDepth.StencilEnable    = recipe.stencil.enabled;
        drawDepth.StencilReadMask  = recipe.stencil.readMask;
        drawDepth.StencilWriteMask = recipe.stencil.writeMask;
        const auto stencilFace     = [](const NativeDrawReplayStencilState::Face& face)
        {
            return D3D12_DEPTH_STENCILOP_DESC{ static_cast<D3D12_STENCIL_OP>(face.fail),
                                               static_cast<D3D12_STENCIL_OP>(face.depthFail),
                                               static_cast<D3D12_STENCIL_OP>(face.pass),
                                               static_cast<D3D12_COMPARISON_FUNC>(face.compare) };
        };
        drawDepth.FrontFace = stencilFace(recipe.stencil.front);
        drawDepth.BackFace  = stencilFace(recipe.stencil.back);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC drawPso{};
        drawPso.pRootSignature = replayDrawRootSignatures[textureCount].Get();
        drawPso.VS             = { recipe.vertexShaderBytes().data(), recipe.vertexShaderBytes().size() };
        // A depth-only recipe binds no pixel shader; its color writes are masked.
        if (!recipe.depthOnly)
            drawPso.PS = { recipe.pixelShaderBytes().data(), recipe.pixelShaderBytes().size() };
        drawPso.BlendState            = drawBlend;
        drawPso.SampleMask            = NativeRasterSampleMask(recipe.schemaVersion, recipe.sampleCount, recipe.sampleMask);
        drawPso.RasterizerState       = drawRaster;
        drawPso.DepthStencilState     = drawDepth;
        drawPso.DSVFormat             = recipe.depth.enabled || recipe.stencil.enabled ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_UNKNOWN;
        drawPso.InputLayout           = { inputElements.data(), static_cast<UINT>(inputElements.size()) };
        drawPso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        drawPso.NumRenderTargets      = 4;
        drawPso.SampleDesc            = { recipe.sampleCount, 0 };
        for (auto& format : drawPso.RTVFormats)
        {
            format = DXGI_FORMAT_UNKNOWN;
        }
        for (std::uint32_t index = 0; index < 4; ++index)
        {
            drawPso.RTVFormats[index] = mapTargetFormat(NativeRenderFrameSlotFormat(drawTarget, index));
        }
        if (replayInfoQueue)
        {
            replayInfoQueue->ClearStoredMessages();
        }
        ComPtr<ID3D12PipelineState> pipeline;
#if REREVVED_DEVELOPER
        if (replayPipelineTiming.load(std::memory_order_relaxed))
        {
            const auto pipelineStart = std::chrono::steady_clock::now();
            hr                       = device->CreateGraphicsPipelineState(&drawPso, IID_PPV_ARGS(&pipeline));
            replayPipelineNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pipelineStart).count()), std::memory_order_relaxed);
        }
        else
#endif
            hr = device->CreateGraphicsPipelineState(&drawPso, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr))
        {
            REXLOG_ERROR("Native D3D12 replay guest-DXIL pipeline creation failed: "
                         "HRESULT 0x{:08X}",
                         static_cast<std::uint32_t>(hr));
            drainReplayInfoQueue("guest-DXIL PSO");
            return fail("could not create replay guest-DXIL pipeline");
        }
        ++replayPipelineCreations;
        if (!frameDraw && guestPipelines.size() >= kMaxReplayGuestPipelineEntries)
        {
            pipelineCacheBytes.fetch_sub(guestPipelines.front().OwnedBytes(), std::memory_order_relaxed);
            guestPipelines.erase(guestPipelines.begin());
        }
        guestPipelines.push_back({ std::move(guestPipelineKey),
                                   preparedState ? *preparedState : NativeDrawReplayPipelineStateKey{},
                                   preparedState ? recipe.shaderVersion : nullptr,
                                   std::move(pipeline),
                                   frameRecordings,
                                   nextFenceValue });
        pipelineCacheBytes.fetch_add(guestPipelines.back().OwnedBytes(), std::memory_order_relaxed);
        guestPipeline = guestPipelines.back().pipeline.Get();
    }
    phaseEnd(PhasePipeline);
    commandList->RSSetViewports(1, &replayViewport);
    commandList->RSSetScissorRects(1, &replayScissor);
    commandList->SetGraphicsRootSignature(replayDrawRootSignatures[textureCount].Get());
    draw.pipeline = guestPipeline;
    commandList->SetPipelineState(draw.pipeline.Get());
    commandList->OMSetRenderTargets(4, targetRtvs, FALSE, recipe.depth.enabled || recipe.stencil.enabled ? &depthDsv : nullptr);
    commandList->OMSetStencilRef(recipe.stencil.reference);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    vertexView.BufferLocation = draw.uploadAddresses[3];
    vertexView.SizeInBytes    = static_cast<UINT>(recipe.vertexBytes().size());
    vertexView.StrideInBytes  = recipe.vertexStrideBytes;
    commandList->IASetVertexBuffers(0, 1, &vertexView);
    D3D12_INDEX_BUFFER_VIEW indexView{};
    indexView.BufferLocation = draw.uploadAddresses[4];
    indexView.SizeInBytes    = static_cast<UINT>(indexByteCount);
    indexView.Format         = DXGI_FORMAT_R32_UINT;
    commandList->IASetIndexBuffer(&indexView);
    commandList->SetGraphicsRootConstantBufferView(0, draw.uploadAddresses[0]);
    commandList->SetGraphicsRootConstantBufferView(1, draw.uploadAddresses[1]);
    commandList->SetGraphicsRootConstantBufferView(2, draw.uploadAddresses[2]);
    if (recipe.textureMask)
    {
        bindDrawHeaps(commandList.Get(), draw);
        auto textureHandle = draw.viewGpu;
        textureHandle.ptr += static_cast<UINT64>(3) * replaySrvStride;
        commandList->SetGraphicsRootDescriptorTable(3, textureHandle);
        commandList->SetGraphicsRootDescriptorTable(4, samplerTable);
    }
    commandList->DrawIndexedInstanced(recipe.indexCount, 1, 0, 0, 0);
    phaseEnd(PhaseDraw);

    if (readback || gamma)
    {
        for (auto& target : replayColorTargets)
        {
            transitionTexture(target.Get(),
                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        // The final gamma pass leaves the resolved target in render-target state;
        // without gamma, the final sample copy leaves it in copy-source state.
        if (reusedOutputBundle && !gamma)
        {
            transitionTexture(replayResolvedTarget.Get(),
                              D3D12_RESOURCE_STATE_COPY_SOURCE,
                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        commandList->RSSetViewports(1, &fullTargetViewport);
        commandList->RSSetScissorRects(1, &fullTargetScissor);
        commandList->SetGraphicsRootSignature(replayCopyRootSignature.Get());
        commandList->SetPipelineState(helper->resolve.Get());
        D3D12_GPU_DESCRIPTOR_HANDLE targetSrvGpu =
            draw.viewGpu;
        targetSrvGpu.ptr += static_cast<UINT64>(2) * replaySrvStride;
        commandList->SetGraphicsRootDescriptorTable(0, targetSrvGpu);
        if (readback)
            commandList->OMSetRenderTargets(1, &resolvedRtv, FALSE, nullptr);
        D3D12_TEXTURE_COPY_LOCATION readbackLocation{};
        readbackLocation.pResource                          = replayReadback.Get();
        readbackLocation.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        readbackLocation.PlacedFootprint.Offset             = 0;
        readbackLocation.PlacedFootprint.Footprint.Format   = colorFormat;
        readbackLocation.PlacedFootprint.Footprint.Width    = recipe.width;
        readbackLocation.PlacedFootprint.Footprint.Height   = recipe.height;
        readbackLocation.PlacedFootprint.Footprint.Depth    = 1;
        readbackLocation.PlacedFootprint.Footprint.RowPitch = outputRowPitch;
        D3D12_TEXTURE_COPY_LOCATION resolvedLocation{};
        resolvedLocation.pResource        = replayResolvedTarget.Get();
        resolvedLocation.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        resolvedLocation.SubresourceIndex = 0;
        const auto recordSampleReadback   = [&](ID3D12Resource* constants,
                                                std::uint64_t   outputOffset)
        {
            commandList->SetGraphicsRootConstantBufferView(1, constants->GetGPUVirtualAddress());
            commandList->DrawInstanced(3, 1, 0, 0);
            transitionTexture(replayResolvedTarget.Get(),
                              D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_COPY_SOURCE);
            readbackLocation.PlacedFootprint.Offset = outputOffset;
            commandList->CopyTextureRegion(&readbackLocation,
                                           0,
                                           0,
                                           0,
                                           &resolvedLocation,
                                           nullptr);
            if (outputOffset + outputPlaneStride < readbackBytes)
            {
                transitionTexture(replayResolvedTarget.Get(),
                                  D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        };
        if (readback)
        {
            recordSampleReadback(replayResolveConstants.Get(), 0);
            recordSampleReadback(replayResolveConstantsSample3.Get(), outputPlaneStride);
            if (recipe.depth.enabled)
            {
                transitionTexture(replayDepthTarget.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                commandList->SetPipelineState(helper->depthRead.Get());
                auto depthGpu = draw.viewGpu;
                depthGpu.ptr += static_cast<UINT64>(depthSrvBase + 2) * replaySrvStride;
                commandList->SetGraphicsRootDescriptorTable(0, depthGpu);
                recordSampleReadback(replayResolveConstants.Get(), outputPlaneStride * 2);
                recordSampleReadback(replayResolveConstantsSample3.Get(), outputPlaneStride * 3);
            }
        }
        if (gamma)
        {
            if (recordingGpuOutput ? recordingGpuOutput->initialized : reusedOutputBundle)
            {
                transitionTexture(replayGammaTarget.Get(),
                                  D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            auto gammaRtv = rtvStart;
            gammaRtv.ptr += static_cast<SIZE_T>(5) * replayRtvStride;
            device->CreateRenderTargetView(replayGammaTarget.Get(), nullptr, gammaRtv);
            commandList->SetPipelineState(helper->gamma.Get());
            commandList->SetGraphicsRootDescriptorTable(0, targetSrvGpu);
            commandList->SetGraphicsRootConstantBufferView(1, replayGammaConstants->GetGPUVirtualAddress());
            commandList->OMSetRenderTargets(1, &gammaRtv, FALSE, nullptr);
            commandList->DrawInstanced(3, 1, 0, 0);
            transitionTexture(replayGammaTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (recordingGpuOutput)
                recordingGpuOutput->initialized = true;
            if (readback)
            {
                readbackLocation.PlacedFootprint.Offset           = (outputPlaneCount - 1) * outputPlaneStride;
                readbackLocation.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                auto gammaSource                                  = resolvedLocation;
                gammaSource.pResource                             = replayGammaTarget.Get();
                commandList->CopyTextureRegion(&readbackLocation, 0, 0, 0, &gammaSource, nullptr);
            }
        }
        // Ordered draws leave attachments in writable state for subsequent work.
        for (auto& target : replayColorTargets)
        {
            transitionTexture(target.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        if (readback && recipe.depth.enabled)
        {
            transitionTexture(replayDepthTarget.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }
    const auto recordingEnd = DevNow();
    phaseEnd(PhaseTail, recordingEnd);
    const auto publishRecording = [&]
    {
#if REREVVED_DEVELOPER
        residency.published = true;
        replayRecordingNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(recordingEnd - recordingStart).count()), std::memory_order_relaxed);
#endif
        ++replayRecordedDraws;
    };
#if REREVVED_DEVELOPER
    if (phaseTiming)
    {
        std::lock_guard timingLock(replayTimingMutex);
        for (std::size_t phase = 0; phase < phaseNanoseconds.size(); ++phase)
            replayRecordingPhaseNanoseconds[phase].fetch_add(phaseNanoseconds[phase], std::memory_order_relaxed);
        for (std::size_t kind = 0; kind < drawUploadBytes.size(); ++kind)
            replayDrawUploadBytes[kind].fetch_add(drawUploadBytes[kind], std::memory_order_relaxed);
        for (std::size_t outcome = 0; outcome < outcomeCounts.size(); ++outcome)
        {
            replayTextureOutcomes[outcome].fetch_add(outcomeCounts[outcome], std::memory_order_relaxed);
            replayTextureOutcomeBytes[outcome].fetch_add(outcomeBytes[outcome], std::memory_order_relaxed);
        }
        for (std::size_t stage = 0; stage < repeatPreviousBytes.size(); ++stage)
        {
            replayConstantRepeatPreviousBytes[stage].fetch_add(repeatPreviousBytes[stage], std::memory_order_relaxed);
            replayConstantRepeatChunkBytes[stage].fetch_add(repeatChunkBytes[stage], std::memory_order_relaxed);
        }
        for (std::size_t kind = 0; kind < residentBytes.size(); ++kind)
            replayResidentGeometryBytes[kind].fetch_add(residentBytes[kind], std::memory_order_relaxed);
        if (vertexResidency)
        {
            residentHits.fetch_add(residency.hit, std::memory_order_relaxed);
            residentBytesAvoided.fetch_add(residency.hit ? residency.bytes : 0, std::memory_order_relaxed);
            residentMisses.fetch_add(residency.miss, std::memory_order_relaxed);
            residentCopyBytes.fetch_add(residency.miss ? residency.bytes : 0, std::memory_order_relaxed);
            if (residency.fallback >= 0)
            {
                residentFallbacks.fetch_add(1, std::memory_order_relaxed);
                residentFallbackReasons[residency.fallback].fetch_add(1, std::memory_order_relaxed);
            }
        }
        replayTextureDescriptorReuses.fetch_add(textureViewReuses, std::memory_order_relaxed);
        replayTextureSrvWrites.fetch_add(textureSrvWrites, std::memory_order_relaxed);
        replayRtvWrites.fetch_add(drawRtvWrites, std::memory_order_relaxed);
        publishRecording();
    }
    else
#endif
        publishRecording();
    if (!finishBatch)
    {
        result.success = true;
        return result;
    }
    hr = commandList->Close();
    if (FAILED(hr))
    {
        return fail("could not close replay command list");
    }
    ID3D12CommandList* lists[] = { commandList.Get() };
    queue->ExecuteCommandLists(1, lists);
    replayFenceValue = nextFenceValue++;
    ++replayQueueSubmissions;
    hr                       = queue->Signal(fence.Get(), replayFenceValue);
    replaySubmissionInFlight = SUCCEEDED(hr);
    if (FAILED(hr))
    {
        logDeviceRemoval("replay signal", hr);
        return fail("replay queue signal failed");
    }
#if REREVVED_DEVELOPER
    const auto waitStart = std::chrono::steady_clock::now();
#endif
    const bool completed = waitForFence(replayFenceValue);
#if REREVVED_DEVELOPER
    replayFenceWaitNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()), std::memory_order_relaxed);
#endif
    if (!completed)
    {
        return fail("replay fence did not complete");
    }
    replaySubmissionInFlight = false;

    if (!readback)
    {
        result.success = true;
        return result;
    }
    std::vector<std::uint8_t> output;
    if (!readReplayOutput(recipe, gamma != nullptr, output, result.error))
        return result;

    if (outputBytes)
    {
        *outputBytes   = std::move(output);
        result.success = true;
        return result;
    }
    std::ofstream outputFile(recipe.outputPath, std::ios::binary | std::ios::trunc);
    if (!outputFile)
    {
        return fail("could not open replay output file");
    }
    outputFile.write(reinterpret_cast<const char*>(output.data()),
                     static_cast<std::streamsize>(output.size()));
    outputFile.flush();
    outputFile.close();
    if (!outputFile)
    {
        return fail("could not flush or close replay output file");
    }
    result.success = true;
    return result;
}

bool NativeRendererD3D12::Impl::readReplayOutput(const NativeDrawReplayRecipe& recipe, bool gamma, std::vector<std::uint8_t>& output, std::string& error)
{
    const std::uint64_t outputPlaneBytes  = static_cast<std::uint64_t>(recipe.width) * recipe.height * 4;
    const std::uint32_t outputRowPitch    = (recipe.width * 4U + 255U) & ~255U;
    const std::uint64_t outputPlaneStride = (static_cast<std::uint64_t>(outputRowPitch) * recipe.height + 511ULL) & ~511ULL;
    const std::uint32_t outputPlaneCount  = (recipe.depth.enabled ? 4U : 2U) + (gamma ? 1U : 0U);
    const auto          readbackBytes     = outputPlaneStride * outputPlaneCount;
    output.resize(static_cast<std::size_t>(outputPlaneBytes * outputPlaneCount));
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(readbackBytes) };
    void*       mapped = nullptr;
    const auto  hr     = replayReadback->Map(0, &readRange, &mapped);
    if (FAILED(hr))
    {
        error = "could not map replay readback";
        return false;
    }
    for (std::size_t plane = 0; plane < outputPlaneCount; ++plane)
    {
        for (std::uint32_t row = 0; row < recipe.height; ++row)
        {
            std::memcpy(output.data() + plane * static_cast<std::size_t>(outputPlaneBytes) +
                            static_cast<std::size_t>(row) * recipe.width * 4U,
                        static_cast<const std::uint8_t*>(mapped) + plane * outputPlaneStride +
                            static_cast<std::size_t>(row) * outputRowPitch,
                        static_cast<std::size_t>(recipe.width) * 4U);
        }
    }
    D3D12_RANGE writtenRange{ 0, 0 };
    replayReadback->Unmap(0, &writtenRange);

    return true;
}

void NativeRendererD3D12::Impl::switchFrameContext(std::size_t index)
{
    if (index == activeFrameContext)
        return;
    cancelRecordingCpuReservation();
    ++frameUploadEpoch;
    const auto exchange = [&](FrameContext& context)
    {
#define FRAME_FIELD(name) std::swap(name, context.name)
        FRAME_FIELD(frameTextures);
        FRAME_FIELD(frameTextureOwners);
        FRAME_FIELD(recordedRenderPrefix);
        FRAME_FIELD(frameColorSnapshots);
        FRAME_FIELD(frameUploadArena);
        FRAME_FIELD(frameRetiredUploads);
        FRAME_FIELD(frameRetiredUploadLeases);
        FRAME_FIELD(frameUploadMapped);
        FRAME_FIELD(frameUploadCursor);
        FRAME_FIELD(frameUploadPrepared);
        FRAME_FIELD(frameCpuTextureFootprintBytes);
        FRAME_FIELD(frameCpuTexturePayloadBudget);
        FRAME_FIELD(frameCpuTextures);
        FRAME_FIELD(frameRetiredCpuTextures);
        FRAME_FIELD(replayGammaConstants);
        FRAME_FIELD(replayGammaTarget);
        FRAME_FIELD(replayInitialSample0);
        FRAME_FIELD(replayInitialSample1);
        FRAME_FIELD(replayInitialSample0Upload);
        FRAME_FIELD(replayInitialSample1Upload);
        FRAME_FIELD(replayColorTargets);
        FRAME_FIELD(replayResolvedTarget);
        FRAME_FIELD(replayReadback);
        FRAME_FIELD(replayDepthTarget);
        FRAME_FIELD(replayDepthInitialized);
        FRAME_FIELD(frameAttachmentCaching);
        FRAME_FIELD(frameColorAttachmentCache);
        FRAME_FIELD(frameDepthAttachmentCache);
        FRAME_FIELD(activeColorPresented);
        FRAME_FIELD(activeDepthPresented);
        FRAME_FIELD(replayDsvHeap);
        FRAME_FIELD(replayDsvResource);
        FRAME_FIELD(replayRtvHeap);
        FRAME_FIELD(replayInitialDepth);
        FRAME_FIELD(replayInitialDepthUpload);
        FRAME_FIELD(replayCopyConstants);
        FRAME_FIELD(replayResolveConstants);
        FRAME_FIELD(replayResolveConstantsSample3);
        FRAME_FIELD(replayOutputBundleKey);
        FRAME_FIELD(replayOutputBundleValid);
        FRAME_FIELD(replaySubmissionInFlight);
        FRAME_FIELD(replayFenceValue);
#undef FRAME_FIELD
    };
    exchange(frameContexts[activeFrameContext]);
    exchange(frameContexts[index]);
    activeFrameContext         = index;
    std::uint64_t payloadBytes = 0, ownedBytes = 0;
    const auto    countPayloads = [&](const auto& context)
    {
        for (const auto& [key, texture] : context.frameCpuTextures)
        {
            payloadBytes += texture.RetainedPayloadBytes();
            ownedBytes += texture.RetainedOwnedBytes();
        }
        for (const auto& texture : context.frameRetiredCpuTextures)
        {
            payloadBytes += texture.RetainedPayloadBytes();
            ownedBytes += texture.RetainedOwnedBytes();
        }
    };
    countPayloads(*this);
    for (const auto& context : frameContexts)
        countPayloads(context);
    frameCpuTexturePayloadBytes.store(payloadBytes, std::memory_order_relaxed);
    frameCpuTextureOwnedBytes.store(ownedBytes, std::memory_order_relaxed);
}

void NativeRendererD3D12::Impl::visitOwnedBytes(detail::OwnedVisitor visitor, bool stateLocked)
{
    ownedStateWalks.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t owned  = ownedRecordingReservation;
    const auto    scalar = [&](std::uint64_t count, std::uint64_t width = 1)
    {
        owned = detail::OwnedAdd(owned, detail::OwnedMultiply(count, width));
    };
    scalar(frameDrawSlots.capacity(), sizeof(ReplayDrawResources));
    scalar(uploadPool.BookkeepingBytes());
    scalar(texturePool.BookkeepingBytes());
    scalar(attachmentPool.BookkeepingBytes());
    scalar(resolvePool.BookkeepingBytes());
    scalar(resolveReturns->BookkeepingBytes());
    scalar(versionTextures.size(), sizeof(VersionTextures::value_type) + 4 * sizeof(void*));
    scalar(versionTexturesOpen.capacity(), sizeof(void*));
    scalar(versionTextureReleases.capacity(), sizeof(VersionTextureRelease));
    scalar(uploadedVertexOwners.bucket_count(), kUploadedVertexOwnerBucket);
    scalar(uploadedVertexOwners.size(), kUploadedVertexOwnerBytes);
    if (vertexResidency)
    {
        scalar(vertexResidency->MetadataBytes());
        scalar(vertexResidency->SnapshotCeilingBytes());
    }
    scalar(replayHelperPipelineCache.capacity(), sizeof(ReplayHelperPipelineCacheEntry));
    for (const auto& batch : frameCommandBatches)
    {
        scalar(batch.helpers.capacity(), sizeof(ReplayHelperPipelineCacheEntry));
        // Sampler tables grow within the owner's heap during recording.
        if (batch.samplerHeap)
            scalar(kFrameSamplerTableBytes);
    }
    const auto proof = [&](const auto& version)
    {
        if (version)
            visitor.Add(version.get(), sizeof(NativeRenderFrameTextureVersion) + sizeof(NativeRenderFrameTextureVersion::Impl));
    };
    const auto request = [&](const std::shared_ptr<TerminalRequest>& terminal)
    {
        if (!terminal)
            return;
        visitor.Add(terminal.get(), sizeof(TerminalRequest) + 2 * (sizeof(NativeDrawReplayResult) + 4 * sizeof(void*)));
        if (terminal->ticket)
        {
            std::lock_guard lock(terminal->ticket->exportMutex);
            visitor.Add(terminal->ticket.get(), detail::OwnedAdd(sizeof(NativeRenderFrameTicket), detail::OwnedMultiply(terminal->ticket->exports.size(), sizeof(decltype(terminal->ticket->exports)::value_type) + 4 * sizeof(void*))));
            for (const auto& [key, version] : terminal->ticket->exports)
            {
                proof(version);
            }
        }
    };
    const auto context = [&](const auto& frame)
    {
        scalar(frame.frameCpuTextures.size(), sizeof(typename decltype(frame.frameCpuTextures)::value_type) + 4 * sizeof(void*));
        scalar(frame.frameTextures.size(), sizeof(typename decltype(frame.frameTextures)::value_type) + 4 * sizeof(void*));
        scalar(frame.frameTextureOwners.size(), sizeof(typename decltype(frame.frameTextureOwners)::value_type) + 4 * sizeof(void*));
        scalar(frame.frameColorSnapshots.size(), sizeof(typename decltype(frame.frameColorSnapshots)::value_type) + 4 * sizeof(void*));
        scalar(frame.frameRetiredCpuTextures.capacity(), sizeof(FrameCpuTexture));
        scalar(frame.frameRetiredUploads.capacity(), sizeof(ComPtr<ID3D12Resource>));
        scalar(frame.frameRetiredUploadLeases.capacity(), sizeof(UploadLease));
        scalar(frame.frameColorAttachmentCache.capacity(), sizeof(ReplayColorAttachments));
        scalar(frame.frameDepthAttachmentCache.capacity(), sizeof(ReplayDepthAttachment));
        for (const auto& [id, owner] : frame.frameTextureOwners)
            if (owner)
                visitor.Add(owner.get(), sizeof(NativeFrameResourceOwner));
        if (frame.recordedRenderPrefix)
        {
            frame.recordedRenderPrefix->validation.VisitOwnedBytes(visitor);
            scalar(frame.recordedRenderPrefix->textures.capacity(), sizeof(NativeRenderFrameTexture));
            detail::VisitNativeRenderFramePassesOwnedBytes(frame.recordedRenderPrefix->passes, visitor);
            for (const auto& texture : frame.recordedRenderPrefix->textures)
                proof(texture.producerVersion);
        }
        const auto cpu = [&](const FrameCpuTexture& texture)
        {
            scalar(texture.uploadedSize);
            if (texture.preparation)
                visitor.Add(texture.preparation.get(), texture.preparation->StorageBytes());
            for (const auto& version : { texture.uploadedVersion, texture.reservedRecordingVersion })
                if (version)
                {
                    visitor.Add(version.get(), version->MetadataBytes() - (version->Payload()->capacity() - version->Payload()->size()));
                    visitor.Add(version->Payload().get(), detail::OwnedAdd(sizeof(std::vector<std::uint8_t>), version->Payload()->capacity()));
                }
        };
        for (const auto& [key, texture] : frame.frameCpuTextures)
            cpu(texture);
        for (const auto& texture : frame.frameRetiredCpuTextures)
            cpu(texture);
    };
    context(*this);
    for (const auto& frame : frameContexts)
        context(frame);
    const auto imports = [&](const FrameImports& values)
    {
        scalar(values.size(), sizeof(FrameImports::value_type) + 4 * sizeof(void*));
        for (const auto& [id, imported] : values)
        {
            proof(imported.descriptor.producerVersion);
            if (imported.owner)
                visitor.Add(imported.owner.get(), sizeof(NativeFrameResourceOwner));
        }
    };
    imports(recordingImports);
    if (recordingFrameLease)
    {
        detail::VisitNativeRenderFrameOwnedBytes(*recordingFrameLease, visitor);
        for (const auto& texture : recordingFrameLease->textures)
            proof(texture.producerVersion);
    }
    {
        std::unique_lock lock(stateMutex, std::defer_lock);
        if (!stateLocked)
            lock.lock();
        request(drawStreamTerminal);
        request(activeTerminalRequest);
        imports(drawStreamImports);
        if (admissionImports)
            imports(*admissionImports);
        if (drawStreamRenderFrame)
        {
            detail::VisitNativeRenderFrameOwnedBytes(*drawStreamRenderFrame, visitor);
            for (const auto& texture : drawStreamRenderFrame->textures)
                proof(texture.producerVersion);
        }
    }
    for (const auto& submitted : submittedFrames)
    {
        scalar(sizeof(SubmittedFrame));
        scalar(submitted.textures.capacity(), sizeof(NativeRenderFrameTexture));
        request(submitted.request);
        for (const auto& texture : submitted.textures)
            proof(texture.producerVersion);
    }
    for (const auto& [key, retained] : retainedFrameTextures)
    {
        scalar(sizeof(RetainedFrameTexture) + 4 * sizeof(void*));
        if (retained.owner)
            visitor.Add(retained.owner.get(), sizeof(NativeFrameResourceOwner));
        proof(retained.version);
    }
    if (surfacesUsed.load(std::memory_order_acquire))
    {
        std::lock_guard lock(surfaceMutex);
        for (const auto& [key, stored] : surfaceStore)
        {
            scalar(sizeof(StoredSurface) + 4 * sizeof(void*));
            if (stored.owner)
                visitor.Add(stored.owner.get(), sizeof(NativeFrameResourceOwner));
        }
        for (const auto& [value, owner] : surfaceRetires)
        {
            scalar(sizeof(value) + sizeof(owner));
            if (owner)
                visitor.Add(owner.get(), sizeof(NativeFrameResourceOwner));
        }
    }
    for (const auto& cache : { &replayGuestPipelineCache, &frameGuestPipelineCache })
    {
        scalar(cache->capacity(), sizeof(ReplayGuestPipelineCacheEntry));
        for (const auto& entry : *cache)
        {
            scalar(entry.key.capacity());
            if (entry.shaderVersion)
                visitor.Add(entry.shaderVersion.get(), sizeof(NativeDrawReplayShaderVersion));
            if (entry.shaderVersion)
                for (const auto& bytecode : { entry.shaderVersion->VertexBytecode(), entry.shaderVersion->PixelBytecode() })
                    visitor.Add(bytecode.get(), detail::OwnedAdd(sizeof(*bytecode), bytecode->capacity()));
        }
    }
    visitor.Add(nullptr, owned);
}

std::pair<std::uint64_t, std::uint64_t> NativeRendererD3D12::Impl::censusOwnedBytes(bool stateLocked)
{
    if (directStream)
        return { directOwnedBytes(), 0 };
    return detail::CensusOwnedBytes([&](detail::OwnedVisitor visitor)
                                    {
                                        visitOwnedBytes(visitor, stateLocked);
                                    });
}

bool NativeRendererD3D12::Impl::ensureOwnerHeap(std::uint64_t work, std::string& error, bool stateLocked, bool retainGrowth)
{
    const auto live = ownedHeap.live.load(std::memory_order_relaxed);
    // A direct stream's heap is funded to the bound when it starts.
    if (directStream)
    {
        if (live <= ownedHeap.limit && work <= ownedHeap.limit - live)
            return true;
        error = "native renderer owner metadata peak exceeds the byte bound";
        return false;
    }
    if (live <= ownedHeap.limit && work <= ownedHeap.limit - live && (!retainGrowth || work <= ownedMaintenanceWork))
        return true;
    std::uint64_t physical, count;
    const auto    fundedScan = detail::ForecastOwnerSet(ownedScanCeiling).peak;
    if (live <= ownedHeap.limit && fundedScan <= ownedHeap.limit - live)
    {
        // Extend a funded packet with its existing linear Owned walk. Its table
        // dies before publishing another domain's larger scratch allowance.
        detail::OwnerSet       owners(detail::OwnedAllocator<const void*>{ &ownedHeap });
        detail::OwnedCollector collector{ owners, nullptr, ownedScanCeiling };
        visitOwnedBytes(collector.Visitor(), stateLocked);
        physical = collector.bytes;
        count    = owners.size();
    }
    else
        std::tie(physical, count) = censusOwnedBytes(stateLocked);
    const auto              extension = retainGrowth && work > ownedMaintenanceWork ? work - ownedMaintenanceWork : 0;
    const auto              next      = std::max(detail::OwnedAdd(live, work), detail::OwnedAdd(ownedHeap.limit, extension));
    constexpr std::uint64_t bound     = 512ULL * 1024 * 1024;
    if (physical > bound || next > bound - physical)
    {
        error = "native renderer owner metadata peak exceeds the byte bound";
        return false;
    }
    std::unique_lock lock(stateMutex, std::defer_lock);
    if (!stateLocked)
        lock.lock();
    const auto prior = ownedHeap.limit;
    ownedHeap.limit  = std::max(prior, next);
    if (retainGrowth)
        ownedMaintenanceWork = std::max(ownedMaintenanceWork, work);
    if (ownedAdmissionSnapshotValid)
        ownedAdmissionBytes = detail::OwnedAdd(ownedAdmissionBytes, ownedHeap.limit - prior);
    ownedScanCeiling = std::max(ownedScanCeiling, count);
    publishOwnedCharge();
    return true;
}

std::uint64_t NativeRendererD3D12::Impl::retainedStorageBytes(StorageDomain domain, detail::OwnerSet* uniqueOwners, bool stateLocked, bool cpuCandidates)
{
    if (domain == StorageDomain::Owned)
    {
        if (directStream)
            return directOwnedBytes();
        std::string error;
        if (!ownedScanCeiling)
        {
            const auto [physical, count] = censusOwnedBytes(stateLocked);
            std::unique_lock lock(stateMutex, std::defer_lock);
            if (!stateLocked)
                lock.lock();
            ownedScanCeiling = count;
        }
        const auto peak             = detail::ForecastOwnerSet(ownedScanCeiling).peak;
        const auto alreadyAllocated = uniqueOwners ? detail::OwnerSetBytes(uniqueOwners->size(), uniqueOwners->bucket_count()) : 0;
        if (!ensureOwnerHeap(peak > alreadyAllocated ? peak - alreadyAllocated : 0, error, stateLocked))
            throw std::bad_alloc{};
        std::optional<detail::OwnerSet> local;
        if (!uniqueOwners)
        {
            local.emplace(detail::OwnedAllocator<const void*>{ &ownedHeap });
            uniqueOwners = &*local;
        }
        detail::OwnedCollector collector{ *uniqueOwners, nullptr, ownedScanCeiling };
        visitOwnedBytes(collector.Visitor(), stateLocked);
        ownedLastScanFullCount = uniqueOwners->size();
        if (!local)
        {
            const auto omitRequestIdentity = [&](const std::shared_ptr<TerminalRequest>& terminal)
            {
                if (terminal)
                {
                    uniqueOwners->erase(terminal.get());
                    uniqueOwners->erase(terminal->ticket.get());
                }
            };
            // Request objects can retire at readiness. They are charged above,
            // but cannot grant credit to future payload allocations by address.
            {
                std::unique_lock lock(stateMutex, std::defer_lock);
                if (!stateLocked)
                    lock.lock();
                omitRequestIdentity(drawStreamTerminal);
                omitRequestIdentity(activeTerminalRequest);
            }
            for (const auto& submitted : submittedFrames)
                omitRequestIdentity(submitted.request);
        }
        return detail::OwnedAdd(collector.bytes, ownedHeap.limit);
    }
    std::uint64_t resourceOccurrences = 0, metadataOccurrences = 0;
    const auto    countContext = [&](const auto& context)
    {
        const auto entries  = detail::OwnedAdd(context.frameCpuTextures.size(), context.frameRetiredCpuTextures.size());
        resourceOccurrences = detail::OwnedAdd(resourceOccurrences, entries);
        if (domain == StorageDomain::CpuTexture)
            metadataOccurrences = detail::OwnedAdd(metadataOccurrences, detail::OwnedMultiply(entries, 5));
        if (domain == StorageDomain::Upload)
            resourceOccurrences = detail::OwnedAdd(resourceOccurrences, detail::OwnedAdd(9, detail::OwnedAdd(context.frameRetiredUploads.size(), context.frameRetiredUploadLeases.size())));
        if (domain == StorageDomain::Texture)
            resourceOccurrences = detail::OwnedAdd(resourceOccurrences, detail::OwnedAdd(context.frameTextures.size(), context.frameColorSnapshots.size()));
        if (domain == StorageDomain::Attachment)
            resourceOccurrences = detail::OwnedAdd(resourceOccurrences, detail::OwnedAdd(12, detail::OwnedAdd(detail::OwnedMultiply(4, context.frameColorAttachmentCache.size()), context.frameDepthAttachmentCache.size())));
    };
    countContext(*this);
    for (const auto& context : frameContexts)
        countContext(context);
    if (domain == StorageDomain::Upload)
        resourceOccurrences = detail::OwnedAdd(resourceOccurrences, uploadPool.HeldCount());
    if (domain == StorageDomain::Texture)
        resourceOccurrences = detail::OwnedAdd(resourceOccurrences, detail::OwnedAdd(texturePool.HeldCount(), detail::OwnedAdd(resolvePool.HeldCount(), resolveReturns->QueuedCount())));
    if (domain == StorageDomain::Texture)
        resourceOccurrences = detail::OwnedAdd(resourceOccurrences, detail::OwnedAdd(versionTextures.size(), versionTextureReleases.size()));
    if (domain == StorageDomain::Attachment)
        resourceOccurrences = detail::OwnedAdd(resourceOccurrences, attachmentPool.HeldCount());
    if (domain == StorageDomain::Texture)
    {
        std::unique_lock lock(stateMutex, std::defer_lock);
        if (!stateLocked)
            lock.lock();
        for (const auto count : { retainedFrameTextures.size(), recordingImports.size(), drawStreamImports.size(), admissionImports ? admissionImports->size() : std::size_t{ 0 } })
            resourceOccurrences = detail::OwnedAdd(resourceOccurrences, count);
        if (surfacesUsed.load(std::memory_order_acquire))
        {
            std::lock_guard surfaces(surfaceMutex);
            resourceOccurrences = detail::OwnedAdd(resourceOccurrences, detail::OwnedAdd(surfaceStore.size(), surfaceRetires.size()));
        }
    }
    if (domain == StorageDomain::Attachment)
    {
        std::lock_guard lock(rawReadbackMutex);
        resourceOccurrences = detail::OwnedAdd(resourceOccurrences, detail::OwnedAdd(frameGpuOutputs.size(), detail::OwnedAdd(submittedFrames.size(), detail::OwnedAdd(liveRawReadbacks.size(), 1))));
    }
    if (domain == StorageDomain::Upload || domain == StorageDomain::Texture)
    {
        const auto countDraw = [&](const auto& draw)
        {
            resourceOccurrences = detail::OwnedAdd(resourceOccurrences, domain == StorageDomain::Upload ? detail::OwnedAdd(5, draw.textureUploads.size()) : draw.textures.size());
        };
        for (const auto& draw : frameDrawSlots)
            countDraw(draw);
        for (const auto& draw : replayDrawSlots)
            countDraw(draw);
    }
    std::string metadataError;
    if (!ensureOwnerHeap(detail::OwnedAdd(detail::ForecastOwnerSet(resourceOccurrences).peak, detail::ForecastOwnerSet(metadataOccurrences).peak), metadataError, stateLocked))
        throw std::bad_alloc{};
    detail::OwnedSet<ID3D12Resource*> resources(detail::OwnedAllocator<ID3D12Resource*>{ &ownedHeap });
    detail::OwnerSet                  metadata(detail::OwnedAllocator<const void*>{ &ownedHeap });
    std::uint64_t                     bytes = 0;
    const auto                        add   = [&](const auto& resource, std::uint64_t knownBytes = 0)
    {
        if (!resource)
            return;
        if (resources.size() >= resourceOccurrences && !resources.contains(resource.Get()))
            throw std::bad_alloc{};
        if (!resources.insert(resource.Get()).second)
            return;
        bytes = detail::OwnedAdd(bytes, knownBytes ? knownBytes : resourceStorageBytes(device.Get(), resource.Get()));
    };
    // A pooled buffer's class is a whole number of 64 KiB placement units,
    // which is its allocation size.
    const auto lease = [&](const UploadLease& held)
    {
        if (held)
            add(held.buffer().resource, held.bytes());
    };
    const auto cpu = [&](const FrameCpuTexture& texture)
    {
        if (domain == StorageDomain::Upload)
            lease(texture.upload);
        if (domain == StorageDomain::Texture)
            add(texture.resource, texture.resourceStorageBytes);
        if (domain == StorageDomain::CpuTexture)
        {
            lease(texture.upload);
            if (texture.preparation && metadata.insert(texture.preparation.get()).second)
                bytes = detail::OwnedAdd(bytes, texture.preparation->StorageBytes());
            bytes = detail::OwnedAdd(bytes, texture.uploadedSize);
            for (const auto& version : { texture.uploadedVersion, (cpuCandidates || recordingCpuReservationLease) ? texture.reservedRecordingVersion : nullptr })
                if (version)
                {
                    if (metadata.insert(version.get()).second)
                        bytes = detail::OwnedAdd(bytes, version->MetadataBytes() - (version->Payload()->capacity() - version->Payload()->size()));
                    if (metadata.insert(version->Payload().get()).second)
                        bytes = detail::OwnedAdd(bytes, version->Payload()->capacity());
                }
        }
    };
    const auto context = [&](const auto& frame)
    {
        for (const auto& [key, texture] : frame.frameCpuTextures)
            cpu(texture);
        for (const auto& texture : frame.frameRetiredCpuTextures)
            cpu(texture);
        if (domain == StorageDomain::Upload)
        {
            lease(frame.frameUploadArena);
            for (const auto& resource : frame.frameRetiredUploads)
                add(resource);
            for (const auto& retired : frame.frameRetiredUploadLeases)
                lease(retired);
            for (const auto& resource : frame.replayInitialDepthUpload)
                add(resource);
            for (const auto* resource : { std::addressof(frame.replayGammaConstants), std::addressof(frame.replayInitialSample0Upload), std::addressof(frame.replayInitialSample1Upload), std::addressof(frame.replayCopyConstants), std::addressof(frame.replayResolveConstants), std::addressof(frame.replayResolveConstantsSample3) })
                add(*resource);
        }
        if (domain == StorageDomain::Texture)
        {
            for (const auto& [id, resource] : frame.frameTextures)
            {
                const auto owner = frame.frameTextureOwners.find(id);
                add(resource, owner != frame.frameTextureOwners.end() && owner->second && owner->second->resource.Get() == resource.Get() ? owner->second->resourceStorageBytes : 0);
            }
            for (const auto& [id, resource] : frame.frameColorSnapshots)
                add(resource);
        }
        if (domain == StorageDomain::Attachment)
        {
            for (const auto& resource : frame.replayColorTargets)
                add(resource);
            for (const auto& entry : frame.frameColorAttachmentCache)
                for (const auto& resource : entry.resources)
                    add(resource);
            for (const auto& entry : frame.frameDepthAttachmentCache)
                add(entry.resource);
            for (const auto* resource : { std::addressof(frame.replayDepthTarget), std::addressof(frame.replayResolvedTarget), std::addressof(frame.replayGammaTarget), std::addressof(frame.replayReadback), std::addressof(frame.replayInitialSample0), std::addressof(frame.replayInitialSample1) })
                add(*resource);
            for (const auto& resource : frame.replayInitialDepth)
                add(resource);
        }
    };
    context(*this);
    for (const auto& frame : frameContexts)
        context(frame);
    if (domain == StorageDomain::Upload)
        uploadPool.ForEachHeld([&](const NativeUploadBuffer& buffer, std::uint64_t held)
                               {
                                   add(buffer.resource, held);
                               });
    if (domain == StorageDomain::Texture || domain == StorageDomain::Attachment)
        (domain == StorageDomain::Texture ? texturePool : attachmentPool).ForEachHeld([&](const ComPtr<ID3D12Resource>& resource, std::uint64_t held)
                                                                                      {
                                                                                          add(resource, held);
                                                                                      });
    if (domain == StorageDomain::Texture)
    {
        const auto pooled = [&](const ComPtr<ID3D12Resource>& resource, std::uint64_t held)
        {
            add(resource, held);
        };
        resolvePool.ForEachHeld(pooled);
        resolveReturns->ForEachQueued(pooled);
        for (const auto& [key, entry] : versionTextures)
            add(entry.resource, entry.bytes);
        for (const auto& release : versionTextureReleases)
            add(release.resource, release.bytes);
    }
    if (domain == StorageDomain::Texture)
    {
        for (const auto& [key, texture] : retainedFrameTextures)
            add(texture.resource, texture.owner && texture.owner->resource.Get() == texture.resource.Get() ? texture.owner->resourceStorageBytes : 0);
        for (const auto& [id, imported] : recordingImports)
            add(imported.owner->resource, imported.owner->resourceStorageBytes);
        if (surfacesUsed.load(std::memory_order_acquire))
        {
            std::lock_guard surfaces(surfaceMutex);
            for (const auto& [key, stored] : surfaceStore)
                if (stored.owner)
                    add(stored.owner->resource, stored.owner->resourceStorageBytes);
            for (const auto& [value, owner] : surfaceRetires)
                if (owner)
                    add(owner->resource, owner->resourceStorageBytes);
        }
        std::unique_lock lock(stateMutex, std::defer_lock);
        if (!stateLocked)
            lock.lock();
        for (const auto& [id, imported] : drawStreamImports)
            add(imported.owner->resource, imported.owner->resourceStorageBytes);
        if (admissionImports)
            for (const auto& [id, imported] : *admissionImports)
                add(imported.owner->resource, imported.owner->resourceStorageBytes);
    }
    if (domain == StorageDomain::Attachment)
    {
        for (const auto& slot : frameGpuOutputs)
            add(slot.resource);
        // Readbacks held for retirement, and one recorded but not yet queued.
        for (const auto& submitted : submittedFrames)
            add(submitted.rawReadback);
        add(submittedRawReadback);
        // Prefix readbacks a caller still holds.
        std::lock_guard lock(rawReadbackMutex);
        for (const auto& weak : liveRawReadbacks)
            if (const auto readback = weak.lock())
                add(readback->buffer);
    }
    const auto draw = [&](const ReplayDrawResources& owner)
    {
        if (domain == StorageDomain::Upload)
        {
            for (const auto* resource : { std::addressof(owner.vertexBuffer), std::addressof(owner.indexBuffer), std::addressof(owner.vertexConstants), std::addressof(owner.pixelConstants), std::addressof(owner.sharedConstants) })
                add(*resource);
            for (const auto& upload : owner.textureUploads)
                lease(upload);
        }
        if (domain == StorageDomain::Texture)
            for (const auto& resource : owner.textures)
                add(resource);
    };
    for (const auto& owner : frameDrawSlots)
        draw(owner);
    for (const auto& owner : replayDrawSlots)
        draw(owner);
    // The resident chunks are a fixed footprint, charged whatever they hold.
    if (domain == StorageDomain::Texture && vertexResidency)
        bytes = detail::OwnedAdd(bytes, vertexResidency->FootprintBytes());
    return bytes;
}

// Count container sizes/fixed owners, not cached payloads already charged.
// Worker refreshes before retirement reaches deleter; request metadata charge
// covers growth until then.
void NativeRendererD3D12::Impl::refreshDirectStructure()
{
    std::uint64_t bytes = 0;
    const auto    add   = [&](std::uint64_t count, std::uint64_t width = 1)
    {
        bytes = detail::OwnedAdd(bytes, detail::OwnedMultiply(count, width));
    };
    constexpr std::uint64_t proof = sizeof(NativeRenderFrameTextureVersion) + sizeof(NativeRenderFrameTextureVersion::Impl);
    // A request's forecast leaves the first batch's slots and sampler tables
    // to this bound.
    add(std::max<std::size_t>(frameDrawSlots.capacity(), kFrameBatchDraws), sizeof(ReplayDrawResources));
    add(kFrameCommandBatches, kFrameSamplerTableBytes);
    add(uploadPool.BookkeepingBytes());
    add(texturePool.BookkeepingBytes());
    // The table's whole capacity, so its growth needs no request charge.
    add(uploadedVertexOwners.bucket_count(), kUploadedVertexOwnerBucket);
    add(kUploadedVertexOwners, kUploadedVertexOwnerBytes);
    if (vertexResidency)
    {
        add(vertexResidency->MetadataBytes());
        add(vertexResidency->SnapshotCeilingBytes());
    }
    add(attachmentPool.BookkeepingBytes());
    add(resolvePool.BookkeepingBytes());
    add(resolveReturns->BookkeepingBytes());
    add(versionTextures.size(), sizeof(VersionTextures::value_type) + 4 * sizeof(void*));
    add(versionTexturesOpen.capacity(), sizeof(void*));
    add(versionTextureReleases.capacity(), sizeof(VersionTextureRelease));
    add(replayHelperPipelineCache.capacity(), sizeof(ReplayHelperPipelineCacheEntry));
    for (const auto& batch : frameCommandBatches)
        add(batch.helpers.capacity(), sizeof(ReplayHelperPipelineCacheEntry));
    const auto context = [&](const auto& frame)
    {
        add(frame.frameCpuTextures.size(), sizeof(typename decltype(frame.frameCpuTextures)::value_type) + 4 * sizeof(void*));
        add(frame.frameTextures.size(), sizeof(typename decltype(frame.frameTextures)::value_type) + 4 * sizeof(void*));
        add(frame.frameTextureOwners.size(), sizeof(typename decltype(frame.frameTextureOwners)::value_type) + 4 * sizeof(void*) + sizeof(NativeFrameResourceOwner));
        add(frame.frameColorSnapshots.size(), sizeof(typename decltype(frame.frameColorSnapshots)::value_type) + 4 * sizeof(void*));
        add(frame.frameRetiredCpuTextures.capacity(), sizeof(FrameCpuTexture));
        add(frame.frameRetiredUploads.capacity(), sizeof(ComPtr<ID3D12Resource>));
        add(frame.frameRetiredUploadLeases.capacity(), sizeof(UploadLease));
        add(frame.frameColorAttachmentCache.capacity(), sizeof(ReplayColorAttachments));
        add(frame.frameDepthAttachmentCache.capacity(), sizeof(ReplayDepthAttachment));
    };
    context(*this);
    for (const auto& frame : frameContexts)
        context(frame);
    for (const auto& submitted : submittedFrames)
    {
        add(sizeof(SubmittedFrame));
        add(submitted.textures.capacity(), sizeof(NativeRenderFrameTexture) + proof);
        if (const auto& request = submitted.request)
        {
            add(sizeof(TerminalRequest) + 2 * (sizeof(NativeDrawReplayResult) + 4 * sizeof(void*)));
            if (request->ticket)
            {
                std::lock_guard lock(request->ticket->exportMutex);
                add(sizeof(NativeRenderFrameTicket));
                add(request->ticket->exports.size(), sizeof(decltype(request->ticket->exports)::value_type) + 4 * sizeof(void*) + proof);
            }
        }
    }
    add(retainedFrameTextures.size(), sizeof(RetainedFrameTexture) + 4 * sizeof(void*) + sizeof(NativeFrameResourceOwner) + proof);
    if (surfacesUsed.load(std::memory_order_acquire))
    {
        std::lock_guard lock(surfaceMutex);
        add(surfaceStore.size(), sizeof(StoredSurface) + 4 * sizeof(void*) + sizeof(NativeFrameResourceOwner));
        add(surfaceRetires.size(), sizeof(decltype(surfaceRetires)::value_type) + sizeof(NativeFrameResourceOwner));
    }
    for (const auto& cache : { &replayGuestPipelineCache, &frameGuestPipelineCache })
        add(cache->capacity(), sizeof(ReplayGuestPipelineCacheEntry));
    directStructureBytes.store(bytes, std::memory_order_relaxed);
}

void NativeRendererD3D12::Impl::refreshOwnedAdmission()
{
    if (directStream)
    {
        refreshDirectStructure();
        {
            std::lock_guard lock(stateMutex);
            ownedAdmissionSnapshotValid = true;
            ownedRefreshRequested       = false;
            publishDirectCharge();
            ++ownedAdmissionRevision;
        }
        stateCv.notify_all();
        return;
    }
    {
        std::lock_guard lock(stateMutex);
        // Admission may release staged pins on cancellation. Scan and publish
        // their identity credits under the same lock as that release.
        try
        {
            if (!ownedScanCeiling)
                ownedScanCeiling = censusOwnedBytes(true).second;
            std::string error;
            if (!ensureOwnerHeap(detail::ForecastOwnerSet(ownedScanCeiling).peak, error, true))
                throw std::bad_alloc{};
            std::optional<detail::OwnerSet> replacement;
            detail::OwnerSet*               output;
            if (ownedAdmissionOwners)
            {
                replacement.emplace(detail::OwnedAllocator<const void*>{ &ownedHeap });
                output = &*replacement;
            }
            else
            {
                ownedAdmissionOwners.emplace(detail::OwnedAllocator<const void*>{ &ownedHeap });
                output = &*ownedAdmissionOwners;
            }
            const auto bytes        = retainedStorageBytes(StorageDomain::Owned, output, true);
            ownedAdmissionFullCount = ownedLastScanFullCount;
            if (replacement)
                ownedAdmissionOwners->swap(*replacement);
            replacement.reset();
            ownedAdmissionBytes           = bytes;
            ownedAdmissionSnapshotImports = admissionImports;
            ownedAdmissionSnapshotValid   = true;
            ownedRefreshRequested         = false;
            // The peak keeps the rescanned charge before an idle collapse.
            publishOwnedCharge();
            if (submittedFrames.empty() && !drawStreamHasPrefix && !drawStreamPending && !drawStreamInFlight && !admissionImports && !ownedCapacityRequest)
            {
                const auto live = ownedHeap.live.load(std::memory_order_relaxed);
                ownedAdmissionBytes -= ownedHeap.limit - live;
                ownedHeap.limit      = live;
                ownedMaintenanceWork = 0;
            }
            publishOwnedCharge();
        }
        catch (const std::exception&)
        {
            ownedAdmissionSnapshotValid = false;
            ownedRefreshRequested       = false;
            ownedCapacityError          = "native renderer owner metadata refresh exceeds the byte bound";
        }
        ++ownedAdmissionRevision;
    }
    stateCv.notify_all();
}

void NativeRendererD3D12::Impl::serviceOwnedCapacity(std::uint64_t requested)
{
    std::string capacityError;
    if (requested)
        reserveStorage(StorageDomain::Owned, requested, capacityError);
    {
        std::lock_guard lock(stateMutex);
        ownedCapacityError = std::move(capacityError);
    }
    refreshOwnedAdmission();
}

bool NativeRendererD3D12::Impl::awaitOwnedSnapshot(std::unique_lock<std::mutex>& lock, std::string& error, bool onWorker)
{
    if (directStream)
        return true;
    while (!ownedAdmissionSnapshotValid)
    {
        ownedCapacityError.clear();
        if (onWorker)
        {
            lock.unlock();
            serviceOwnedCapacity(0);
            lock.lock();
        }
        else
        {
            const auto revision   = ownedAdmissionRevision;
            ownedRefreshRequested = true;
            notifyStream();
            stateCv.wait(lock, [&]
                         {
                             return ownedAdmissionRevision != revision || stopRequested || runtimeFailed;
                         });
        }
        if (stopRequested || runtimeFailed)
        {
            error = "native D3D12 draw stream is unavailable";
            return false;
        }
        if (!ownedCapacityError.empty())
        {
            error = ownedCapacityError;
            return false;
        }
    }
    return true;
}

bool NativeRendererD3D12::Impl::admitTerminalInputs(const NativeRenderFrame& frame, std::unique_lock<std::mutex>& lock, std::string& error, bool onWorker)
try
{
    constexpr std::uint64_t bound = 512ULL * 1024 * 1024;
    // A direct stream bounds the frame when its owned admission charges it.
    if (directStream)
        return true;
    if (!onWorker)
    {
        waitCounted(lock, [&]
                    {
                        return (!drawStreamPending && !drawStreamInFlight && !ownedAdmissionBusy && !admissionImports) ||
                               !drawStreamStarted || stopRequested || runtimeFailed;
                    });
        if (!drawStreamStarted || !initializationDone || !initializationSucceeded ||
            !initialized.load(std::memory_order_acquire) || stopRequested || runtimeFailed)
        {
            error = "native D3D12 draw stream is unavailable";
            return false;
        }
    }
    if (!awaitOwnedSnapshot(lock, error, onWorker))
        return false;
    const auto    forecast = frameMetadataReservation(frame);
    const auto    peak     = detail::ForecastOwnerSet(forecast.occurrences).peak;
    const auto    live     = ownedHeap.live.load(std::memory_order_relaxed);
    const auto    limit    = std::max(ownedHeap.limit, detail::OwnedAdd(live, peak));
    std::uint64_t inputBytes;
    if (ownedAdmissionBytes <= bound && limit - ownedHeap.limit <= bound - ownedAdmissionBytes)
    {
        ownedAdmissionBytes += limit - ownedHeap.limit;
        ownedHeap.limit = limit;
        publishOwnedCharge();
        detail::OwnerSet       owners(detail::OwnedAllocator<const void*>{ &ownedHeap });
        detail::OwnedCollector collector{ owners, nullptr, forecast.occurrences };
        detail::VisitNativeRenderFrameOwnedBytes(frame, collector.Visitor());
        inputBytes = collector.bytes;
    }
    else
    {
        inputBytes = detail::CensusOwnedBytes([&](detail::OwnedVisitor visitor)
                                              {
                                                  detail::VisitNativeRenderFrameOwnedBytes(frame, visitor);
                                              })
                         .first;
    }
    if (inputBytes > bound)
    {
        error = "native render frame retained input capacities exceed the owned byte bound";
        return false;
    }
    return true;
}
catch (const std::bad_alloc&)
{
    error = "native render frame retained input capacities exceed the owned byte bound";
    return false;
}

// A direct request's own owners, counted once each within the frame, and
// its metadata; the stream's state is not walked. Past the bound when the
// owners do not fit the owner heap.
std::uint64_t NativeRendererD3D12::Impl::directFrameBytes(const NativeRenderFrame& frame)
try
{
    const auto             forecast = frameMetadataReservation(frame, true);
    detail::OwnerSet       owners(detail::OwnedAllocator<const void*>{ &ownedHeap });
    detail::OwnedCollector collector{ owners, nullptr, detail::OwnedAdd(forecast.occurrences, detail::OwnedAdd(detail::OwnedMultiply(3, frame.textures.size()), 6)) };
    detail::VisitNativeRenderFrameOwnedBytes(frame, collector.Visitor());
    for (const auto& texture : frame.textures)
        if (texture.producerVersion)
            collector(texture.producerVersion.get(), sizeof(NativeRenderFrameTextureVersion) + sizeof(NativeRenderFrameTextureVersion::Impl));
    return detail::OwnedAdd(detail::OwnedAdd(collector.bytes, forecast.metadata),
                            detail::OwnedMultiply(frame.textures.size(), sizeof(FrameImports::value_type) + 4 * sizeof(void*)));
}
catch (const std::bad_alloc&)
{
    return kOwnedByteBound + 1;
}

// Under stateMutex, a quiet stream refuses a prefix that cannot append after
// waiting.
bool NativeRendererD3D12::Impl::admitDirect(const NativeRenderFrame& frame, FrameImports& imports, std::unique_lock<std::mutex>& lock, std::string& error, bool charge, bool refusePrefix)
{
    const auto bytes = charge ? directFrameBytes(frame) : 0;
    if (charge && !awaitDirectFit(bytes, lock, error))
        return false;
    if (refusePrefix && drawStreamHasPrefix)
    {
        error = "native D3D12 draw stream is unavailable or owns an unfinished prefix";
        return false;
    }
    if (!pinFrameImports(frame, imports, error))
        return false;
    if (charge)
        chargeDirectAdmission(bytes);
    return true;
}

// A raw draw frame's own owners, counted once each, as directFrameBytes
// counts a frame's.
std::uint64_t NativeRendererD3D12::Impl::directDrawBytes(const std::vector<NativeDrawReplayRecipe>& draws)
try
{
    detail::OwnerSet       owners(detail::OwnedAllocator<const void*>{ &ownedHeap });
    detail::OwnedCollector collector{ owners };
    collector(nullptr, detail::OwnedMultiply(draws.capacity(), sizeof(NativeDrawReplayRecipe)));
    for (const auto& draw : draws)
        detail::VisitNativeDrawOwnedBytes(draw, collector.Visitor());
    return collector.bytes;
}
catch (const std::bad_alloc&)
{
    return kOwnedByteBound + 1;
}

// Only one synchronous caller fills a released slot before unlocking
// stateMutex.
bool NativeRendererD3D12::Impl::awaitDirectFit(std::uint64_t bytes, std::unique_lock<std::mutex>& lock, std::string& error)
{
    waitCounted(lock, [&]
                {
                    return (directSyncIdle() && (directFits(bytes) || !directReleasePending())) || stopRequested || runtimeFailed || !drawStreamStarted;
                });
    if (stopRequested || runtimeFailed || !drawStreamStarted)
    {
        error = "native D3D12 draw stream is unavailable";
        return false;
    }
    if (!directFits(bytes))
    {
        error = "native direct stream frame bytes exceed the owned byte bound";
        return false;
    }
    return true;
}

// The worker takes a synchronous request's charge with it from the slot.
void NativeRendererD3D12::Impl::chargeDirectAdmission(std::uint64_t bytes)
{
    directChargeBytes.fetch_add(bytes, std::memory_order_relaxed);
    directAdmittedBytes += bytes;
    publishDirectCharge();
}

bool NativeRendererD3D12::Impl::awaitOwnedAdmission(const NativeRenderFrame& frame, FrameImports& imports, std::unique_lock<std::mutex>& lock, std::string& error, bool terminalTicket, bool onWorker, bool refusePrefix)
try
{
    constexpr std::uint64_t bound = 512ULL * 1024 * 1024;
    if (!onWorker)
        stateCv.wait(lock, [&]
                     {
                         return (!drawStreamPending && !drawStreamInFlight && !ownedAdmissionBusy && !admissionImports) || stopRequested || runtimeFailed || !drawStreamStarted;
                     });
    if (stopRequested || runtimeFailed || !drawStreamStarted)
    {
        error = "native D3D12 draw stream is unavailable";
        return false;
    }

    // A guest admission needs the stream quiet; queued enqueues wait it out.
    struct GuestAdmission
    {
        Impl* renderer;

        ~GuestAdmission()
        {
            if (renderer && !--renderer->guestAdmissions)
                renderer->stateCv.notify_all();
        }
    } guestAdmission{ onWorker ? nullptr : this };

    if (!onWorker)
        ++guestAdmissions;
    if (frame.typedGuestWords && !castableGuestWords.load(std::memory_order_relaxed))
    {
        error = "render frame typed guest-word views need castable storage the device lacks";
        return false;
    }
    if (directStream)
        return admitDirect(frame, imports, lock, error, !onWorker, refusePrefix);
    if (!awaitOwnedSnapshot(lock, error, onWorker))
        return false;
    const auto forecast          = frameMetadataReservation(frame);
    const auto projectionCeiling = detail::OwnedAdd(forecast.occurrences, detail::OwnedAdd(detail::OwnedMultiply(3, frame.textures.size()), 6));
    const auto projectionPeak    = detail::ForecastOwnerSet(projectionCeiling).peak;
    const auto pinBytes          = detail::OwnedMultiply(frame.textures.size(), sizeof(FrameImports::value_type) + 4 * sizeof(void*));
    const auto stageCeiling      = detail::OwnedAdd(ownedAdmissionFullCount, detail::OwnedMultiply(3, frame.textures.size()));
    const auto stage             = detail::ForecastOwnerSet(stageCeiling);
    const auto priorLimit        = ownedHeap.limit;
    const auto priorWork         = ownedMaintenanceWork;
    const auto persistent        = ownedHeap.live.load(std::memory_order_relaxed);
    const auto snapshotBytes     = ownedAdmissionOwners ? detail::OwnerSetBytes(ownedAdmissionOwners->size(), ownedAdmissionOwners->bucket_count()) : 0;
    const auto stageGrowth       = stage.bytes > snapshotBytes ? stage.bytes - snapshotBytes : 0;
    const auto preflight         = std::max(detail::OwnedAdd(projectionPeak, pinBytes), detail::OwnedAdd(stage.peak, stageGrowth));
    const auto initialLimit      = std::max(priorLimit, detail::OwnedAdd(persistent, preflight));
    if (ownedAdmissionBytes > bound || initialLimit - priorLimit > bound - ownedAdmissionBytes)
    {
        error = "native renderer admission metadata peak exceeds the byte bound";
        return false;
    }
    ownedAdmissionBytes += initialLimit - priorLimit;
    ownedHeap.limit      = initialLimit;
    ownedMaintenanceWork = std::max(priorWork, stage.peak);
    ownedScanCeiling     = std::max(ownedScanCeiling, stageCeiling);
    publishOwnedCharge();
    if (!pinFrameImports(frame, imports, error))
        return false;
    // This caller owns the single admission slot. Its local map stays alive
    // while capacity eviction and quiet FIFO retirement run on the worker.
    admissionImports = &imports;

    struct ReleaseAdmission
    {
        Impl& renderer;

        ~ReleaseAdmission()
        {
            renderer.admissionImports              = nullptr;
            renderer.ownedAdmissionSnapshotImports = nullptr;
            renderer.ownedAdmissionSnapshotValid   = false;
            if (renderer.ownedAdmissionOwners)
                renderer.ownedAdmissionOwners->clear();
            renderer.ownedCapacityRequest = 0;
            renderer.notifyStream();
        }
    } release{ *this };

    std::string capacityError;
    for (;;)
    {
        if (!onWorker)
            stateCv.wait(lock, [&]
                         {
                             return (!drawStreamPending && !drawStreamInFlight && !ownedAdmissionBusy) || stopRequested || runtimeFailed || !drawStreamStarted;
                         });
        if (stopRequested || runtimeFailed || !drawStreamStarted)
        {
            error = "native D3D12 draw stream is unavailable";
            return false;
        }
        if (!ownedAdmissionSnapshotValid)
        {
            error = ownedCapacityError.empty() ? "native renderer owner metadata refresh exceeds the byte bound" : ownedCapacityError;
            return false;
        }
        std::uint64_t stagedBytes = 0;
        std::uint64_t requested   = 0;
        {
            if (!ensureOwnerHeap(projectionPeak, error, true, false))
                return false;
            detail::OwnerSet       owners(detail::OwnedAllocator<const void*>{ &ownedHeap });
            detail::OwnedCollector collector{ owners, &*ownedAdmissionOwners, projectionCeiling };
            if (ownedAdmissionSnapshotImports != &imports)
            {
                stagedBytes = detail::OwnedMultiply(imports.size(), sizeof(FrameImports::value_type) + 4 * sizeof(void*));
                collector(nullptr, stagedBytes);
                for (const auto& [id, imported] : imports)
                {
                    if (imported.owner)
                        collector(imported.owner.get(), sizeof(NativeFrameResourceOwner));
                    if (imported.descriptor.producerVersion)
                        collector(imported.descriptor.producerVersion.get(), sizeof(NativeRenderFrameTextureVersion) + sizeof(NativeRenderFrameTextureVersion::Impl));
                }
                stagedBytes = collector.bytes;
            }
            detail::VisitNativeRenderFrameOwnedBytes(frame, collector.Visitor());
            for (const auto& texture : frame.textures)
            {
                if (texture.producerVersion)
                    collector(texture.producerVersion.get(), sizeof(NativeRenderFrameTextureVersion) + sizeof(NativeRenderFrameTextureVersion::Impl));
            }
            auto bytes                 = detail::OwnedAdd(collector.bytes, terminalTicket ? sizeof(TerminalRequest) + sizeof(NativeRenderFrameTicket) + 2 * (sizeof(NativeDrawReplayResult) + 4 * sizeof(void*)) : sizeof(NativeDrawReplayResult) + 4 * sizeof(void*));
            bytes                      = detail::OwnedAdd(bytes, forecast.metadata);
            const auto count           = detail::OwnedAdd(ownedAdmissionFullCount, detail::OwnedAdd(owners.size(), detail::OwnedAdd(detail::OwnedMultiply(2, forecast.cpuKeys), detail::OwnedAdd(detail::OwnedMultiply(2, frame.textures.size()), 6))));
            const auto future          = detail::ForecastOwnerSet(count);
            const auto currentSnapshot = detail::OwnerSetBytes(ownedAdmissionOwners->size(), ownedAdmissionOwners->bucket_count());
            const auto growth          = detail::OwnedAdd(future.bytes > currentSnapshot ? future.bytes - currentSnapshot : 0, forecast.prefixGrowth);
            const auto work            = std::max(future.peak, forecast.prefixPeak);
            const auto persistentHeap  = ownedHeap.live.load(std::memory_order_relaxed) - detail::OwnerSetBytes(owners.size(), owners.bucket_count());
            const auto envelope        = std::max(projectionPeak, detail::OwnedAdd(work, growth));
            const auto nextLimit       = detail::OwnedAdd(persistentHeap, envelope);
            const auto baseline        = ownedAdmissionBytes - ownedHeap.limit;
            if (baseline <= bound && bytes <= bound - baseline && nextLimit <= bound - baseline - bytes)
            {
                ownedAdmissionBytes  = detail::OwnedAdd(baseline, nextLimit);
                ownedHeap.limit      = nextLimit;
                ownedScanCeiling     = count;
                ownedMaintenanceWork = work;
                publishOwnedCharge();
                return true;
            }
            requested = detail::OwnedAdd(bytes - stagedBytes, nextLimit > ownedHeap.limit ? nextLimit - ownedHeap.limit : 0);
        }
        // Recompute unique owners after a worker refresh before accepting its
        // scalar failure: staged pins may already be included in the snapshot.
        if (!capacityError.empty())
        {
            error = std::move(capacityError);
            return false;
        }
        const auto revision      = ownedAdmissionRevision;
        const auto live          = ownedHeap.live.load(std::memory_order_relaxed);
        const auto retainedLimit = std::max(priorLimit, detail::OwnedAdd(live, detail::OwnedAdd(stage.peak, stageGrowth)));
        if (retainedLimit < ownedHeap.limit)
        {
            ownedAdmissionBytes -= ownedHeap.limit - retainedLimit;
            ownedHeap.limit      = retainedLimit;
            ownedMaintenanceWork = priorWork;
            publishOwnedCharge();
        }
        ownedCapacityError.clear();
        if (onWorker)
        {
            lock.unlock();
            serviceOwnedCapacity(requested);
            lock.lock();
        }
        else
        {
            ownedCapacityRequest  = requested;
            ownedRefreshRequested = true;
            notifyStream();
            stateCv.wait(lock, [&]
                         {
                             return ownedAdmissionRevision != revision || stopRequested || runtimeFailed;
                         });
        }
        capacityError = ownedCapacityError;
    }
}

catch (const std::bad_alloc&)
{
    error = "native renderer admission metadata peak exceeds the byte bound";
    return false;
}

// requestOnly reads no renderer state, for callers off the worker: it charges
// no first batch of draw slots, which a direct stream's structure bound
// holds, and forecasts the prefix state as fresh.
NativeRendererD3D12::Impl::FrameOwnerForecast NativeRendererD3D12::Impl::frameMetadataReservation(const NativeRenderFrame& frame, bool requestOnly) const
{
    // Reserve upcoming nodes and worst retained vector growth before recording.
    // Payloads are charged by the unique-owner scanner, independently of nodes.
    FrameOwnerForecast forecast;
    const auto         textureNodes = sizeof(RetainedFrameTexture) * 2 + sizeof(NativeRenderFrameTexture) * 4 +
                                      sizeof(NativeRenderFrameTextureVersion) + sizeof(NativeRenderFrameTextureVersion::Impl) +
                                      sizeof(NativeFrameResourceOwner) + 24 * sizeof(void*);
    std::uint64_t      bytes        = sizeof(SubmittedFrame) + sizeof(TerminalRequest) + sizeof(NativeRenderFrameTicket) +
                                      2 * (sizeof(NativeDrawReplayResult) + 4 * sizeof(void*)) +
                                      (requestOnly || frameDrawSlots.capacity() ? 0 : kFrameBatchDraws * sizeof(ReplayDrawResources) + kFrameCommandBatches * kFrameSamplerTableBytes);
    bytes                           = detail::OwnedAdd(bytes, detail::OwnedMultiply(frame.textures.size(), textureNodes));
    bytes                           = detail::OwnedAdd(bytes, detail::OwnedMultiply(frame.passes.size(), 2 * sizeof(std::string) + 80));
    const auto attachment           = [&]()
    {
        bytes = detail::OwnedAdd(bytes, 2 * sizeof(ReplayColorAttachments) + 2 * sizeof(ReplayDepthAttachment));
    };
    if (frame.outputTextureId && !frame.retainOnly)
        attachment();
    for (const auto& pass : frame.passes)
    {
        attachment();
        bytes = detail::OwnedAdd(bytes, detail::OwnedMultiply(pass.resolves.size(), 2 * sizeof(std::pair<std::uint32_t, std::uint32_t>)));
        // Forecast physical storage without validating it. Queued prefixes
        // report malformed draw storage through their recording future.
        const auto addDraw = [&](const NativeDrawReplayRecipe& draw, const NativeDrawReplayAdmission* admission)
        {
            forecast.occurrences = detail::OwnedAdd(forecast.occurrences, detail::OwnedAdd((admission ? 7 : 6) + (draw.sharedIndices ? 1 : 0), detail::OwnedMultiply(3, draw.textures.size())));
            if (!admission || !draw.shaderVersion)
                forecast.legacyKeyCapacity = std::max(forecast.legacyKeyCapacity, replayGuestPipelineKeyCapacity(draw));
            const auto node = [&](const void* owner)
            {
                if (owner)
                    bytes = detail::OwnedAdd(bytes, sizeof(std::shared_ptr<const void>) + 8 * sizeof(void*));
            };
            node(admission);
            node(draw.shaderVersion.get());
            for (const auto* owner : { draw.sharedVertexData.get(), draw.sharedVertexShaderDxil.get(), draw.sharedPixelShaderDxil.get() })
                node(owner);
            node(draw.sharedIndices.get());
            for (std::size_t slot = 0; slot < draw.textures.size(); ++slot)
            {
                const auto& texture = draw.textures[slot];
                node(texture.textureVersion.get());
                node(texture.sharedBytes.get());
                if (!texture.frameTextureId && (texture.width || slot < static_cast<std::size_t>(std::popcount(draw.textureMask))))
                {
                    forecast.cpuKeys = detail::OwnedAdd(forecast.cpuKeys, 1);
                    bytes            = detail::OwnedAdd(bytes, sizeof(FrameCpuTextureKey) + 2 * sizeof(FrameCpuTexture) + 4 * sizeof(void*) + sizeof(ReplayTexturePreparation));
                    bytes            = detail::OwnedAdd(bytes, detail::OwnedMultiply(detail::OwnedMultiply(texture.mipLevels, texture.dimension == NativeDrawReplayTextureDimension::TextureCube ? 6 : 1), sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) + sizeof(UINT) + sizeof(UINT64)));
                }
            }
        };
        for (const auto& draw : pass.draws)
            addDraw(draw, nullptr);
        for (const auto& admission : pass.admittedDraws)
            if (admission)
                addDraw(admission->Recipe(), admission.get());
    }
    forecast.metadata = detail::OwnedAdd(bytes, forecast.legacyKeyCapacity);
    if (!requestOnly && recordedRenderPrefix)
        recordedRenderPrefix->validation.ForecastOwnedHeap(frame, forecast.prefixGrowth, forecast.prefixPeak);
    else
    {
        forecast.metadata   = detail::OwnedAdd(forecast.metadata, NativeRenderFramePrefixState::OwnedMetadataBytes());
        std::uint64_t draws = 0, textures = 0;
        for (const auto& pass : frame.passes)
        {
            const auto addDraw = [&](const NativeDrawReplayRecipe& draw)
            {
                draws    = detail::OwnedAdd(draws, 1);
                textures = detail::OwnedAdd(textures, draw.textures.size());
            };
            for (const auto& draw : pass.draws)
                addDraw(draw);
            for (const auto& admission : pass.admittedDraws)
                if (admission)
                    addDraw(admission->Recipe());
        }
        const auto fresh = [&](std::uint64_t additions, std::uint64_t node)
        {
            const auto value      = detail::ForecastOwnerSet(0, 8, additions, node);
            forecast.prefixGrowth = detail::OwnedAdd(forecast.prefixGrowth, value.bytes);
            forecast.prefixPeak   = detail::OwnedAdd(forecast.prefixPeak, value.peak);
        };
        fresh(frame.textures.size(), 24);
        fresh(detail::OwnedAdd(detail::OwnedMultiply(3, draws), textures), 32);
        fresh(draws, 32);
        fresh(draws, 32);
        fresh(draws, 32);
        fresh(textures, 32);
        fresh(frame.passes.size(), 24);
    }
    return forecast;
}

bool NativeRendererD3D12::Impl::reserveStorage(StorageDomain domain, std::uint64_t bytes, std::string& error)
{
    constexpr std::uint64_t bound = 512ULL * 1024 * 1024;
    const auto              fits  = [&]()
    {
        std::uint64_t retained;
        try
        {
            retained = retainedStorageBytes(domain);
        }
        catch (const std::bad_alloc&)
        {
            if (domain != StorageDomain::Owned)
                throw;
            retained = detail::OwnedAdd(censusOwnedBytes().first, ownedHeap.limit);
        }
        return retained <= bound && bytes <= bound - retained;
    };
    if (fits())
        return true;
    cancelRecordingCpuReservation();
    const auto evictIdle = [&]()
    {
        for (std::size_t index = 0; index < frameContexts.size(); ++index)
            if (index != activeFrameContext && std::none_of(submittedFrames.begin(), submittedFrames.end(), [&](const auto& submitted)
                                                            {
                                                                return submitted.context == index;
                                                            }))
            {
                uncountCpuTextures(frameContexts[index], false);
                frameContexts[index] = FrameContext{};
            }
        if (domain == StorageDomain::Upload)
            uploadPool.ReleaseIdle();
        // Under pressure every stored version no open batch uses leaves.
        if (domain == StorageDomain::Texture)
        {
            trimVersionTextures(0);
            releaseVersionTextures();
        }
        if (domain == StorageDomain::Texture || domain == StorageDomain::Attachment)
        {
            auto&      pool      = domain == StorageDomain::Texture ? texturePool : attachmentPool;
            const auto completed = fence ? fence->GetCompletedValue() : 0;
            if (completed != UINT64_MAX)
                pool.Reclaim(completed);
            pool.ReleaseIdle();
        }
        if (domain == StorageDomain::Texture)
        {
            // Under pressure a returned destination is released, as before pooling.
            resolveReturns->Drain(resolvePool, 0, [](ComPtr<ID3D12Resource>&) noexcept
                                  {
                                      return false;
                                  });
            const auto completed = fence ? fence->GetCompletedValue() : 0;
            if (completed != UINT64_MAX)
                resolvePool.Reclaim(completed);
            resolvePool.ReleaseIdle();
        }
    };
    evictIdle();
    const auto evictCompleted = [&]()
    {
        if (domain == StorageDomain::Owned)
            for (auto* cache : { &replayGuestPipelineCache, &frameGuestPipelineCache })
            {
                std::erase_if(*cache, [&](const auto& entry)
                              {
                                  const auto pinned = [&](const auto& draw)
                                  {
                                      return draw.pipeline.Get() == entry.pipeline.Get();
                                  };
                                  const bool evict = !std::any_of(frameDrawSlots.begin(), frameDrawSlots.end(), pinned) &&
                                                     !std::any_of(replayDrawSlots.begin(), replayDrawSlots.end(), pinned);
                                  if (evict)
                                      pipelineCacheBytes.fetch_sub(entry.OwnedBytes(), std::memory_order_relaxed);
                                  return evict;
                              });
                if (cache->empty())
                    std::vector<ReplayGuestPipelineCacheEntry>{}.swap(*cache);
            }
        if (domain == StorageDomain::Texture || domain == StorageDomain::Owned)
            std::erase_if(retainedFrameTextures, [](const auto& entry)
                          {
                              return entry.second.owner && entry.second.owner.use_count() == 1;
                          });
        if (domain == StorageDomain::Texture)
            resolveReturns->Drain(resolvePool, 0, [](ComPtr<ID3D12Resource>&) noexcept
                                  {
                                      return false;
                                  });
    };
    if (!fits())
        evictCompleted();
    while (!fits() && !submittedFrames.empty())
    {
        if (!retireSubmittedFrames(true, error, true))
            return false;
        evictIdle();
        if (!fits())
            evictCompleted();
    }
    if (fits())
        return true;
    error = "native renderer aggregate retained storage exceeds the byte bound";
    return false;
}

std::shared_ptr<NativeFrameResourceOwner> NativeRendererD3D12::Impl::resolveImportOwner(const NativeRenderFrameTexture& texture, std::string& error)
{
    std::shared_ptr<NativeFrameResourceOwner> owner;
    if (!texture.producerVersion && surfacesUsed.load(std::memory_order_acquire))
    {
        std::lock_guard lock(surfaceMutex);
        if (const auto found = surfaceStore.find(texture.persistentKey); found != surfaceStore.end())
        {
            const auto& stored = found->second.descriptor;
            if (stored.width != texture.width || stored.height != texture.height || stored.format != texture.format || !found->second.owner || !found->second.owner->resource ||
                (texture.importedWords != NativeGuestWordPacking::None && !found->second.owner->castable))
            {
                error = "native surface import does not match its stored texture";
                return {};
            }
            return found->second.owner;
        }
    }
    if (texture.producerVersion)
    {
        const auto& version    = texture.producerVersion->impl;
        const auto  canonical  = version ? version->canonical.lock() : nullptr;
        const auto  generation = version ? version->generation.lock() : nullptr;
        if (!version || !canonical || canonical.get() != texture.producerVersion.get() ||
            canonical.owner_before(texture.producerVersion) || texture.producerVersion.owner_before(canonical) ||
            generation != rendererGeneration || version->queue != queue.Get() || !version->fenceValue ||
            version->fenceValue >= nextFenceValue || version->descriptor.persistentKey != texture.persistentKey ||
            version->descriptor.width != texture.width || version->descriptor.height != texture.height || version->descriptor.format != texture.format ||
            !(owner = version->resource.lock()) || !owner->resource)
        {
            error = "native texture import has no exact same-queue producer proof";
            return {};
        }
    }
    else
    {
        const auto found = retainedFrameTextures.find(texture.persistentKey);
        if (found == retainedFrameTextures.end() || found->second.descriptor.width != texture.width ||
            found->second.descriptor.height != texture.height || found->second.descriptor.format != texture.format)
        {
            error = "native texture import has no matching completed producer version";
            return {};
        }
        owner = found->second.owner;
    }
    if (!owner || !owner->resource)
    {
        error = "native texture import resource expired";
        return {};
    }
    return owner;
}

bool NativeRendererD3D12::Impl::pinFrameImports(const NativeRenderFrame& frame, FrameImports& imports, std::string& error)
{
    // Prefix admission precedes declaration validation on the worker.
    if (frame.textures.size() > NativeRenderBatchCommands)
    {
        for (const auto& texture : frame.textures)
            if (texture.imported && frameUsesTexture(frame, texture.id))
            {
                auto owner = resolveImportOwner(texture, error);
                if (!owner)
                    return false;
                imports.emplace(texture.id, PinnedFrameTexture{ texture, std::move(owner) });
            }
        return true;
    }

    std::array<std::pair<std::uint64_t, bool>, NativeRenderBatchCommands> references{};
    std::size_t                                                           referenceCount = 0;
    for (const auto& texture : frame.textures)
        if (texture.imported)
            references[referenceCount++].first = texture.id;
    if (!referenceCount)
        return true;
    const auto referenceEnd = references.begin() + referenceCount;
    std::sort(references.begin(), referenceEnd, [](const auto& left, const auto& right)
              {
                  return left.first < right.first;
              });
    const auto findReference = [&](std::uint64_t id)
    {
        return std::lower_bound(references.begin(), referenceEnd, id, [](const auto& reference, std::uint64_t value)
                                {
                                    return reference.first < value;
                                });
    };
    const auto require = [&](std::uint64_t id)
    {
        const auto found = findReference(id);
        if (found != referenceEnd && found->first == id)
            found->second = true;
    };
    require(frame.outputTextureId);
    for (const auto& pass : frame.passes)
        for (std::size_t index = 0; index < pass.DrawCount(); ++index)
            for (const auto& texture : pass.Draw(index).textures)
                require(texture.frameTextureId);
    for (const auto& texture : frame.textures)
        if (texture.imported && findReference(texture.id)->second)
        {
            auto owner = resolveImportOwner(texture, error);
            if (!owner)
                return false;
            imports.emplace(texture.id, PinnedFrameTexture{ texture, std::move(owner) });
        }
    return true;
}

bool NativeRendererD3D12::Impl::importFrameTexture(const NativeRenderFrameTexture& texture, std::string& error)
{
    std::shared_ptr<NativeFrameResourceOwner> owner;
    if (const auto found = recordingImports.find(texture.id); found != recordingImports.end())
    {
        if (found->second.descriptor != texture)
        {
            error = "native texture import changed its admitted descriptor";
            return false;
        }
        owner = found->second.owner;
    }
    else
        owner = resolveImportOwner(texture, error);
    if (!owner || !owner->resource)
        return false;
    frameTextures.emplace(texture.id, owner->resource);
    frameTextureOwners.emplace(texture.id, std::move(owner));
    return true;
}

void NativeRendererD3D12::Impl::exportFrameTextures(const NativeRenderFrame& frame, const std::shared_ptr<TerminalRequest>& request)
{
    if (!request)
        return;
    std::lock_guard lock(request->ticket->exportMutex);
    for (const auto& texture : frame.textures)
    {
        if (!texture.persistentKey || !frameTextureOwners.contains(texture.id))
            continue;
        if (texture.imported && texture.producerVersion)
        {
            request->ticket->exports.emplace(texture.persistentKey, texture.producerVersion);
            continue;
        }
        auto version              = std::shared_ptr<NativeRenderFrameTextureVersion>(new NativeRenderFrameTextureVersion);
        version->impl             = std::make_shared<NativeRenderFrameTextureVersion::Impl>();
        version->impl->descriptor = texture;
        version->impl->descriptor.producerVersion.reset();
        version->impl->generation = rendererGeneration;
        version->impl->queue      = queue.Get();
        version->impl->fenceValue = replayFenceValue;
        version->impl->resource   = frameTextureOwners.at(texture.id);
        version->impl->canonical  = version;
        request->ticket->exports.emplace(texture.persistentKey, std::move(version));
    }
}

void NativeRendererD3D12::Impl::storeFrameSurfaces(const NativeRenderFrame& frame)
{
    if (frame.surfaces.empty() && frame.releasedSurfaces.empty())
        return;
    std::lock_guard lock(surfaceMutex);
    // Work recorded so far may still sample an owner leaving the store.
    const auto retire = [&](std::shared_ptr<NativeFrameResourceOwner> owner)
    {
        if (owner)
        {
            surfaceRetires.emplace_back(nextFenceValue, std::move(owner));
            surfacesRetiring.store(true, std::memory_order_relaxed);
        }
    };
    for (const auto& surface : frame.surfaces)
    {
        const auto owner   = frameTextureOwners.find(surface.textureId);
        const auto texture = std::find_if(frame.textures.begin(), frame.textures.end(), [&](const NativeRenderFrameTexture& declared)
                                          {
                                              return declared.id == surface.textureId;
                                          });
        // An assembled surface was stored as its tiles were recorded.
        if (surface.width || owner == frameTextureOwners.end() || !owner->second || texture == frame.textures.end() || texture->imported)
            continue;
        auto& stored = surfaceStore[surface.key];
        retire(std::exchange(stored.owner, owner->second));
        stored.descriptor = *texture;
        surfacesUsed.store(true, std::memory_order_release);
    }
    for (const auto key : frame.releasedSurfaces)
        if (const auto found = surfaceStore.find(key); found != surfaceStore.end())
        {
            retire(std::move(found->second.owner));
            surfaceStore.erase(found);
        }
}

// The stored texture of each pending version is created at its first tile
// and written only by its tiles' copies, in recording order; draws import it
// once the registry published the version, after every tile recorded.
bool NativeRendererD3D12::Impl::recordSurfaceAssembly(const NativeRenderFrameSurface& surface, const NativeRenderFrameTexture& tile, FrameCommandBatch& owner, std::string& error)
{
    const auto source = frameTextures.find(tile.id);
    if (source == frameTextures.end() || !source->second || tile.imported || surface.x > surface.width || tile.width > surface.width - surface.x ||
        surface.y > surface.height || tile.height > surface.height - surface.y)
    {
        error = "native surface assembly does not fit its stored texture";
        return false;
    }
    auto descriptor          = tile;
    descriptor.width         = surface.width;
    descriptor.height        = surface.height;
    descriptor.persistentKey = surface.key;
    std::shared_ptr<NativeFrameResourceOwner> stored;
    {
        std::lock_guard lock(surfaceMutex);
        if (const auto found = surfaceStore.find(surface.key); found != surfaceStore.end() && found->second.owner && found->second.owner->resource &&
                                                               found->second.descriptor.width == descriptor.width && found->second.descriptor.height == descriptor.height &&
                                                               found->second.descriptor.format == descriptor.format)
            stored = found->second.owner;
    }
    auto&      commandList = owner.commandList;
    const auto shaderState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto transition  = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        commandList->ResourceBarrier(1, &barrier);
    };
    const bool fresh = !stored;
    if (fresh)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = descriptor.width;
        desc.Height           = descriptor.height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format                            = mapTextureFormat(descriptor.format);
        desc.SampleDesc.Count                  = 1;
        // Outside surfaceMutex: making room may retire stored surfaces.
        if (!reserveStorage(StorageDomain::Texture, device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes, error))
            return false;
        stored             = std::make_shared<NativeFrameResourceOwner>();
        stored->serial     = ++textureResourceSerial;
        stored->castable   = descriptor.format == NativeDrawReplayTextureFormat::R32Uint && castingDevice;
        const auto created = [&]()
        {
            if (!stored->castable)
                return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&stored->resource)));
            D3D12_RESOURCE_DESC1 desc1{};
            desc1.Dimension        = desc.Dimension;
            desc1.Width            = desc.Width;
            desc1.Height           = desc.Height;
            desc1.DepthOrArraySize = desc.DepthOrArraySize;
            desc1.MipLevels        = desc.MipLevels;
            desc1.Format           = desc.Format;
            desc1.SampleDesc       = desc.SampleDesc;
            return SUCCEEDED(castingDevice->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, static_cast<UINT32>(kGuestWordCastFormats.size()), kGuestWordCastFormats.data(), IID_PPV_ARGS(&stored->resource)));
        };
        if (!created())
        {
            error = "could not create native surface assembly texture";
            return false;
        }
        stored->resourceStorageBytes = resourceStorageBytes(device.Get(), stored->resource.Get());
        // COMMON is the one layout legacy barriers share with enhanced ones.
        if (stored->castable)
            transition(stored->resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    else
        transition(stored->resource.Get(), shaderState, D3D12_RESOURCE_STATE_COPY_DEST);
    transition(source->second.Get(), shaderState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source->second.Get();
    from.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = stored->resource.Get();
    to.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    commandList->CopyTextureRegion(&to, surface.x, surface.y, 0, &from, nullptr);
    transition(source->second.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, shaderState);
    transition(stored->resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, shaderState);
    if (fresh)
    {
        std::lock_guard lock(surfaceMutex);
        auto&           entry = surfaceStore[surface.key];
        // Work recorded so far may still sample an owner leaving the store.
        if (auto replaced = std::exchange(entry.owner, std::move(stored)))
        {
            surfaceRetires.emplace_back(nextFenceValue, std::move(replaced));
            surfacesRetiring.store(true, std::memory_order_relaxed);
        }
        entry.descriptor = descriptor;
        surfacesUsed.store(true, std::memory_order_release);
    }
    return true;
}

bool NativeRendererD3D12::Impl::recordSurfaceConversion(const NativeRenderFrameSurface& surface, const NativeRenderFrameTexture& tile, ReplayDrawResources& resources, FrameCommandBatch& owner, std::string& error)
{
#if REREVVED_DEVELOPER
    const auto started = std::chrono::steady_clock::now();
#endif
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    const auto source = frameTextures.find(tile.id);
    const bool placed = surface.width != 0;
    const auto x = placed ? surface.x : 0, y = placed ? surface.y : 0;
    const auto width = placed ? surface.width : tile.width, height = placed ? surface.height : tile.height;
    if (source == frameTextures.end() || !source->second || tile.imported || tile.format != NativeDrawReplayTextureFormat::R32Uint || x > width ||
        tile.width > width - x || y > height || tile.height > height - y)
        return fail("native surface depth conversion does not fit its stored texture");
    NativeRenderFrameTexture                  descriptor{ tile.id, width, height, NativeDrawReplayTextureFormat::R32Float, surface.depthKey };
    std::shared_ptr<NativeFrameResourceOwner> stored;
    if (placed)
    {
        std::lock_guard lock(surfaceMutex);
        if (const auto found = surfaceStore.find(surface.depthKey); found != surfaceStore.end() && found->second.owner && found->second.owner->resource &&
                                                                    found->second.descriptor.width == width && found->second.descriptor.height == height &&
                                                                    found->second.descriptor.format == descriptor.format)
            stored = found->second.owner;
    }
    if (!ensureFrameGuestWordRootSignature(error))
        return false;
    if (!surfaceDepthPipeline)
    {
        if (!compileCachedReplayShader(kReplayCopyShaderSource, "copy_vs", "vs_5_1", replayCopyVertexShader) ||
            !compileCachedReplayShader(kSurfaceDepthShaderSource, "convert_depth", "ps_5_1", surfaceDepthShader))
            return fail("could not compile native surface depth conversion shader");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature                                   = frameGuestWordRootSignature.Get();
        desc.VS                                               = { replayCopyVertexShader->GetBufferPointer(), replayCopyVertexShader->GetBufferSize() };
        desc.PS                                               = { surfaceDepthShader->GetBufferPointer(), surfaceDepthShader->GetBufferSize() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask                                       = D3D12_DEFAULT_SAMPLE_MASK;
        desc.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable                  = TRUE;
        desc.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets                                 = 1;
        desc.RTVFormats[0]                                    = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc                                       = { 1, 0 };
        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&surfaceDepthPipeline))))
            return fail("could not create native surface depth conversion pipeline");
        ++replayPipelineCreations;
    }
    auto&      commandList = owner.commandList;
    const auto shaderState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto transition  = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        commandList->ResourceBarrier(1, &barrier);
    };
    const bool fresh = !stored;
    if (fresh)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = width;
        desc.Height           = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format                            = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count                  = 1;
        desc.Flags                             = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // Outside surfaceMutex: making room may retire stored surfaces.
        if (!reserveStorage(StorageDomain::Texture, device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes, error))
            return false;
        stored         = std::make_shared<NativeFrameResourceOwner>();
        stored->serial = ++textureResourceSerial;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&stored->resource))))
            return fail("could not create native surface depth conversion texture");
        stored->resourceStorageBytes = resourceStorageBytes(device.Get(), stored->resource.Get());
    }
    else
        transition(stored->resource.Get(), shaderState, D3D12_RESOURCE_STATE_RENDER_TARGET);

    struct Constants
    {
        std::uint32_t sampleA, sampleB, packing, padding;
        std::int32_t  offsetX, offsetY;
    };

    const Constants       constants{ 0, 0, 0, 0, static_cast<std::int32_t>(x), static_cast<std::int32_t>(y) };
    constexpr std::size_t constantBytes = 256;
    const auto            limit         = frameUploadArena ? frameUploadArena.bytes() : 0;
    if (!frameUploadMapped || frameUploadCursor > limit || constantBytes > limit - frameUploadCursor)
        return fail("native surface depth conversion constants exceed frame upload storage");
    auto* mapped = frameUploadMapped + frameUploadCursor;
    std::memset(mapped, 0, constantBytes);
    std::memcpy(mapped, &constants, sizeof(constants));
    const auto constantAddress = frameUploadArena.buffer().gpu + frameUploadCursor;
    frameUploadCursor += constantBytes;
    if (!ensureReplayDescriptorHeaps(resources))
        return fail("could not create native surface depth conversion descriptors");
    resources.pipeline = surfaceDepthPipeline;
    commandList->SetPipelineState(surfaceDepthPipeline.Get());
    const auto                      srvHandle = resources.viewCpu;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R32_UINT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(source->second.Get(), &srv, srvHandle);
    // The table's second slot stays a valid null view.
    auto unused = srvHandle;
    unused.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateShaderResourceView(nullptr, &srv, unused);
    const auto rtvHandle = replayRtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(stored->resource.Get(), nullptr, rtvHandle);
    bindDrawHeaps(commandList.Get(), resources);
    commandList->SetGraphicsRootSignature(frameGuestWordRootSignature.Get());
    commandList->SetGraphicsRootDescriptorTable(0, resources.viewGpu);
    commandList->SetGraphicsRootConstantBufferView(1, constantAddress);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{ float(x), float(y), float(tile.width), float(tile.height), 0, 1 };
    const D3D12_RECT     scissor{ LONG(x), LONG(y), LONG(x + tile.width), LONG(y + tile.height) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
    transition(stored->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, shaderState);
    if (fresh)
    {
        std::lock_guard lock(surfaceMutex);
        auto&           entry = surfaceStore[surface.depthKey];
        // Work recorded so far may still sample an owner leaving the store.
        if (auto replaced = std::exchange(entry.owner, std::move(stored)))
        {
            surfaceRetires.emplace_back(nextFenceValue, std::move(replaced));
            surfacesRetiring.store(true, std::memory_order_relaxed);
        }
        entry.descriptor = descriptor;
        surfacesUsed.store(true, std::memory_order_release);
    }
#if REREVVED_DEVELOPER
    surfaceConversions.fetch_add(1, std::memory_order_relaxed);
    surfaceConversionNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()), std::memory_order_relaxed);
#endif
    return true;
}

void NativeRendererD3D12::Impl::retireSurfaces()
{
    if (!surfacesRetiring.load(std::memory_order_relaxed))
        return;
    std::vector<std::shared_ptr<NativeFrameResourceOwner>> retired;
    {
        std::lock_guard lock(surfaceMutex);
        const auto      completed = fence ? fence->GetCompletedValue() : 0;
        try
        {
            while (!surfaceRetires.empty() && completed != UINT64_MAX && completed >= surfaceRetires.front().first)
            {
                retired.push_back(std::move(surfaceRetires.front().second));
                surfaceRetires.pop_front();
            }
        }
        catch (const std::bad_alloc&)
        {
            // Without room to carry them out, the rest wait for a later pass.
        }
        surfacesRetiring.store(!surfaceRetires.empty(), std::memory_order_relaxed);
    }
    // Their releases, or posts back to the resolve pool, run outside the lock.
    retired.clear();
    drainResolveReturns();
}

bool NativeRendererD3D12::Impl::retireSubmittedFrames(bool wait, std::string& error, bool cancelable)
{
    retireSurfaces();
    while (!submittedFrames.empty())
    {
        auto&      submitted = submittedFrames.front();
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX)
        {
            error = "native terminal fence device removal";
            return false;
        }
        if (completed < submitted.fenceValue)
        {
            if (!wait)
                return true;
            if (!waitForFence(submitted.fenceValue, cancelable))
            {
                error = "native terminal fence did not complete";
                return false;
            }
        }
        const auto previous = activeFrameContext;
        switchFrameContext(submitted.context);
        replaySubmissionInFlight = false;
        frameUploadPrepared      = false;
        auto result              = std::move(submitted.result);
        try
        {
            if (submitted.output && streamTitlePublisher)
            {
                auto lease = streamTitlePublisher(submitted.output->resource.Get(), fence.Get(), submitted.fenceValue);
                if (!lease || !lease->Token())
                {
                    result.success = false;
                    result.error   = "title presentation rejected the completed native GPU output";
                }
                else
                {
                    result.presenterToken   = lease->Token();
                    submitted.output->lease = lease;
                    gpuOutputLease.store(std::move(lease), std::memory_order_release);
                }
            }
            else if (submitted.output)
            {
                auto ticket = streamOutputPublisher(submitted.output->resource.Get(), fence.Get(), submitted.fenceValue);
                if (!ticket || !ticket->token())
                {
                    result.success = false;
                    result.error   = "SDK rejected the completed native GPU output";
                }
                else
                {
                    result.presenterToken    = ticket->token();
                    result.gpuOutputTicket   = ticket;
                    submitted.output->ticket = ticket;
                    gpuOutputTicket.store(std::move(ticket), std::memory_order_release);
                }
            }
            if (result.success && submitted.rawReadback)
                mapRawReadback(submitted.rawReadback.Get(), submitted.rawFootprint, submitted.rawBytes, result);
            if (result.success)
            {
                std::map<std::uint64_t, RetainedFrameTexture> retained;
                for (const auto& texture : submitted.textures)
                    if (texture.persistentKey && frameTextureOwners.contains(texture.id))
                        retained.emplace(texture.persistentKey, RetainedFrameTexture{ texture, frameTextures.at(texture.id), frameTextureOwners.at(texture.id), submitted.request->ticket->Export(texture.persistentKey) });
                retainedFrameTextures = std::move(retained);
            }
        }
        catch (const std::exception& exception)
        {
            result.success = false;
            result.error   = std::string("native terminal retirement exception: ") + exception.what();
        }
        catch (...)
        {
            result.success = false;
            result.error   = "native terminal retirement failed with an unknown exception";
        }
        const bool failed = !result.success;
        if (failed)
            error = result.error;
        recordedRenderPrefix.reset();
        frameTextures.clear();
        frameTextureOwners.clear();
        frameColorSnapshots.clear();
        frameRetiredUploads.clear();
        frameRetiredUploadLeases.clear();
        uncountCpuTextures(*this, true);
        frameRetiredCpuTextures.clear();
        drainResolveReturns();
        result.completionFenceValue = submitted.fenceValue;
        submitted.request->SetCompletion(std::move(result));
        submittedFrames.pop_front();
        // Deferred frames do not pass retainCompleted; age pipelines here too.
        pruneFramePipelines();
        switchFrameContext(previous);
        stateCv.notify_all();
        if (failed)
            return false;
        wait = false;
    }
    return true;
}

bool NativeRendererD3D12::Impl::mapRawReadback(ID3D12Resource* readback, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, UINT64 bytes, NativeDrawReplayResult& result)
{
    void*             mapped = nullptr;
    const D3D12_RANGE range{ 0, static_cast<SIZE_T>(bytes) };
    if (FAILED(readback->Map(0, &range, &mapped)))
    {
        result.success = false;
        result.error   = "raw_readback_map_failed";
        return false;
    }
    result.rawRowPitch = footprint.Footprint.Width * 4;
    result.rawOutput.resize(std::size_t(result.rawRowPitch) * footprint.Footprint.Height);
    for (UINT y = 0; y < footprint.Footprint.Height; ++y)
        std::memcpy(result.rawOutput.data() + std::size_t(y) * result.rawRowPitch,
                    static_cast<const std::uint8_t*>(mapped) + footprint.Offset + std::size_t(y) * footprint.Footprint.RowPitch,
                    result.rawRowPitch);
    const D3D12_RANGE written{ 0, 0 };
    readback->Unmap(0, &written);
    return true;
}

void NativeRendererD3D12::Impl::pruneFramePipelines()
{
    // Long unused and no longer referenced by any submitted command list.
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
        return;
    std::erase_if(frameGuestPipelineCache, [&](const auto& entry)
                  {
                      const bool prune = frameRecordings - entry.lastUsed >= kFramePipelineRetention && completed >= entry.lastUsedFence;
                      if (prune)
                          pipelineCacheBytes.fetch_sub(entry.OwnedBytes(), std::memory_order_relaxed);
                      return prune;
                  });
}

bool NativeRendererD3D12::Impl::acquireFrameContext(std::string& error)
{
    if (!retireSubmittedFrames(false, error))
        return false;
    while (submittedFrames.size() == frameContexts.size())
        if (!retireSubmittedFrames(true, error, true))
            return false;
    for (std::size_t index = 0; index < frameContexts.size(); ++index)
        if (std::none_of(submittedFrames.begin(), submittedFrames.end(), [&](const auto& submitted)
                         {
                             return submitted.context == index;
                         }))
        {
            switchFrameContext(index);
            return true;
        }
    error = "native frame context capacity is unavailable";
    return false;
}

bool NativeRendererD3D12::Impl::prepareFrameCommandBatch(FrameCommandBatch& batch, std::string& error)
{
    if (batch.fenceValue)
    {
#if REREVVED_DEVELOPER
        const auto waitStart = std::chrono::steady_clock::now();
#endif
        const bool completed = waitForFence(batch.fenceValue, true);
#if REREVVED_DEVELOPER
        replayFenceWaitNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()), std::memory_order_relaxed);
#endif
        if (!completed)
        {
            error = "native draw owner fence did not complete";
            return false;
        }
        batch.fenceValue = 0;
    }
    if (!batch.allocator && FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&batch.allocator))))
    {
        error = "could not create native draw owner allocator";
        return false;
    }
    if (!batch.commandList)
    {
        if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, batch.allocator.Get(), nullptr, IID_PPV_ARGS(&batch.commandList))) ||
            FAILED(batch.commandList->Close()))
        {
            error = "could not create native draw owner command list";
            return false;
        }
    }
    const auto createHeap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, std::size_t descriptors, ComPtr<ID3D12DescriptorHeap>& heap)
    {
        if (heap)
            return true;
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type           = type;
        desc.NumDescriptors = static_cast<UINT>(descriptors);
        desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap))))
            return false;
        ++replayDescriptorHeapCreations;
        return true;
    };
    if (!batch.samplerHeap)
    {
        batch.samplers.clear();
        batch.samplers.entries.reserve(kFrameSamplerEntries);
    }
    if (!createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kFrameCommandDraws * kDrawViewRegion, batch.viewHeap) ||
        !createHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kFrameSamplerEntries, batch.samplerHeap))
    {
        error = "could not create native frame owner descriptor heaps";
        return false;
    }
    // The owner's previous list completed, so tables without room for one
    // more draw's table start over.
    if (batch.samplers.entries.size() + kNativeDrawReplayTextureSlots > kFrameSamplerEntries)
        batch.samplers.clear();
    batch.helpers.clear();
    if (FAILED(batch.allocator->Reset()) || FAILED(batch.commandList->Reset(batch.allocator.Get(), nullptr)))
    {
        error = "could not reset native frame command owner";
        return false;
    }
    if (vertexResidency)
        vertexResidency->BeginBatch(static_cast<std::uint32_t>(&batch - frameCommandBatches.data()));
    ID3D12DescriptorHeap* heaps[] = { batch.viewHeap.Get(), batch.samplerHeap.Get() };
    batch.commandList->SetDescriptorHeaps(2, heaps);
    resetChunkConstants();
    return true;
}

bool NativeRendererD3D12::Impl::frameBatchFull(const FrameCommandBatch& owner, std::size_t ownerSlots, std::size_t ownerCommands, std::size_t commands, bool descriptors)
{
    // A draw slot also needs room for a whole sampler table it may not share.
    const bool samplers = descriptors && owner.samplers.entries.size() + kNativeDrawReplayTextureSlots > kFrameSamplerEntries;
    if (!(descriptors && ownerSlots == kFrameCommandDraws) && !samplers && commands <= NativeRenderBatchCommands - ownerCommands)
        return false;
    replayChunkSubmissions.fetch_add(1, std::memory_order_relaxed);
    if (samplers)
        replaySamplerTableFlushes.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ReplayDrawResources& NativeRendererD3D12::Impl::frameDrawSlot(FrameCommandBatch& owner, std::size_t ownerIndex, std::size_t slot)
{
    // Owners interleave, so the slots in use stay a dense prefix, and growth
    // within the reserved capacity keeps earlier slots in place.
    const auto index = slot * kFrameCommandBatches + ownerIndex;
    if (frameDrawSlots.size() <= index)
        frameDrawSlots.resize(index + 1);
    auto&      draw   = frameDrawSlots[index];
    const auto offset = slot * kDrawViewRegion * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto       cpu    = owner.viewHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += offset;
    if (draw.viewCpu.ptr != cpu.ptr)
    {
        draw.forgetDescriptors();
        draw.viewCpu = cpu;
        draw.viewGpu = owner.viewHeap->GetGPUDescriptorHandleForHeapStart();
        draw.viewGpu.ptr += offset;
    }
    return draw;
}

bool NativeRendererD3D12::Impl::frameSamplerTable(FrameCommandBatch& owner, const D3D12_SAMPLER_DESC* table, std::uint32_t count, D3D12_GPU_DESCRIPTOR_HANDLE& handle)
{
    auto&      tables = owner.samplers;
    const auto bytes  = count * sizeof(D3D12_SAMPLER_DESC);
    const auto equal  = [&](std::uint32_t offset)
    {
        return offset + count <= tables.entries.size() && std::memcmp(tables.entries.data() + offset, table, bytes) == 0;
    };
    std::optional<std::uint32_t> found;
    // Consecutive draws usually repeat the previous table.
    if (tables.lastCount == count && equal(tables.lastOffset))
        found = tables.lastOffset;
    const auto hash = found ? 0 : std::hash<std::string_view>{}({ reinterpret_cast<const char*>(table), bytes });
    if (!found)
    {
        const auto [first, last] = tables.tables.equal_range(hash);
        for (auto entry = first; entry != last && !found; ++entry)
            if (equal(entry->second))
                found = entry->second;
    }
    if (found)
    {
        replaySamplerTableHits.fetch_add(1, std::memory_order_relaxed);
        replayDescriptorReuses.fetch_add(count, std::memory_order_relaxed);
    }
    else
    {
        if (tables.entries.size() + count > kFrameSamplerEntries)
            return false;
        found              = static_cast<std::uint32_t>(tables.entries.size());
        const auto stride  = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        auto       written = owner.samplerHeap->GetCPUDescriptorHandleForHeapStart();
        written.ptr += static_cast<SIZE_T>(*found) * stride;
        for (std::uint32_t slot = 0; slot < count; ++slot, written.ptr += stride)
            device->CreateSampler(table + slot, written);
        tables.entries.insert(tables.entries.end(), table, table + count);
        tables.tables.emplace(hash, *found);
        replaySamplerTables.fetch_add(1, std::memory_order_relaxed);
    }
    tables.lastOffset = *found;
    tables.lastCount  = count;
    handle            = owner.samplerHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(*found) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    return true;
}

bool NativeRendererD3D12::Impl::submitFrameCommandBatch(FrameCommandBatch& batch, std::string& error)
{
    if (residencyCloseFault.exchange(0, std::memory_order_relaxed) || FAILED(batch.commandList->Close()))
    {
        error = "could not close native frame command owner";
        return false;
    }
    ID3D12CommandList* lists[] = { batch.commandList.Get() };
    queue->ExecuteCommandLists(1, lists);
    replaySubmissionInFlight = true;
    replayFenceValue         = nextFenceValue++;
    ++replayQueueSubmissions;
    publishVersionTextures(replayFenceValue);
    const auto batchIndex = static_cast<std::uint32_t>(&batch - frameCommandBatches.data());
    const auto hr         = residencySignalFault.exchange(0, std::memory_order_relaxed) ? E_FAIL : queue->Signal(fence.Get(), replayFenceValue);
    if (FAILED(hr))
    {
        // The list executed, so its completion is unknown.
        if (vertexResidency)
        {
            vertexResidency->QuarantineBatch(batchIndex);
            publishVertexResidency();
        }
        logDeviceRemoval("native frame signal", hr);
        error = "native frame queue signal failed";
        return false;
    }
    if (vertexResidency)
        vertexResidency->SubmitBatch(batchIndex, replayFenceValue);
    batch.fenceValue = replayFenceValue;
    nextCommandOwner = (static_cast<std::size_t>(&batch - frameCommandBatches.data()) + 1) % kFrameCommandBatches;
    if (recordingGpuOutput)
        recordingGpuOutput->fenceValue = replayFenceValue;
    return true;
}

bool NativeRendererD3D12::Impl::completeFrameCommands(std::string& error)
{
    if (!replaySubmissionInFlight)
        return true;
    // The last signal on this queue covers both owners, in submission order.
#if REREVVED_DEVELOPER
    const auto waitStart = std::chrono::steady_clock::now();
#endif
    const bool completed = waitForFence(replayFenceValue);
#if REREVVED_DEVELOPER
    replayFenceWaitNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count()), std::memory_order_relaxed);
#endif
    if (!completed)
    {
        error = "native frame command fences did not complete";
        return false;
    }
    replaySubmissionInFlight  = false;
    const auto completedValue = fence->GetCompletedValue();
    for (auto& batch : frameCommandBatches)
    {
        if (batch.fenceValue && batch.fenceValue <= completedValue)
        {
            batch.fenceValue = 0;
            batch.helpers.clear();
        }
    }
    return true;
}

bool NativeRendererD3D12::Impl::ensureFrameGuestWordRootSignature(std::string& error)
{
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    if (frameGuestWordRootSignature)
        return true;
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType      = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.Flags          = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    D3D12_ROOT_PARAMETER1 parameters[2]{};
    parameters[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges   = &range;
    parameters[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rootDesc.Desc_1_1.NumParameters = 2;
    rootDesc.Desc_1_1.pParameters   = parameters;
    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rootDesc, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&frameGuestWordRootSignature))))
        return fail("could not create native guest-word resolve root signature");
    ++replayRootSignatureCreations;
    return true;
}

bool NativeRendererD3D12::Impl::ensureFrameGuestWordPipeline(std::uint32_t sampleCount, bool depth, std::string& error)
{
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    auto& pipeline = frameGuestWordPipelines[{ sampleCount, depth }];
    if (pipeline)
        return true;
    if (!ensureFrameGuestWordRootSignature(error))
        return false;
    auto&      shader = frameGuestWordShaders[(depth ? 2 : 0) + (sampleCount == 1 ? 1 : 0)];
    const auto source = std::string(depth ? "#define DEPTH\n" : "") + (sampleCount == 1 ? "#define SINGLE_SAMPLE\n" : "") + kFrameGuestWordShaderSource;
    if (!compileCachedReplayShader(kReplayCopyShaderSource, "copy_vs", "vs_5_1", replayCopyVertexShader) ||
        !compileCachedReplayShader(source.c_str(), "resolve_guest_words", "ps_5_1", shader))
        return fail("could not compile native guest-word resolve shader");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature                                   = frameGuestWordRootSignature.Get();
    desc.VS                                               = { replayCopyVertexShader->GetBufferPointer(), replayCopyVertexShader->GetBufferSize() };
    desc.PS                                               = { shader->GetBufferPointer(), shader->GetBufferSize() };
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask                                       = D3D12_DEFAULT_SAMPLE_MASK;
    desc.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable                  = TRUE;
    desc.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets                                 = 1;
    desc.RTVFormats[0]                                    = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc                                       = { 1, 0 };
    if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
        return fail("could not create native guest-word resolve pipeline");
    ++replayPipelineCreations;
    return true;
}

bool NativeRendererD3D12::Impl::resolveFrameTexture(const NativeRenderFrameResolve& resolve,
                                                    const NativeRenderFrameTexture& destination,
                                                    ReplayDrawResources&            resources,
                                                    FrameCommandBatch&              owner,
                                                    std::string&                    error)
{
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    auto* source = resolve.depth ? replayDepthTarget.Get() : replayColorTargets[resolve.colorTarget].Get();
    if (!source)
        return fail("native resolve requires an ordered source attachment");
    auto&      commandList       = owner.commandList;
    const auto sourceDescription = source->GetDesc();
    const auto sampleCount       = sourceDescription.SampleDesc.Count;
    const auto format            = mapTextureFormat(destination.format);
    const bool guestWords        = resolve.guestWords != NativeGuestWordPacking::None;
    if (guestWords && !ensureFrameGuestWordPipeline(sampleCount, resolve.depth, error))
        return false;
    if (!guestWords && !getReplayHelperPipelines(sampleCount, format, error))
        return false;
    auto&      shader     = sampleCount == 1 ? frameResolveSingleShader : frameResolveShader;
    const auto sourceText = (sampleCount == 1 ? std::string("#define SINGLE_SAMPLE\n") : std::string{}) + kFrameResolveShaderSource;
    if (!guestWords && !compileCachedReplayShader(sourceText.c_str(), "resolve_frame", "ps_5_1", shader))
        return fail("could not compile native frame resolve shader");
    auto& pipeline      = guestWords ? frameGuestWordPipelines[{ sampleCount, resolve.depth }] : frameResolvePipelines[{ sampleCount, format }];
    auto* rootSignature = guestWords ? frameGuestWordRootSignature.Get() : replayCopyRootSignature.Get();
    if (!pipeline)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature                                   = replayCopyRootSignature.Get();
        desc.VS                                               = { replayCopyVertexShader->GetBufferPointer(), replayCopyVertexShader->GetBufferSize() };
        desc.PS                                               = { shader->GetBufferPointer(), shader->GetBufferSize() };
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask                                       = D3D12_DEFAULT_SAMPLE_MASK;
        desc.RasterizerState.FillMode                         = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode                         = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable                  = TRUE;
        desc.PrimitiveTopologyType                            = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets                                 = 1;
        desc.RTVFormats[0]                                    = format;
        desc.SampleDesc                                       = { 1, 0 };
        if (FAILED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline))))
            return fail("could not create native frame resolve pipeline");
        ++replayPipelineCreations;
    }
    auto&      target    = frameTextures[destination.id];
    const bool newTarget = !target;
    // A pooled destination rests in shader-read state with an earlier
    // frame's texels.
    bool reused = false;
    if (newTarget)
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = destination.width;
        desc.Height           = destination.height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format                            = format;
        desc.SampleDesc.Count                  = 1;
        desc.Flags                             = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        // Packed guest words are written as R32_UINT; castable storage lets a
        // draw sample them through an RGBA8, R10G10B10A2 or R16G16_FLOAT view.
        const bool    castable    = guestWords && castingDevice;
        std::uint64_t pooledBytes = 0;
        if ((target = takeResolvePooled(desc, castable, pooledBytes)))
            reused = true;
        else if (!reserveStorage(StorageDomain::Texture, device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes, error))
            return false;
        else if (castable)
        {
            D3D12_RESOURCE_DESC1 desc1{};
            desc1.Dimension        = desc.Dimension;
            desc1.Width            = desc.Width;
            desc1.Height           = desc.Height;
            desc1.DepthOrArraySize = desc.DepthOrArraySize;
            desc1.MipLevels        = desc.MipLevels;
            desc1.Format           = desc.Format;
            desc1.SampleDesc       = desc.SampleDesc;
            desc1.Flags            = desc.Flags;
            if (FAILED(castingDevice->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc1, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, static_cast<UINT32>(kGuestWordCastFormats.size()), kGuestWordCastFormats.data(), IID_PPV_ARGS(&target))))
                return fail("could not create native frame resolve texture");
            // COMMON is the one layout legacy barriers share with enhanced ones.
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET };
            commandList->ResourceBarrier(1, &barrier);
        }
        else if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target))))
            return fail("could not create native frame resolve texture");
        auto retained                      = std::make_shared<NativeFrameResourceOwner>();
        retained->resource                 = target;
        retained->resourceStorageBytes     = reused ? pooledBytes : resourceStorageBytes(device.Get(), target.Get());
        retained->castable                 = castable;
        retained->serial                   = ++textureResourceSerial;
        retained->returns                  = resolveReturns;
        frameTextureOwners[destination.id] = std::move(retained);
    }

    struct Constants
    {
        std::uint32_t sampleA, sampleB;
        float         scale;
        std::uint32_t swap;
        std::int32_t  offsetX, offsetY;
        std::uint32_t roundUnorm8, fixed16;
    };

    struct GuestWordConstants
    {
        std::uint32_t sampleA, sampleB, packing, padding;
        std::int32_t  offsetX, offsetY;
    };

    const std::int32_t offsetX = static_cast<std::int32_t>(resolve.sourceX) - static_cast<std::int32_t>(resolve.destinationX);
    const std::int32_t offsetY = static_cast<std::int32_t>(resolve.sourceY);
    // The only scaled conversion (render frame validation): k_16_16 codes to
    // float16 halves, which the shader computes as the SDK does.
    const bool               fixed16 = sourceDescription.Format == DXGI_FORMAT_R16G16_SNORM && format == DXGI_FORMAT_R16G16_FLOAT && resolve.valueScale == 32.0F;
    const Constants          constants{ resolve.samples[0], resolve.samples[1], resolve.valueScale, resolve.swapRedBlue ? 1U : 0U, offsetX, offsetY, sourceDescription.Format == DXGI_FORMAT_R8G8B8A8_UNORM && format == DXGI_FORMAT_R8G8B8A8_UNORM && resolve.valueScale == 1.0F ? 1U : 0U, fixed16 ? 1U : 0U };
    const GuestWordConstants guestConstants{ resolve.samples[0], resolve.samples[1], static_cast<std::uint32_t>(resolve.guestWords), 0, offsetX, offsetY };
    constexpr std::size_t    constantBytes = 256;
    const auto               limit         = frameUploadArena ? frameUploadArena.bytes() : 0;
    if (!frameUploadMapped || frameUploadCursor > limit || constantBytes > limit - frameUploadCursor)
        return fail("native resolve constants exceed frame upload storage");
    auto* mapped = frameUploadMapped + frameUploadCursor;
    std::memset(mapped, 0, constantBytes);
    if (guestWords)
        std::memcpy(mapped, &guestConstants, sizeof(guestConstants));
    else
        std::memcpy(mapped, &constants, sizeof(constants));
    const auto constantAddress = frameUploadArena.buffer().gpu + frameUploadCursor;
    frameUploadCursor += constantBytes;
    resources.pipeline = pipeline;
    commandList->SetPipelineState(resources.pipeline.Get());
    const auto shaderState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto sourceState = resolve.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    const auto transition  = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        commandList->ResourceBarrier(1, &barrier);
    };
    transition(source, sourceState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!newTarget || reused)
        transition(target.Get(), shaderState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!ensureReplayDescriptorHeaps(resources))
        return fail("could not create native resolve descriptors");
    const auto                      srvHandle = resources.viewCpu;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = resolve.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : sourceDescription.Format;
    srv.ViewDimension           = sampleCount == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (sampleCount == 1)
        srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(source, &srv, srvHandle);
    if (guestWords)
    {
        // The stencil plane for depth words; a null view keeps the color table valid.
        auto stencilHandle = srvHandle;
        stencilHandle.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto stencil   = srv;
        stencil.Format = resolve.depth ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R32_UINT;
        if (resolve.depth && sampleCount == 1)
            stencil.Texture2D.PlaneSlice = 1;
        device->CreateShaderResourceView(resolve.depth ? source : nullptr, &stencil, stencilHandle);
    }
    const auto rtvHandle = replayRtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtvHandle);
    // A created destination starts zeroed; a reused one is cleared where this
    // first resolve does not write it.
    if (reused && (resolve.destinationX != 0 || resolve.width < destination.width || resolve.height < destination.height))
    {
        constexpr float zero[4]{};
        commandList->ClearRenderTargetView(rtvHandle, zero, 0, nullptr);
    }
    bindDrawHeaps(commandList.Get(), resources);
    commandList->SetGraphicsRootSignature(rootSignature);
    commandList->SetGraphicsRootDescriptorTable(0, resources.viewGpu);
    commandList->SetGraphicsRootConstantBufferView(1, constantAddress);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{ float(resolve.destinationX), 0, float(resolve.width), float(resolve.height), 0, 1 };
    const D3D12_RECT     scissor{ LONG(resolve.destinationX), 0, LONG(resolve.destinationX + resolve.width), LONG(resolve.height) };
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
    transition(target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, shaderState);
    transition(source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, sourceState);

    return true;
}

bool NativeRendererD3D12::Impl::copyFrameColorBits(std::uint32_t sourcePass, bool restore, FrameCommandBatch& owner, std::string& error)
{
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    auto* color = replayColorTargets[0].Get();
    if (!color)
        return fail("native color alias requires an ordered attachment");
    auto&      commandList = owner.commandList;
    const auto desc        = color->GetDesc();
    if (desc.SampleDesc.Count != 1 ||
        (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_R16G16_SNORM))
        return fail("native color alias requires a single-sample 32-bit color format");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64                             bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto& buffer = frameColorSnapshots[sourcePass];
    if (restore)
    {
        if (!buffer || buffer->GetDesc().Width < bytes)
            return fail("native color alias has no complete saved bytes");
    }
    else
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width     = bytes;
        bufferDesc.Height = bufferDesc.DepthOrArraySize = bufferDesc.MipLevels = bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout                                                                                    = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!reserveStorage(StorageDomain::Texture, bytes, error))
            return false;
        // Default-heap buffers promote from COMMON at their first copy.
        if (buffer || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer))))
            return fail("could not allocate native color alias bytes");
    }
    const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        commandList->ResourceBarrier(1, &barrier);
    };
    const auto copyState = restore ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
    transition(color, D3D12_RESOURCE_STATE_RENDER_TARGET, copyState);
    D3D12_TEXTURE_COPY_LOCATION textureLocation{};
    textureLocation.pResource = color;
    textureLocation.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION bufferLocation{};
    bufferLocation.pResource       = buffer.Get();
    bufferLocation.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    bufferLocation.PlacedFootprint = footprint;
    // The buffer retains raw words. Its footprint adopts the destination view
    // only when restoring, without a shader conversion or CPU readback.
    if (restore)
        commandList->CopyTextureRegion(&textureLocation, 0, 0, 0, &bufferLocation, nullptr);
    else
        commandList->CopyTextureRegion(&bufferLocation, 0, 0, 0, &textureLocation, nullptr);
    transition(color, copyState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!restore)
        transition(buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);

    return true;
}

bool NativeRendererD3D12::Impl::clearRenderFramePass(const NativeRenderFramePass& pass,
                                                     FrameCommandBatch&           owner,
                                                     std::string&                 error)
{
    const auto fail = [&](const char* message)
    {
        error = message;
        return false;
    };
    auto& commandList = owner.commandList;
    if (!ensureReplayDescriptorHeaps(replayDrawSlots[0]))
        return fail("could not prepare native clear descriptor heaps");
    const auto          target = GetNativeRenderFrameTarget(pass);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = target.width;
    desc.Height           = target.height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc                        = { target.sampleCount, 0 };
    desc.Flags                             = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type               = D3D12_HEAP_TYPE_DEFAULT;
    const auto ensureTarget = [&](ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES state, bool preserve)
    {
        if (resource)
        {
            const auto previous = resource->GetDesc();
            if (previous.Width == desc.Width && previous.Height == desc.Height &&
                previous.Format == desc.Format && previous.SampleDesc.Count == desc.SampleDesc.Count)
                return true;
            if (preserve)
                return false;
            returnPooled(attachmentPool, resource);
        }
        if ((resource = takePooled(attachmentPool, desc, nullptr, true)))
            return true;
        if (!reserveStorage(StorageDomain::Attachment, device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes, error))
            return false;
        const auto started = DevNow();
        const bool created = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));
        countAttachmentCreate(started);
        return created;
    };
    // A resume pass already activated its attachments and keeps them.
    const bool preserve = pass.preservePreviousAttachments || pass.resumeAttachments;
    if (frameAttachmentCaching && !preserve)
        activateFrameColorAttachments(frameKey(replayColorKey(target)));
    for (std::uint32_t slot = 0; slot < replayColorTargets.size(); ++slot)
    {
        desc.Format = mapTargetFormat(NativeRenderFrameSlotFormat(target, slot));
        if (!ensureTarget(replayColorTargets[slot], D3D12_RESOURCE_STATE_RENDER_TARGET, preserve))
            return fail("could not prepare native clear color target");
    }
    if (!preserve)
    {
        // As for a fresh draw: park another key's attachment with its state.
        if (frameAttachmentCaching)
            activateFrameDepthAttachment(frameKey({ target.width, target.height, target.sampleCount, DXGI_FORMAT_R24G8_TYPELESS }));
        replayDepthInitialized = false;
    }
    const bool                  needsDepth      = std::any_of(pass.clears.begin(), pass.clears.end(), [](const auto& clear)
                                                              {
                                            return clear.depth || clear.stencil;
                                                              });
    const bool                  initializeDepth = needsDepth && !replayDepthInitialized;
    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
    if (needsDepth)
    {
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (frameAttachmentCaching && !replayDepthInitialized)
            activateFrameDepthAttachment(frameKey({ target.width, target.height, target.sampleCount, desc.Format }));
        if (!ensureTarget(replayDepthTarget, D3D12_RESOURCE_STATE_DEPTH_WRITE, replayDepthInitialized))
            return fail("could not prepare native clear depth target");
        if (!replayDsvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            heapDesc.NumDescriptors = 1;
            if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&replayDsvHeap))))
                return fail("could not create native clear depth descriptor");
        }
        depthDsv = replayDsvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv.ViewDimension = target.sampleCount == 1 ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device->CreateDepthStencilView(replayDepthTarget.Get(), &dsv, depthDsv);
        replayDsvResource = replayDepthTarget.Get();
    }
    const auto                                 stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtvs;
    for (std::size_t index = 0; index < rtvs.size(); ++index)
    {
        rtvs[index] = replayRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvs[index].ptr += index * stride;
        device->CreateRenderTargetView(replayColorTargets[index].Get(), nullptr, rtvs[index]);
        if (!preserve)
        {
            constexpr float zero[4]{};
            commandList->ClearRenderTargetView(rtvs[index], zero, 0, nullptr);
        }
    }
    if (initializeDepth)
        commandList->ClearDepthStencilView(depthDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0F, 0, 0, nullptr);
    for (const auto& clear : pass.clears)
    {
        const D3D12_RECT rectangle{ clear.rectangle.left, clear.rectangle.top, clear.rectangle.right, clear.rectangle.bottom };
        if (clear.color)
            for (std::size_t index = 0; index < rtvs.size(); ++index)
                if (clear.colorTargets & (1U << index))
                    commandList->ClearRenderTargetView(rtvs[index], clear.colorValue.data(), 1, &rectangle);
        if (clear.depth || clear.stencil)
        {
            const auto flags = static_cast<D3D12_CLEAR_FLAGS>((clear.depth ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                              (clear.stencil ? D3D12_CLEAR_FLAG_STENCIL : 0));
            commandList->ClearDepthStencilView(depthDsv, flags, clear.depthValue, clear.stencilValue, 1, &rectangle);
        }
    }

    replayDepthInitialized |= needsDepth;
    return true;
}

void NativeRendererD3D12::Impl::cancelRecordingCpuReservation()
{
    recordingCpuReservationLease = nullptr;
    const auto clear             = [&](auto& context)
    {
        for (auto& [key, texture] : context.frameCpuTextures)
        {
            texture.reservedRecordingVersion.reset();
            texture.reservationToken    = 0;
            texture.reservationConflict = false;
        }
        for (auto& texture : context.frameRetiredCpuTextures)
        {
            texture.reservedRecordingVersion.reset();
            texture.reservationToken    = 0;
            texture.reservationConflict = false;
        }
    };
    clear(*this);
    for (auto& context : frameContexts)
        clear(context);
}

bool NativeRendererD3D12::Impl::recordingCpuVersionReserved(const FrameCpuTexture& texture, const std::shared_ptr<const NativeDrawReplayTextureVersion>& version) const
{
    const auto& reserved = texture.reservedRecordingVersion;
    return version && reserved && recordingCpuReservationLease && recordingCpuReservationLease == recordingFrameLease &&
           recordingCpuReservationContext == activeFrameContext && texture.reservationToken == recordingCpuReservationToken &&
           reserved.get() == version.get() && !reserved.owner_before(version) && !version.owner_before(reserved) && version->MatchesOwner(version);
}

void NativeRendererD3D12::Impl::reserveRecordingCpuTextures(const NativeRenderFrame& frame)
{
    if (recordingFrameLease != &frame)
    {
        cancelRecordingCpuReservation();
        return;
    }
    try
    {
        // Preparation's maps and retired/pool temporaries have already died.
        if (retainedStorageBytes(StorageDomain::CpuTexture, nullptr, false, true) > kMaxReplayTextureFootprintBytes)
        {
            cancelRecordingCpuReservation();
            return;
        }
        recordingCpuReservationToken   = recordingCpuReservationToken == std::numeric_limits<std::uint64_t>::max() ? 1 : recordingCpuReservationToken + 1;
        recordingCpuReservationLease   = &frame;
        recordingCpuReservationContext = activeFrameContext;
        for (auto& [key, texture] : frameCpuTextures)
            if (texture.reservedRecordingVersion)
                texture.reservationToken = recordingCpuReservationToken;
    }
    catch (const std::bad_alloc&)
    {
        // The union shortcut is optional; original per-entry gates remain safe.
        cancelRecordingCpuReservation();
    }
}

bool NativeRendererD3D12::Impl::prepareFrameCpuTextures(const NativeRenderFrame& frame, std::string& error, bool keepUnused, bool preserveRecorded)
{
    cancelRecordingCpuReservation();
    // Every earlier recording submitted its batches or failed.
    discardVersionTextures();
    sweepVersionTextures();
    std::map<FrameCpuTextureKey, FrameCpuTexture> current;
    if (keepUnused)
        for (const auto& [key, texture] : frameCpuTextures)
            current.try_emplace(key);
    // The payload of each key this frame samples, alive through this call.
    std::map<FrameCpuTextureKey, std::pair<std::span<const std::uint8_t>, bool>> sampled;
    for (const auto& pass : frame.passes)
    {
        for (std::size_t drawIndex = 0; drawIndex < pass.DrawCount(); ++drawIndex)
        {
            const auto& draw      = pass.Draw(drawIndex);
            const auto* admission = pass.admittedDraws.empty() ? nullptr : pass.admittedDraws[drawIndex].get();
            for (std::uint32_t slot = 0; slot < static_cast<std::uint32_t>(std::popcount(draw.textureMask)); ++slot)
                if (!draw.textures[slot].frameTextureId)
                {
                    const auto& source  = draw.textures[slot];
                    const auto* binding = admission ? &admission->TextureBindings()[slot] : nullptr;
                    if (versionTextureStore && storedVersion(source, binding))
                        continue;
                    const auto key   = frameCpuTextureKey(source, binding);
                    auto&      entry = current.try_emplace(key).first->second;
                    sampled.try_emplace(key, source.payloadBytes(), bool(source.sharedBytes));
                    const auto version   = binding ? binding->textureVersion : nullptr;
                    const bool canonical = version && version->MatchesOwner(version) && version->Layout() == binding->ResourceLayout() &&
                                           source.textureVersion.get() == version.get() && !source.textureVersion.owner_before(version) && !version.owner_before(source.textureVersion);
                    if (!canonical || (entry.reservedRecordingVersion &&
                                       (entry.reservedRecordingVersion.get() != version.get() || entry.reservedRecordingVersion.owner_before(version) || version.owner_before(entry.reservedRecordingVersion))))
                    {
                        entry.reservationConflict = true;
                        entry.reservedRecordingVersion.reset();
                    }
                    else if (!entry.reservationConflict)
                        entry.reservedRecordingVersion = version;
                }
        }
    }
    std::map<FrameCpuTextureLayout, std::shared_ptr<const ReplayTexturePreparation>> preparations;
    std::uint64_t                                                                    footprintBytes = 0, preparationBytes = 0;
    for (auto& [key, texture] : current)
    {
        const auto [entry, inserted] = preparations.try_emplace(key.first);
        if (inserted)
        {
            const auto prior = frameCpuTextures.lower_bound({ key.first, 0 });
            if (prior != frameCpuTextures.end() && prior->first.first == key.first && prior->second.preparation)
                entry->second = prior->second.preparation;
            else
            {
                NativeDrawReplayTexture description;
                std::tie(description.width, description.height, description.mipLevels, description.format, description.dimension) = key.first;
                entry->second                                                                                                     = prepareReplayTexture(device.Get(), description);
                replayTexturePreparationCreations.fetch_add(1, std::memory_order_relaxed);
            }
            preparationBytes += entry->second->StorageBytes();
        }
        texture.preparation = entry->second;
        const auto bytes    = texture.preparation->totalBytes;
        if (bytes > 128ULL * 1024ULL * 1024ULL || preparationBytes > kMaxReplayTextureFootprintBytes - footprintBytes ||
            bytes > kMaxReplayTextureFootprintBytes - footprintBytes - preparationBytes)
        {
            if (keepUnused)
                return prepareFrameCpuTextures(frame, error, false, preserveRecorded);
            error = "frame CPU texture uploads exceed the footprint bound";
            return false;
        }
        footprintBytes += bytes;
    }

    // Reuse checks completed fences, prior owners, layout, and complete private
    // payload bytes before uploading.
    const auto holds = [&](const FrameCpuTexture& texture, const FrameCpuTextureKey& key)
    {
        const auto found = sampled.find(key);
        return texture.resource && found != sampled.end() && retainsPayload(texture, found->second.first);
    };
    // A texture recorded by pending work keeps its other state until the frame
    // retires; its storage waits in the pool for the next signal, or stays
    // with the entry, as before pooling, when the pool refuses it.
    const auto retireCpuTexture = [&](FrameCpuTexture&& texture)
    {
        if (holdPooled(texturePool, texture.resource, texture.resourceStorageBytes))
            texture.resourceStorageBytes = 0;
        frameRetiredCpuTextures.push_back(std::move(texture));
    };
    const auto adopt = [&](FrameCpuTexture& texture, FrameCpuTexture&& source)
    {
        auto       candidate             = std::move(texture.reservedRecordingVersion);
        const auto conflict              = texture.reservationConflict;
        const auto priorKey              = texture.priorKey;
        texture                          = std::move(source);
        texture.reservedRecordingVersion = std::move(candidate);
        texture.reservationConflict      = conflict;
        texture.priorKey                 = priorKey;
    };
    for (auto& [key, texture] : current)
        if (const auto found = frameCpuTextures.find(key); found != frameCpuTextures.end())
        {
            texture.priorKey = true;
            if (holds(found->second, key))
                adopt(texture, std::move(found->second));
            else if (replaySubmissionInFlight && found->second.recorded && !(preserveRecorded && found->second.recordedShared))
                retireCpuTexture(std::move(found->second));
            else if (!(replaySubmissionInFlight && found->second.recorded) && sampled.contains(key))
                // Other bytes under a reused payload address: equal bytes held
                // elsewhere, or then any storage of the layout, serve the key.
                continue;
            else
                adopt(texture, std::move(found->second));
            frameCpuTextures.erase(found);
        }
    // A sampled key without storage takes an entry of its layout holding the
    // same bytes under another payload owner: one this frame no longer
    // samples, or one a preceding graph segment kept but this one does not.
    std::vector<FrameCpuTextureKey> adopted;
    for (auto& [key, texture] : current)
    {
        if (texture.resource || !sampled.contains(key))
            continue;
        const auto search = [&](auto& entries, bool kept) -> bool
        {
            for (auto other = entries.lower_bound({ key.first, 0 }); other != entries.end() && other->first.first == key.first; ++other)
                if (other->first != key && (!kept || !sampled.contains(other->first)) &&
                    other->second.resource && retainsPayload(other->second, sampled.at(key).first))
                {
                    // The donor's ownership flags describe another payload
                    // owner; recording rechecks these bytes for this one.
                    adopt(texture, std::move(other->second));
                    texture.recorded = texture.recordedShared = false;
                    if (kept)
                        adopted.push_back(other->first);
                    else
                        entries.erase(other);
                    return true;
                }
            return false;
        };
        if (search(frameCpuTextures, false) || search(current, true))
            replayTextureContentReuses.fetch_add(1, std::memory_order_relaxed);
    }
    for (const auto& key : adopted)
        current.erase(key);
    std::multimap<FrameCpuTextureLayout, FrameCpuTexture> reusable;
    for (auto& [key, texture] : frameCpuTextures)
        if (replaySubmissionInFlight && texture.recorded)
            retireCpuTexture(std::move(texture));
        else
            reusable.emplace(key.first, std::move(texture));
    for (auto& [key, texture] : current)
    {
        const auto found = reusable.find(key.first);
        if (!texture.resource && found != reusable.end())
        {
            auto       candidate             = std::move(texture.reservedRecordingVersion);
            const auto conflict              = texture.reservationConflict;
            const auto priorKey              = texture.priorKey;
            texture                          = std::move(found->second);
            texture.reservedRecordingVersion = std::move(candidate);
            texture.reservationConflict      = conflict;
            texture.priorKey                 = priorKey;
            reusable.erase(found);
            texture.recorded = false;
        }
        // Prefix validation pins shared owners; terminal identity rechecks bytes.
        // Borrowed recipe identities and pooled layouts cannot keep this flag.
        texture.recorded &= preserveRecorded && texture.recordedShared;
    }
    // A graph segment keeps unclaimed storage for later segments' payloads,
    // under an odd identity that never names a payload buffer.
    for (auto& [layout, texture] : reusable)
        if (keepUnused && texture.resource && texture.resourceSerial)
            current.try_emplace({ layout, static_cast<std::uintptr_t>(texture.resourceSerial << 1 | 1) }, std::move(texture));
        else if (replaySubmissionInFlight)
            retireCpuTexture(std::move(texture));
        else
            returnPooled(texturePool, texture.resource, texture.resourceStorageBytes);
    // Immutable source owners and fallback snapshots share unused footprint
    // allowance after charging retained device preparation.
    // If that space is unavailable, keep the original complete-upload path.
    frameCpuTexturePayloadBudget = kMaxReplayTextureFootprintBytes - footprintBytes - preparationBytes;
    std::uint64_t payloadBytes   = 0;
    for (auto& [key, texture] : current)
    {
        if (texture.RetainedPayloadBytes() > frameCpuTexturePayloadBudget - payloadBytes)
        {
            texture.uploadedBytes.reset();
            texture.uploadedSize = 0;
            texture.uploadedVersion.reset();
        }
        payloadBytes += texture.RetainedPayloadBytes();
    }
    frameCpuTextures            = std::move(current);
    payloadBytes                = 0;
    std::uint64_t ownedBytes    = 0;
    const auto    countPayloads = [&](const auto& context)
    {
        for (const auto& [key, texture] : context.frameCpuTextures)
        {
            payloadBytes += texture.RetainedPayloadBytes();
            ownedBytes += texture.RetainedOwnedBytes();
        }
        for (const auto& texture : context.frameRetiredCpuTextures)
        {
            payloadBytes += texture.RetainedPayloadBytes();
            ownedBytes += texture.RetainedOwnedBytes();
        }
    };
    countPayloads(*this);
    for (const auto& context : frameContexts)
        countPayloads(context);
    frameCpuTexturePayloadBytes.store(payloadBytes, std::memory_order_relaxed);
    frameCpuTextureOwnedBytes.store(ownedBytes, std::memory_order_relaxed);
    if (!reserveStorage(StorageDomain::CpuTexture, 0, error))
        return false;
    // Unused preceding allocations retire here, before new GPU allocations.
    return true;
}

bool NativeRendererD3D12::Impl::prepareFrameAttachmentCache(const NativeRenderFrame& frame, std::string& error, bool keepUnused)
{
    frameAttachmentCaching = false;
    const auto completed   = fence->GetCompletedValue();
    if (completed == UINT64_MAX)
    {
        error = "native attachment reuse requires a completed preceding frame";
        return false;
    }
    frameAttachmentByteBound.store(0, std::memory_order_relaxed);
    std::vector<ReplayAttachmentKey> colors, depths;
    const auto                       add = [](auto& keys, ReplayAttachmentKey key)
    {
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            keys.push_back(key);
    };
    if (keepUnused)
    {
        for (const auto& entry : frameColorAttachmentCache)
            add(colors, entry.key);
        if (replayColorTargets[0])
            add(colors, currentColorKey());
    }
    for (const auto& pass : frame.passes)
    {
        auto key      = replayColorKey(GetNativeRenderFrameTarget(pass));
        key.presented = frame.identityAttachments && pass.presented;
        add(colors, key);
    }
    if (!frame.retainOnly)
        add(colors, { 1280, 720, 1, mapTargetFormat(NativeDrawReplayTargetFormat::Rgba8) });
    for (auto key : colors)
    {
        key.format       = DXGI_FORMAT_R24G8_TYPELESS;
        key.extraFormats = {};
        add(depths, key);
    }
    constexpr std::uint64_t maxBytes    = 512ULL * 1024 * 1024;
    std::uint64_t           bytes       = 0;
    const bool              outputSlots = frame.returnGpuOutput || std::any_of(frameGpuOutputs.begin(), frameGpuOutputs.end(), [](const auto& slot)
                                                                               {
                                                                      return bool(slot.resource);
                                                                               });
    if (outputSlots)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = 1280;
        desc.Height           = 720;
        desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format                                                    = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.Flags                                                     = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const auto allocation                                          = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        if (!allocation || allocation > maxBytes / frameGpuOutputs.size())
        {
            error = "native output slots exceed the attachment byte bound";
            return false;
        }
        bytes = allocation * frameGpuOutputs.size();
    }
    const auto account = [&](const ReplayAttachmentKey& key, bool depth)
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = key.width;
        desc.Height           = key.height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc                        = { key.samples, 0 };
        desc.Flags                             = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (std::size_t slot = 0; slot < (depth ? 1U : 4U); ++slot)
        {
            desc.Format           = slot && key.extraFormats[slot - 1] != DXGI_FORMAT_UNKNOWN ? key.extraFormats[slot - 1] : key.format;
            const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            if (!allocation || allocation > maxBytes - bytes)
                return false;
            bytes += allocation;
        }
        return true;
    };
    const bool bounded = colors.size() * 4 + depths.size() + (outputSlots ? frameGpuOutputs.size() : 0) <= NativeRenderBatchCommands &&
                         std::all_of(colors.begin(), colors.end(), [&](const auto& key)
                                     {
                                         return account(key, false);
                                     }) &&
                         std::all_of(depths.begin(), depths.end(), [&](const auto& key)
                                     {
                                         return account(key, true);
                                     });
    if (!bounded)
    {
        if (replaySubmissionInFlight && !completeFrameCommands(error))
            return false;
        if (keepUnused)
            return prepareFrameAttachmentCache(frame, error);
        frameColorAttachmentCache.clear();
        frameDepthAttachmentCache.clear();
        return true;
    }
    // Keep only layouts required by this frame, after the previous frame fenced.
    // Cached contents never imply initialization; normal clears still run.
    std::vector<ReplayColorAttachments> nextColors;
    for (const auto& key : colors)
    {
        const auto found = std::find_if(frameColorAttachmentCache.begin(), frameColorAttachmentCache.end(), [&](const auto& entry)
                                        {
                                            return entry.key == key;
                                        });
        nextColors.push_back(found == frameColorAttachmentCache.end() ? ReplayColorAttachments{ key, {} } : std::move(*found));
    }
    for (auto& entry : frameColorAttachmentCache)
        for (auto& resource : entry.resources)
            returnPooled(attachmentPool, resource);
    frameColorAttachmentCache = std::move(nextColors);
    std::vector<ReplayDepthAttachment> nextDepths;
    for (const auto& key : depths)
    {
        const auto found = std::find_if(frameDepthAttachmentCache.begin(), frameDepthAttachmentCache.end(), [&](const auto& entry)
                                        {
                                            return entry.key == key;
                                        });
        nextDepths.push_back(found == frameDepthAttachmentCache.end() ? ReplayDepthAttachment{ key, {} } : std::move(*found));
    }
    for (auto& entry : frameDepthAttachmentCache)
        returnPooled(attachmentPool, entry.resource);
    frameDepthAttachmentCache = std::move(nextDepths);
    if (replayColorTargets[0] && std::find(colors.begin(), colors.end(), currentColorKey()) == colors.end())
        for (auto& resource : replayColorTargets)
            returnPooled(attachmentPool, resource);
    if (replayDepthTarget && std::find(depths.begin(), depths.end(), currentDepthKey()) == depths.end())
    {
        returnPooled(attachmentPool, replayDepthTarget);
        replayDepthInitialized = false;
    }
    frameAttachmentByteBound.store(bytes, std::memory_order_relaxed);
    frameAttachmentCaching = true;
    return true;
}

void NativeRendererD3D12::Impl::activateFrameColorAttachments(const ReplayAttachmentKey& key)
{
    if (replayColorTargets[0] && currentColorKey() == key)
        return;
    if (replayColorTargets[0])
    {
        const auto current = currentColorKey();
        const auto found   = std::find_if(frameColorAttachmentCache.begin(), frameColorAttachmentCache.end(), [&](const auto& entry)
                                          {
                                            return entry.key == current;
                                          });
        if (found != frameColorAttachmentCache.end())
            found->resources = std::move(replayColorTargets);
    }
    // Targets of a key outside the cache were not parked.
    for (auto& resource : replayColorTargets)
        returnPooled(attachmentPool, resource);
    const auto found = std::find_if(frameColorAttachmentCache.begin(), frameColorAttachmentCache.end(), [&](const auto& entry)
                                    {
                                        return entry.key == key;
                                    });
    if (found != frameColorAttachmentCache.end())
    {
        for (const auto& resource : found->resources)
            if (resource)
                ++frameAttachmentReuses;
        replayColorTargets = std::move(found->resources);
    }
    activeColorPresented = key.presented;
}

void NativeRendererD3D12::Impl::activateFrameDepthAttachment(const ReplayAttachmentKey& key, bool restoreInitialized)
{
    if (replayDepthTarget && currentDepthKey() == key)
        return;
    if (replayDepthTarget)
    {
        const auto current = currentDepthKey();
        const auto found   = std::find_if(frameDepthAttachmentCache.begin(), frameDepthAttachmentCache.end(), [&](const auto& entry)
                                          {
                                            return entry.key == current;
                                          });
        if (found != frameDepthAttachmentCache.end())
        {
            found->resource    = std::move(replayDepthTarget);
            found->initialized = replayDepthInitialized;
        }
    }
    // A depth target of a key outside the cache was not parked.
    returnPooled(attachmentPool, replayDepthTarget);
    const auto found = std::find_if(frameDepthAttachmentCache.begin(), frameDepthAttachmentCache.end(), [&](const auto& entry)
                                    {
                                        return entry.key == key;
                                    });
    if (found != frameDepthAttachmentCache.end())
    {
        if (found->resource)
            ++frameAttachmentReuses;
        replayDepthTarget = std::move(found->resource);
        if (restoreInitialized)
            replayDepthInitialized = replayDepthTarget && found->initialized;
    }
    else if (restoreInitialized)
        replayDepthInitialized = false;
    activeDepthPresented = key.presented;
}

bool NativeRendererD3D12::Impl::recordRawReadback(std::uint64_t textureId, FrameCommandBatch& owner, std::string& error)
{
    const auto texture = frameTextures.find(textureId);
    if (texture == frameTextures.end())
    {
        error = "native resolve readback has no destination";
        return false;
    }
    auto       readback = std::make_shared<D3D12RawReadback>();
    const auto desc     = texture->second->GetDesc();
    device->GetCopyableFootprints(&desc, 0, 1, 0, &readback->footprint, nullptr, nullptr, &readback->bytes);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width     = readback->bytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout                                                                        = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!reserveStorage(StorageDomain::Attachment, readback->bytes, error) ||
        FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback->buffer))))
    {
        error = "raw_readback_allocation_failed";
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = texture->second.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    owner.commandList->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture->second.Get();
    source.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource       = readback->buffer.Get();
    destination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = readback->footprint;
    owner.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    owner.commandList->ResourceBarrier(1, &barrier);
    // The GPU writes the buffer until the frame retires, whoever holds the handle.
    frameRetiredUploads.push_back(readback->buffer);
    recordingRawReadbacks.emplace_back(textureId, readback);
    std::lock_guard lock(rawReadbackMutex);
    std::erase_if(liveRawReadbacks, [](const auto& entry)
                  {
                      return entry.expired();
                  });
    liveRawReadbacks.push_back(readback);
    return true;
}

bool NativeRendererD3D12::Impl::prepareFrameUploads(const NativeRenderFrame& frame, std::string& error, bool terminal, std::size_t firstPass)
{
    // Whatever follows, earlier constant uploads are no longer bound again.
    ++frameUploadEpoch;
    constexpr std::size_t maxBytes = 512U * 1024U * 1024U;
    std::size_t           bytes    = 0;
    const auto            account  = [&](std::size_t size)
    {
        const auto aligned = (size + 255U) & ~std::size_t(255U);
        if (aligned > maxBytes - bytes)
            return false;
        bytes += aligned;
        return true;
    };
    for (const auto& pass : std::span(frame.passes).subspan(firstPass))
    {
        for (std::size_t drawIndex = 0; drawIndex < pass.DrawCount(); ++drawIndex)
        {
            const auto& draw = pass.Draw(drawIndex);
            if (!account(draw.vertexConstants.size()) || !account(draw.pixelConstants.size()) ||
                !account(draw.sharedConstants.size()) || !account(draw.vertexBytes().size()) ||
                !account(draw.indexSpan().size() * sizeof(std::uint32_t)))
            {
                error = "native frame uploads exceed the owned byte bound";
                return false;
            }
        }
        // Each resolve and each depth conversion it feeds takes 256 constant bytes.
        std::size_t conversions = 0;
        for (const auto& resolve : pass.resolves)
            for (const auto& surface : frame.surfaces)
                conversions += surface.depthKey && surface.textureId == resolve.textureId;
        if (!account((pass.resolves.size() + conversions) * 256))
        {
            error = "native frame resolve uploads exceed the owned byte bound";
            return false;
        }
    }
    if (terminal && !frame.retainOnly && (!account(4096) || !account(3584) || !account(336) || !account(3 * 16) || !account(3 * sizeof(std::uint32_t))))
    {
        error = "native terminal uploads exceed the owned byte bound";
        return false;
    }
    // Clear-only packets leave a reusable arena untouched; zero-byte packets
    // preserve the preparation proof for earlier unfinished upload users.
    if (bytes && replaySubmissionInFlight && frameUploadPrepared && frameUploadArena)
    {
        frameRetiredUploadLeases.push_back(std::move(frameUploadArena));
        frameUploadMapped   = nullptr;
        frameUploadPrepared = false;
    }
    if (bytes && (!frameUploadArena || frameUploadArena.bytes() < bytes))
    {
        frameUploadMapped = nullptr;
        frameUploadArena.reset();
        frameUploadPrepared = false;
        std::string admitError;
        frameUploadArena = uploadPool.Acquire(bytes, 0, [&](std::uint64_t classBytes)
                                              {
                                                  return reserveStorage(StorageDomain::Upload, classBytes, admitError);
                                              });
        if (!frameUploadArena)
        {
            error = admitError.empty() ? "could not allocate native frame upload storage" : std::move(admitError);
            return false;
        }
        frameUploadMapped = frameUploadArena.buffer().mapped;
        ++(frameUploadArena.reused() ? replayDrawUploadReuses : replayDrawUploadCreations);
    }
    else if (bytes)
        ++replayDrawUploadReuses;
    frameUploadCursor = 0;
    if (bytes)
        frameUploadPrepared = true;
    return true;
}

bool NativeRendererD3D12::Impl::recordRenderFramePasses(const NativeRenderFrame& frame, std::size_t firstPass, FrameCommandBatch*& owner, std::size_t& ownerIndex, std::size_t& ownerSlots, std::size_t& ownerCommands, std::string& error)
{
    const auto flush = [&]()
    {
        if (!owner)
            return true;
        if (!submitFrameCommandBatch(*owner, error))
            return false;
        owner      = nullptr;
        ownerIndex = (ownerIndex + 1) % kFrameCommandBatches;
        return true;
    };
    const auto acquire = [&](std::size_t commands, bool descriptors, ReplayDrawResources*& resources)
    {
        {
            std::lock_guard lock(stateMutex);
            if (stopRequested)
            {
                frameRecordingCancelled = true;
                error                   = "native frame stopped between commands";
                return false;
            }
        }
        if (owner && frameBatchFull(*owner, ownerSlots, ownerCommands, commands, descriptors))
            if (!flush())
                return false;
        if (!owner)
        {
            owner = &frameCommandBatches[ownerIndex];
            if (!prepareFrameCommandBatch(*owner, error))
                return false;
            ownerSlots = ownerCommands = 0;
        }
        resources = descriptors ? &frameDrawSlot(*owner, ownerIndex, ownerSlots++) : nullptr;
        ownerCommands += commands;
        return true;
    };
    const auto prepareTarget = [&](NativeRenderFramePass::Target target)
    {
        // Uncached attachment replacement cannot release a submitted resource.
        // Cached layouts instead stay owned through the terminal frame fence.
        if (!frameAttachmentCaching && replayColorTargets[0] &&
            replayColorKey(replayColorTargets) != replayColorKey(target))
            return flush() && completeFrameCommands(error);
        return true;
    };
    ReplayDrawResources* resources = nullptr;

    struct ResetPassNamespace
    {
        bool& presented;

        ~ResetPassNamespace()
        {
            presented = false;
        }
    } resetNamespace{ framePassPresented };

    for (std::size_t passIndex = firstPass; passIndex < frame.passes.size(); ++passIndex)
    {
        const auto& pass = frame.passes[passIndex];
        if (!prepareTarget(GetNativeRenderFrameTarget(pass)))
            return false;
        framePassPresented = frameIdentityAttachments && pass.presented;
        if (pass.resumeAttachments)
        {
            // The assembler resumes only attachments this chain recorded; a
            // missing one is a refusal, never a silently fresh target.
            const auto target = GetNativeRenderFrameTarget(pass);
            if (!frameAttachmentCaching || !framePassPresented)
            {
                error = "native resume requires identity attachment caching";
                return false;
            }
            activateFrameColorAttachments(frameKey(replayColorKey(target)));
            activateFrameDepthAttachment(frameKey({ target.width, target.height, target.sampleCount, DXGI_FORMAT_R24G8_TYPELESS }), true);
            if (!replayColorTargets[0])
            {
                error = "native resume found no presented attachments";
                return false;
            }
        }
        const bool preserve       = pass.preservePreviousAttachments || pass.resumeAttachments;
        const bool explicitClears = !pass.clears.empty();
        if (pass.colorSourcePass)
        {
            NativeRenderFramePass initialize;
            initialize.target = GetNativeRenderFrameTarget(pass);
            if (!acquire(1, false, resources) || !clearRenderFramePass(initialize, *owner, error) ||
                !acquire(1, false, resources) || !copyFrameColorBits(pass.colorSourcePass, true, *owner, error))
                return false;
            if (explicitClears)
            {
                initialize.preservePreviousAttachments = true;
                initialize.clears                      = pass.clears;
                if (!acquire(pass.clears.size(), false, resources) || !clearRenderFramePass(initialize, *owner, error))
                    return false;
            }
        }
        else if (explicitClears || (!pass.DrawCount() && !preserve))
        {
            // Fresh resolve-only passes need initialized targets before copying.
            const auto commands = explicitClears ? pass.clears.size() : 1;
            if (!acquire(commands, false, resources) || !clearRenderFramePass(pass, *owner, error))
                return false;
        }
        for (std::size_t index = 0; index < pass.DrawCount(); ++index)
        {
            if (!acquire(1, true, resources))
                return false;
            const auto drawResult = executeOffscreenReplayOnRendererThread(
                pass.Draw(index), explicitClears || pass.colorSourcePass || preserve || index != 0, false, nullptr, nullptr, 0, false, resources, owner, pass.admittedDraws.empty() ? nullptr : pass.admittedDraws[index].get(), recordingFrameLease == &frame);
            if (!drawResult.success)
            {
                error = drawResult.error;
                return false;
            }
        }
        for (const auto& resolve : pass.resolves)
        {
            const NativeRenderFrameTexture* destination     = nullptr;
            const auto                      findDestination = [&](const auto& textures)
            {
                for (const auto& texture : textures)
                    if (texture.id == resolve.textureId)
                        return &texture;
                return static_cast<const NativeRenderFrameTexture*>(nullptr);
            };
            destination = findDestination(frame.textures);
            if (!destination && recordedRenderPrefix)
                destination = findDestination(recordedRenderPrefix->textures);
            if (!destination || !acquire(1, true, resources) ||
                !resolveFrameTexture(resolve, *destination, *resources, *owner, error))
                return false;
            ReplayDrawResources* copyResources = nullptr;
            if (resolve.readback && (!acquire(1, false, copyResources) || !recordRawReadback(resolve.textureId, *owner, error)))
                return false;
            for (const auto& surface : frame.surfaces)
            {
                if (surface.width && surface.textureId == resolve.textureId &&
                    (!acquire(1, false, copyResources) || !recordSurfaceAssembly(surface, *destination, *owner, error)))
                    return false;
                ReplayDrawResources* conversionResources = nullptr;
                if (surface.depthKey && surface.textureId == resolve.textureId &&
                    (!acquire(1, true, conversionResources) || !recordSurfaceConversion(surface, *destination, *conversionResources, *owner, error)))
                    return false;
            }
        }
        const auto snapshotIndex = static_cast<std::uint32_t>(passIndex + 1);
        if (frameColorSnapshots.contains(snapshotIndex) &&
            (!acquire(1, false, resources) || !copyFrameColorBits(snapshotIndex, false, *owner, error)))
            return false;
    }

    return true;
}

bool NativeRendererD3D12::Impl::matchesRenderFramePrefix(const NativeRenderFrame& frame, std::string& error)
{
    const auto& prefix = *recordedRenderPrefix;
    // Only a tail terminal ends a chain: its join put the held passes in front.
    if (!prefix.joined || frame.passes.size() < prefix.passCount)
    {
        error = "terminal native graph must continue the recorded prefix as a tail";
        return false;
    }
    if (frame.identityAttachments != prefix.identityAttachments)
    {
        error = "terminal native graph changed its attachment identity";
        return false;
    }
    for (const auto& pass : frame.passes)
        if (pass.colorSourcePass && pass.colorSourcePass <= prefix.passCount)
        {
            error = "recorded native prefix cannot be a deferred color snapshot source";
            return false;
        }
    return true;
}

NativeDrawReplayResult NativeRendererD3D12::Impl::recordRenderFramePrefix(const NativeRenderFrame& prefix)
{
    NativeDrawReplayResult result;
    const bool             extending = recordedRenderPrefix.has_value();
    if (!recordedRenderPrefix)
    {
        recordedRenderPrefix.emplace(&ownedHeap);
        recordedRenderPrefix->identityAttachments = prefix.identityAttachments;
    }
    if (!recordedRenderPrefix->validation.Append(prefix, result.error))
        return result;
    if (prefix.identityAttachments != recordedRenderPrefix->identityAttachments)
    {
        result.error = "native prefix changed its attachment identity";
        return result;
    }
    frameIdentityAttachments = prefix.identityAttachments;
    recordingRawReadbacks.clear();
    // Packet storage may recycle only after its preceding native use completed.
    // Logical frame resources and attachments survive the remaining graph.
    if (!prepareFrameAttachmentCache(prefix, result.error, true) || !prepareFrameCpuTexturesTimed(prefix, result.error, true, extending) ||
        !prepareFrameUploads(prefix, result.error, false))
        return result;
    reserveRecordingCpuTextures(prefix);
    auto& recorded = *recordedRenderPrefix;
    if (!extending)
    {
        ++frameRecordings;
        frameTextures.clear();
        frameTextureOwners.clear();
        frameColorSnapshots.clear();
    }
    frameCpuTextureFootprintBytes = 0;
    for (const auto& texture : prefix.textures)
    {
        if (texture.imported && !frameTextures.contains(texture.id))
        {
            if (!importFrameTexture(texture, result.error))
            {
                if (recordingFrameLease == &prefix ? recordingImports.contains(texture.id) : frameUsesTexture(prefix, texture.id))
                    return result;
                result.error.clear();
            }
        }
        if (std::none_of(recorded.textures.begin(), recorded.textures.end(), [&](const auto& prior)
                         {
                             return prior.id == texture.id;
                         }))
            recorded.textures.push_back(texture);
    }
    recorded.passCount += prefix.passes.size();
    frameDrawSlots.reserve(kFrameBatchDraws);
    FrameCommandBatch* owner      = nullptr;
    std::size_t        ownerIndex = nextCommandOwner, ownerSlots = 0, ownerCommands = 0;
    if (!recordRenderFramePasses(prefix, 0, owner, ownerIndex, ownerSlots, ownerCommands, result.error) ||
        (owner && !submitFrameCommandBatch(*owner, result.error)))
        return result;
    storeFrameSurfaces(prefix);
    for (auto& [id, readback] : recordingRawReadbacks)
        result.rawReadbacks.emplace_back(id, std::move(readback));
    recordingRawReadbacks.clear();
    result.success = true;
    // The queue signals in submission order, so the latest signal covers this
    // prefix. It is not a completion claim; callers compare the fence.
    result.completionFenceValue = replayFenceValue;
    return result;
}

// A recorded packet's passes stay with the renderer until the chain's tail
// terminal joins them, so the guest need not keep or resend them.
void NativeRendererD3D12::Impl::holdRecordedPrefixPasses(NativeRenderFrame& packet, NativeDrawReplayResult& result)
{
    auto& held = recordedRenderPrefix->passes;
    try
    {
        if (held.capacity() - held.size() < packet.passes.size())
            held.reserve(std::max(held.size() + packet.passes.size(), 2 * held.capacity()));
    }
    catch (const std::bad_alloc&)
    {
        result.success = false;
        result.error   = "could not hold the recorded native prefix passes";
        return;
    }
    for (auto& pass : packet.passes)
        held.push_back(std::move(pass));
    recordedRenderPrefix->typedGuestWords |= packet.typedGuestWords;
}

// A tail terminal continues the chain the renderer holds: its prefix state
// validates it in place, then the held passes join in front of its own,
// before admission charges them. Every failure here breaks the chain.
bool NativeRendererD3D12::Impl::joinRecordedPrefix(NativeRenderFrame& tail, std::unique_lock<std::mutex>& lock, std::string& error)
try
{
    if (!recordedRenderPrefix || recordedRenderPrefix->joined)
    {
        error = "native terminal continues a prefix the renderer does not hold";
        return false;
    }

    struct CountWalks
    {
        Impl&         renderer;
        std::uint64_t start;

        ~CountWalks()
        {
            renderer.ownedJoinWalks.fetch_add(renderer.ownedStateWalks.load(std::memory_order_relaxed) - start, std::memory_order_relaxed);
        }
    } countWalks{ *this, ownedStateWalks.load(std::memory_order_relaxed) };

    // FinishTerminal reuses its admission snapshot; scalar bytes may double
    // count until refresh, but owners stay unique.
    auto& prefix = *recordedRenderPrefix;
    // A direct stream's worker moves the chain's charge to the joined frame.
    if (!directStream)
    {
        if (!awaitOwnedSnapshot(lock, error, true))
            return false;
        const auto              forecast = frameMetadataReservation(tail);
        constexpr std::uint64_t bound    = 512ULL * 1024 * 1024;
        const auto              live     = ownedHeap.live.load(std::memory_order_relaxed);
        const auto              limit    = std::max(ownedHeap.limit, detail::OwnedAdd(live, detail::OwnedAdd(forecast.prefixGrowth, forecast.prefixPeak)));
        const auto              baseline = ownedAdmissionBytes - ownedHeap.limit;
        if (baseline > bound || limit > bound - baseline)
        {
            error = "native tail terminal metadata exceeds the byte bound";
            return false;
        }
        ownedAdmissionBytes = detail::OwnedAdd(baseline, limit);
        ownedHeap.limit     = limit;
        // A later census also walks the tail's owners inside the prefix state.
        if (ownedScanCeiling)
            ownedScanCeiling = detail::OwnedAdd(ownedScanCeiling, forecast.occurrences);
        publishOwnedCharge();
    }
    if (!prefix.validation.FinishTerminal(tail, error) ||
        !JoinNativeRenderFrameTail(tail, std::move(prefix.passes), prefix.textures, prefix.typedGuestWords, error))
        return false;
    prefix.joined = true;
    return true;
}
catch (const std::bad_alloc&)
{
    error = "native tail terminal metadata exceeds the byte bound";
    return false;
}

NativeDrawReplayResult NativeRendererD3D12::Impl::executeRenderFrame(const NativeRenderFrame& frame)
{
    NativeDrawReplayResult result;
    result.outputPath       = frame.outputPath;
    recordingGpuOutput      = nullptr;
    const auto prefixPasses = recordedRenderPrefix ? recordedRenderPrefix->passCount : 0;
    submittedGpuOutput      = nullptr;
    submittedRawReadback.Reset();
    if (recordedRenderPrefix && !matchesRenderFramePrefix(frame, result.error))
        return result;
    // Only a recorded prefix returns resolve readbacks.
    for (std::size_t index = prefixPasses; index < frame.passes.size(); ++index)
        if (std::any_of(frame.passes[index].resolves.begin(), frame.passes[index].resolves.end(), [](const auto& resolve)
                        {
                            return resolve.readback;
                        }))
        {
            result.error = "terminal native frame cannot return resolve readbacks";
            return result;
        }
    frameIdentityAttachments = frame.identityAttachments;

    struct ResetOutput
    {
        FrameGpuOutputSlot*& slot;

        ~ResetOutput()
        {
            slot = nullptr;
        }
    } resetOutput{ recordingGpuOutput };

    if (frame.returnGpuOutput)
    {
        auto& slot = frameGpuOutputs[frameGpuOutputIndex];
        while (std::any_of(submittedFrames.begin(), submittedFrames.end(), [&](const auto& submitted)
                           {
                               return submitted.output == &slot;
                           }))
            if (!retireSubmittedFrames(true, result.error, true))
                return result;
        if (!waitForFence(slot.fenceValue, true))
        {
            result.error = "native output producer did not complete before reuse";
            return result;
        }
        const auto copyDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFenceWaitTimeoutMs);
        while ((slot.ticket && !slot.ticket->AwaitRetirement(0)) || (slot.lease && !slot.lease->AwaitReuse(std::chrono::milliseconds(0))))
        {
            // A pending consumer copy must not block publication of another
            // terminal whose actual native fence has completed.
            if (!retireSubmittedFrames(false, result.error))
                return result;
            std::unique_lock lock(stateMutex);
            if (stopRequested)
            {
                frameRecordingCancelled = true;
                result.error            = "native frame stopped while waiting for SDK output capacity";
                return result;
            }
            if (std::chrono::steady_clock::now() >= copyDeadline || (slot.lease && (slot.lease->DeadlineExpired() || slot.lease->State() == OutputLeaseState::Failed)))
            {
                result.error = "native GPU output consumer copy did not retire";
                return result;
            }
            if (slot.lease)
            {
                lock.unlock();
                slot.lease->AwaitRetirement(std::chrono::milliseconds(10));
            }
            else
            {
                stateCv.wait_for(lock, std::chrono::milliseconds(10), [&]
                                 {
                                     return stopRequested;
                                 });
            }
        }
        slot.ticket.reset();
        slot.lease.reset();
        recordingGpuOutput  = &slot;
        submittedGpuOutput  = &slot;
        frameGpuOutputIndex = (frameGpuOutputIndex + 1) % frameGpuOutputs.size();
    }
    else if (!frame.retainOnly && !awaitFrameGpuOutputs(result.error))
        return result;
    if (frame.returnGpuOutput && !streamOutputPublisher && !streamTitlePublisher)
    {
        result.error = "native GPU output requires a shared-device publisher";
        return result;
    }
    if (!prepareFrameAttachmentCache(frame, result.error, recordedRenderPrefix.has_value()))
        return result;

    struct ResetCaching
    {
        bool& enabled;

        ~ResetCaching()
        {
            enabled = false;
        }
    } resetCaching{ frameAttachmentCaching };

    // Full-frame admission bounds the working set; pipelines age by frame
    // recordings and leave the cache only once retired and long unused.
    if (!recordedRenderPrefix)
    {
        ++frameRecordings;
        frameTextures.clear();
        frameTextureOwners.clear();
    }
    frameColorSnapshots.clear();
    if (!prepareFrameCpuTexturesTimed(frame, result.error, false, recordedRenderPrefix.has_value()))
        return result;
    reserveRecordingCpuTextures(frame);
    frameCpuTextureFootprintBytes = 0;
    for (const auto& texture : frame.textures)
        if (texture.imported)
        {
            if (!importFrameTexture(texture, result.error))
            {
                if (recordingFrameLease == &frame ? recordingImports.contains(texture.id) : frameUsesTexture(frame, texture.id))
                    return result;
                result.error.clear();
            }
        }
    const auto retainCompleted = [&]()
    {
        exportFrameTextures(frame, recordingTerminalRequest);
        // The terminal native fence completed. Keep only versions
        // explicitly carried by this submission, retiring unused old versions.
        std::map<std::uint64_t, RetainedFrameTexture> retained;
        for (const auto& texture : frame.textures)
            if (texture.persistentKey && frameTextureOwners.contains(texture.id))
                retained.emplace(texture.persistentKey, RetainedFrameTexture{ texture, frameTextures.at(texture.id), frameTextureOwners.at(texture.id), recordingTerminalRequest ? recordingTerminalRequest->ticket->Export(texture.persistentKey) : texture.producerVersion });
        retainedFrameTextures = std::move(retained);
        recordedRenderPrefix.reset();
        pruneFramePipelines();
        frameTextures.clear();
        frameTextureOwners.clear();
        frameColorSnapshots.clear();
        frameRetiredUploads.clear();
        frameRetiredUploadLeases.clear();
        uncountCpuTextures(*this, true);
        frameRetiredCpuTextures.clear();
        frameUploadPrepared = false;
    };
    for (const auto& pass : frame.passes)
        if (pass.colorSourcePass)
            frameColorSnapshots.try_emplace(pass.colorSourcePass);
    if (!prepareFrameUploads(frame, result.error, true, prefixPasses))
        return result;
    frameDrawSlots.reserve(kFrameBatchDraws);

    FrameCommandBatch* owner      = nullptr;
    std::size_t        ownerIndex = nextCommandOwner, ownerSlots = 0, ownerCommands = 0;
    const auto         flush = [&]()
    {
        if (!owner)
            return true;
        if (!submitFrameCommandBatch(*owner, result.error))
            return false;
        owner      = nullptr;
        ownerIndex = (ownerIndex + 1) % kFrameCommandBatches;
        return true;
    };
    // Terminal gamma shares the final remaining-pass owner and descriptor range.
    const auto acquire = [&](std::size_t commands, bool descriptors, ReplayDrawResources*& resources)
    {
        {
            std::lock_guard lock(stateMutex);
            if (stopRequested)
            {
                frameRecordingCancelled = true;
                result.error            = "native frame stopped before terminal commands";
                return false;
            }
        }
        if (owner && frameBatchFull(*owner, ownerSlots, ownerCommands, commands, descriptors))
            if (!flush())
                return false;
        if (!owner)
        {
            owner = &frameCommandBatches[ownerIndex];
            if (!prepareFrameCommandBatch(*owner, result.error))
                return false;
            ownerSlots = ownerCommands = 0;
        }
        resources = descriptors ? &frameDrawSlot(*owner, ownerIndex, ownerSlots++) : nullptr;
        ownerCommands += commands;
        return true;
    };
    const auto prepareTarget = [&](NativeRenderFramePass::Target target)
    {
        if (!frameAttachmentCaching && replayColorTargets[0] &&
            replayColorKey(replayColorTargets) != replayColorKey(target))
            return flush() && completeFrameCommands(result.error);
        return true;
    };
    ReplayDrawResources* resources = nullptr;
    if (!recordRenderFramePasses(frame, prefixPasses, owner, ownerIndex, ownerSlots, ownerCommands, result.error))
        return result;
    storeFrameSurfaces(frame);

    if (frame.retainOnly)
    {
        ComPtr<ID3D12Resource>             rawReadback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64                             rawBytes = 0;
        if (frame.returnRawOutput)
        {
            auto*      texture = frameTextures.at(frame.outputTextureId).Get();
            const auto desc    = texture->GetDesc();
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &rawBytes);
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width     = rawBytes;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
            buffer.Layout                                                                        = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (!reserveStorage(StorageDomain::Attachment, rawBytes, result.error) || !acquire(1, false, resources) || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rawReadback))))
            {
                result.error = "raw_readback_allocation_failed";
                return result;
            }
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource   = texture;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
            owner->commandList->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture;
            source.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource       = rawReadback.Get();
            destination.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = footprint;
            owner->commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            owner->commandList->ResourceBarrier(1, &barrier);
        }
        if (!flush() || (!deferFrameCompletion && !completeFrameCommands(result.error)))
            return result;
        // A deferred frame maps its readback when it retires at its fence.
        if (rawReadback && deferFrameCompletion)
        {
            submittedRawReadback  = std::move(rawReadback);
            submittedRawFootprint = footprint;
            submittedRawBytes     = rawBytes;
        }
        else if (rawReadback && !mapRawReadback(rawReadback.Get(), footprint, rawBytes, result))
            return result;
        if (!deferFrameCompletion)
            retainCompleted();
        else
            exportFrameTextures(frame, recordingTerminalRequest);
        result.success              = true;
        result.completionFenceValue = replayFenceValue;
        return result;
    }

    // Present the native resolved texture through the existing fenced gamma
    // path. These are internal blit shaders, not guest shader identities.
    NativeDrawReplayRecipe present;
    present.schemaVersion = 2;
    present.width         = 1280;
    present.height        = 720;
    present.sampleCount = present.sampleMask = 1;
    present.vertexShaderHash                 = 1;
    present.pixelShaderHash                  = 2;
    const auto copyBytecode                  = [](ID3DBlob* blob, std::vector<std::uint8_t>& bytes)
    {
        const auto* begin = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
        bytes.assign(begin, begin + blob->GetBufferSize());
    };
    copyBytecode(replayCopyVertexShader.Get(), present.vertexShaderDxil);
    copyBytecode(replayCopyPixelShader.Get(), present.pixelShaderDxil);
    present.vertexAttributeCount = 1;
    present.vertexStrideBytes    = 16;
    present.vertexData.resize(3 * 16);
    present.indices    = { 0, 1, 2 };
    present.indexCount = 3;
    present.vertexConstants.resize(4096);
    present.pixelConstants.resize(3584);
    present.sharedConstants.resize(336);
    present.viewport                   = { 0, 0, 1280, 720, 0, 1 };
    present.scissor                    = { 0, 0, 1280, 720 };
    present.textureMask                = 1;
    present.textures[0].width          = 1280;
    present.textures[0].height         = 720;
    present.textures[0].frameTextureId = frame.outputTextureId;
    present.textures[0].swizzle        = frame.outputSwizzle;
    std::vector<std::uint8_t> output;
    const bool                readback = frame.returnResolvedOutput || !frame.outputPath.empty();
    if (!prepareTarget({ present.width, present.height, present.sampleCount, present.targetFormat }) || !acquire(1, true, resources))
        return result;
    const auto presented = executeOffscreenReplayOnRendererThread(present, false, readback, &frame.gammaTable, nullptr, 0, false, resources, owner);
    ++frameUploadEpoch;
    if (vertexResidency)
        vertexResidency->AdvanceSwap();
    if (!presented.success)
    {
        result.error = presented.error;
        return result;
    }
    if (!flush() || (!deferFrameCompletion && !completeFrameCommands(result.error)))
        return result;
    if (readback)
    {
        if (!readReplayOutput(present, true, output, result.error))
            return result;
        constexpr std::size_t planeBytes = 1280U * 720U * 4U;
        if (output.size() != planeBytes * 3)
        {
            result.error = "native render frame returned an incomplete gamma output";
            return result;
        }
        const auto* pixels = output.data() + planeBytes * 2;
        if (!frame.outputPath.empty())
        {
            std::ofstream file(frame.outputPath, std::ios::binary | std::ios::noreplace);
            if (!file || !file.write(reinterpret_cast<const char*>(pixels), planeBytes))
            {
                result.error = "could not write native render frame output";
                return result;
            }
            file.flush();
            file.close();
            if (!file)
            {
                result.error = "could not flush native render frame output";
                return result;
            }
        }
        if (frame.returnResolvedOutput)
            result.resolvedOutput.assign(pixels, pixels + planeBytes);
    }
    if (frame.returnGpuOutput && !deferFrameCompletion && streamTitlePublisher)
    {
        auto lease = streamTitlePublisher(replayGammaTarget.Get(), fence.Get(), replayFenceValue);
        if (!lease || !lease->Token())
        {
            result.error = "title presentation rejected the completed native GPU output";
            return result;
        }
        result.presenterToken          = lease->Token();
        recordingGpuOutput->lease      = lease;
        recordingGpuOutput->fenceValue = replayFenceValue;
        gpuOutputLease.store(std::move(lease), std::memory_order_release);
    }
    else if (frame.returnGpuOutput && !deferFrameCompletion)
    {
        auto ticket = streamOutputPublisher(replayGammaTarget.Get(), fence.Get(), replayFenceValue);
        if (!ticket || !ticket->token())
        {
            result.error = "SDK rejected the completed native GPU output";
            return result;
        }
        result.presenterToken          = ticket->token();
        result.gpuOutputTicket         = ticket;
        recordingGpuOutput->ticket     = ticket;
        recordingGpuOutput->fenceValue = replayFenceValue;
        gpuOutputTicket.store(std::move(ticket), std::memory_order_release);
    }
    result.success              = true;
    result.completionFenceValue = replayFenceValue;
    if (!deferFrameCompletion)
        retainCompleted();
    else
        exportFrameTextures(frame, recordingTerminalRequest);
    return result;
}

bool NativeRendererD3D12::Impl::awaitGpuOutput(bool requireCopy, std::string& error, std::uint64_t* value)
{
    if (auto lease = gpuOutputLease.load(std::memory_order_acquire))
        return awaitGpuOutputLease(lease, requireCopy, error, value);
    return awaitGpuOutputTicket(gpuOutputTicket.load(std::memory_order_acquire), requireCopy, error, value);
}

bool NativeRendererD3D12::Impl::awaitFrameGpuOutputs(std::string& error)
{
    for (const auto& slot : frameGpuOutputs)
        if (!awaitGpuOutputTicket(slot.ticket, false, error) || !awaitGpuOutputLease(slot.lease, false, error))
            return false;
    return true;
}

void NativeRendererD3D12::Impl::detachReplayGpuObjects()
{
    if (!detachingFrameContexts)
    {
        detachedRetiredUploads.clear();
        detachingFrameContexts = true;
        const auto previous    = activeFrameContext;
        for (std::size_t index = 0; index < frameContexts.size(); ++index)
            if (index != previous)
            {
                switchFrameContext(index);
                detachReplayGpuObjects();
            }
        switchFrameContext(previous);
        detachingFrameContexts = false;
    }
    for (auto& slot : frameGpuOutputs)
        (void)slot.resource.Detach();
    // Deferred raw readbacks the abandoned queue may still write.
    for (auto& submitted : submittedFrames)
        (void)submitted.rawReadback.Detach();
    (void)submittedRawReadback.Detach();
    for (auto& batch : frameCommandBatches)
    {
        (void)batch.commandList.Detach();
        (void)batch.allocator.Detach();
        (void)batch.viewHeap.Detach();
        (void)batch.samplerHeap.Detach();
        for (auto& helper : batch.helpers)
            helper.detach();
    }
    for (auto& entry : frameColorAttachmentCache)
        for (auto& resource : entry.resources)
            (void)resource.Detach();
    for (auto& entry : frameDepthAttachmentCache)
        (void)entry.resource.Detach();
    detachUpload(frameUploadArena);
    frameUploadPrepared = false;
    for (auto& resource : frameRetiredUploads)
        detachedRetiredUploads.push_back(resource.Detach());
    for (auto& retired : frameRetiredUploadLeases)
        detachUpload(retired);
    frameUploadMapped = nullptr;
    for (auto& draw : frameDrawSlots)
        draw.detach();
    for (auto& [key, texture] : frameCpuTextures)
    {
        (void)texture.resource.Detach();
        detachUpload(texture.upload);
    }
    for (auto& texture : frameRetiredCpuTextures)
    {
        (void)texture.resource.Detach();
        detachUpload(texture.upload);
    }
    for (auto& [key, texture] : retainedFrameTextures)
        (void)texture.resource.Detach();
    for (auto& [id, buffer] : frameColorSnapshots)
        (void)buffer.Detach();
    for (auto& [id, texture] : frameTextures)
        (void)texture.Detach();
    for (auto& [id, owner] : frameTextureOwners)
        (void)owner->resource.Detach();
    for (auto& [key, retained] : retainedFrameTextures)
        if (retained.owner)
            (void)retained.owner->resource.Detach();
    {
        std::lock_guard lock(surfaceMutex);
        for (auto& [key, stored] : surfaceStore)
            if (stored.owner)
                (void)stored.owner->resource.Detach();
        for (auto& [value, owner] : surfaceRetires)
            if (owner)
                (void)owner->resource.Detach();
    }
    for (auto& [key, pipeline] : frameResolvePipelines)
        (void)pipeline.Detach();
    (void)frameResolveShader.Detach();
    (void)frameResolveSingleShader.Detach();
    for (auto& [key, pipeline] : frameGuestWordPipelines)
        (void)pipeline.Detach();
    for (auto& shader : frameGuestWordShaders)
        (void)shader.Detach();
    (void)surfaceDepthPipeline.Detach();
    (void)surfaceDepthShader.Detach();
    (void)frameGuestWordRootSignature.Detach();
    detachReplayCache();
    for (auto& draw : replayDrawSlots)
    {
        draw.detach();
    }
    (void)replayInitialSample0.Detach();
    (void)replayInitialSample1.Detach();
    (void)replayInitialSample0Upload.Detach();
    (void)replayInitialSample1Upload.Detach();
    for (auto& target : replayColorTargets)
    {
        (void)target.Detach();
    }
    (void)replayDepthTarget.Detach();
    replayDepthInitialized = false;
    (void)replayDsvHeap.Detach();
    replayDsvResource = nullptr;
    for (auto& object : replayInitialDepth)
    {
        (void)object.Detach();
    }
    for (auto& object : replayInitialDepthUpload)
    {
        (void)object.Detach();
    }
    if (!detachingFrameContexts && vertexResidency)
    {
        // Resident chunks may still be read by in-flight lists: leak them.
        clearVertexResidency(SeamDetach);
        vertexResidency->DetachAll();
        for (auto& chunk : residencyChunks)
            (void)chunk.Detach();
        publishVertexResidency();
    }
    if (!detachingFrameContexts)
    {
        uploadPool.DetachAll([](NativeUploadBuffer& buffer) noexcept
                             {
                                 (void)buffer.resource.Detach();
                             });
        detachPooled(texturePool);
        detachPooled(attachmentPool);
        // Owners still holding a lost destination post to the closed queue
        // and release it themselves.
        resolveReturns->DetachAll([](ComPtr<ID3D12Resource>& resource) noexcept
                                  {
                                      (void)resource.Detach();
                                  });
        detachPooled(resolvePool);
        resolveReturns = std::make_shared<TextureReturns>();
        for (auto& [key, entry] : versionTextures)
            (void)entry.resource.Detach();
        for (auto& release : versionTextureReleases)
            (void)release.resource.Detach();
        versionTextures.clear();
        versionTexturesOpen.clear();
        versionTextureReleases.clear();
        versionTextureBytes = 0;
        noteVersionTextureBytes();
    }
    // Prefix readbacks outlive this detach in their holders; leak exactly one
    // reference each so a late release cannot free a buffer still written.
    // An unretired one already leaked its retired-upload reference.
    if (!detachingFrameContexts)
    {
        std::lock_guard lock(rawReadbackMutex);
        for (const auto& weak : liveRawReadbacks)
            if (const auto readback = weak.lock();
                readback && readback->buffer && std::find(detachedRetiredUploads.begin(), detachedRetiredUploads.end(), readback->buffer.Get()) == detachedRetiredUploads.end())
                readback->buffer->AddRef();
        detachedRetiredUploads.clear();
    }
}

void NativeRendererD3D12::Impl::shutdownOnRendererThread()
{
    cancelRecordingCpuReservation();
    clearVertexResidency(SeamShutdown);
    ownedRawOwnerOccurrences = 0;
    {
        std::lock_guard lock(stateMutex);
        ownedAdmissionSnapshotValid   = false;
        ownedAdmissionSnapshotImports = nullptr;
        ownedRefreshRequested         = false;
        // The stable permit survives drain failure and retained GPU owners.
        // Shutdown never lowers it while persistent tables can deallocate.
    }
    for (auto& slot : frameGpuOutputs)
    {
        std::string outputError;
        const bool  ticketRetired = awaitGpuOutputTicket(slot.ticket, false, outputError);
        const bool  leaseRetired  = awaitGpuOutputLease(slot.lease, false, outputError);
        if ((!ticketRetired || !leaseRetired) && device && SUCCEEDED(device->GetDeviceRemovedReason()))
        {
            // A native queue fence cannot retire a concurrent consumer source read.
            (void)slot.resource.Detach();
            gpuObjectsAbandoned.store(true, std::memory_order_release);
            REXLOG_ERROR("Native GPU output drain failed: {}", outputError);
        }
        slot.ticket.reset();
        slot.lease.reset();
    }
    gpuOutputTicket.store({}, std::memory_order_release);
    gpuOutputLease.store({}, std::memory_order_release);
#if REREVVED_DEVELOPER
    if (initialized.load(std::memory_order_acquire) && replayRecordedDraws.load(std::memory_order_relaxed))
    {
        REXLOG_INFO("Native D3D12 replay timing: draws={}, submissions={}, recording_ns={}, fence_wait_ns={}, texture_upload_ns={}, draw_upload_ns={}",
                    replayRecordedDraws.load(std::memory_order_relaxed),
                    replayQueueSubmissions.load(std::memory_order_relaxed),
                    replayRecordingNanoseconds.load(std::memory_order_relaxed),
                    replayFenceWaitNanoseconds.load(std::memory_order_relaxed),
                    replayTextureUploadNanoseconds.load(std::memory_order_relaxed),
                    replayDrawUploadNanoseconds.load(std::memory_order_relaxed));
    }
#endif
    frameAttachmentByteBound.store(0, std::memory_order_relaxed);
    bool abandonGpuObjects = false;
    if (initialized.load(std::memory_order_acquire) && queue && fence && !waitForGpu())
    {
        const HRESULT reason = device ? device->GetDeviceRemovedReason() : E_FAIL;
        abandonGpuObjects    = !FAILED(reason);
        if (abandonGpuObjects)
        {
            // A live device with an uncompleted fence may still reference every
            // submitted object. Let process teardown reclaim them instead of
            // releasing storage that the GPU may still be using.
            REXLOG_ERROR("Native D3D12 drain failed on a live device; abandoning GPU objects until process exit");
        }
    }
    initialized.store(false, std::memory_order_release);
    if (abandonGpuObjects)
    {
        detachReplayGpuObjects();
        (void)commandList.Detach();
        for (auto& allocator : allocators)
        {
            (void)allocator.Detach();
        }
        for (auto& backBuffer : backBuffers)
        {
            (void)backBuffer.Detach();
        }
        (void)rtvHeap.Detach();
        (void)swapChain.Detach();
        (void)queue.Detach();
        (void)fence.Detach();
        (void)replayInfoQueue.Detach();
        (void)device.Detach();
        (void)adapter.Detach();
        (void)factory.Detach();
        fenceEvent = nullptr;
        gpuObjectsAbandoned.store(true, std::memory_order_release);
        return;
    }
    for (auto& slot : frameGpuOutputs)
        slot = {};
    frameGpuOutputIndex = 0;
    recordingGpuOutput  = nullptr;
    recordedRenderPrefix.reset();
    frameTextures.clear();
    frameTextureOwners.clear();
    frameRetiredUploads.clear();
    frameRetiredUploadLeases.clear();
    frameRetiredCpuTextures.clear();
    submittedFrames.clear();
    for (auto& context : frameContexts)
        context = FrameContext{};
    activeFrameContext = nextCommandOwner = 0;
    rendererGeneration                    = std::make_shared<int>(0);
    frameColorSnapshots.clear();
    frameResolvePipelines.clear();
    retainedFrameTextures.clear();
    {
        std::lock_guard lock(surfaceMutex);
        surfaceStore.clear();
        surfaceRetires.clear();
    }
    frameDrawSlots.clear();
    for (auto& batch : frameCommandBatches)
        batch = {};
    frameCpuTextures.clear();
    frameCpuTexturePayloadBytes.store(0, std::memory_order_relaxed);
    frameCpuTextureOwnedBytes.store(0, std::memory_order_relaxed);
    frameCpuTexturePayloadBudget = 0;
    frameColorAttachmentCache.clear();
    frameDepthAttachmentCache.clear();
    frameAttachmentCaching = false;
    releaseVertexResidency();
    frameUploadMapped = nullptr;
    frameUploadArena.reset();
    frameUploadPrepared = false;
    frameResolveShader.Reset();
    frameResolveSingleShader.Reset();
    commandList.Reset();
    for (auto& allocator : allocators)
    {
        allocator.Reset();
    }
    releaseBackBuffers();
    for (auto& draw : replayDrawSlots)
    {
        draw.reset();
    }
    replayInitialSample0.Reset();
    replayInitialSample1.Reset();
    replayInitialSample0Upload.Reset();
    replayInitialSample1Upload.Reset();
    for (auto& target : replayColorTargets)
    {
        target.Reset();
    }
    replayDepthTarget.Reset();
    replayDepthInitialized = false;
    replayDsvHeap.Reset();
    for (auto& object : replayInitialDepth)
    {
        object.Reset();
    }
    for (auto& object : replayInitialDepthUpload)
    {
        object.Reset();
    }
    clearReplayCache();
    // Every lease holder is released; a restart may bring another device.
    uploadPool.ReleaseIdle();
    versionTextures.clear();
    versionTexturesOpen.clear();
    versionTextureReleases.clear();
    versionTextureBytes = 0;
    noteVersionTextureBytes();
    texturePool.ReleaseAll();
    attachmentPool.ReleaseAll();
    resolveReturns->Close();
    resolvePool.ReleaseAll();
    resolveReturns           = std::make_shared<TextureReturns>();
    replayFenceValue         = 0;
    replaySubmissionInFlight = false;
    rtvHeap.Reset();
    swapChain.Reset();
    queue.Reset();
    fence.Reset();
    replayInfoQueue.Reset();
    castingDevice.Reset();
    castableGuestWords.store(false, std::memory_order_relaxed);
    device.Reset();
    sharedStreamDevice.Reset();
    streamOutputPublisher = {};
    streamTitlePublisher  = {};
    adapter.Reset();
    factory.Reset();
    if (fenceEvent)
    {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
    if (terminalEvent)
        CloseHandle(std::exchange(terminalEvent, nullptr));
    {
        std::lock_guard eventLock(requestEventMutex);
        if (requestEvent)
            CloseHandle(std::exchange(requestEvent, nullptr));
    }
    fenceValues.fill(0);
    nextFenceValue = 1;
    width          = 0;
    height         = 0;
}

#endif

void NativeRendererD3D12::rendererThreadMain(std::uintptr_t nativeWindow,
                                             std::uint32_t  width,
                                             std::uint32_t  height)
{
#if defined(_WIN32)
    const bool initializationSucceeded =
        impl->initializeOnRendererThread(nativeWindow, width, height);
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initializationSucceeded = initializationSucceeded;
        impl->initializationDone      = true;
    }
    impl->notifyStream();
    if (!initializationSucceeded)
    {
        return;
    }

    for (;;)
    {
        bool          resize          = false;
        std::uint32_t requestedWidth  = 0;
        std::uint32_t requestedHeight = 0;
        {
            std::unique_lock lock(impl->stateMutex);
            impl->stateCv.wait(lock, [this]()
                               {
                                   return impl->stopRequested || impl->resizePending;
                               });
            if (impl->stopRequested)
            {
                break;
            }
            resize              = impl->resizePending;
            requestedWidth      = impl->requestedWidth;
            requestedHeight     = impl->requestedHeight;
            impl->resizePending = false;
        }

        bool requestSucceeded = true;
        if (resize)
        {
            requestSucceeded = impl->resizeOnRendererThread(
                requestedWidth, requestedHeight);
        }
        if (!requestSucceeded)
        {
            handleRendererFailure();
            break;
        }
    }
    impl->shutdownOnRendererThread();
#else
    (void)nativeWindow;
    (void)width;
    (void)height;
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initializationSucceeded = false;
        impl->initializationDone      = true;
    }
    impl->notifyStream();
#endif
    impl->initialized.store(false, std::memory_order_release);
}

void NativeRendererD3D12::headlessReplayThreadMain(
    NativeDrawReplayRecipe               recipe,
    std::promise<NativeDrawReplayResult> resultPromise)
{
#if defined(_WIN32)
    NativeDrawReplayResult result;
    try
    {
        if (!impl->initializeHeadlessOnRendererThread(recipe.width, recipe.height))
        {
            result.outputPath = recipe.outputPath;
            result.error      = "headless D3D12 initialization failed";
        }
        else
        {
            result = impl->executeOffscreenReplayOnRendererThread(recipe);
        }
    }
    catch (const std::exception& exception)
    {
        result.outputPath = recipe.outputPath;
        result.error      = std::string("native replay exception: ") + exception.what();
    }
    catch (...)
    {
        result.outputPath = recipe.outputPath;
        result.error      = "native replay failed with an unknown exception";
    }
    impl->shutdownOnRendererThread();
    resultPromise.set_value(std::move(result));
#else
    (void)recipe;
    NativeDrawReplayResult result;
    result.error = "native D3D12 replay requires Windows";
    resultPromise.set_value(std::move(result));
#endif
}

void NativeRendererD3D12::drawStreamThreadMain()
{
#if defined(_WIN32)
    bool        initializationSucceeded = false;
    std::string initializationError;
    try
    {
        initializationSucceeded = impl->initializeHeadlessOnRendererThread(1280, 720);
        if (!initializationSucceeded)
            initializationError = "headless D3D12 draw stream initialization failed";
    }
    catch (const std::exception& exception)
    {
        initializationError = std::string("headless D3D12 draw stream initialization exception: ") + exception.what();
    }
    catch (...)
    {
        initializationError = "headless D3D12 draw stream initialization failed with an unknown exception";
    }
    if (!initializationSucceeded)
    {
        try
        {
            impl->shutdownOnRendererThread();
        }
        catch (...)
        {
        }
    }
    else
    {
        impl->refreshOwnedAdmission();
        {
            std::lock_guard lock(impl->stateMutex);
            if (!impl->ownedAdmissionSnapshotValid)
            {
                initializationSucceeded = false;
                initializationError     = impl->ownedCapacityError;
            }
        }
        if (!initializationSucceeded)
        {
            try
            {
                impl->shutdownOnRendererThread();
            }
            catch (...)
            {
            }
        }
    }
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initializationSucceeded = initializationSucceeded;
        impl->initializationDone      = true;
        impl->initializationError     = std::move(initializationError);
        impl->ownedAdmissionBusy      = false;
        if (!initializationSucceeded)
            impl->runtimeFailed = true;
    }
    impl->notifyStream();
    if (!initializationSucceeded)
        return;

    // The worker is busy except in its idle wait for a request or terminal.
    // Each pass and the loop's exit publish the span so far, so a worker that
    // never idles still reports it.
#if REREVVED_DEVELOPER
    auto       busySince   = std::chrono::steady_clock::now();
    const auto publishBusy = [&]
    {
        const auto now = std::chrono::steady_clock::now();
        impl->streamBusyNanoseconds.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - busySince).count()), std::memory_order_relaxed);
        busySince = now;
    };
#else
    const auto publishBusy = []
    {
    };
#endif
    for (;;)
    {
        publishBusy();
        std::function<void()> beforeRequest;
        {
            std::lock_guard lock(impl->stateMutex);
            beforeRequest = impl->beforeRequestTestHook;
        }
        if (beforeRequest)
            beforeRequest();
        std::vector<NativeDrawReplayRecipe>                 draws;
        std::filesystem::path                               sampleOutputPath;
        std::optional<NativeDrawStreamOutput>               outputRequest;
        std::optional<NativeRenderFrame>                    renderFrame;
        std::optional<std::promise<NativeDrawReplayResult>> promise;
        std::shared_ptr<Impl::TerminalRequest>              terminal;
        bool                                                deferred = false;
        bool                                                cancel = false, prefixRequest = false;
        // A queued request the worker admits at intake, and its refusal.
        bool          queuedIntake = false, allowAppend = false, refused = false, refusalBreaksChain = false;
        std::string   refusalError;
        std::string   retirementError;
        std::uint64_t requestBytes              = 0; // native_direct_stream charge
        const auto    submittedBeforeRetirement = impl->submittedFrames.size();
        if (submittedBeforeRetirement)
        {
            std::lock_guard lock(impl->stateMutex);
            impl->ownedAdmissionBusy = true;
        }
        if (!impl->retireSubmittedFrames(false, retirementError))
        {
            std::lock_guard lock(impl->stateMutex);
            impl->runtimeFailed = true;
            impl->stopRequested = true;
        }
        if (submittedBeforeRetirement)
        {
            bool refresh = impl->submittedFrames.size() != submittedBeforeRetirement;
            {
                std::lock_guard lock(impl->stateMutex);
                refresh |= !impl->ownedAdmissionSnapshotValid;
                if (!refresh)
                    impl->ownedAdmissionBusy = false;
            }
            if (refresh)
            {
                impl->refreshOwnedAdmission();
                std::lock_guard lock(impl->stateMutex);
                impl->ownedAdmissionBusy = false;
            }
            impl->stateCv.notify_all();
        }
        {
            std::unique_lock lock(impl->stateMutex);
            if (!impl->stopRequested && !impl->drawStreamPending && (impl->ownedCapacityRequest || impl->ownedRefreshRequested))
            {
                const auto requested     = std::exchange(impl->ownedCapacityRequest, 0);
                impl->ownedAdmissionBusy = true;
                lock.unlock();
                impl->serviceOwnedCapacity(requested);
                {
                    std::lock_guard capacityLock(impl->stateMutex);
                    impl->ownedAdmissionBusy = false;
                }
                impl->stateCv.notify_all();
                continue;
            }
            if (!impl->stopRequested && !impl->drawStreamPending)
            {
                lock.unlock();
                if (!impl->submittedFrames.empty())
                {
                    const auto value = impl->submittedFrames.front().fenceValue;
                    if (FAILED(impl->fence->SetEventOnCompletion(value, impl->terminalEvent)))
                    {
                        std::lock_guard failureLock(impl->stateMutex);
                        impl->runtimeFailed = true;
                        impl->stopRequested = true;
                        continue;
                    }
                    if (impl->fence->GetCompletedValue() >= value)
                        continue;
                }
                HANDLE events[] = { impl->requestEvent, impl->terminalEvent };
                publishBusy();
                WaitForMultipleObjects(2, events, FALSE, INFINITE);
#if REREVVED_DEVELOPER
                busySince = std::chrono::steady_clock::now();
#endif
                continue;
            }
            if (impl->stopRequested && !impl->drawStreamPending)
                break;
            // The slot, filled only while the queue is empty, is the oldest.
            if (impl->drawStreamSlotFilled())
            {
                draws                  = std::move(impl->drawStreamDraws);
                sampleOutputPath       = std::move(impl->drawStreamSampleOutputPath);
                outputRequest          = std::move(impl->drawStreamOutput);
                renderFrame            = std::exchange(impl->drawStreamRenderFrame, std::nullopt);
                prefixRequest          = std::exchange(impl->drawStreamPrefixRequest, false);
                promise                = std::exchange(impl->drawStreamPromise, std::nullopt);
                terminal               = std::exchange(impl->drawStreamTerminal, {});
                impl->recordingImports = std::exchange(impl->drawStreamImports, {});
                requestBytes           = std::exchange(impl->directAdmittedBytes, 0);
            }
            else
            {
                auto& queued = impl->drawStreamQueue.front();
                requestBytes = queued.directBytes;
                renderFrame.emplace(std::move(queued.frame));
                prefixRequest = queued.prefix;
                allowAppend   = queued.allowAppend;
                promise       = std::move(queued.promise);
                terminal      = std::move(queued.terminal);
                impl->drawStreamQueue.pop_front();
                queuedIntake = true;
            }
            impl->drawStreamPending = !impl->drawStreamQueue.empty();
            if (impl->stopRequested)
                cancel = true;
            else
            {
                impl->drawStreamInFlight      = true;
                impl->frameRecordingCancelled = false;
                if (queuedIntake)
                {
                    // Every earlier request has recorded and refreshed the owner
                    // snapshot, so this admission sees the state a caller saw
                    // once the slot drained. A refusal fails only this request.
                    Impl::FrameImports imports;
                    bool               admitted = false;
                    if (prefixRequest && !allowAppend && impl->drawStreamHasPrefix)
                        refusalError = "native D3D12 draw stream is unavailable or owns an unfinished prefix";
                    else if (!prefixRequest && renderFrame->continuedPrefixPasses && !impl->joinRecordedPrefix(*renderFrame, lock, refusalError))
                        refusalBreaksChain = true;
                    else
                    {
                        // The joined frame holds its chain's passes and charge.
                        if (!prefixRequest && renderFrame->continuedPrefixPasses)
                            requestBytes += std::exchange(impl->directChainBytes, 0);
                        admitted = (!terminal || impl->admitTerminalInputs(*renderFrame, lock, refusalError, true)) &&
                                   impl->awaitOwnedAdmission(*renderFrame, imports, lock, refusalError, terminal != nullptr, true);
                        // The caller's chain already moved past a refused packet
                        // or a terminal that would close an open prefix, and an
                        // open prefix cannot end without its terminal.
                        refusalBreaksChain = !admitted && (prefixRequest || impl->drawStreamHasPrefix);
                    }
                    if (impl->stopRequested)
                        cancel = true;
                    else if (!admitted)
                    {
                        refused = true;
                        impl->rebuildQueuedPrefixOpen();
                    }
                    else
                    {
                        impl->recordingImports = std::move(imports);
                        if (prefixRequest)
                            impl->drawStreamHasPrefix = true;
                    }
                }
                if (!cancel && !refused)
                    impl->activeTerminalRequest = terminal;
            }
            impl->directInFlightTerminal = !prefixRequest;
        }
        impl->notifyStream();

        NativeDrawReplayResult result;
        result.outputPath = sampleOutputPath.empty() && outputRequest
                                ? outputRequest->outputPath
                                : sampleOutputPath;
        if (cancel)
        {
            result.error = "native draw stream stopped before frame submission";
        }
        else if (refused)
        {
            result.error = std::move(refusalError);
        }
        else if (renderFrame || impl->awaitFrameGpuOutputs(result.error))
        {
            try
            {
                impl->recordingFrameLease = renderFrame ? &*renderFrame : nullptr;
                if (renderFrame)
                {
                    const auto forecast = impl->frameMetadataReservation(*renderFrame);
                    // A direct stream charged the metadata at admission.
                    bool available = impl->directStream || impl->reserveStorage(Impl::StorageDomain::Owned, forecast.metadata, result.error);
                    if (available)
                    {
                        impl->ownedRecordingReservation       = forecast.metadata;
                        impl->ownedRecordingLegacyKeyCapacity = forecast.legacyKeyCapacity;
                    }
                    // A frame that completes synchronously retires every earlier
                    // deferred frame first, so completions and retained
                    // textures only move forward.
                    const bool deferrable = terminal && !renderFrame->returnResolvedOutput && renderFrame->outputPath.empty();
                    if (!deferrable && !prefixRequest)
                        while (available && !impl->submittedFrames.empty())
                            available = impl->retireSubmittedFrames(true, result.error);
                    const bool activeSubmitted = std::any_of(impl->submittedFrames.begin(), impl->submittedFrames.end(), [&](const auto& submitted)
                                                             {
                                                                 return submitted.context == impl->activeFrameContext;
                                                             });
                    if (available && (activeSubmitted || !impl->recordedRenderPrefix))
                        available = impl->acquireFrameContext(result.error);
                    if (available)
                    {
                        impl->recordingTerminalRequest = terminal;
                        impl->deferFrameCompletion     = deferrable;

                        // An exception while recording still ends the packet as failed.
                        struct PacketScope
                        {
                            Impl* renderer;
                            bool  open = true;

                            void end(bool success)
                            {
                                if (open)
                                {
                                    open = false;
                                    renderer->endResidencyPacket(success);
                                }
                            }

                            ~PacketScope()
                            {
                                end(false);
                            }
                        } packetScope{ impl.get() };

                        impl->beginResidencyPacket();
                        result = prefixRequest ? impl->recordRenderFramePrefix(*renderFrame) : impl->executeRenderFrame(*renderFrame);
                        packetScope.end(result.success);
                        if (terminal && result.success && impl->deferFrameCompletion)
                        {
                            terminal->submitted.store(true);
                            impl->submittedFrames.push_back({ impl->activeFrameContext, impl->replayFenceValue, renderFrame->textures, terminal, result, impl->submittedGpuOutput, std::move(impl->submittedRawReadback), impl->submittedRawFootprint, impl->submittedRawBytes });
                            deferred = true;
                        }
                        impl->deferFrameCompletion = false;
                        impl->recordingTerminalRequest.reset();
                    }
                }
                else
                {
                    while (!impl->submittedFrames.empty())
                        if (!impl->retireSubmittedFrames(true, result.error))
                            break;
                    impl->ownedRawOwnerOccurrences = 0;
                    for (const auto& draw : draws)
                        impl->ownedRawOwnerOccurrences = detail::OwnedAdd(impl->ownedRawOwnerOccurrences, detail::OwnedAdd(6, detail::OwnedMultiply(3, draw.textures.size())));
                    const bool                readback = !sampleOutputPath.empty() || outputRequest.has_value();
                    std::vector<std::uint8_t> output;
                    for (std::size_t index = 0; index < draws.size(); ++index)
                    {
                        const bool last   = index + 1 == draws.size();
                        const auto replay = impl->executeOffscreenReplayOnRendererThread(
                            draws[index],
                            index != 0,
                            last && readback,
                            outputRequest && last ? &outputRequest->gammaTable : nullptr,
                            readback && last ? &output : nullptr,
                            index % kReplayBatchDraws,
                            last || (index + 1) % kReplayBatchDraws == 0);
                        if (!replay.success)
                        {
                            result.error = "native draw stream draw " + std::to_string(index) + ": " + replay.error;
                            break;
                        }
                        if (!last)
                            continue;

                        if (readback)
                        {
                            const auto planeBytes     = static_cast<std::size_t>(draws[index].width) *
                                                        draws[index].height * 4U;
                            const auto colorBytes     = planeBytes * 2U;
                            const auto expectedPlanes = (draws[index].depth.enabled ? 4U : 2U) +
                                                        (outputRequest ? 1U : 0U);
                            if (output.size() != planeBytes * expectedPlanes)
                            {
                                result.error = "native draw stream readback has an incomplete sample layout";
                                break;
                            }
                            if (!sampleOutputPath.empty())
                            {
                                std::ofstream file(sampleOutputPath, std::ios::binary | std::ios::noreplace);
                                if (!file)
                                {
                                    result.error = "could not open native draw stream sample output";
                                    break;
                                }
                                file.write(reinterpret_cast<const char*>(output.data()),
                                           static_cast<std::streamsize>(colorBytes));
                                file.flush();
                                file.close();
                                if (!file)
                                {
                                    result.error = "could not write native draw stream sample output";
                                    break;
                                }
                            }
                            if (outputRequest && !outputRequest->outputPath.empty())
                            {
                                const auto    gammaOffset = planeBytes * (expectedPlanes - 1U);
                                std::ofstream file(outputRequest->outputPath,
                                                   std::ios::binary | std::ios::noreplace);
                                if (!file)
                                {
                                    result.error = "could not open native draw stream output";
                                    break;
                                }
                                file.write(reinterpret_cast<const char*>(output.data() + gammaOffset),
                                           static_cast<std::streamsize>(planeBytes));
                                file.flush();
                                file.close();
                                if (!file)
                                {
                                    result.error = "could not write native draw stream output";
                                    break;
                                }
                            }
                            if (outputRequest && outputRequest->returnResolvedOutput)
                            {
                                const auto gammaOffset = planeBytes * (expectedPlanes - 1U);
                                result.resolvedOutput.assign(output.begin() + gammaOffset, output.end());
                            }
                        }
                        result.success              = true;
                        result.completionFenceValue = impl->replayFenceValue;
                    }
                }
            }
            catch (const std::exception& exception)
            {
                result.error = std::string("native draw stream exception: ") + exception.what();
            }
            catch (...)
            {
                result.error = "native draw stream failed with an unknown exception";
            }
        }
        // An observed owner stop cancels recording without poisoning restart.
        // Other failures remain terminal even when Shutdown races with them.
        impl->cancelRecordingCpuReservation();
        draws.clear();
        impl->recordingFrameLease = nullptr;
        if (renderFrame && prefixRequest && result.success)
            impl->holdRecordedPrefixPasses(*renderFrame, result);
        const bool failed            = !cancel && (!refused || refusalBreaksChain) && !impl->frameRecordingCancelled && !result.success;
        const bool terminalCompleted = renderFrame && !prefixRequest && result.success;
        // Terminal matching and recording consumed the prefix inventories.
        // Submitted contexts and descriptors retain every remaining GPU owner.
        if (impl->directStream)
        {
            // A recorded packet's passes stay held, and charged, until its
            // chain's tail. Every other charge leaves with its frame, and the
            // deleter releases it once the frame is destroyed.
            Impl::RetiredDeleter::Retired retired;
            {
                std::lock_guard lock(impl->stateMutex);
                if (prefixRequest && result.success)
                    impl->directChainBytes += requestBytes;
                else
                    retired.charge = requestBytes;
                if (terminalCompleted || failed)
                    retired.charge += std::exchange(impl->directChainBytes, 0);
                impl->directDeleterBytes.fetch_add(retired.charge, std::memory_order_relaxed);
            }
            // Bound what the renderer keeps before the deleter releases a charge.
            impl->refreshDirectStructure();
            const auto charge = retired.charge;
            retired.frame     = std::move(renderFrame);
            if (terminalCompleted)
                retired.prefix = std::move(impl->recordedRenderPrefix);
            try
            {
                impl->retiredDeleter.Push(std::move(retired));
            }
            catch (...)
            {
                // Push destroyed the frame with its argument.
                impl->releaseDirect(charge);
            }
        }
        renderFrame.reset();
        if (terminalCompleted)
        {
            impl->recordedRenderPrefix.reset();
#if REREVVED_DEVELOPER
            impl->attachmentMissFrames.fetch_add(1, std::memory_order_relaxed);
#endif
        }
        impl->recordingImports.clear();
        impl->ownedRecordingReservation       = 0;
        impl->ownedRawOwnerOccurrences        = 0;
        impl->ownedRecordingLegacyKeyCapacity = 0;
        impl->refreshOwnedAdmission();
        {
            std::lock_guard lock(impl->stateMutex);
            if (terminalCompleted)
                impl->drawStreamHasPrefix = false;
            if (failed)
            {
                impl->runtimeFailed = true;
                impl->stopRequested = true;
            }
            if (impl->directStream)
            {
                impl->directInFlightTerminal = false;
                impl->publishDirectCharge();
            }
            // Recipes retired before readiness; publish failure and settlement
            // before another caller can claim the single request slot. A
            // refused admission submitted nothing, so it names no fence.
            if (!result.success)
                result.submittedFenceValue = refused ? 0 : impl->replayFenceValue;
            if (promise)
                promise->set_value(std::move(result));
            if (terminal)
            {
                auto recordingResult                 = result;
                recordingResult.completionFenceValue = 0;
                recordingResult.presenterToken       = 0;
                recordingResult.gpuOutputTicket.reset();
                recordingResult.resolvedOutput.clear();
                terminal->SetRecording(std::move(recordingResult));
                if (!deferred)
                    terminal->SetCompletion(std::move(result));
            }
            impl->activeTerminalRequest.reset();
            if (failed)
                impl->cancelQueuedRequests();
            if (!impl->directStream && impl->submittedFrames.empty() && !impl->drawStreamHasPrefix && !impl->drawStreamPending && !impl->admissionImports && !impl->ownedRefreshRequested && !impl->ownedCapacityRequest && impl->ownedAdmissionSnapshotValid)
            {
                const auto live = impl->ownedHeap.live.load(std::memory_order_relaxed);
                impl->ownedAdmissionBytes -= impl->ownedHeap.limit - live;
                impl->ownedHeap.limit      = live;
                impl->ownedMaintenanceWork = 0;
                impl->publishOwnedCharge();
            }
            impl->drawStreamInFlight = false;
        }
        impl->notifyStream();
        if (failed)
            break;
    }
    publishBusy();
    impl->retiredDeleter.Drain();
    std::string terminalError;
    while (!impl->submittedFrames.empty())
        if (!impl->retireSubmittedFrames(true, terminalError))
            break;
    for (auto& submitted : impl->submittedFrames)
    {
        NativeDrawReplayResult failed;
        failed.error               = terminalError.empty() ? "native terminal completion was not proved" : terminalError;
        failed.submittedFenceValue = submitted.fenceValue;
        submitted.request->SetCompletion(std::move(failed));
    }
    impl->shutdownOnRendererThread();
#else
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initializationSucceeded = false;
        impl->initializationDone      = true;
        impl->runtimeFailed           = true;
    }
    impl->notifyStream();
#endif
    impl->initialized.store(false, std::memory_order_release);
}

void NativeRendererD3D12::frameReplayThreadMain(
    NativeFrameReplayRecipe              recipe,
    std::promise<NativeDrawReplayResult> resultPromise)
{
    NativeDrawReplayResult result;
    result.outputPath = recipe.outputPath;
#if defined(_WIN32)
    try
    {
        const auto execute = [&]() -> bool
        {
            if (!impl->initializeHeadlessOnRendererThread(640, 720))
            {
                result.error = "headless frame D3D12 initialization failed";
                return false;
            }
            impl->ownedRawOwnerOccurrences = 0;
            for (const auto& half : recipe.halves)
                for (const auto& draw : half)
                    impl->ownedRawOwnerOccurrences = detail::OwnedAdd(impl->ownedRawOwnerOccurrences, detail::OwnedAdd(6, detail::OwnedMultiply(3, draw.textures.size())));
            constexpr std::size_t     planeBytes = 640U * 720U * 4U;
            std::vector<std::uint8_t> finalImage(planeBytes * 2);
            std::vector<std::uint8_t> samples;
            if (!recipe.sampleOutputPath.empty())
                samples.resize(planeBytes * 4);
            for (std::size_t half = 0; half < recipe.halves.size(); ++half)
            {
                const auto& draws = recipe.halves[half];
                for (std::size_t index = 0; index < draws.size(); ++index)
                {
                    const bool                last = index + 1 == draws.size();
                    std::vector<std::uint8_t> output;
                    const auto                drawResult = impl->executeOffscreenReplayOnRendererThread(
                        draws[index], index != 0, last, last ? &recipe.gammaTable : nullptr, &output, index % kReplayBatchDraws, last || (index + 1) % kReplayBatchDraws == 0);
                    if (!drawResult.success)
                    {
                        result.error = "frame half " + std::to_string(half) + " draw " +
                                       std::to_string(index) + ": " + drawResult.error;
                        return false;
                    }
                    if (!last)
                        continue;
                    const auto expectedPlanes = draws[index].depth.enabled ? 5U : 3U;
                    if (output.size() != planeBytes * expectedPlanes)
                    {
                        result.error = "native frame readback has an unexpected plane layout";
                        return false;
                    }
                    if (!samples.empty())
                        std::memcpy(samples.data() + half * planeBytes * 2, output.data(), planeBytes * 2);
                    const auto* gammaImage = output.data() + planeBytes * (expectedPlanes - 1);
                    for (std::size_t y = 0; y < 720; ++y)
                        std::memcpy(finalImage.data() + (y * 1280 + half * 640) * 4,
                                    gammaImage + y * 640 * 4,
                                    640 * 4);
                }
            }
            const auto write = [&](const std::filesystem::path&     path,
                                   const std::vector<std::uint8_t>& bytes)
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file.write(reinterpret_cast<const char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()));
                file.flush();
                file.close();
                return bool(file);
            };
            if ((!samples.empty() && !write(recipe.sampleOutputPath, samples)) ||
                (!recipe.outputPath.empty() && !write(recipe.outputPath, finalImage)))
            {
                result.error = "could not write native frame output";
                return false;
            }
            if (recipe.returnResolvedOutput)
                result.resolvedOutput = std::move(finalImage);
            result.completionFenceValue = impl->replayFenceValue;
            return true;
        };
        result.success = execute();
    }
    catch (const std::exception& exception)
    {
        result.error = std::string("native frame exception: ") + exception.what();
    }
    catch (...)
    {
        result.error = "native frame failed with an unknown exception";
    }
    impl->shutdownOnRendererThread();
#else
    result.error = "native D3D12 replay requires Windows";
#endif
    resultPromise.set_value(std::move(result));
}

void NativeRendererD3D12::drawFramesThreadMain(NativeDrawFramesRecipe               recipe,
                                               std::promise<NativeDrawReplayResult> promise)
{
    NativeDrawReplayResult result;
    result.outputPath = recipe.outputDirectory;
#if defined(_WIN32)
    try
    {
        if (!impl->initializeHeadlessOnRendererThread(1280, 720))
            throw std::runtime_error("native menu frame device initialization failed");
        impl->ownedRawOwnerOccurrences = 0;
        for (const auto& frame : recipe.frames)
            for (const auto& draw : frame)
                impl->ownedRawOwnerOccurrences = detail::OwnedAdd(impl->ownedRawOwnerOccurrences, detail::OwnedAdd(6, detail::OwnedMultiply(3, draw.textures.size())));
        for (std::size_t frame = 0; frame < recipe.frames.size(); ++frame)
        {
            const auto& draws = recipe.frames[frame];
            for (std::size_t draw = 0; draw < draws.size(); ++draw)
            {
                const bool                last = draw + 1 == draws.size();
                std::vector<std::uint8_t> output;
                const auto                replay = impl->executeOffscreenReplayOnRendererThread(
                    draws[draw], draw != 0, last, nullptr, &output, draw % kReplayBatchDraws, last || (draw + 1) % kReplayBatchDraws == 0);
                if (!replay.success)
                    throw std::runtime_error("native menu frame " + std::to_string(frame) + " draw " +
                                             std::to_string(draw) + ": " + replay.error);
                if (!last)
                    continue;
                constexpr std::size_t colorBytes = 1280U * 720U * 4U * 2U;
                if (output.size() != colorBytes * (draws[draw].depth.enabled ? 2U : 1U))
                    throw std::runtime_error("native menu frame has an unexpected sample layout");
                std::ofstream file(recipe.outputDirectory / ("frame-" + std::to_string(frame) + ".rgba"), std::ios::binary);
                file.write(reinterpret_cast<const char*>(output.data()), colorBytes);
                file.close();
                if (!file)
                    throw std::runtime_error("could not save native menu frame samples");
            }
        }
        result.success = true;
    }
    catch (const std::exception& exception)
    {
        result.error = exception.what();
    }
    catch (...)
    {
        result.error = "native menu frames failed with an unknown exception";
    }
    impl->shutdownOnRendererThread();
#else
    result.error = "native menu frames require Windows D3D12";
#endif
    promise.set_value(std::move(result));
}

void NativeRendererD3D12::handleRendererFailure()
{
    std::function<void()> requestQuit;
    {
        std::lock_guard lock(impl->stateMutex);
        if (impl->runtimeFailed)
        {
            return;
        }
        impl->runtimeFailed = true;
        impl->stopRequested = true;
        requestQuit         = impl->requestDeferredQuit;
    }
    REXLOG_ERROR("Native D3D12 renderer entered terminal failure; requesting deferred quit");
    impl->notifyStream();
    if (requestQuit)
    {
        requestQuit();
    }
}

bool NativeRendererD3D12::Initialize(rex::ui::Window& window)
{
#if !defined(_WIN32)
    (void)window;
    REXLOG_ERROR("The native renderer currently requires Windows D3D12");
    return false;
#else
    {
        std::lock_guard lock(impl->stateMutex);
        if (impl->drawStreamStarted)
        {
            REXLOG_ERROR("Native D3D12 cannot initialize a window while a headless draw stream is active");
            return false;
        }
    }
    if (impl->initialized.load(std::memory_order_acquire))
    {
        return true;
    }
    if (impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
    {
        REXLOG_ERROR("Native D3D12 cannot reinitialize after abandoning in-flight GPU objects");
        return false;
    }
    if (impl->rendererThread.joinable())
    {
        impl->rendererThread.join();
    }

    const HWND hwnd   = static_cast<HWND>(window.GetNativeWindowHandle());
    const auto width  = window.GetActualPhysicalWidth();
    const auto height = window.GetActualPhysicalHeight();
    if (!hwnd)
    {
        REXLOG_ERROR("Native D3D12 could not acquire the ReRevved HWND surface");
        return false;
    }
    if (width == 0 || height == 0)
    {
        REXLOG_ERROR("Native D3D12 surface has no drawable pixel extent");
        return false;
    }

    ResetGuestDevicePublication();
    ResetGuestTextureObservation();
    auto* appContext = &window.app_context();
    {
        std::lock_guard lock(impl->stateMutex);
        impl->stopRequested           = false;
        impl->initializationDone      = false;
        impl->initializationSucceeded = false;
        impl->resizePending           = false;
        impl->requestedWidth          = width;
        impl->requestedHeight         = height;
        impl->runtimeFailed           = false;
        impl->requestDeferredQuit     = [appContext]()
        {
            appContext->RequestDeferredQuit();
        };
    }

    impl->rendererThread = std::thread(
        &NativeRendererD3D12::rendererThreadMain,
        this,
        reinterpret_cast<std::uintptr_t>(hwnd),
        width,
        height);
    {
        std::unique_lock lock(impl->stateMutex);
        impl->stateCv.wait(lock, [this]()
                           {
                               return impl->initializationDone;
                           });
        if (!impl->initializationSucceeded)
        {
            lock.unlock();
            impl->rendererThread.join();
            return false;
        }
    }
    return true;
#endif
}

void NativeRendererD3D12::ConfigureReplayDiagnostics()
{
#if defined(_WIN32)
    enableReplayDebugLayer();
    enableDred();
#endif
}

NativeDrawReplayCacheStats NativeRendererD3D12::ReplayCacheStats() const noexcept
{
#if !defined(_WIN32)
    return {};
#else
    NativeDrawReplayCacheStats stats;
    stats.helperShaderCompilations    = impl->replayHelperShaderCompilations.load(std::memory_order_relaxed);
    stats.rootSignatureCreations      = impl->replayRootSignatureCreations.load(std::memory_order_relaxed);
    stats.pipelineCreations           = impl->replayPipelineCreations.load(std::memory_order_relaxed);
    stats.descriptorHeapCreations     = impl->replayDescriptorHeapCreations.load(std::memory_order_relaxed);
    stats.immutableConstantCreations  = impl->replayImmutableConstantCreations.load(std::memory_order_relaxed);
    stats.outputBundleCreations       = impl->replayOutputBundleCreations.load(std::memory_order_relaxed);
    stats.outputBundleReuses          = impl->replayOutputBundleReuses.load(std::memory_order_relaxed);
    stats.drawUploadCreations         = impl->replayDrawUploadCreations.load(std::memory_order_relaxed);
    stats.drawUploadReuses            = impl->replayDrawUploadReuses.load(std::memory_order_relaxed);
    stats.textureCreations            = impl->replayTextureCreations.load(std::memory_order_relaxed);
    stats.textureReuses               = impl->replayTextureReuses.load(std::memory_order_relaxed);
    stats.textureUploadCreations      = impl->replayTextureUploadCreations.load(std::memory_order_relaxed);
    stats.textureUploadReuses         = impl->replayTextureUploadReuses.load(std::memory_order_relaxed);
    stats.colorTargetCreations        = impl->replayColorTargetCreations.load(std::memory_order_relaxed);
    stats.colorTargetReuses           = impl->replayColorTargetReuses.load(std::memory_order_relaxed);
    stats.depthTargetCreations        = impl->replayDepthTargetCreations.load(std::memory_order_relaxed);
    stats.depthTargetReuses           = impl->replayDepthTargetReuses.load(std::memory_order_relaxed);
    stats.frameAttachmentReuses       = impl->frameAttachmentReuses.load(std::memory_order_relaxed);
    stats.frameAttachmentByteBound    = impl->frameAttachmentByteBound.load(std::memory_order_relaxed);
    stats.texturePayloadReuses        = impl->replayTexturePayloadReuses.load(std::memory_order_relaxed);
    stats.texturePayloadBytes         = impl->frameCpuTexturePayloadBytes.load(std::memory_order_relaxed);
    stats.texturePreparationCreations = impl->replayTexturePreparationCreations.load(std::memory_order_relaxed);
    stats.textureVersionReuses        = impl->replayTextureVersionReuses.load(std::memory_order_relaxed);
    stats.descriptorReuses            = impl->replayDescriptorReuses.load(std::memory_order_relaxed);
    stats.textureContentReuses        = impl->replayTextureContentReuses.load(std::memory_order_relaxed);
    stats.samplerTables               = impl->replaySamplerTables.load(std::memory_order_relaxed);
    stats.samplerTableHits            = impl->replaySamplerTableHits.load(std::memory_order_relaxed);
    stats.samplerTableFlushes         = impl->replaySamplerTableFlushes.load(std::memory_order_relaxed);
    stats.ownedChargeBytes            = impl->ownedChargePublished.load(std::memory_order_relaxed);
    stats.ownedChargePeakBytes        = impl->ownedChargePeak.load(std::memory_order_relaxed);
    const auto attachmentTakes        = impl->attachmentPool.Stats();
    stats.attachmentPoolMisses        = attachmentTakes.misses;
    stats.attachmentPoolEvictedBytes  = attachmentTakes.evictedBytes;
    const auto pool                   = impl->uploadPool.Stats();
    stats.uploadPoolHits              = pool.hits;
    stats.uploadPoolCreations         = pool.creations;
    stats.uploadPoolEvictions         = pool.evictions;
    stats.uploadPoolIdleBytes         = pool.idleBytes;
    stats.uploadPoolLiveBytes         = pool.liveBytes;
    const auto textures               = impl->texturePool.Stats();
    stats.texturePoolHits             = textures.hits;
    stats.texturePoolEvictions        = textures.evictions;
    stats.texturePoolIdleBytes        = textures.idleBytes;
    stats.texturePoolPendingBytes     = textures.pendingBytes;
    const auto attachments            = impl->attachmentPool.Stats();
    stats.attachmentPoolHits          = attachments.hits;
    stats.attachmentPoolEvictions     = attachments.evictions;
    stats.attachmentPoolIdleBytes     = attachments.idleBytes;
    const auto resolves               = impl->resolvePool.Stats();
    stats.resolvePoolHits             = resolves.hits;
    stats.resolvePoolEvictions        = resolves.evictions;
    stats.resolvePoolIdleBytes        = resolves.idleBytes;
    stats.resolvePoolPendingBytes     = resolves.pendingBytes;
    stats.versionTextureHits          = impl->versionTextureHits.load(std::memory_order_relaxed);
    stats.versionTextureUploads       = impl->versionTextureUploads.load(std::memory_order_relaxed);
    stats.versionTextureEvictions     = impl->versionTextureEvictions.load(std::memory_order_relaxed);
    stats.versionTextureBytes         = impl->versionTextureHeldBytes.load(std::memory_order_relaxed);
#if REREVVED_DEVELOPER
    for (std::size_t site = 0; site < stats.attachmentSiteMisses.size(); ++site)
        stats.attachmentSiteMisses[site] = impl->attachmentSiteMisses[site].load(std::memory_order_relaxed);
    static_assert(NativeDrawReplayCacheStats::kAttachmentMissGaps.size() == kPoolMissGapBuckets);
    std::array<PoolMissLog<PooledTextureKey>::Entry, std::tuple_size_v<decltype(stats.attachmentMissShapes)>> missed;
    impl->attachmentMisses.Top(missed, stats.attachmentMissUntracked);
    for (std::size_t index = 0; index < missed.size(); ++index)
    {
        const auto& entry                 = missed[index];
        stats.attachmentMissShapes[index] = { static_cast<std::uint32_t>(entry.key.width), entry.key.height, entry.key.samples, entry.key.format, entry.key.flags, entry.key.castable, entry.misses, entry.gaps };
    }
    stats.attachmentMissFrames = impl->attachmentMissFrames.load(std::memory_order_relaxed);
#endif
    return stats;
#endif
}

namespace
{

std::atomic<std::size_t> defaultDrawStreamQueueDepth{ 1 };
std::atomic<bool>        defaultDirectStream{ false };

} // namespace

void NativeRendererD3D12::SetDefaultDrawStreamQueueDepth(std::size_t depth) noexcept
{
    defaultDrawStreamQueueDepth.store(depth, std::memory_order_relaxed);
}

void NativeRendererD3D12::SetDefaultDirectStream(bool enabled) noexcept
{
    defaultDirectStream.store(enabled, std::memory_order_relaxed);
}

NativeDrawReplayCounts NativeRendererD3D12::ReplayCounts() const noexcept
{
    NativeDrawReplayCounts counts;
    counts.recordedDraws    = impl->replayRecordedDraws.load(std::memory_order_relaxed);
    counts.queueSubmissions = impl->replayQueueSubmissions.load(std::memory_order_relaxed);
    counts.chunkSubmissions = impl->replayChunkSubmissions.load(std::memory_order_relaxed);
    return counts;
}

#if REREVVED_DEVELOPER
void NativeRendererD3D12::SetPipelineTiming(bool enabled) noexcept
{
    impl->replayPipelineTiming.store(enabled, std::memory_order_relaxed);
}

NativeDrawReplayTimingStats NativeRendererD3D12::ReplayTimingStats() const noexcept
{
#if !defined(_WIN32)
    return {};
#else
    NativeDrawReplayTimingStats stats;
    std::lock_guard             timingLock(impl->replayTimingMutex);
    stats.recordedDraws         = impl->replayRecordedDraws.load(std::memory_order_relaxed);
    stats.queueSubmissions      = impl->replayQueueSubmissions.load(std::memory_order_relaxed);
    stats.chunkSubmissions      = impl->replayChunkSubmissions.load(std::memory_order_relaxed);
    stats.streamBusyNanoseconds = impl->streamBusyNanoseconds.load(std::memory_order_relaxed);
    for (std::size_t stage = 0; stage < stats.constantRepeatPreviousBytes.size(); ++stage)
    {
        stats.constantRepeatPreviousBytes[stage] = impl->replayConstantRepeatPreviousBytes[stage].load(std::memory_order_relaxed);
        stats.constantRepeatChunkBytes[stage]    = impl->replayConstantRepeatChunkBytes[stage].load(std::memory_order_relaxed);
    }
    stats.textureDescriptorReuses = impl->replayTextureDescriptorReuses.load(std::memory_order_relaxed);
    stats.textureSrvWrites        = impl->replayTextureSrvWrites.load(std::memory_order_relaxed);
    stats.rtvWrites               = impl->replayRtvWrites.load(std::memory_order_relaxed);
    for (std::size_t kind = 0; kind < stats.residentGeometryBytes.size(); ++kind)
        stats.residentGeometryBytes[kind] = impl->replayResidentGeometryBytes[kind].load(std::memory_order_relaxed);
    stats.recordingNanoseconds = impl->replayRecordingNanoseconds.load(std::memory_order_relaxed);
    for (std::size_t phase = 0; phase < stats.recordingPhaseNanoseconds.size(); ++phase)
        stats.recordingPhaseNanoseconds[phase] = impl->replayRecordingPhaseNanoseconds[phase].load(std::memory_order_relaxed);
    for (std::size_t kind = 0; kind < stats.drawUploadBytes.size(); ++kind)
        stats.drawUploadBytes[kind] = impl->replayDrawUploadBytes[kind].load(std::memory_order_relaxed);
    stats.texturePrepareNanoseconds = impl->replayTexturePrepareNanoseconds.load(std::memory_order_relaxed);
    for (std::size_t outcome = 0; outcome < stats.textureOutcomes.size(); ++outcome)
    {
        stats.textureOutcomes[outcome]     = impl->replayTextureOutcomes[outcome].load(std::memory_order_relaxed);
        stats.textureOutcomeBytes[outcome] = impl->replayTextureOutcomeBytes[outcome].load(std::memory_order_relaxed);
    }
    stats.fenceWaitNanoseconds         = impl->replayFenceWaitNanoseconds.load(std::memory_order_relaxed);
    stats.textureUploadNanoseconds     = impl->replayTextureUploadNanoseconds.load(std::memory_order_relaxed);
    stats.drawUploadNanoseconds        = impl->replayDrawUploadNanoseconds.load(std::memory_order_relaxed);
    stats.pipelineNanoseconds          = impl->replayPipelineNanoseconds.load(std::memory_order_relaxed);
    stats.surfaceConversions           = impl->surfaceConversions.load(std::memory_order_relaxed);
    stats.surfaceConversionNanoseconds = impl->surfaceConversionNanoseconds.load(std::memory_order_relaxed);
    stats.streamQueueWaits             = impl->streamQueueWaits.load(std::memory_order_relaxed);
    stats.streamQueueWaitNanoseconds   = impl->streamQueueWaitNanoseconds.load(std::memory_order_relaxed);
    stats.streamFutureWaits            = impl->streamFutureWaits.load(std::memory_order_relaxed);
    stats.streamFutureWaitNanoseconds  = impl->streamFutureWaitNanoseconds.load(std::memory_order_relaxed);
    stats.attachmentCreations          = impl->attachmentCreations.load(std::memory_order_relaxed);
    stats.attachmentCreateNanoseconds  = impl->attachmentCreateNanoseconds.load(std::memory_order_relaxed);
    stats.residentHits                 = impl->residentHits.load(std::memory_order_relaxed);
    stats.residentBytesAvoided         = impl->residentBytesAvoided.load(std::memory_order_relaxed);
    stats.residentMisses               = impl->residentMisses.load(std::memory_order_relaxed);
    stats.residentCopyBytes            = impl->residentCopyBytes.load(std::memory_order_relaxed);
    stats.residentFallbacks            = impl->residentFallbacks.load(std::memory_order_relaxed);
    for (std::size_t reason = 0; reason < stats.residentFallbackReasons.size(); ++reason)
        stats.residentFallbackReasons[reason] = impl->residentFallbackReasons[reason].load(std::memory_order_relaxed);
    stats.residentEvictions      = impl->residentEvictions.load(std::memory_order_relaxed);
    stats.residentExpiries       = impl->residentExpiries.load(std::memory_order_relaxed);
    stats.residentClears         = impl->residentClears.load(std::memory_order_relaxed);
    stats.residentUploadFailures = impl->residentUploadFailures.load(std::memory_order_relaxed);
    stats.residentInitFailures   = impl->residentInitFailures.load(std::memory_order_relaxed);
    stats.residentBytes          = impl->residentFootprint.load(std::memory_order_relaxed);
    stats.residentEntries        = impl->residentEntries.load(std::memory_order_relaxed);
    stats.residentPayloadBytes   = impl->residentPayloadBytes.load(std::memory_order_relaxed);
    stats.residentRetiredBytes   = impl->residentRetiredBytes.load(std::memory_order_relaxed);
    return stats;
#endif
}
#endif

NativeDrawReplayResult NativeRendererD3D12::ReplayOffscreen(
    const NativeDrawReplayRecipe& recipe)
{
    NativeDrawReplayResult result;
    result.outputPath = recipe.outputPath;
#if !defined(_WIN32)
    result.error = "native D3D12 replay requires Windows";
    return result;
#else
    std::string validationError;
    if (!ValidateNativeDrawReplayRecipe(recipe, validationError))
    {
        result.error = std::move(validationError);
        return result;
    }
    if (std::any_of(recipe.textures.begin(), recipe.textures.end(), [](const auto& texture)
                    {
                        return texture.frameTextureId != 0;
                    }))
    {
        result.error = "frame texture references require an ordered render frame";
        return result;
    }
    if (recipe.outputPath.empty())
    {
        result.error = "native D3D12 replay output path must be supplied by the caller";
        return result;
    }
    if (impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
    {
        result.error = "native D3D12 cannot replay after abandoning in-flight GPU objects";
        return result;
    }
    if (impl->rendererThread.joinable())
    {
        result.error = "native D3D12 renderer thread is already running";
        return result;
    }
    std::promise<NativeDrawReplayResult> resultPromise;
    auto                                 resultFuture = resultPromise.get_future();
    impl->rendererThread                              = std::thread(
        &NativeRendererD3D12::headlessReplayThreadMain,
        this,
        recipe,
        std::move(resultPromise));
    result = resultFuture.get();
    impl->rendererThread.join();
    return result;
#endif
}

NativeDrawReplayResult NativeRendererD3D12::ReplayFrame(NativeFrameReplayRecipe recipe)
{
    NativeDrawReplayResult result;
    result.outputPath = recipe.outputPath;
    if (!ValidateNativeFrameReplayRecipe(recipe, result.error))
        return result;
    if (recipe.outputPath.empty() && !recipe.returnResolvedOutput)
    {
        result.error = "native frame requires distinct output paths";
        return result;
    }
    std::error_code pathError;
    if (!recipe.outputPath.empty() && !recipe.sampleOutputPath.empty())
    {
        const auto outputCanonical = std::filesystem::weakly_canonical(recipe.outputPath, pathError);
        if (pathError)
        {
            result.error = "native frame output path could not be resolved";
            return result;
        }
        const auto samplesCanonical = std::filesystem::weakly_canonical(recipe.sampleOutputPath, pathError);
#if defined(_WIN32)
        const bool aliases = equivalentWindowsPath(outputCanonical, samplesCanonical);
#else
        const bool aliases = outputCanonical == samplesCanonical;
#endif
        if (pathError || aliases)
        {
            result.error = "native frame requires distinct output paths";
            return result;
        }
    }
    const bool outputExists = !recipe.outputPath.empty() && std::filesystem::exists(recipe.outputPath, pathError);
    if (pathError || outputExists)
    {
        result.error = "native frame output must be a fresh writable path";
        return result;
    }
    if (!recipe.sampleOutputPath.empty())
    {
        const bool samplesExist = std::filesystem::exists(recipe.sampleOutputPath, pathError);
        if (pathError || samplesExist)
        {
            result.error = "native frame sample output must be a fresh writable path";
            return result;
        }
    }
    if (impl->gpuObjectsAbandoned.load(std::memory_order_acquire) || impl->rendererThread.joinable())
    {
        result.error = "native renderer is unavailable for frame replay";
        return result;
    }
    std::promise<NativeDrawReplayResult> promise;
    auto                                 future = promise.get_future();
    impl->rendererThread                        = std::thread(&NativeRendererD3D12::frameReplayThreadMain,
                                                              this,
                                                              std::move(recipe),
                                                              std::move(promise));
    result                                      = future.get();
    impl->rendererThread.join();
    return result;
}

bool NativeRendererD3D12::Resize(std::uint32_t width, std::uint32_t height)
{
    // A minimized window has no drawable extent. Keep the last valid request
    // and let the next non-zero callback wake the renderer.
    if (width == 0 || height == 0)
    {
        return true;
    }
    std::lock_guard lock(impl->stateMutex);
    if (impl->drawStreamStarted || !impl->initialized.load(std::memory_order_acquire) ||
        impl->stopRequested || impl->runtimeFailed)
    {
        return false;
    }

    // The UI thread only updates this mailbox; the renderer consumes the
    // latest dimensions and performs all DXGI/D3D12 work itself.
    impl->requestedWidth  = width;
    impl->requestedHeight = height;
    impl->resizePending   = true;
    impl->stateCv.notify_one();
    return true;
}

NativeDrawReplayResult NativeRendererD3D12::ReplayDrawFrames(const NativeDrawFramesRecipe& recipe)
{
    NativeDrawReplayResult result;
    result.outputPath = recipe.outputDirectory;
    if (!ValidateNativeDrawFramesRecipe(recipe, result.error))
        return result;
    if (impl->rendererThread.joinable() || impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
    {
        result.error = "native renderer is unavailable for menu frame replay";
        return result;
    }
    std::error_code error;
    if (recipe.outputDirectory.empty() || !std::filesystem::create_directory(recipe.outputDirectory, error))
    {
        result.error = "native menu replay requires a fresh output directory";
        return result;
    }
    std::promise<NativeDrawReplayResult> promise;
    auto                                 future = promise.get_future();
    impl->rendererThread                        = std::thread(&NativeRendererD3D12::drawFramesThreadMain, this, recipe, std::move(promise));
    result                                      = future.get();
    impl->rendererThread.join();
    return result;
}

bool NativeRendererD3D12::StartDrawStream(std::string& error, ID3D12Device* sharedDevice, NativeGpuOutputPublisher publisher)
{
    return startDrawStream(error, sharedDevice, std::move(publisher), {});
}

bool NativeRendererD3D12::StartTitleDrawStream(std::string& error, ID3D12Device* sharedDevice, NativeTitleOutputPublisher publisher)
{
    if (!publisher)
    {
        error = "native title draw stream requires a publisher";
        return false;
    }
    return startDrawStream(error, sharedDevice, {}, std::move(publisher));
}

bool NativeRendererD3D12::startDrawStream(std::string& error, ID3D12Device* sharedDevice, NativeGpuOutputPublisher publisher, NativeTitleOutputPublisher titlePublisher)
{
    error.clear();
#if !defined(_WIN32)
    (void)sharedDevice;
    (void)publisher;
    (void)titlePublisher;
    error = "native D3D12 draw stream requires Windows";
    return false;
#else
    std::unique_lock lock(impl->stateMutex);
    if (impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
    {
        error = "native D3D12 draw stream cannot start after abandoning GPU objects";
        return false;
    }
    if (impl->runtimeFailed)
    {
        error = "native D3D12 draw stream is in a terminal failure state";
        return false;
    }
    if (impl->rendererThread.joinable() || impl->initialized.load(std::memory_order_acquire))
    {
        error = "native D3D12 renderer has an incompatible active mode";
        return false;
    }
    if ((publisher || titlePublisher) && !sharedDevice)
    {
        error = "native GPU output requires an explicit host device";
        return false;
    }
    impl->sharedStreamDevice    = sharedDevice;
    impl->streamOutputPublisher = std::move(publisher);
    impl->streamTitlePublisher  = std::move(titlePublisher);

    impl->stopRequested           = false;
    impl->initializationDone      = false;
    impl->initializationSucceeded = false;
    impl->initializationError.clear();
    impl->drawStreamStarted           = true;
    impl->drawStreamPending           = false;
    impl->drawStreamInFlight          = false;
    impl->ownedAdmissionBusy          = true;
    impl->ownedAdmissionSnapshotValid = false;
    impl->ownedScanCeiling            = 0;
    impl->ownedAdmissionFullCount     = 0;
    impl->ownedRefreshRequested       = false;
    impl->ownedCapacityRequest        = 0;
    impl->ownedCapacityError.clear();
    impl->drawStreamDraws.clear();
    impl->drawStreamSampleOutputPath.clear();
    impl->drawStreamOutput.reset();
    impl->drawStreamRenderFrame.reset();
    impl->drawStreamPrefixRequest = false;
    impl->drawStreamHasPrefix     = false;
    impl->drawStreamPromise.reset();
    impl->drawStreamQueue.clear();
    impl->drawStreamQueueDepth.store(std::max<std::size_t>(1, defaultDrawStreamQueueDepth.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    // A direct stream funds the owner heap to the bound once; a census stream
    // after one starts from what is live.
    const bool direct = defaultDirectStream.load(std::memory_order_relaxed);
    if (direct)
        impl->ownedHeap.limit = std::max(impl->ownedHeap.limit, Impl::kOwnedByteBound);
    else if (impl->directStream)
        impl->ownedHeap.limit = impl->ownedHeap.live.load(std::memory_order_relaxed);
    impl->directStream           = direct;
    impl->directInFlightTerminal = false;
    impl->directAdmittedBytes    = 0;
    impl->directChainBytes       = 0;
    impl->directChargeBytes.store(0, std::memory_order_relaxed);
    impl->directDeleterBytes.store(0, std::memory_order_relaxed);
    impl->directStructureBytes.store(0, std::memory_order_relaxed);
    impl->guestAdmissions  = 0;
    impl->queuedPrefixOpen = false;
    impl->rendererThread   = std::thread(&NativeRendererD3D12::drawStreamThreadMain, this);
    impl->stateCv.wait(lock, [this]()
                       {
                           return impl->initializationDone;
                       });
    if (!impl->initializationSucceeded)
    {
        error = impl->initializationError.empty() ? "headless D3D12 draw stream initialization failed"
                                                  : impl->initializationError;
        lock.unlock();
        impl->rendererThread.join();
        std::lock_guard stateLock(impl->stateMutex);
        impl->drawStreamStarted = false;
        return false;
    }
    return true;
#endif
}

bool NativeRendererD3D12::AwaitGpuOutput(std::string& error, std::uint64_t* copyFenceValue)
{
    error.clear();
#if defined(_WIN32)
    return impl->awaitGpuOutput(true, error, copyFenceValue);
#else
    (void)copyFenceValue;
    error = "native GPU output requires Windows";
    return false;
#endif
}

NativeRendererD3D12::GpuOutputStatus NativeRendererD3D12::PollGpuOutput(const std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>& ticket, std::uint64_t* copyFenceValue)
{
    if (copyFenceValue)
        *copyFenceValue = 0;
#if !defined(_WIN32)
    return GpuOutputStatus::Failed;
#else
    if (!ticket)
        return GpuOutputStatus::Failed;
    using Status = rex::ui::d3d12::GpuGuestOutputTicket::Status;
    switch (ticket->status())
    {
        case Status::kCompleted:
            if (copyFenceValue)
                *copyFenceValue = ticket->copy_fence_value();
            return GpuOutputStatus::Completed;
        case Status::kCanceled:
            return GpuOutputStatus::Canceled;
        case Status::kFailed:
            return GpuOutputStatus::Failed;
        default:
            return GpuOutputStatus::Pending;
    }
#endif
}

bool NativeRendererD3D12::AwaitGpuOutput(const std::shared_ptr<rex::ui::d3d12::GpuGuestOutputTicket>& ticket, std::string& error, std::uint64_t* copyFenceValue)
{
#if !defined(_WIN32)
    error = "native D3D12 GPU output requires Windows";
    return false;
#else
    return awaitGpuOutputTicket(ticket, true, error, copyFenceValue);
#endif
}

NativeRendererD3D12::GpuOutputStatus NativeRendererD3D12::PollGpuOutput(const std::shared_ptr<NativeOutputLease>& lease, std::uint64_t* copySubmissionId)
{
    if (copySubmissionId)
        *copySubmissionId = 0;
    if (!lease)
        return GpuOutputStatus::Failed;
    switch (lease->State())
    {
        case OutputLeaseState::Retired:
            if (copySubmissionId)
                *copySubmissionId = lease->CopySubmissionId();
            return GpuOutputStatus::Completed;
        case OutputLeaseState::Cancelled:
            return GpuOutputStatus::Canceled;
        case OutputLeaseState::Failed:
            return GpuOutputStatus::Failed;
        default:
            return GpuOutputStatus::Pending;
    }
}

bool NativeRendererD3D12::AwaitGpuOutput(const std::shared_ptr<NativeOutputLease>& lease, std::string& error, std::uint64_t* copySubmissionId)
{
#if !defined(_WIN32)
    error = "native D3D12 GPU output requires Windows";
    return false;
#else
    return awaitGpuOutputLease(lease, true, error, copySubmissionId);
#endif
}

NativeDrawReplayResult NativeRendererD3D12::SubmitDrawFrame(
    std::vector<NativeDrawReplayRecipe>   draws,
    const std::filesystem::path&          sampleOutputPath,
    std::optional<NativeDrawStreamOutput> output)
{
    NativeDrawReplayResult result;
    result.outputPath = sampleOutputPath.empty() && output ? output->outputPath : sampleOutputPath;
#if !defined(_WIN32)
    (void)draws;
    (void)sampleOutputPath;
    (void)output;
    result.error = "native D3D12 draw stream requires Windows";
    return result;
#else
    if (!ValidateNativeDrawFrame(draws, result.error))
        return result;
    std::promise<NativeDrawReplayResult> promise;
    auto                                 future = promise.get_future();
    {
        std::unique_lock lock(impl->stateMutex);
        impl->waitCounted(lock, [this]()
                          {
                              return (!impl->drawStreamPending && !impl->drawStreamInFlight && !impl->ownedAdmissionBusy && !impl->admissionImports) ||
                                     !impl->drawStreamStarted || impl->stopRequested ||
                                     impl->runtimeFailed ||
                                     impl->gpuObjectsAbandoned.load(std::memory_order_acquire);
                          });
        if (!impl->drawStreamStarted || !impl->initialized.load(std::memory_order_acquire) ||
            impl->stopRequested || impl->runtimeFailed ||
            impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
        {
            result.error = "native D3D12 draw stream is unavailable";
            return result;
        }
        if (impl->drawStreamHasPrefix)
        {
            result.error = "native draw stream owns an unfinished render-frame prefix";
            return result;
        }
        std::filesystem::path sampleCanonicalPath;
        std::filesystem::path outputCanonicalPath;
        std::error_code       pathError;
        if (!sampleOutputPath.empty())
        {
            sampleCanonicalPath = std::filesystem::weakly_canonical(sampleOutputPath, pathError);
            if (pathError)
            {
                result.error = "native draw stream sample output path could not be resolved";
                return result;
            }
        }
        if (output)
        {
            if (output->outputPath.empty() && !output->returnResolvedOutput)
            {
                result.error = "native draw stream output requires a fresh path or returned pixels";
                return result;
            }
        }
        if (output && !output->outputPath.empty())
        {
            pathError.clear();
            outputCanonicalPath = std::filesystem::weakly_canonical(output->outputPath, pathError);
            if (pathError)
            {
                result.error = "native draw stream output path could not be resolved";
                return result;
            }
            if (!sampleOutputPath.empty() &&
                equivalentWindowsPath(sampleCanonicalPath, outputCanonicalPath))
            {
                result.error = "native draw stream output paths must be distinct";
                return result;
            }
        }
        if (!sampleOutputPath.empty())
        {
            pathError.clear();
            if (std::filesystem::exists(sampleOutputPath, pathError) || pathError)
            {
                result.error = "native draw stream sample output must be a fresh writable path";
                return result;
            }
        }
        if (output && !output->outputPath.empty())
        {
            pathError.clear();
            if (std::filesystem::exists(output->outputPath, pathError) || pathError)
            {
                result.error = "native draw stream output must be a fresh writable path";
                return result;
            }
        }
        // The path is copied before any charge, so nothing after it throws.
        auto          slotSamplePath = sampleOutputPath;
        std::uint64_t directBytes    = 0;
        if (impl->directStream)
        {
            directBytes = impl->directDrawBytes(draws);
            if (!impl->awaitDirectFit(directBytes, lock, result.error))
                return result;
            // A prefix may have recorded while the frame waited.
            if (impl->drawStreamHasPrefix)
            {
                result.error = "native draw stream owns an unfinished render-frame prefix";
                return result;
            }
        }
        impl->drawStreamDraws = std::move(draws);
        impl->drawStreamRenderFrame.reset();
        impl->drawStreamSampleOutputPath = std::move(slotSamplePath);
        impl->drawStreamOutput           = std::move(output);
        impl->drawStreamPromise.emplace(std::move(promise));
        if (impl->directStream)
            impl->chargeDirectAdmission(directBytes);
        impl->drawStreamPending = true;
    }
    impl->notifyStream();
    return impl->getCounted(future);
#endif
}

NativeDrawReplayResult NativeRendererD3D12::SubmitRenderFrame(NativeRenderFrame frame)
{
    NativeDrawReplayResult result;
    result.outputPath = frame.outputPath;
#if !defined(_WIN32)
    result.error = "native D3D12 render frames require Windows";
    return result;
#else
    // A tail terminal is validated against the prefix it continues and joined
    // to it by the worker at intake, at every queue depth.
    const bool tail = frame.continuedPrefixPasses != 0;
    if (!tail && !ValidateNativeRenderFrame(frame, result.error))
        return result;
    std::promise<NativeDrawReplayResult> promise;
    auto                                 future = promise.get_future();
    if (tail)
    {
        if (frame.typedGuestWords && !impl->castableGuestWords.load(std::memory_order_relaxed))
        {
            result.error = "render frame typed guest-word views need castable storage the device lacks";
            return result;
        }
        std::error_code filesystemError;
        if (!frame.outputPath.empty() && (std::filesystem::exists(frame.outputPath, filesystemError) || filesystemError))
        {
            result.error = "native render frame output must be a fresh writable path";
            return result;
        }
        {
            std::unique_lock lock(impl->stateMutex);
            const auto       directBytes = impl->directStream ? impl->directFrameBytes(frame) : 0;
            bool             tooLarge    = false;
            if (!impl->awaitQueueRoom(lock, false, directBytes, &tooLarge))
            {
                // A tail that cannot queue leaves its prefix without an end.
                impl->refuseDirect(tooLarge);
                lock.unlock();
                impl->notifyStream();
                result.error = tooLarge ? "native direct stream frame bytes exceed the owned byte bound" : "native D3D12 draw stream is unavailable";
                return result;
            }
            auto& entry       = impl->drawStreamQueue.emplace_back();
            entry.frame       = std::move(frame);
            entry.directBytes = directBytes;
            entry.promise.emplace(std::move(promise));
            impl->reserveDirect(directBytes);
            impl->drawStreamPending = true;
            impl->queuedPrefixOpen  = false;
        }
        impl->notifyStream();
        return impl->getCounted(future);
    }
    {
        std::unique_lock lock(impl->stateMutex);
        impl->waitCounted(lock, [this]()
                          {
                              return (!impl->drawStreamPending && !impl->drawStreamInFlight) || !impl->drawStreamStarted ||
                                     impl->stopRequested || impl->runtimeFailed || impl->gpuObjectsAbandoned.load(std::memory_order_acquire);
                          });
        if (!impl->drawStreamStarted || !impl->initialized.load(std::memory_order_acquire) ||
            impl->stopRequested || impl->runtimeFailed || impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
        {
            result.error = "native D3D12 draw stream is unavailable";
            return result;
        }
        std::error_code error;
        if (!frame.outputPath.empty() && (std::filesystem::exists(frame.outputPath, error) || error))
        {
            result.error = "native render frame output must be a fresh writable path";
            return result;
        }
        Impl::FrameImports imports;
        if (!impl->awaitOwnedAdmission(frame, imports, lock, result.error))
            return result;
        impl->drawStreamDraws.clear();
        impl->drawStreamSampleOutputPath.clear();
        impl->drawStreamOutput.reset();
        impl->drawStreamRenderFrame.emplace(std::move(frame));
        impl->drawStreamImports = std::move(imports);
        impl->drawStreamPromise.emplace(std::move(promise));
        impl->drawStreamPending = true;
        impl->queuedPrefixOpen  = false;
    }
    impl->notifyStream();
    return impl->getCounted(future);
#endif
}

bool NativeRendererD3D12::QueueRenderFrame(NativeRenderFrame frame, std::shared_ptr<NativeRenderFrameTicket>& ticket, std::string& error)
{
    error.clear();
    ticket.reset();
#if !defined(_WIN32)
    error = "native D3D12 render frames require Windows";
    return false;
#else
    // As in SubmitRenderFrame, a tail terminal always takes the worker's intake.
    const bool tail = frame.continuedPrefixPasses != 0;
    if (!tail && !ValidateNativeRenderFrame(frame, error))
        return false;
    const bool queued = tail || impl->drawStreamQueueDepth.load(std::memory_order_relaxed) > 1;
    if (queued)
    {
        // The worker charges the frame at intake; the cheap refusals stay here.
        if (frame.typedGuestWords && !impl->castableGuestWords.load(std::memory_order_relaxed))
        {
            error = "render frame typed guest-word views need castable storage the device lacks";
            return false;
        }
    }
    else
    {
        std::unique_lock lock(impl->stateMutex);
        if (!impl->admitTerminalInputs(frame, lock, error, false))
            return false;
    }
    auto request                = std::make_shared<Impl::TerminalRequest>();
    request->ticket             = std::shared_ptr<NativeRenderFrameTicket>(new NativeRenderFrameTicket);
    request->ticket->recording  = request->recording.get_future().share();
    request->ticket->completion = request->completion.get_future().share();
    if (queued)
    {
        std::error_code filesystemError;
        if (!frame.outputPath.empty() && (std::filesystem::exists(frame.outputPath, filesystemError) || filesystemError))
        {
            error = "native render frame output must be a fresh writable path";
            return false;
        }
        {
            std::unique_lock lock(impl->stateMutex);
            const auto       directBytes = impl->directStream ? impl->directFrameBytes(frame) : 0;
            bool             tooLarge    = false;
            if (!impl->awaitQueueRoom(lock, false, directBytes, &tooLarge))
            {
                impl->refuseDirect(tooLarge && (tail || impl->queuedPrefixOpen));
                lock.unlock();
                impl->notifyStream();
                error = tooLarge ? "native direct stream frame bytes exceed the owned byte bound" : "native D3D12 draw stream is unavailable";
                return false;
            }
            auto& entry       = impl->drawStreamQueue.emplace_back();
            entry.frame       = std::move(frame);
            entry.directBytes = directBytes;
            entry.terminal    = request;
            impl->reserveDirect(directBytes);
            impl->drawStreamPending = true;
            impl->queuedPrefixOpen  = false;
            ticket                  = request->ticket;
        }
        impl->notifyStream();
        return true;
    }
    {
        std::unique_lock lock(impl->stateMutex);
        impl->waitCounted(lock, [this]()
                          {
                              return (!impl->drawStreamPending && !impl->drawStreamInFlight) || !impl->drawStreamStarted ||
                                     impl->stopRequested || impl->runtimeFailed || impl->gpuObjectsAbandoned.load(std::memory_order_acquire);
                          });
        if (!impl->drawStreamStarted || !impl->initialized.load(std::memory_order_acquire) || impl->stopRequested ||
            impl->runtimeFailed || impl->gpuObjectsAbandoned.load(std::memory_order_acquire))
        {
            error = "native D3D12 draw stream is unavailable";
            return false;
        }
        std::error_code filesystemError;
        if (!frame.outputPath.empty() && (std::filesystem::exists(frame.outputPath, filesystemError) || filesystemError))
        {
            error = "native render frame output must be a fresh writable path";
            return false;
        }
        Impl::FrameImports imports;
        if (!impl->awaitOwnedAdmission(frame, imports, lock, error, true))
            return false;
        impl->drawStreamDraws.clear();
        impl->drawStreamSampleOutputPath.clear();
        impl->drawStreamOutput.reset();
        impl->drawStreamRenderFrame.emplace(std::move(frame));
        impl->drawStreamImports       = std::move(imports);
        impl->drawStreamTerminal      = request;
        impl->drawStreamPrefixRequest = false;
        impl->drawStreamPending       = true;
        impl->queuedPrefixOpen        = false;
        ticket                        = request->ticket;
    }
    impl->notifyStream();
    return true;
#endif
}

void NativeRendererD3D12::RequestDrawStreamStop()
{
    {
        std::lock_guard lock(impl->stateMutex);
        impl->stopRequested = true;
        impl->resizePending = false;
        NativeDrawReplayResult canceled;
        canceled.error = "native draw stream stopped before terminal submission";
        for (const auto& request : { impl->drawStreamTerminal, impl->activeTerminalRequest })
            if (request && !request->submitted.load())
            {
                request->SetRecording(canceled);
                request->SetCompletion(canceled);
            }
        for (const auto& queued : impl->drawStreamQueue)
            if (queued.terminal)
            {
                queued.terminal->SetRecording(canceled);
                queued.terminal->SetCompletion(canceled);
            }
    }
    impl->notifyStream();
}

bool NativeRendererD3D12::RecordRenderFramePrefix(NativeRenderFrame prefix, std::string& error)
{
    if (!ValidateNativeRenderFramePrefix(prefix, error))
        return false;
    return submitRenderFramePasses(std::move(prefix), false, error);
}

bool NativeRendererD3D12::AppendRenderFramePasses(NativeRenderFrame passes, std::string& error)
{
    return submitRenderFramePasses(std::move(passes), true, error);
}

bool NativeRendererD3D12::submitRenderFramePasses(NativeRenderFrame prefix, bool allowAppend, std::string& error)
{
    std::shared_future<NativeDrawReplayResult> recording;
    if (!queueRenderFramePasses(std::move(prefix), allowAppend, recording, error))
        return false;
    const auto& result = impl->getCounted(recording);
    error              = result.error;
    return result.success;
}

bool NativeRendererD3D12::QueueRenderFramePasses(NativeRenderFrame passes, std::shared_future<NativeDrawReplayResult>& recording, std::string& error)
{
    return queueRenderFramePasses(std::move(passes), true, recording, error);
}

bool NativeRendererD3D12::queueRenderFramePasses(NativeRenderFrame prefix, bool allowAppend, std::shared_future<NativeDrawReplayResult>& recording, std::string& error)
{
    error.clear();
    recording = {};
#if !defined(_WIN32)
    error = "native D3D12 render prefixes require Windows";
    return false;
#else
    std::promise<NativeDrawReplayResult> promise;
    auto                                 future = promise.get_future().share();
    if (impl->drawStreamQueueDepth.load(std::memory_order_relaxed) > 1)
    {
        // The worker charges the passes at intake; the cheap refusals stay here.
        if (prefix.typedGuestWords && !impl->castableGuestWords.load(std::memory_order_relaxed))
        {
            error = "render frame typed guest-word views need castable storage the device lacks";
            return false;
        }
        {
            std::unique_lock lock(impl->stateMutex);
            const auto       directBytes = impl->directStream ? impl->directFrameBytes(prefix) : 0;
            bool             tooLarge    = false;
            if (!impl->awaitQueueRoom(lock, true, directBytes, &tooLarge) || (!allowAppend && impl->queuedPrefixOpen))
            {
                // A packet that cannot fit before its own tail retires breaks
                // the chain it would continue.
                impl->refuseDirect(tooLarge && impl->queuedPrefixOpen);
                lock.unlock();
                impl->notifyStream();
                error = tooLarge ? "native direct stream frame bytes exceed the owned byte bound" : "native D3D12 draw stream is unavailable or owns an unfinished prefix";
                return false;
            }
            auto& entry       = impl->drawStreamQueue.emplace_back();
            entry.frame       = std::move(prefix);
            entry.directBytes = directBytes;
            entry.promise.emplace(std::move(promise));
            entry.prefix      = true;
            entry.allowAppend = allowAppend;
            impl->reserveDirect(directBytes);
            impl->drawStreamPending = true;
            impl->queuedPrefixOpen  = true;
        }
        recording = std::move(future);
        impl->notifyStream();
        return true;
    }
    {
        std::unique_lock lock(impl->stateMutex);
        impl->waitCounted(lock, [this]()
                          {
                              return (!impl->drawStreamPending && !impl->drawStreamInFlight) || !impl->drawStreamStarted ||
                                     impl->stopRequested || impl->runtimeFailed || impl->gpuObjectsAbandoned.load(std::memory_order_acquire);
                          });
        if (!impl->drawStreamStarted || !impl->initialized.load(std::memory_order_acquire) || impl->stopRequested ||
            impl->runtimeFailed || impl->gpuObjectsAbandoned.load(std::memory_order_acquire) || (!allowAppend && impl->drawStreamHasPrefix))
        {
            error = "native D3D12 draw stream is unavailable or owns an unfinished prefix";
            return false;
        }
        Impl::FrameImports imports;
        if (!impl->awaitOwnedAdmission(prefix, imports, lock, error, false, false, !allowAppend))
            return false;
        impl->drawStreamDraws.clear();
        impl->drawStreamSampleOutputPath.clear();
        impl->drawStreamOutput.reset();
        impl->drawStreamRenderFrame.emplace(std::move(prefix));
        impl->drawStreamImports = std::move(imports);
        impl->drawStreamPromise.emplace(std::move(promise));
        impl->drawStreamPrefixRequest = true;
        impl->drawStreamHasPrefix     = true;
        impl->drawStreamPending       = true;
        impl->queuedPrefixOpen        = true;
    }
    recording = std::move(future);
    impl->notifyStream();
    return true;
#endif
}

void NativeRendererD3D12::Shutdown()
{
    if (!impl->rendererThread.joinable())
    {
        std::lock_guard lock(impl->stateMutex);
        impl->initialized.store(false, std::memory_order_release);
        impl->requestDeferredQuit = {};
        impl->drawStreamStarted   = false;
        impl->drawStreamPending   = false;
        impl->drawStreamInFlight  = false;
        impl->drawStreamOutput.reset();
        impl->drawStreamRenderFrame.reset();
        impl->drawStreamPrefixRequest = false;
        impl->drawStreamHasPrefix     = false;
        impl->drawStreamQueue.clear();
        impl->queuedPrefixOpen = false;
        impl->notifyStream();
        return;
    }
    RequestDrawStreamStop();
    if (impl->rendererThread.get_id() != std::this_thread::get_id())
    {
        impl->rendererThread.join();
    }
    std::lock_guard lock(impl->stateMutex);
    impl->requestDeferredQuit = {};
    impl->drawStreamStarted   = false;
    impl->drawStreamPending   = false;
    impl->drawStreamInFlight  = false;
    impl->drawStreamOutput.reset();
    impl->drawStreamRenderFrame.reset();
    impl->drawStreamPrefixRequest = false;
    impl->drawStreamHasPrefix     = false;
    impl->drawStreamQueue.clear();
    impl->queuedPrefixOpen = false;
}

bool NativeRendererD3D12::Initialized() const noexcept
{
    return impl->initialized.load(std::memory_order_acquire);
}

bool NativeRendererD3D12::CastableGuestWordTextures() const noexcept
{
    return impl->castableGuestWords.load(std::memory_order_relaxed);
}

std::uint64_t WaitStreamFence(ID3D12Fence* fence, std::uint64_t value, std::chrono::steady_clock::time_point deadline)
{
#if defined(_WIN32)
    auto completed = fence->GetCompletedValue();
    if (completed >= value)
        return completed;
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event && SUCCEEDED(fence->SetEventOnCompletion(value, event)))
    {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        WaitForSingleObject(event, remaining > 0 ? static_cast<DWORD>(std::min<long long>(remaining + 1, INFINITE - 1)) : 0);
        CloseHandle(event);
        return fence->GetCompletedValue();
    }
    if (event)
        CloseHandle(event);
    for (;;)
    {
        completed = fence->GetCompletedValue();
        if (completed >= value || std::chrono::steady_clock::now() >= deadline)
            return completed;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#else
    (void)value;
    (void)deadline;
    return fence ? std::numeric_limits<std::uint64_t>::max() : 0;
#endif
}

ID3D12Fence* NativeRendererD3D12::AcquireStreamFence() const
{
#if defined(_WIN32)
    std::lock_guard lock(impl->stateMutex);
    if (!impl->drawStreamStarted || !impl->initializationSucceeded || !impl->fence)
        return nullptr;
    impl->fence->AddRef();
    return impl->fence.Get();
#else
    return nullptr;
#endif
}

ID3D12CommandQueue* NativeRendererD3D12::commandQueueForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->queue.Get();
#else
    return nullptr;
#endif
}

void NativeRendererD3D12::beforeRequestForTesting(std::function<void()> callback)
{
    {
        std::lock_guard lock(impl->stateMutex);
        impl->beforeRequestTestHook = std::move(callback);
    }
    impl->notifyStream();
}

std::uint64_t NativeRendererD3D12::checkedResourceStorageForTesting() const
{
#if defined(_WIN32)
    // Called only by the worker before-request hook, between frame mutations.
    std::uint64_t checked = 0;
    const auto    check   = [&](const auto& resource, std::uint64_t bytes)
    {
        if (!resource)
            return;
        if (!bytes || bytes != resourceStorageBytes(impl->device.Get(), resource.Get()))
            throw std::runtime_error("retained resource storage charge differs from its actual allocation");
        ++checked;
    };
    const auto owner = [&](const auto& owner)
    {
        if (owner)
            check(owner->resource, owner->resourceStorageBytes);
    };
    const auto context = [&](const auto& frame)
    {
        for (const auto& [key, texture] : frame.frameCpuTextures)
            check(texture.resource, texture.resourceStorageBytes);
        for (const auto& texture : frame.frameRetiredCpuTextures)
            check(texture.resource, texture.resourceStorageBytes);
        for (const auto& [id, retained] : frame.frameTextureOwners)
            owner(retained);
    };
    context(*impl);
    for (const auto& frame : impl->frameContexts)
        context(frame);
    for (const auto* pool : { &impl->texturePool, &impl->attachmentPool, &impl->resolvePool })
        pool->ForEachHeld([&](const ComPtr<ID3D12Resource>& resource, std::uint64_t bytes)
                          {
                              check(resource, bytes);
                          });
    for (const auto& [key, entry] : impl->versionTextures)
        check(entry.resource, entry.bytes);
    for (const auto& [key, texture] : impl->retainedFrameTextures)
        owner(texture.owner);
    for (const auto& [id, imported] : impl->recordingImports)
        owner(imported.owner);
    {
        std::lock_guard surfaces(impl->surfaceMutex);
        for (const auto& [key, stored] : impl->surfaceStore)
            owner(stored.owner);
        for (const auto& [value, retired] : impl->surfaceRetires)
            owner(retired);
    }
    std::lock_guard lock(impl->stateMutex);
    for (const auto& [id, imported] : impl->drawStreamImports)
        owner(imported.owner);
    if (impl->admissionImports)
        for (const auto& [id, imported] : *impl->admissionImports)
            owner(imported.owner);
    return checked;
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencyEntriesForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->vertexResidency ? impl->vertexResidency->Entries() : 0;
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencyPendingForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->vertexResidency ? impl->vertexResidency->PendingPins() : 0;
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencyRetiredBytesForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->vertexResidency ? impl->vertexResidency->RetiredBytes() : 0;
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencyAllocatedBytesForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->vertexResidency ? impl->vertexResidency->FootprintBytes() : 0;
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencySwapForTesting() const noexcept
{
#if defined(_WIN32)
    // Called only by the worker before-request hook, between frame mutations.
    return impl->vertexResidency ? impl->vertexResidency->Swap() : 0;
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencyBarriersForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->residencyBarriers.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencyPreBarriersForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->residencyPreBarriers.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencyAdmissionsForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->residencyAdmissions.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::vertexResidencySeamEntriesForTesting(int seam) const noexcept
{
#if defined(_WIN32)
    return impl->residencySeamEntries[static_cast<std::size_t>(seam)].load(std::memory_order_relaxed);
#else
    return ~std::uint64_t(0);
#endif
}

std::vector<ID3D12Resource*> NativeRendererD3D12::vertexResidencyChunksForTesting() const
{
    std::vector<ID3D12Resource*> chunks;
#if defined(_WIN32)
    for (const auto& chunk : impl->residencyChunks)
        if (chunk)
        {
            chunk->AddRef();
            chunks.push_back(chunk.Get());
        }
#endif
    return chunks;
}

#if REREVVED_DEVELOPER
std::unique_lock<std::mutex> NativeRendererD3D12::replayTimingLockForTesting()
{
#if defined(_WIN32)
    return std::unique_lock<std::mutex>(impl->replayTimingMutex);
#else
    return {};
#endif
}
#endif

std::uint64_t NativeRendererD3D12::vertexResidencyOwnedBytesForTesting() const
{
#if defined(_WIN32)
    return impl->retainedStorageBytes(Impl::StorageDomain::Owned);
#else
    return 0;
#endif
}

void NativeRendererD3D12::setVertexResidencyFaultForTesting(int fault)
{
#if defined(_WIN32)
    std::atomic<int>* const faults[] = { &impl->residencyInitFault, &impl->residencyInitFault, &impl->residencyStagingFault, &impl->residencyCloseFault, &impl->residencySignalFault, &impl->residencySchedulingFault, &impl->residencyClearFault, &impl->residencyThrowFault };
    if (fault >= 1 && fault <= 8)
        faults[fault - 1]->store(fault == 2 ? 2 : 1, std::memory_order_relaxed);
#endif
}

std::uint64_t NativeRendererD3D12::retainedUploadStorageForTesting() const
{
#if defined(_WIN32)
    // Called only by the worker before-request hook, between frame mutations.
    return impl->retainedStorageBytes(Impl::StorageDomain::Upload);
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::retainedTextureStorageForTesting(bool attachments) const
{
#if defined(_WIN32)
    // Called only by the worker before-request hook, between frame mutations.
    return impl->retainedStorageBytes(attachments ? Impl::StorageDomain::Attachment : Impl::StorageDomain::Texture);
#else
    return 0;
#endif
}

std::vector<ID3D12Resource*> NativeRendererD3D12::versionTexturesForTesting() const
{
    std::vector<ID3D12Resource*> resources;
#if defined(_WIN32)
    // Called only by the worker before-request hook, between frame mutations.
    for (const auto& [key, entry] : impl->versionTextures)
    {
        entry.resource->AddRef();
        resources.push_back(entry.resource.Get());
    }
#endif
    return resources;
}

ID3D12Resource* NativeRendererD3D12::versionTextureForTesting(const NativeDrawReplayTextureVersion* version) const
{
#if defined(_WIN32)
    if (const auto found = impl->versionTextures.find(version); found != impl->versionTextures.end() && !found->second.version.expired())
    {
        found->second.resource->AddRef();
        return found->second.resource.Get();
    }
#endif
    return nullptr;
}

void NativeRendererD3D12::setVersionTextureCapForTesting(std::uint64_t bytes)
{
#if defined(_WIN32)
    impl->versionTextureCap = bytes;
#endif
}

void NativeRendererD3D12::setVersionTextureStoreForTesting(bool enabled)
{
#if defined(_WIN32)
    impl->versionTextureStore = enabled;
#endif
}

void NativeRendererD3D12::setCastableGuestWordsForTesting(bool castable)
{
#if defined(_WIN32)
    impl->castableGuestWords.store(castable, std::memory_order_relaxed);
#endif
}

std::vector<ID3D12Resource*> NativeRendererD3D12::pooledTexturesForTesting() const
{
    std::vector<ID3D12Resource*> resources;
#if defined(_WIN32)
    // Called only by the worker before-request hook, between frame mutations.
    for (const auto* pool : { &impl->texturePool, &impl->attachmentPool, &impl->resolvePool })
        pool->ForEachHeld([&](const ComPtr<ID3D12Resource>& resource, std::uint64_t)
                          {
                              resource->AddRef();
                              resources.push_back(resource.Get());
                          });
#endif
    return resources;
}

std::vector<ID3D12Resource*> NativeRendererD3D12::uploadPoolBuffersForTesting() const
{
    std::vector<ID3D12Resource*> buffers;
#if defined(_WIN32)
    // Called only by the worker before-request hook, between frame mutations.
    const auto add = [&](ID3D12Resource* resource)
    {
        if (resource && std::find(buffers.begin(), buffers.end(), resource) == buffers.end())
        {
            resource->AddRef();
            buffers.push_back(resource);
        }
    };
    const auto lease = [&](const UploadLease& held)
    {
        if (held)
            add(held.buffer().resource.Get());
    };
    impl->uploadPool.ForEachHeld([&](const NativeUploadBuffer& buffer, std::uint64_t)
                                 {
                                     add(buffer.resource.Get());
                                 });
    const auto context = [&](const auto& frame)
    {
        lease(frame.frameUploadArena);
        for (const auto& retired : frame.frameRetiredUploadLeases)
            lease(retired);
        for (const auto& [key, texture] : frame.frameCpuTextures)
            lease(texture.upload);
        for (const auto& texture : frame.frameRetiredCpuTextures)
            lease(texture.upload);
    };
    context(*impl);
    for (const auto& frame : impl->frameContexts)
        context(frame);
    for (const auto& draw : impl->frameDrawSlots)
        for (const auto& upload : draw.textureUploads)
            lease(upload);
    for (const auto& draw : impl->replayDrawSlots)
        for (const auto& upload : draw.textureUploads)
            lease(upload);
#endif
    return buffers;
}

std::shared_ptr<const void> NativeRendererD3D12::completedTextureForTesting(const std::shared_ptr<const NativeRenderFrameTextureVersion>& version,
                                                                            ID3D12Resource*&                                              resource) const
{
    resource = nullptr;
#if defined(_WIN32)
    // The test borrower holds this owner through its independent readback.
    // Recording alone cannot expose pixels that are still being produced.
    if (!version || !version->impl || !impl->fence)
        return {};
    const auto& proof     = version->impl;
    const auto  completed = impl->fence->GetCompletedValue();
    if (completed == UINT64_MAX || !proof->fenceValue || completed < proof->fenceValue ||
        proof->generation.lock() != impl->rendererGeneration || proof->queue != impl->queue.Get() ||
        proof->canonical.lock() != version)
        return {};
    const auto owner = proof->resource.lock();
    if (!owner || !owner->resource)
        return {};
    resource = owner->resource.Get();
    return owner;
#else
    return {};
#endif
}

std::uint64_t NativeRendererD3D12::joinOwnedWalksForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->ownedJoinWalks.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::uint64_t NativeRendererD3D12::ownedWalksForTesting() const noexcept
{
#if defined(_WIN32)
    return impl->ownedStateWalks.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

std::pair<std::uint64_t, std::uint64_t> NativeRendererD3D12::directChargesForTesting() const noexcept
{
#if defined(_WIN32)
    return { impl->directChargeBytes.load(std::memory_order_relaxed), impl->directPersistentBytes() };
#else
    return {};
#endif
}

std::array<std::uint64_t, 3> NativeRendererD3D12::directPersistentForTesting() const noexcept
{
#if defined(_WIN32)
    return { impl->frameCpuTextureOwnedBytes.load(std::memory_order_relaxed), impl->pipelineCacheBytes.load(std::memory_order_relaxed), impl->directStructureBytes.load(std::memory_order_relaxed) };
#else
    return {};
#endif
}

std::size_t NativeRendererD3D12::frameCommandDrawsForTesting() noexcept
{
#if defined(_WIN32)
    return kFrameCommandDraws;
#else
    return 0;
#endif
}

std::size_t NativeRendererD3D12::stagedImportCountForTesting() const
{
#if defined(_WIN32)
    std::lock_guard lock(impl->stateMutex);
    return impl->admissionImports ? impl->admissionImports->size() : 0;
#else
    return 0;
#endif
}

} // namespace rerevved::gpu
