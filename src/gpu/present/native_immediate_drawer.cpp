#include "native_immediate_drawer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

#include <plume_d3d12.h>
#include <rex/math.h>
#include <rex/ui/graphics_util.h>

#include "native_immediate_pixel.h"
#include "native_immediate_vertex.h"

namespace rerevved::gpu
{

using namespace plume;
using namespace rex::ui;

namespace
{

struct TextureHandle final : ImmediateTexture
{
    explicit TextureHandle(std::shared_ptr<const NativeOverlayTexture> data)
    : ImmediateTexture(data->width, data->height)
    , data(std::move(data))
    {
    }

    std::shared_ptr<const NativeOverlayTexture> data;
};

RenderRect scissor(const NativeOverlayCommand& command, const NativeOverlaySnapshot& frame, uint32_t width, uint32_t height)
{
    const auto& draw = command.draw;
    if (!draw.scissor)
        return RenderRect(0, 0, width, height);
    const float sx = width / frame.width, sy = height / frame.height;
    const float left   = rex::clamp_float(draw.scissor_left * sx, 0.0F, float(width));
    const float top    = rex::clamp_float(draw.scissor_top * sy, 0.0F, float(height));
    const float right  = rex::clamp_float(draw.scissor_right * sx, left, float(width));
    const float bottom = rex::clamp_float(draw.scissor_bottom * sy, top, float(height));
    // Same 16.8 top-left inclusion rule as ImmediateDrawer::ScissorToRenderTarget.
    const auto pixel = [](float value)
    {
        return (FloatToD3D11Fixed16p8(value) + 127) >> 8;
    };
    return RenderRect(pixel(left), pixel(top), pixel(right), pixel(bottom));
}

} // namespace

void NativeOverlayMailbox::SetReady(bool ready)
{
    std::lock_guard lock(mutex_);
    ready_ = ready;
    if (!ready)
        latest_.reset();
}

bool NativeOverlayMailbox::Ready() const
{
    std::lock_guard lock(mutex_);
    return ready_ && !failed_.load();
}

void NativeOverlayMailbox::Publish(std::unique_ptr<NativeOverlaySnapshot> snapshot)
{
    std::lock_guard lock(mutex_);
    if (!ready_)
        return;
    snapshot->sequence = ++sequence_;
    latest_            = std::move(snapshot);
}

std::shared_ptr<const NativeOverlaySnapshot> NativeOverlayMailbox::Latest() const
{
    std::lock_guard lock(mutex_);
    return latest_;
}

void NativeOverlayMailbox::Fail()
{
    failed_.store(true);
}

bool NativeOverlayMailbox::Failed() const
{
    return failed_.load();
}

NativeImmediateDrawer::NativeImmediateDrawer(std::shared_ptr<NativeOverlayMailbox> mailbox)
: mailbox_(std::move(mailbox))
{
}

std::unique_ptr<ImmediateTexture> NativeImmediateDrawer::CreateTexture(uint32_t width, uint32_t height, ImmediateTextureFilter filter, bool repeated, const uint8_t* data)
{
    if (!mailbox_->Ready())
        return nullptr;
    // D3D12's supported 2D extent also bounds arithmetic and upload allocation.
    if (!width || !height || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    {
        mailbox_->Fail();
        return nullptr;
    }
    try
    {
        auto texture      = std::make_shared<NativeOverlayTexture>();
        texture->width    = width;
        texture->height   = height;
        texture->filter   = filter;
        texture->repeated = repeated;
        texture->rgba.resize(size_t(width) * height * 4);
        if (data)
            std::memcpy(texture->rgba.data(), data, texture->rgba.size());
        return std::make_unique<TextureHandle>(std::move(texture));
    }
    catch (const std::exception&)
    {
        mailbox_->Fail();
        return nullptr;
    }
}

void NativeImmediateDrawer::Begin(UIDrawContext& context, float width, float height)
{
    ImmediateDrawer::Begin(context, width, height);
    batchOpen_ = false;
    try
    {
        recording_         = std::make_unique<NativeOverlaySnapshot>();
        recording_->width  = coordinate_space_width();
        recording_->height = coordinate_space_height();
    }
    catch (const std::exception&)
    {
        mailbox_->Fail();
    }
}

void NativeImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch)
{
    batchOpen_ = false;
    if (!recording_ || mailbox_->Failed())
        return;
    if (batch.vertex_count < 0 || batch.index_count < 0 || (batch.vertex_count && !batch.vertices))
    {
        mailbox_->Fail();
        return;
    }
    try
    {
        auto& copy = recording_->batches.emplace_back();
        if (batch.vertex_count)
            copy.vertices.assign(batch.vertices, batch.vertices + batch.vertex_count);
        copy.indexed = batch.indices != nullptr;
        if (copy.indexed && batch.index_count)
            copy.indices.assign(batch.indices, batch.indices + batch.index_count);
        batchOpen_ = true;
    }
    catch (const std::exception&)
    {
        mailbox_->Fail();
    }
}

void NativeImmediateDrawer::Draw(const ImmediateDraw& draw)
{
    if (!batchOpen_ || mailbox_->Failed())
        return;
    auto& batch = recording_->batches.back();
    if (!draw.count)
        return;
    bool valid = draw.count >= 0 && draw.index_offset >= 0 &&
                 (draw.primitive_type == ImmediatePrimitiveType::kTriangles || draw.primitive_type == ImmediatePrimitiveType::kLines);
    if (batch.indexed)
    {
        valid = valid && uint64_t(draw.index_offset) + uint64_t(draw.count) <= batch.indices.size();
        if (valid)
            for (int i = 0; i < draw.count; ++i)
            {
                const int64_t vertex = int64_t(batch.indices[draw.index_offset + i]) + draw.base_vertex;
                valid                = valid && vertex >= 0 && uint64_t(vertex) < batch.vertices.size();
            }
    }
    else
        valid = valid && draw.base_vertex >= 0 && uint64_t(draw.base_vertex) + uint64_t(draw.count) <= batch.vertices.size();
    if (!valid)
    {
        mailbox_->Fail();
        return;
    }
    try
    {
        NativeOverlayCommand command;
        command.draw         = draw;
        command.draw.texture = nullptr; // Never publish a borrowed SDK handle.
        if (draw.texture)
        {
            const auto* texture = dynamic_cast<const TextureHandle*>(draw.texture);
            if (!texture)
            {
                mailbox_->Fail();
                return;
            }
            command.texture = texture->data;
        }
        batch.commands.push_back(std::move(command));
    }
    catch (const std::exception&)
    {
        mailbox_->Fail();
    }
}

void NativeImmediateDrawer::EndDrawBatch()
{
    batchOpen_ = false;
}

void NativeImmediateDrawer::End()
{
    if (recording_ && !mailbox_->Failed())
        mailbox_->Publish(std::move(recording_));
    recording_.reset();
    ImmediateDrawer::End();
}

void NativeImmediateDrawer::Clear()
{
    const auto previous = mailbox_->Latest();
    if (!previous || previous->batches.empty())
        return;
    try
    {
        mailbox_->Publish(std::make_unique<NativeOverlaySnapshot>());
    }
    catch (const std::exception&)
    {
        mailbox_->Fail();
    }
}

struct NativeOverlayCompositor::Impl
{
    struct Texture
    {
        std::weak_ptr<const NativeOverlayTexture> source;
        std::unique_ptr<RenderTexture>            image;
        std::unique_ptr<RenderSampler>            sampler;
        std::unique_ptr<RenderDescriptorSet>      descriptors;
    };

    struct Slot
    {
        std::shared_ptr<const NativeOverlaySnapshot> snapshot;
        std::unique_ptr<RenderBuffer>                geometry;
        size_t                                       capacity = 0;
        std::vector<std::unique_ptr<RenderBuffer>>   textureUploads;
    };

    std::unique_ptr<RenderShader>                  vertex, pixel;
    std::unique_ptr<RenderPipelineLayout>          layout;
    std::unique_ptr<RenderPipeline>                triangles, lines;
    std::map<size_t, Slot>                         slots;
    std::map<const NativeOverlayTexture*, Texture> textures;
    size_t                                         geometryAllocations = 0;
    std::shared_ptr<const NativeOverlayTexture>    white               = std::make_shared<NativeOverlayTexture>(NativeOverlayTexture{ 1, 1, ImmediateTextureFilter::kNearest, false, { 255, 255, 255, 255 } });

    static RenderDescriptorSetDesc setDesc()
    {
        static const RenderDescriptorRange ranges[] = {
            { RenderDescriptorRangeType::TEXTURE, 0, 1 }, { RenderDescriptorRangeType::SAMPLER, 1, 1 }
        };
        return RenderDescriptorSetDesc(ranges, 2);
    }

    bool initialize(RenderDevice& device)
    {
        if (triangles && lines)
            return true;
        vertex                                    = device.createShader(kNativeImmediateVertexDxil, sizeof(kNativeImmediateVertexDxil), "VSMain", RenderShaderFormat::DXIL);
        pixel                                     = device.createShader(kNativeImmediatePixelDxil, sizeof(kNativeImmediatePixelDxil), "PSMain", RenderShaderFormat::DXIL);
        const auto                    descriptors = setDesc();
        const RenderPushConstantRange constants(0, 0, 0, 8, RenderShaderStageFlag::VERTEX);
        RenderPipelineLayoutDesc      desc;
        desc.descriptorSetDescs      = &descriptors;
        desc.descriptorSetDescsCount = 1;
        desc.pushConstantRanges      = &constants;
        desc.pushConstantRangesCount = 1;
        desc.allowInputLayout        = true;
        layout                       = device.createPipelineLayout(desc);
        const auto* native           = dynamic_cast<D3D12PipelineLayout*>(layout.get());
        if (!vertex || !pixel || !native || !native->rootSignature)
            return false;
        const RenderInputSlot    input(0, sizeof(ImmediateVertex));
        const RenderInputElement elements[] = {
            { "POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, offsetof(ImmediateVertex, x) },
            { "TEXCOORD", 0, 1, RenderFormat::R32G32_FLOAT, 0, offsetof(ImmediateVertex, u) },
            { "COLOR", 0, 2, RenderFormat::R8G8B8A8_UNORM, 0, offsetof(ImmediateVertex, color) }
        };
        RenderGraphicsPipelineDesc pipeline;
        pipeline.pipelineLayout                     = layout.get();
        pipeline.vertexShader                       = vertex.get();
        pipeline.pixelShader                        = pixel.get();
        pipeline.renderTargetCount                  = 1;
        pipeline.renderTargetFormat[0]              = RenderFormat::R8G8B8A8_UNORM;
        pipeline.renderTargetBlend[0]               = RenderBlendDesc::AlphaBlend();
        pipeline.renderTargetBlend[0].dstBlendAlpha = RenderBlend::ONE;
        pipeline.depthClipEnabled                   = true;
        pipeline.inputSlots                         = &input;
        pipeline.inputSlotsCount                    = 1;
        pipeline.inputElements                      = elements;
        pipeline.inputElementsCount                 = 3;
        triangles                                   = device.createGraphicsPipeline(pipeline);
        pipeline.primitiveTopology                  = RenderPrimitiveTopology::LINE_LIST;
        lines                                       = device.createGraphicsPipeline(pipeline);
        const auto* t                               = dynamic_cast<D3D12GraphicsPipeline*>(triangles.get());
        const auto* l                               = dynamic_cast<D3D12GraphicsPipeline*>(lines.get());
        return t && t->d3d && l && l->d3d;
    }

    bool reserveGeometry(RenderDevice& device, Slot& slot, size_t size)
    {
        if (size <= slot.capacity)
            return true;
        const size_t capacity = std::max(size, std::min(slot.capacity * 2, size_t(UINT32_MAX)));
        auto         buffer   = device.createBuffer(RenderBufferDesc::UploadBuffer(capacity, RenderBufferFlag::VERTEX | RenderBufferFlag::INDEX));
        if (!buffer)
            return false;
        slot.geometry = std::move(buffer);
        slot.capacity = capacity;
        ++geometryAllocations;
        return true;
    }

    Texture* texture(RenderDevice& device, RenderCommandList& list, Slot& slot, const std::shared_ptr<const NativeOverlayTexture>& source)
    {
        if (auto found = textures.find(source.get()); found != textures.end())
            return &found->second;
        Texture texture;
        texture.source           = source;
        texture.image            = device.createTexture(RenderTextureDesc::Texture2D(source->width, source->height, 1, RenderFormat::R8G8B8A8_UNORM));
        const uint32_t    pitch  = (source->width * 4 + 255) & ~255U;
        auto              upload = device.createBuffer(RenderBufferDesc::UploadBuffer(size_t(pitch) * source->height));
        RenderSamplerDesc sampler;
        sampler.minFilter  = source->filter == ImmediateTextureFilter::kNearest ? RenderFilter::NEAREST : RenderFilter::LINEAR;
        sampler.magFilter  = sampler.minFilter;
        sampler.mipmapMode = RenderMipmapMode::NEAREST;
        sampler.addressU = sampler.addressV = sampler.addressW = source->repeated ? RenderTextureAddressMode::WRAP : RenderTextureAddressMode::CLAMP;
        sampler.maxLOD                                         = 0;
        texture.sampler                                        = device.createSampler(sampler);
        texture.descriptors                                    = device.createDescriptorSet(setDesc());
        const auto* nativeImage                                = dynamic_cast<D3D12Texture*>(texture.image.get());
        if (!nativeImage || !nativeImage->d3d || !upload || !texture.sampler || !texture.descriptors)
            return nullptr;
        auto* mapped = static_cast<uint8_t*>(upload->map());
        if (!mapped)
            return nullptr;
        for (uint32_t y = 0; y < source->height; ++y)
            std::memcpy(mapped + size_t(y) * pitch, source->rgba.data() + size_t(y) * source->width * 4, size_t(source->width) * 4);
        upload->unmap();
        texture.descriptors->setTexture(0, texture.image.get(), RenderTextureLayout::SHADER_READ);
        texture.descriptors->setSampler(1, texture.sampler.get());
        // Install ownership before recording any GPU references, including on a later failure.
        auto& stored  = textures.emplace(source.get(), std::move(texture)).first->second;
        auto* staging = slot.textureUploads.emplace_back(std::move(upload)).get();
        list.barriers(RenderBarrierStage::COPY, RenderTextureBarrier(stored.image.get(), RenderTextureLayout::COPY_DEST));
        list.copyTextureRegion(RenderTextureCopyLocation::Subresource(stored.image.get()),
                               RenderTextureCopyLocation::PlacedFootprint(staging, RenderFormat::R8G8B8A8_UNORM, source->width, source->height, 1, pitch / 4));
        list.barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(stored.image.get(), RenderTextureLayout::SHADER_READ));
        return &stored;
    }
};

NativeOverlayCompositor::NativeOverlayCompositor()
: impl_(std::make_unique<Impl>())
{
}

NativeOverlayCompositor::~NativeOverlayCompositor() = default;

size_t NativeOverlayCompositor::LiveTextures() const
{
    return impl_->textures.size();
}

size_t NativeOverlayCompositor::GeometryAllocations() const
{
    return impl_->geometryAllocations;
}

size_t NativeOverlayCompositor::PendingTextureUploads() const
{
    size_t count = 0;
    for (const auto& [index, slot] : impl_->slots)
        count += slot.textureUploads.size();
    return count;
}

bool NativeOverlayCompositor::Record(RenderDevice& device, RenderCommandList& list, size_t slotIndex, std::shared_ptr<const NativeOverlaySnapshot> snapshot, uint32_t width, uint32_t height)
{
    try
    {
        auto& state = *impl_;
        auto& slot  = state.slots[slotIndex];
        // The caller has retired this slot's host fence; geometry capacity is reusable.
        slot.snapshot.reset();
        slot.textureUploads.clear();
        std::erase_if(state.textures, [](const auto& entry)
                      {
                          return entry.second.source.expired();
                      });
        slot.snapshot = std::move(snapshot);
        if (!slot.snapshot || slot.snapshot->batches.empty())
            return true;
        const auto& frame = *slot.snapshot;
        if (!(frame.width > 0) || !(frame.height > 0) || !std::isfinite(frame.width) || !std::isfinite(frame.height) || !state.initialize(device))
            return false;
        const auto aligned = [](size_t size)
        {
            return (size + 3) & ~size_t(3);
        };
        size_t required = 0;
        for (const auto& batch : frame.batches)
        {
            if (batch.commands.empty())
                continue;
            required += aligned(batch.vertices.size() * sizeof(ImmediateVertex));
            if (batch.indexed)
                required += aligned(batch.indices.size() * sizeof(uint16_t));
            if (required > UINT32_MAX)
                return false;
        }
        if (!required)
            return true;
        if (!state.reserveGeometry(device, slot, required))
            return false;
        auto* mapped = static_cast<uint8_t*>(slot.geometry->map());
        if (!mapped)
            return false;
        size_t offset = 0;
        for (const auto& batch : frame.batches)
        {
            if (batch.commands.empty())
                continue;
            const size_t vertices = batch.vertices.size() * sizeof(ImmediateVertex);
            std::memcpy(mapped + offset, batch.vertices.data(), vertices);
            offset += aligned(vertices);
            if (batch.indexed)
            {
                const size_t indices = batch.indices.size() * sizeof(uint16_t);
                std::memcpy(mapped + offset, batch.indices.data(), indices);
                offset += aligned(indices);
            }
        }
        slot.geometry->unmap();
        offset = 0;
        list.setViewports(RenderViewport(0, 0, float(width), float(height)));
        list.setGraphicsPipelineLayout(state.layout.get());
        const float inverse[] = { 1.0F / frame.width, 1.0F / frame.height };
        list.setGraphicsPushConstants(0, inverse);
        const RenderInputSlot input(0, sizeof(ImmediateVertex));
        for (const auto& batch : frame.batches)
        {
            if (batch.commands.empty())
                continue;
            const uint32_t               vertexBytes = uint32_t(batch.vertices.size() * sizeof(ImmediateVertex));
            const RenderVertexBufferView vertexView(slot.geometry->at(offset), vertexBytes);
            offset += aligned(vertexBytes);
            list.setVertexBuffers(0, &vertexView, 1, &input);
            if (batch.indexed)
            {
                const uint32_t              indexBytes = uint32_t(batch.indices.size() * sizeof(uint16_t));
                const RenderIndexBufferView indexView(slot.geometry->at(offset), indexBytes, RenderFormat::R16_UINT);
                offset += aligned(indexBytes);
                list.setIndexBuffer(&indexView);
            }
            else
                list.setIndexBuffer(nullptr);
            for (const auto& command : batch.commands)
            {
                const auto rect = scissor(command, frame, width, height);
                if (rect.right <= rect.left || rect.bottom <= rect.top || !command.draw.count)
                    continue;
                auto* texture = state.texture(device, list, slot, command.texture ? command.texture : state.white);
                if (!texture)
                    return false;
                list.setScissors(rect);
                list.setGraphicsDescriptorSet(texture->descriptors.get(), 0);
                list.setPipeline(command.draw.primitive_type == ImmediatePrimitiveType::kLines ? state.lines.get() : state.triangles.get());
                if (batch.indexed)
                    list.drawIndexedInstanced(command.draw.count, 1, command.draw.index_offset, command.draw.base_vertex, 0);
                else
                    list.drawInstanced(command.draw.count, 1, command.draw.base_vertex, 0);
            }
        }
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

} // namespace rerevved::gpu
