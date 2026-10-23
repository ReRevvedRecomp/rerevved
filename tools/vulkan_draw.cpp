#include "vulkan_draw.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace rerevved::gpu
{
namespace
{

#define INSTANCE_FUNCTIONS(X)                 \
    X(DestroyInstance)                        \
    X(EnumeratePhysicalDevices)               \
    X(GetPhysicalDeviceProperties)            \
    X(GetPhysicalDeviceFeatures2)             \
    X(GetPhysicalDeviceQueueFamilyProperties) \
    X(GetPhysicalDeviceMemoryProperties)      \
    X(GetPhysicalDeviceFormatProperties)      \
    X(GetPhysicalDeviceImageFormatProperties) \
    X(CreateDevice)                           \
    X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X)        \
    X(DestroyDevice)               \
    X(GetDeviceQueue)              \
    X(CreateBuffer)                \
    X(DestroyBuffer)               \
    X(GetBufferMemoryRequirements) \
    X(AllocateMemory)              \
    X(FreeMemory)                  \
    X(BindBufferMemory)            \
    X(MapMemory)                   \
    X(UnmapMemory)                 \
    X(CreateImage)                 \
    X(DestroyImage)                \
    X(GetImageMemoryRequirements)  \
    X(BindImageMemory)             \
    X(CreateImageView)             \
    X(DestroyImageView)            \
    X(CreateSampler)               \
    X(DestroySampler)              \
    X(CreateShaderModule)          \
    X(DestroyShaderModule)         \
    X(CreateDescriptorSetLayout)   \
    X(DestroyDescriptorSetLayout)  \
    X(CreatePipelineLayout)        \
    X(DestroyPipelineLayout)       \
    X(CreateDescriptorPool)        \
    X(DestroyDescriptorPool)       \
    X(AllocateDescriptorSets)      \
    X(UpdateDescriptorSets)        \
    X(CreateRenderPass)            \
    X(DestroyRenderPass)           \
    X(CreateFramebuffer)           \
    X(DestroyFramebuffer)          \
    X(CreateGraphicsPipelines)     \
    X(CreateComputePipelines)      \
    X(DestroyPipeline)             \
    X(CreateCommandPool)           \
    X(DestroyCommandPool)          \
    X(AllocateCommandBuffers)      \
    X(BeginCommandBuffer)          \
    X(EndCommandBuffer)            \
    X(CmdPipelineBarrier)          \
    X(CmdCopyBufferToImage)        \
    X(CmdBeginRenderPass)          \
    X(CmdEndRenderPass)            \
    X(CmdClearAttachments)         \
    X(CmdBindPipeline)             \
    X(CmdBindDescriptorSets)       \
    X(CmdBindVertexBuffers)        \
    X(CmdBindIndexBuffer)          \
    X(CmdDrawIndexed)              \
    X(CmdDispatch)                 \
    X(CreateFence)                 \
    X(DestroyFence)                \
    X(QueueSubmit)                 \
    X(WaitForFences)

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
    }
}

struct Buffer
{
    VkBuffer       handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize   size   = 0;
};

struct Image
{
    VkImage        handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view   = VK_NULL_HANDLE;
};

struct DrawResources
{
    HMODULE                   loader              = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkCreateInstance      createInstance      = nullptr;
#define DECLARE_FUNCTION(name) PFN_vk##name name = nullptr;
    INSTANCE_FUNCTIONS(DECLARE_FUNCTION)
    DEVICE_FUNCTIONS(DECLARE_FUNCTION)
#undef DECLARE_FUNCTION
    VkInstance                         instance = VK_NULL_HANDLE;
    VkPhysicalDevice                   physical = VK_NULL_HANDLE;
    VkDevice                           device   = VK_NULL_HANDLE;
    VkQueue                            queue    = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties   memoryProperties{};
    std::vector<Buffer>                buffers;
    std::vector<Image>                 images;
    std::vector<VkShaderModule>        modules;
    std::vector<VkDescriptorSetLayout> setLayouts;
    std::vector<VkPipelineLayout>      pipelineLayouts;
    std::vector<VkPipeline>            pipelines;
    VkSampler                          sampler     = VK_NULL_HANDLE;
    VkDescriptorPool                   descriptors = VK_NULL_HANDLE;
    VkRenderPass                       renderPass  = VK_NULL_HANDLE;
    VkFramebuffer                      framebuffer = VK_NULL_HANDLE;
    VkCommandPool                      commandPool = VK_NULL_HANDLE;
    VkFence                            fence       = VK_NULL_HANDLE;

    ~DrawResources()
    {
        // A submitted healthy-device timeout keeps this entire owner alive.
        if (device && DestroyDevice)
        {
            if (fence)
            {
                DestroyFence(device, fence, nullptr);
            }
            if (commandPool)
            {
                DestroyCommandPool(device, commandPool, nullptr);
            }
            for (auto pipeline : pipelines)
            {
                DestroyPipeline(device, pipeline, nullptr);
            }
            if (framebuffer)
            {
                DestroyFramebuffer(device, framebuffer, nullptr);
            }
            if (renderPass)
            {
                DestroyRenderPass(device, renderPass, nullptr);
            }
            if (descriptors)
            {
                DestroyDescriptorPool(device, descriptors, nullptr);
            }
            for (auto layout : pipelineLayouts)
            {
                DestroyPipelineLayout(device, layout, nullptr);
            }
            for (auto layout : setLayouts)
            {
                DestroyDescriptorSetLayout(device, layout, nullptr);
            }
            for (auto module : modules)
            {
                DestroyShaderModule(device, module, nullptr);
            }
            if (sampler)
            {
                DestroySampler(device, sampler, nullptr);
            }
            for (const auto& image : images)
            {
                if (image.view)
                {
                    DestroyImageView(device, image.view, nullptr);
                }
                if (image.handle)
                {
                    DestroyImage(device, image.handle, nullptr);
                }
                if (image.memory)
                {
                    FreeMemory(device, image.memory, nullptr);
                }
            }
            for (const auto& buffer : buffers)
            {
                if (buffer.handle)
                {
                    DestroyBuffer(device, buffer.handle, nullptr);
                }
                if (buffer.memory)
                {
                    FreeMemory(device, buffer.memory, nullptr);
                }
            }
            DestroyDevice(device, nullptr);
        }
        if (instance && DestroyInstance)
        {
            DestroyInstance(instance, nullptr);
        }
        if (loader)
        {
            FreeLibrary(loader);
        }
    }

    std::uint32_t memoryType(std::uint32_t bits, VkMemoryPropertyFlags flags)
    {
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index)
        {
            if ((bits & (1U << index)) && (memoryProperties.memoryTypes[index].propertyFlags & flags) == flags)
            {
                return index;
            }
        }
        throw std::runtime_error("Vulkan device lacks required memory type");
    }

    Buffer makeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, const void* initial = nullptr)
    {
        buffers.emplace_back();
        auto& buffer = buffers.back();
        buffer.size  = bytes;
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size        = bytes;
        info.usage       = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(CreateBuffer(device, &info, nullptr, &buffer.handle), "vkCreateBuffer");
        VkMemoryRequirements requirements;
        GetBufferMemoryRequirements(device, buffer.handle, &requirements);
        VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.allocationSize  = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(AllocateMemory(device, &allocation, nullptr, &buffer.memory), "vkAllocateMemory buffer");
        check(BindBufferMemory(device, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
        if (initial)
        {
            void* mapped = nullptr;
            check(MapMemory(device, buffer.memory, 0, bytes, 0, &mapped), "vkMapMemory upload");
            std::memcpy(mapped, initial, static_cast<std::size_t>(bytes));
            UnmapMemory(device, buffer.memory);
        }
        return buffer;
    }

    Image makeImage(std::uint32_t width, std::uint32_t height, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkComponentMapping components, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
    {
        images.emplace_back();
        auto&             image = images.back();
        VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType   = VK_IMAGE_TYPE_2D;
        info.format      = format;
        info.extent      = { width, height, 1 };
        info.mipLevels   = 1;
        info.arrayLayers = 1;
        info.samples     = samples;
        info.tiling      = VK_IMAGE_TILING_OPTIMAL;
        info.usage       = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(CreateImage(device, &info, nullptr, &image.handle), "vkCreateImage");
        VkMemoryRequirements requirements;
        GetImageMemoryRequirements(device, image.handle, &requirements);
        VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocation.allocationSize  = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(AllocateMemory(device, &allocation, nullptr, &image.memory), "vkAllocateMemory image");
        check(BindImageMemory(device, image.handle, image.memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image            = image.handle;
        view.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        view.format           = info.format;
        view.components       = components;
        view.subresourceRange = { aspect, 0, 1, 0, 1 };
        check(CreateImageView(device, &view, nullptr, &image.view), "vkCreateImageView");
        return image;
    }

    VkShaderModule makeModule(const std::vector<std::uint32_t>& words)
    {
        if (words.size() < 5 || words[0] != 0x07230203U)
        {
            throw std::runtime_error("invalid SPIR-V module header");
        }
        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        info.codeSize = words.size() * sizeof(std::uint32_t);
        info.pCode    = words.data();
        VkShaderModule module;
        check(CreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
        modules.push_back(module);
        return module;
    }
};

VkBlendFactor blendFactor(NativeDrawReplayBlendFactor factor)
{
    constexpr std::array values{ VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR };
    const auto           index = static_cast<std::size_t>(factor);
    if (index >= values.size())
    {
        throw std::runtime_error("unsupported blend factor");
    }
    if (factor == NativeDrawReplayBlendFactor::BlendFactor || factor == NativeDrawReplayBlendFactor::InverseBlendFactor)
    {
        throw std::runtime_error("recipe has no explicit constant blend factor");
    }
    return values[index];
}

VkBlendOp blendOp(NativeDrawReplayBlendOp op)
{
    constexpr std::array values{ VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX };
    const auto           index = static_cast<std::size_t>(op);
    if (index >= values.size())
    {
        throw std::runtime_error("unsupported blend operation");
    }
    return values[index];
}

VkSamplerAddressMode addressMode(std::uint32_t mode)
{
    switch (mode)
    {
        case 1:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 2:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
            throw std::runtime_error("unsupported sampler address mode");
    }
}

bool supportedSamplerBorder(const NativeDrawReplaySampler& sampler)
{
    if (std::find(sampler.address.begin(), sampler.address.end(), 4) == sampler.address.end())
    {
        return true;
    }
    return sampler.border == std::array<float, 4>{} || sampler.border == std::array<float, 4>{ 0, 0, 0, 1 } || sampler.border == std::array<float, 4>{ 1, 1, 1, 1 };
}

VkFormat textureFormat(NativeDrawReplayTextureFormat format)
{
    switch (format)
    {
        case NativeDrawReplayTextureFormat::Rgba8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case NativeDrawReplayTextureFormat::Bc1:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case NativeDrawReplayTextureFormat::Bc2:
            return VK_FORMAT_BC2_UNORM_BLOCK;
        default:
            throw std::runtime_error("unsupported Vulkan texture format");
    }
}

std::size_t textureBlockBytes(NativeDrawReplayTextureFormat format)
{
    return format == NativeDrawReplayTextureFormat::Bc1 ? 8U : 16U;
}

const char* textureFormatName(NativeDrawReplayTextureFormat format)
{
    switch (format)
    {
        case NativeDrawReplayTextureFormat::Rgba8:
            return "RGBA8";
        case NativeDrawReplayTextureFormat::Bc1:
            return "BC1";
        case NativeDrawReplayTextureFormat::Bc2:
            return "BC2";
        default:
            return "unsupported";
    }
}

std::string textureFormatReport(std::span<const NativeDrawReplayRecipe> recipes)
{
    std::ostringstream report;
    bool               compressed = false;
    for (std::size_t index = 0; index < recipes.size(); ++index)
    {
        if (index)
        {
            report << ',';
        }
        const auto format = recipes[index].textures[0].format;
        report << textureFormatName(format);
        compressed |= format != NativeDrawReplayTextureFormat::Rgba8;
    }
    if (compressed)
    {
        report << "; textureCompressionBC=1";
    }
    return report.str();
}

void admit(const NativeDrawReplayRecipe& recipe)
{
    const bool originalPair   = recipe.vertexShaderHash == 0xC3BAB94E67553E12ULL && recipe.pixelShaderHash == 0x64D8E6A475FFCB2DULL;
    const bool additionalPair = recipe.vertexShaderHash == 0x1EE55F3AB5213177ULL && recipe.pixelShaderHash == 0x47F2D46F3B8F1668ULL;
    if (!originalPair && !additionalPair)
    {
        throw std::runtime_error("Vulkan offscreen supports only the approved shader pairs");
    }
    if (recipe.sampleCount != 4 || recipe.sampleMask != 9 || recipe.targetFormat != NativeDrawReplayTargetFormat::Rgba8 || recipe.textureMask != 1)
    {
        throw std::runtime_error("Vulkan offscreen requires RGBA8, sampleCount 4, mask 9, and texture mask 1");
    }
    if (recipe.stencil.enabled || !recipe.depth.initialSamples.empty())
    {
        throw std::runtime_error("Vulkan offscreen does not support stencil state or initial depth samples");
    }
    if (recipe.depth.compare < 1 || recipe.depth.compare > 8 || !std::isfinite(recipe.depth.initialClear) || recipe.depth.initialClear < 0 || recipe.depth.initialClear > 1)
    {
        throw std::runtime_error("unsupported depth comparison or clear value");
    }
    if (!recipe.initialSample0.empty() || !recipe.initialSample1.empty())
    {
        throw std::runtime_error("Vulkan offscreen does not support initial color sample payloads");
    }
    if (recipe.blend.logicOpEnabled || recipe.blend.alphaToCoverage)
    {
        throw std::runtime_error("Vulkan offscreen does not support logic operations or alpha-to-coverage");
    }
    if (recipe.blend.writeMask > 0x0F)
    {
        throw std::runtime_error("Vulkan offscreen has an invalid color write mask");
    }
    (void)blendFactor(recipe.blend.sourceColor);
    (void)blendFactor(recipe.blend.destinationColor);
    (void)blendFactor(recipe.blend.sourceAlpha);
    (void)blendFactor(recipe.blend.destinationAlpha);
    (void)blendOp(recipe.blend.colorOp);
    (void)blendOp(recipe.blend.alphaOp);
    if (recipe.rasterizer.cull > 2 || recipe.vertexAttributeCount != 3 || recipe.vertexStrideBytes < 48 || recipe.vertexBytes().empty() || recipe.indices.empty() || recipe.indexCount != recipe.indices.size() || recipe.indexCount % 3 != 0)
    {
        throw std::runtime_error("Vulkan offscreen requires three float4 attributes and indexed triangles");
    }
    if (!recipe.rasterizer.depthClip)
    {
        throw std::runtime_error("Vulkan offscreen requires depth clipping");
    }
    if (recipe.vertexConstants.size() != 4096 || recipe.pixelConstants.size() != 4096 || recipe.sharedConstants.size() != 336)
    {
        throw std::runtime_error("Vulkan specialization requires 4096-byte VS/PS and 336-byte shared constants");
    }
    if (recipe.textures.empty())
    {
        throw std::runtime_error("Vulkan offscreen requires one CPU texture");
    }
    const auto& texture = recipe.textures[0];
    if (texture.frameTextureId || (texture.format != NativeDrawReplayTextureFormat::Rgba8 && texture.format != NativeDrawReplayTextureFormat::Bc1 && texture.format != NativeDrawReplayTextureFormat::Bc2) || texture.dimension != NativeDrawReplayTextureDimension::Texture2D || texture.mipLevels != 1 || !texture.width || !texture.height)
    {
        throw std::runtime_error("Vulkan offscreen requires a CPU RGBA8, BC1 or BC2 single-mip 2D texture");
    }
    const auto textureBytes = texture.format == NativeDrawReplayTextureFormat::Rgba8
                                  ? static_cast<std::uint64_t>(texture.width) * texture.height * 4
                                  : ((static_cast<std::uint64_t>(texture.width) + 3) / 4) * ((static_cast<std::uint64_t>(texture.height) + 3) / 4) * textureBlockBytes(texture.format);
    if (texture.payloadBytes().size() != textureBytes)
    {
        throw std::runtime_error("texture payload does not match tightly packed RGBA8, BC1 or BC2 extent");
    }
    if (std::any_of(texture.swizzle.begin(), texture.swizzle.end(), [](auto value)
                    {
                        return value > 5;
                    }))
    {
        throw std::runtime_error("unsupported texture swizzle");
    }
    if (recipe.samplers.size() > 1)
    {
        throw std::runtime_error("Vulkan specialization uses exactly one sampler");
    }
    const auto& sampler = recipe.samplers.empty() ? recipe.sampler : recipe.samplers[0];
    if (sampler.anisotropy || !std::isfinite(sampler.minLod) || !std::isfinite(sampler.maxLod) || !std::isfinite(sampler.mipBias) || sampler.minLod < 0 || sampler.maxLod < sampler.minLod)
    {
        throw std::runtime_error("unsupported sampler anisotropy or LOD state");
    }
    for (auto mode : sampler.address)
    {
        (void)addressMode(mode);
    }
    if (!supportedSamplerBorder(sampler))
    {
        throw std::runtime_error("unsupported Vulkan custom sampler border color");
    }
    const auto& viewport = recipe.viewport;
    if (!recipe.width || !recipe.height || !std::isfinite(viewport.x) || !std::isfinite(viewport.y) || !std::isfinite(viewport.width) || !std::isfinite(viewport.height) || viewport.width <= 0 || viewport.height <= 0 || viewport.minDepth < 0 || viewport.maxDepth > 1 || viewport.minDepth > viewport.maxDepth || !std::isfinite(viewport.minDepth) || !std::isfinite(viewport.maxDepth))
    {
        throw std::runtime_error("unsupported viewport");
    }
    if (recipe.scissor.left < 0 || recipe.scissor.top < 0 || recipe.scissor.right <= recipe.scissor.left || recipe.scissor.bottom <= recipe.scissor.top || static_cast<std::uint32_t>(recipe.scissor.right) > recipe.width || static_cast<std::uint32_t>(recipe.scissor.bottom) > recipe.height)
    {
        throw std::runtime_error("unsupported scissor");
    }
    if (static_cast<std::uint64_t>(recipe.width) * recipe.height * 8 > 0xFFFFFFFFULL)
    {
        throw std::runtime_error("sample helper byte addressing exceeds 32 bits");
    }
    const auto vertexCount = recipe.vertexBytes().size() / recipe.vertexStrideBytes;
    if (recipe.vertexBytes().size() % recipe.vertexStrideBytes || std::any_of(recipe.indices.begin(), recipe.indices.end(), [vertexCount](auto index)
                                                                              {
                                                                                  return index >= vertexCount;
                                                                              }))
    {
        throw std::runtime_error("vertex payload or indices exceed the supplied geometry");
    }
}

void imageBarrier(DrawResources& resources, VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask       = sourceAccess;
    barrier.dstAccessMask       = destinationAccess;
    barrier.oldLayout           = before;
    barrier.newLayout           = after;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = { aspect, 0, 1, 0, 1 };
    resources.CmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

bool ValidateVulkanOffscreenPass(std::span<const NativeDrawReplayRecipe> recipes,
                                 std::string&                            error)
{
    error.clear();
    try
    {
        if (recipes.empty() || recipes.size() > 2)
        {
            throw std::runtime_error("Vulkan offscreen pass requires one or two draws");
        }
        const auto& first = recipes.front();
        admit(first);
        for (std::size_t index = 1; index < recipes.size(); ++index)
        {
            const auto& draw = recipes[index];
            admit(draw);
            if (draw.width != first.width || draw.height != first.height || draw.sampleCount != first.sampleCount || draw.sampleMask != first.sampleMask ||
                draw.targetFormat != first.targetFormat || draw.vertexStrideBytes != first.vertexStrideBytes || draw.vertexAttributeCount != first.vertexAttributeCount ||
                draw.textureMask != first.textureMask || draw.viewport.x != first.viewport.x || draw.viewport.y != first.viewport.y ||
                draw.viewport.width != first.viewport.width || draw.viewport.height != first.viewport.height || draw.viewport.minDepth != first.viewport.minDepth ||
                draw.viewport.maxDepth != first.viewport.maxDepth || draw.scissor.left != first.scissor.left || draw.scissor.top != first.scissor.top ||
                draw.scissor.right != first.scissor.right || draw.scissor.bottom != first.scissor.bottom || draw.blend.enabled != first.blend.enabled ||
                draw.blend.alphaToCoverage != first.blend.alphaToCoverage || draw.blend.logicOpEnabled != first.blend.logicOpEnabled ||
                draw.blend.sourceColor != first.blend.sourceColor || draw.blend.destinationColor != first.blend.destinationColor ||
                draw.blend.colorOp != first.blend.colorOp || draw.blend.sourceAlpha != first.blend.sourceAlpha ||
                draw.blend.destinationAlpha != first.blend.destinationAlpha || draw.blend.alphaOp != first.blend.alphaOp || draw.blend.writeMask != first.blend.writeMask ||
                draw.depth.enabled != first.depth.enabled || draw.depth.writeEnabled != first.depth.writeEnabled || draw.depth.compare != first.depth.compare ||
                draw.depth.initialClear != first.depth.initialClear || draw.clearColor != first.clearColor ||
                draw.rasterizer.cull != first.rasterizer.cull || draw.rasterizer.frontCounterClockwise != first.rasterizer.frontCounterClockwise ||
                draw.stencil.enabled != first.stencil.enabled || draw.stencil.reference != first.stencil.reference || draw.stencil.readMask != first.stencil.readMask ||
                draw.stencil.writeMask != first.stencil.writeMask || draw.vertexShaderHash != first.vertexShaderHash || draw.pixelShaderHash != first.pixelShaderHash)
            {
                throw std::runtime_error("Vulkan pass draws do not share fixed target or pipeline state");
            }
            const auto& firstSampler = first.samplers.empty() ? first.sampler : first.samplers[0];
            const auto& drawSampler  = draw.samplers.empty() ? draw.sampler : draw.samplers[0];
            if (drawSampler.minLinear != firstSampler.minLinear || drawSampler.magLinear != firstSampler.magLinear || drawSampler.mipLinear != firstSampler.mipLinear ||
                drawSampler.anisotropy != firstSampler.anisotropy || drawSampler.address != firstSampler.address || drawSampler.border != firstSampler.border ||
                drawSampler.minLod != firstSampler.minLod || drawSampler.maxLod != firstSampler.maxLod || drawSampler.mipBias != firstSampler.mipBias)
            {
                throw std::runtime_error("Vulkan pass draws do not share sampler state");
            }
        }
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool ReplayVulkanOffscreenPass(std::span<const NativeDrawReplayRecipe> recipes,
                               const std::vector<std::uint32_t>&       vertexSpirv,
                               const std::vector<std::uint32_t>&       pixelSpirv,
                               const std::vector<std::uint32_t>&       sampleSpirv,
                               const VulkanDrawPassOptions&            options,
                               VulkanDrawOutput&                       output,
                               std::string&                            error)
{
    output = {};
    error.clear();
    auto  owner = std::make_unique<DrawResources>();
    auto& r     = *owner;
    try
    {
        if (!ValidateVulkanOffscreenPass(recipes, error))
        {
            throw std::runtime_error(error);
        }
        if (std::any_of(options.clearColor.begin(), options.clearColor.end(), [](float value)
                        {
                            return !std::isfinite(value) || value < 0 || value > 1;
                        }) ||
            !std::isfinite(options.clearDepth) || options.clearDepth < 0 || options.clearDepth > 1)
        {
            throw std::runtime_error("Vulkan offscreen pass has an invalid clear value");
        }
        const auto& recipe = recipes.front();
        r.loader           = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!r.loader)
        {
            throw std::runtime_error("system Vulkan loader vulkan-1.dll is unavailable");
        }
        r.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(r.loader, "vkGetInstanceProcAddr"));
        if (!r.getInstanceProcAddr)
        {
            throw std::runtime_error("Vulkan loader lacks vkGetInstanceProcAddr");
        }
        r.createInstance = reinterpret_cast<PFN_vkCreateInstance>(r.getInstanceProcAddr(nullptr, "vkCreateInstance"));
        if (!r.createInstance)
        {
            throw std::runtime_error("Vulkan loader lacks vkCreateInstance");
        }
        VkApplicationInfo application{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        application.pApplicationName = "ReRevved offscreen draw";
        application.apiVersion       = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceInfo.pApplicationInfo = &application;
        check(r.createInstance(&instanceInfo, nullptr, &r.instance), "vkCreateInstance Vulkan 1.2");
#define LOAD_INSTANCE(name)                                                                 \
    r.name = reinterpret_cast<PFN_vk##name>(r.getInstanceProcAddr(r.instance, "vk" #name)); \
    if (!r.name)                                                                            \
        throw std::runtime_error("Vulkan loader lacks vk" #name);
        INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
        std::uint32_t physicalCount = 0;
        check(r.EnumeratePhysicalDevices(r.instance, &physicalCount, nullptr), "vkEnumeratePhysicalDevices count");
        std::vector<VkPhysicalDevice> physicalDevices(physicalCount);
        check(r.EnumeratePhysicalDevices(r.instance, &physicalCount, physicalDevices.data()), "vkEnumeratePhysicalDevices");
        std::uint32_t              queueFamily = 0;
        VkPhysicalDeviceProperties properties{};
        for (auto physical : physicalDevices)
        {
            r.GetPhysicalDeviceProperties(physical, &properties);
            VkPhysicalDeviceScalarBlockLayoutFeatures scalar{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES };
            VkPhysicalDeviceFeatures2                 features{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            features.pNext = &scalar;
            r.GetPhysicalDeviceFeatures2(physical, &features);
            if (properties.apiVersion < VK_API_VERSION_1_2 || !scalar.scalarBlockLayout || !properties.limits.standardSampleLocations || !(properties.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_4_BIT) || (recipe.depth.enabled && !(properties.limits.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_4_BIT)) || (std::any_of(recipes.begin(), recipes.end(), [](const auto& draw)
                                                                                                                                                                                                                                                                                                                                          {
                                                                                                                                                                                                                                                                                                                                              return draw.textures[0].format != NativeDrawReplayTextureFormat::Rgba8;
                                                                                                                                                                                                                                                                                                                                          }) &&
                                                                                                                                                                                                                                                                                                                              !features.features.textureCompressionBC))
            {
                continue;
            }
            std::uint32_t queueCount = 0;
            r.GetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queueCount);
            r.GetPhysicalDeviceQueueFamilyProperties(physical, &queueCount, queues.data());
            for (std::uint32_t index = 0; index < queueCount; ++index)
            {
                if (queues[index].queueCount && (queues[index].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                {
                    r.physical  = physical;
                    queueFamily = index;
                    break;
                }
            }
            if (r.physical)
            {
                break;
            }
        }
        if (!r.physical)
        {
            throw std::runtime_error("no Vulkan 1.2 adapter with scalarBlockLayout, standard MSAA4 locations, and graphics/compute queue");
        }
        std::ostringstream report;
        report << properties.deviceName << "; vendor=" << properties.vendorID << "; device=" << properties.deviceID
               << "; driver=" << properties.driverVersion << "; api=" << VK_VERSION_MAJOR(properties.apiVersion) << '.' << VK_VERSION_MINOR(properties.apiVersion)
               << "; scalarBlockLayout=1; standardSampleLocations=1; samples=4; sampleMask=9; queueFamily=" << queueFamily
               << "; textureFormat=" << textureFormatReport(recipes)
               << "; depth=" << (recipe.depth.enabled ? "D24S8" : "disabled") << "; apiValidation=disabled";
        output.adapterReport = report.str();
        if (std::any_of(recipes.begin(), recipes.end(), [&](const auto& draw)
                        {
                            return draw.width > properties.limits.maxFramebufferWidth || draw.height > properties.limits.maxFramebufferHeight || draw.textures[0].width > properties.limits.maxImageDimension2D || draw.textures[0].height > properties.limits.maxImageDimension2D || draw.vertexStrideBytes > properties.limits.maxVertexInputBindingStride;
                        }) ||
            properties.limits.maxColorAttachments < 4 || properties.limits.maxBoundDescriptorSets < 5)
        {
            throw std::runtime_error("recipe exceeds Vulkan device image, framebuffer, or vertex limits");
        }
        if (recipe.viewport.width > properties.limits.maxViewportDimensions[0] || recipe.viewport.height > properties.limits.maxViewportDimensions[1] || recipe.viewport.x < properties.limits.viewportBoundsRange[0] || recipe.viewport.y < properties.limits.viewportBoundsRange[0] || recipe.viewport.x + recipe.viewport.width > properties.limits.viewportBoundsRange[1] || recipe.viewport.y + recipe.viewport.height > properties.limits.viewportBoundsRange[1])
        {
            throw std::runtime_error("recipe viewport exceeds Vulkan device limits");
        }
        VkFormatProperties formatProperties{};
        r.GetPhysicalDeviceFormatProperties(r.physical, VK_FORMAT_R8G8B8A8_UNORM, &formatProperties);
        VkFormatFeatureFlags requiredFormats = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (recipe.blend.enabled)
        {
            requiredFormats |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        }
        const auto& samplerState = recipe.samplers.empty() ? recipe.sampler : recipe.samplers[0];
        if (samplerState.minLinear || samplerState.magLinear)
        {
            requiredFormats |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
        if ((formatProperties.optimalTilingFeatures & requiredFormats) != requiredFormats)
        {
            throw std::runtime_error("RGBA8 optimal image format lacks required Vulkan features");
        }
        VkImageFormatProperties imageProperties{};
        check(r.GetPhysicalDeviceImageFormatProperties(r.physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &imageProperties), "vkGetPhysicalDeviceImageFormatProperties MSAA target");
        if (!(imageProperties.sampleCounts & VK_SAMPLE_COUNT_4_BIT) || recipe.width > imageProperties.maxExtent.width || recipe.height > imageProperties.maxExtent.height)
        {
            throw std::runtime_error("RGBA8 sampled render target does not support recipe MSAA4 extent");
        }
        for (const auto& draw : recipes)
        {
            const auto drawTextureFormat = textureFormat(draw.textures[0].format);
            r.GetPhysicalDeviceFormatProperties(r.physical, drawTextureFormat, &formatProperties);
            VkFormatFeatureFlags requiredTexture  = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            const auto&          drawSamplerState = draw.samplers.empty() ? draw.sampler : draw.samplers[0];
            if (drawSamplerState.minLinear || drawSamplerState.magLinear)
            {
                requiredTexture |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            }
            if ((formatProperties.optimalTilingFeatures & requiredTexture) != requiredTexture)
            {
                throw std::runtime_error("texture format lacks required Vulkan sampling/upload features");
            }
            check(r.GetPhysicalDeviceImageFormatProperties(r.physical, drawTextureFormat, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &imageProperties), "vkGetPhysicalDeviceImageFormatProperties texture");
            if (draw.textures[0].width > imageProperties.maxExtent.width || draw.textures[0].height > imageProperties.maxExtent.height)
            {
                throw std::runtime_error("texture format does not support recipe extent");
            }
        }
        if (recipe.depth.enabled)
        {
            r.GetPhysicalDeviceFormatProperties(r.physical, VK_FORMAT_D24_UNORM_S8_UINT, &formatProperties);
            if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            {
                throw std::runtime_error("D24S8 depth attachment format is unsupported");
            }
            check(r.GetPhysicalDeviceImageFormatProperties(r.physical, VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &imageProperties), "vkGetPhysicalDeviceImageFormatProperties depth");
            if (!(imageProperties.sampleCounts & VK_SAMPLE_COUNT_4_BIT) || recipe.width > imageProperties.maxExtent.width || recipe.height > imageProperties.maxExtent.height)
            {
                throw std::runtime_error("D24S8 depth attachment does not support recipe MSAA4 extent");
            }
        }
        if (std::abs(samplerState.mipBias) > properties.limits.maxSamplerLodBias)
        {
            throw std::runtime_error("sampler mip bias exceeds Vulkan device limit");
        }
        float                   priority = 1;
        VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount       = 1;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceScalarBlockLayoutFeatures scalar{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES };
        scalar.scalarBlockLayout = VK_TRUE;
        VkDeviceCreateInfo       deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.textureCompressionBC = std::any_of(recipes.begin(), recipes.end(), [](const auto& draw)
                                                           {
                                                               return draw.textures[0].format != NativeDrawReplayTextureFormat::Rgba8;
                                                           });
        deviceInfo.pEnabledFeatures          = &enabledFeatures;
        deviceInfo.pNext                     = &scalar;
        deviceInfo.queueCreateInfoCount      = 1;
        deviceInfo.pQueueCreateInfos         = &queueInfo;
        check(r.CreateDevice(r.physical, &deviceInfo, nullptr, &r.device), "vkCreateDevice");
#define LOAD_DEVICE(name)                                                               \
    r.name = reinterpret_cast<PFN_vk##name>(r.GetDeviceProcAddr(r.device, "vk" #name)); \
    if (!r.name)                                                                        \
        throw std::runtime_error("Vulkan device lacks vk" #name);
        DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
        r.GetDeviceQueue(r.device, queueFamily, 0, &r.queue);
        r.GetPhysicalDeviceMemoryProperties(r.physical, &r.memoryProperties);
        r.buffers.reserve(1 + recipes.size() * 6);
        r.images.reserve(4 + recipes.size() + (recipe.depth.enabled ? 1 : 0));
        r.modules.reserve(3);
        r.setLayouts.reserve(6);
        r.pipelineLayouts.reserve(2);
        r.pipelines.reserve(recipes.size() + 1);
        std::vector<Buffer> vertices, indices, vertexConstants, pixelConstants, sharedConstants, textureUploads;
        std::vector<Image>  textures;
        vertices.reserve(recipes.size());
        indices.reserve(recipes.size());
        vertexConstants.reserve(recipes.size());
        pixelConstants.reserve(recipes.size());
        sharedConstants.reserve(recipes.size());
        textureUploads.reserve(recipes.size());
        textures.reserve(recipes.size());
        for (const auto& draw : recipes)
        {
            vertices.push_back(r.makeBuffer(draw.vertexBytes().size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, draw.vertexBytes().data()));
            indices.push_back(r.makeBuffer(draw.indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, draw.indices.data()));
            vertexConstants.push_back(r.makeBuffer(draw.vertexConstants.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, draw.vertexConstants.data()));
            pixelConstants.push_back(r.makeBuffer(draw.pixelConstants.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, draw.pixelConstants.data()));
            sharedConstants.push_back(r.makeBuffer(draw.sharedConstants.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, draw.sharedConstants.data()));
            textureUploads.push_back(r.makeBuffer(draw.textures[0].payloadBytes().size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, draw.textures[0].payloadBytes().data()));
        }
        const auto planeBytes = static_cast<VkDeviceSize>(recipe.width) * recipe.height * 4;
        if (planeBytes * 2 > properties.limits.maxStorageBufferRange)
        {
            throw std::runtime_error("sample output exceeds Vulkan storage-buffer range");
        }
        const auto           samples = r.makeBuffer(planeBytes * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        constexpr std::array componentMap{ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE };
        for (const auto& draw : recipes)
        {
            const auto& swizzle = draw.textures[0].swizzle;
            textures.push_back(r.makeImage(draw.textures[0].width, draw.textures[0].height, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, { componentMap[swizzle[0]], componentMap[swizzle[1]], componentMap[swizzle[2]], componentMap[swizzle[3]] }, textureFormat(draw.textures[0].format)));
        }
        std::array<Image, 4> targets;
        for (auto& target : targets)
        {
            target = r.makeImage(recipe.width, recipe.height, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, {});
        }
        Image depthTarget;
        if (recipe.depth.enabled)
        {
            depthTarget = r.makeImage(recipe.width, recipe.height, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, {}, VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        }
        VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.minFilter    = samplerState.minLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.magFilter    = samplerState.magLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.mipmapMode   = samplerState.mipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = addressMode(samplerState.address[0]);
        sampler.addressModeV = addressMode(samplerState.address[1]);
        sampler.addressModeW = addressMode(samplerState.address[2]);
        sampler.minLod       = samplerState.minLod;
        sampler.maxLod       = samplerState.maxLod;
        sampler.mipLodBias   = samplerState.mipBias;
        sampler.borderColor  = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        if (std::find(samplerState.address.begin(), samplerState.address.end(), 4) != samplerState.address.end())
        {
            if (samplerState.border == std::array<float, 4>{ 0, 0, 0, 1 })
            {
                sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            }
            else if (samplerState.border == std::array<float, 4>{ 1, 1, 1, 1 })
            {
                sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            }
            else if (samplerState.border != std::array<float, 4>{})
            {
                throw std::runtime_error("unsupported Vulkan custom sampler border color");
            }
        }
        check(r.CreateSampler(r.device, &sampler, nullptr, &r.sampler), "vkCreateSampler");
        std::array<VkDescriptorSetLayout, 5> graphicsLayouts{};
        for (std::size_t set = 0; set < graphicsLayouts.size(); ++set)
        {
            std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
            std::uint32_t                               count = 0;
            if (set == 0)
            {
                bindings[count++] = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            }
            if (set == 3)
            {
                bindings[count++] = { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            }
            if (set == 4)
            {
                bindings[count++] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr };
                bindings[count++] = { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
                bindings[count++] = { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
            }
            VkDescriptorSetLayoutCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            info.bindingCount = count;
            info.pBindings    = bindings.data();
            check(r.CreateDescriptorSetLayout(r.device, &info, nullptr, &graphicsLayouts[set]), "vkCreateDescriptorSetLayout graphics");
            r.setLayouts.push_back(graphicsLayouts[set]);
        }
        std::array<VkDescriptorSetLayoutBinding, 2> sampleBindings{ { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }, { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } } };
        VkDescriptorSetLayoutCreateInfo             sampleLayoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        sampleLayoutInfo.bindingCount = static_cast<std::uint32_t>(sampleBindings.size());
        sampleLayoutInfo.pBindings    = sampleBindings.data();
        VkDescriptorSetLayout sampleLayout;
        check(r.CreateDescriptorSetLayout(r.device, &sampleLayoutInfo, nullptr, &sampleLayout), "vkCreateDescriptorSetLayout samples");
        r.setLayouts.push_back(sampleLayout);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipelineLayoutInfo.setLayoutCount = static_cast<std::uint32_t>(graphicsLayouts.size());
        pipelineLayoutInfo.pSetLayouts    = graphicsLayouts.data();
        VkPipelineLayout graphicsPipelineLayout;
        check(r.CreatePipelineLayout(r.device, &pipelineLayoutInfo, nullptr, &graphicsPipelineLayout), "vkCreatePipelineLayout graphics");
        r.pipelineLayouts.push_back(graphicsPipelineLayout);
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts    = &sampleLayout;
        VkPipelineLayout samplePipelineLayout;
        check(r.CreatePipelineLayout(r.device, &pipelineLayoutInfo, nullptr, &samplePipelineLayout), "vkCreatePipelineLayout samples");
        r.pipelineLayouts.push_back(samplePipelineLayout);
        std::array<VkDescriptorPoolSize, 4> poolSizes{ { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, static_cast<std::uint32_t>(recipes.size() + 1) }, { VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<std::uint32_t>(recipes.size()) }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<std::uint32_t>(recipes.size() * 3) }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } } };
        VkDescriptorPoolCreateInfo          pool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool.maxSets       = static_cast<std::uint32_t>(recipes.size() * graphicsLayouts.size() + 1);
        pool.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
        pool.pPoolSizes    = poolSizes.data();
        check(r.CreateDescriptorPool(r.device, &pool, nullptr, &r.descriptors), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo setsInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        setsInfo.descriptorPool     = r.descriptors;
        setsInfo.descriptorSetCount = static_cast<std::uint32_t>(graphicsLayouts.size());
        setsInfo.pSetLayouts        = graphicsLayouts.data();
        std::vector<std::array<VkDescriptorSet, 5>> graphicsSets(recipes.size());
        for (auto& sets : graphicsSets)
        {
            check(r.AllocateDescriptorSets(r.device, &setsInfo, sets.data()), "vkAllocateDescriptorSets graphics");
        }
        setsInfo.descriptorSetCount = 1;
        setsInfo.pSetLayouts        = &sampleLayout;
        VkDescriptorSet sampleSet;
        check(r.AllocateDescriptorSets(r.device, &setsInfo, &sampleSet), "vkAllocateDescriptorSets samples");
        for (std::size_t index = 0; index < recipes.size(); ++index)
        {
            std::array<VkDescriptorImageInfo, 2>  imageInfos{ { { VK_NULL_HANDLE, textures[index].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { r.sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED } } };
            std::array<VkDescriptorBufferInfo, 3> bufferInfos{ { { vertexConstants[index].handle, 0, vertexConstants[index].size }, { pixelConstants[index].handle, 0, pixelConstants[index].size }, { sharedConstants[index].handle, 0, sharedConstants[index].size } } };
            std::array<VkWriteDescriptorSet, 5>   writes{};
            const std::array<VkDescriptorSet, 5>  destinationSets{ graphicsSets[index][0], graphicsSets[index][3], graphicsSets[index][4], graphicsSets[index][4], graphicsSets[index][4] };
            const std::array<std::uint32_t, 5>    destinationBindings{ 0, 0, 0, 1, 2 };
            const std::array<VkDescriptorType, 5> descriptorTypes{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER };
            for (std::size_t writeIndex = 0; writeIndex < writes.size(); ++writeIndex)
            {
                writes[writeIndex].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[writeIndex].dstSet          = destinationSets[writeIndex];
                writes[writeIndex].dstBinding      = destinationBindings[writeIndex];
                writes[writeIndex].descriptorCount = 1;
                writes[writeIndex].descriptorType  = descriptorTypes[writeIndex];
            }
            writes[0].pImageInfo  = &imageInfos[0];
            writes[1].pImageInfo  = &imageInfos[1];
            writes[2].pBufferInfo = &bufferInfos[0];
            writes[3].pBufferInfo = &bufferInfos[1];
            writes[4].pBufferInfo = &bufferInfos[2];
            r.UpdateDescriptorSets(r.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        std::array<VkDescriptorImageInfo, 1>  sampleImageInfos{ { { VK_NULL_HANDLE, targets[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } } };
        std::array<VkDescriptorBufferInfo, 1> sampleBufferInfos{ { { samples.handle, 0, samples.size } } };
        std::array<VkWriteDescriptorSet, 2>   sampleWrites{};
        sampleWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sampleSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampleImageInfos.data(), nullptr, nullptr };
        sampleWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sampleSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, sampleBufferInfos.data(), nullptr };
        r.UpdateDescriptorSets(r.device, static_cast<std::uint32_t>(sampleWrites.size()), sampleWrites.data(), 0, nullptr);
        std::array<VkAttachmentDescription, 5> attachments{};
        std::array<VkAttachmentReference, 4>   attachmentReferences{};
        std::array<VkImageView, 5>             attachmentViews{};
        for (std::uint32_t index = 0; index < 4; ++index)
        {
            attachments[index].format         = VK_FORMAT_R8G8B8A8_UNORM;
            attachments[index].samples        = VK_SAMPLE_COUNT_4_BIT;
            attachments[index].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[index].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[index].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[index].initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments[index].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachmentReferences[index]       = { index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            attachmentViews[index]            = targets[index].view;
        }
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 4;
        subpass.pColorAttachments    = attachmentReferences.data();
        VkAttachmentReference depthReference{ 4, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        if (recipe.depth.enabled)
        {
            attachments[4].format         = VK_FORMAT_D24_UNORM_S8_UINT;
            attachments[4].samples        = VK_SAMPLE_COUNT_4_BIT;
            attachments[4].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[4].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[4].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[4].initialLayout = attachments[4].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachmentViews[4]                                        = depthTarget.view;
            subpass.pDepthStencilAttachment                           = &depthReference;
        }
        VkRenderPassCreateInfo renderPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        renderPassInfo.attachmentCount = recipe.depth.enabled ? 5 : 4;
        renderPassInfo.pAttachments    = attachments.data();
        renderPassInfo.subpassCount    = 1;
        renderPassInfo.pSubpasses      = &subpass;
        check(r.CreateRenderPass(r.device, &renderPassInfo, nullptr, &r.renderPass), "vkCreateRenderPass");
        VkFramebufferCreateInfo framebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        framebuffer.renderPass      = r.renderPass;
        framebuffer.attachmentCount = renderPassInfo.attachmentCount;
        framebuffer.pAttachments    = attachmentViews.data();
        framebuffer.width           = recipe.width;
        framebuffer.height          = recipe.height;
        framebuffer.layers          = 1;
        check(r.CreateFramebuffer(r.device, &framebuffer, nullptr, &r.framebuffer), "vkCreateFramebuffer");
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage                   = VK_SHADER_STAGE_VERTEX_BIT;
        stages[1].stage                   = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[0].module                  = r.makeModule(vertexSpirv);
        stages[1].module                  = r.makeModule(pixelSpirv);
        stages[0].pName = stages[1].pName = "main";
        VkVertexInputBindingDescription                  vertexBinding{ 0, recipe.vertexStrideBytes, VK_VERTEX_INPUT_RATE_VERTEX };
        std::array<VkVertexInputAttributeDescription, 3> attributes{ { { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 }, { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 } } };
        VkPipelineVertexInputStateCreateInfo             vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount   = 1;
        vertexInput.pVertexBindingDescriptions      = &vertexBinding;
        vertexInput.vertexAttributeDescriptionCount = 3;
        vertexInput.pVertexAttributeDescriptions    = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport                        viewport{ recipe.viewport.x, recipe.viewport.y, recipe.viewport.width, recipe.viewport.height, recipe.viewport.minDepth, recipe.viewport.maxDepth };
        VkRect2D                          scissor{ { recipe.scissor.left, recipe.scissor.top }, { static_cast<std::uint32_t>(recipe.scissor.right - recipe.scissor.left), static_cast<std::uint32_t>(recipe.scissor.bottom - recipe.scissor.top) } };
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = viewportState.scissorCount = 1;
        viewportState.pViewports                                 = &viewport;
        viewportState.pScissors                                  = &scissor;
        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.lineWidth   = 1;
        raster.cullMode    = recipe.rasterizer.cull == 0 ? VK_CULL_MODE_NONE : recipe.rasterizer.cull == 1 ? VK_CULL_MODE_FRONT_BIT
                                                                                                           : VK_CULL_MODE_BACK_BIT;
        // DXC's inverted clip Y and Vulkan's positive-height viewport retain
        // D3D's screen-space winding. Vulkan names it in framebuffer coordinates.
        raster.frontFace = recipe.rasterizer.frontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
        multisample.pSampleMask          = &recipe.sampleMask;
        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable  = recipe.depth.enabled;
        depth.depthWriteEnable = recipe.depth.enabled && recipe.depth.writeEnabled;
        depth.depthCompareOp   = static_cast<VkCompareOp>(recipe.depth.compare - 1);
        std::array<VkPipelineColorBlendAttachmentState, 4> blendAttachments{};
        for (auto& blend : blendAttachments)
        {
            blend.blendEnable         = recipe.blend.enabled;
            blend.colorWriteMask      = recipe.blend.writeMask;
            blend.srcColorBlendFactor = blendFactor(recipe.blend.sourceColor);
            blend.dstColorBlendFactor = blendFactor(recipe.blend.destinationColor);
            blend.colorBlendOp        = blendOp(recipe.blend.colorOp);
            blend.srcAlphaBlendFactor = blendFactor(recipe.blend.sourceAlpha);
            blend.dstAlphaBlendFactor = blendFactor(recipe.blend.destinationAlpha);
            blend.alphaBlendOp        = blendOp(recipe.blend.alphaOp);
        }
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 4;
        blend.pAttachments    = blendAttachments.data();
        VkGraphicsPipelineCreateInfo pipeline{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pipeline.stageCount          = 2;
        pipeline.pStages             = stages.data();
        pipeline.pVertexInputState   = &vertexInput;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState      = &viewportState;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState   = &multisample;
        pipeline.pDepthStencilState  = &depth;
        pipeline.pColorBlendState    = &blend;
        pipeline.layout              = graphicsPipelineLayout;
        pipeline.renderPass          = r.renderPass;
        VkPipeline graphicsPipeline;
        check(r.CreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &graphicsPipeline), "vkCreateGraphicsPipelines");
        r.pipelines.push_back(graphicsPipeline);
        VkComputePipelineCreateInfo compute{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        compute.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = r.makeModule(sampleSpirv);
        compute.stage.pName  = "main";
        compute.layout       = samplePipelineLayout;
        VkPipeline samplePipeline;
        check(r.CreateComputePipelines(r.device, VK_NULL_HANDLE, 1, &compute, nullptr, &samplePipeline), "vkCreateComputePipelines");
        r.pipelines.push_back(samplePipeline);
        VkCommandPoolCreateInfo commandPoolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        commandPoolInfo.queueFamilyIndex = queueFamily;
        check(r.CreateCommandPool(r.device, &commandPoolInfo, nullptr, &r.commandPool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo commandInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        commandInfo.commandPool        = r.commandPool;
        commandInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer command;
        check(r.AllocateCommandBuffers(r.device, &commandInfo, &command), "vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(r.BeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        for (std::size_t index = 0; index < recipes.size(); ++index)
        {
            imageBarrier(r, command, textures[index].handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy textureCopy{};
            textureCopy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            textureCopy.imageExtent      = { recipes[index].textures[0].width, recipes[index].textures[0].height, 1 };
            r.CmdCopyBufferToImage(command, textureUploads[index].handle, textures[index].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &textureCopy);
            imageBarrier(r, command, textures[index].handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        for (const auto& target : targets)
        {
            imageBarrier(r, command, target.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        }
        if (recipe.depth.enabled)
        {
            imageBarrier(r, command, depthTarget.handle, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        }
        std::array<VkClearValue, 5> clears{};
        for (auto& clear : clears)
        {
            for (std::size_t component = 0; component < 4; ++component)
            {
                clear.color.float32[component] = options.clearColor[component];
            }
        }
        clears[4].depthStencil = { options.clearDepth, options.clearStencil };
        VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        pass.renderPass        = r.renderPass;
        pass.framebuffer       = r.framebuffer;
        pass.renderArea.extent = { recipe.width, recipe.height };
        pass.clearValueCount   = renderPassInfo.attachmentCount;
        pass.pClearValues      = clears.data();
        r.CmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        const VkClearRect                resetRect{ { { 0, 0 }, { recipe.width, recipe.height } }, 0, 1 };
        std::array<VkClearAttachment, 5> resetAttachments{};
        for (std::uint32_t index = 0; index < 4; ++index)
        {
            resetAttachments[index].aspectMask                  = VK_IMAGE_ASPECT_COLOR_BIT;
            resetAttachments[index].colorAttachment             = index;
            resetAttachments[index].clearValue.color.float32[0] = options.clearColor[0];
            resetAttachments[index].clearValue.color.float32[1] = options.clearColor[1];
            resetAttachments[index].clearValue.color.float32[2] = options.clearColor[2];
            resetAttachments[index].clearValue.color.float32[3] = options.clearColor[3];
        }
        resetAttachments[4].aspectMask              = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        resetAttachments[4].clearValue.depthStencil = { options.clearDepth, options.clearStencil };
        for (std::size_t index = 0; index < recipes.size(); ++index)
        {
            if (index != 0 && options.resetBetweenDraws)
            {
                r.CmdClearAttachments(command, recipe.depth.enabled ? 5 : 4, resetAttachments.data(), 1, &resetRect);
            }
            r.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
            r.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipelineLayout, 0, 5, graphicsSets[index].data(), 0, nullptr);
            VkDeviceSize vertexOffset = 0;
            r.CmdBindVertexBuffers(command, 0, 1, &vertices[index].handle, &vertexOffset);
            r.CmdBindIndexBuffer(command, indices[index].handle, 0, VK_INDEX_TYPE_UINT32);
            r.CmdDrawIndexed(command, recipes[index].indexCount, 1, 0, 0, 0);
        }
        r.CmdEndRenderPass(command);
        imageBarrier(r, command, targets[0].handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        r.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, samplePipeline);
        r.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, samplePipelineLayout, 0, 1, &sampleSet, 0, nullptr);
        r.CmdDispatch(command, (recipe.width + 7) / 8, (recipe.height + 7) / 8, 1);
        VkBufferMemoryBarrier readback{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        readback.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
        readback.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
        readback.srcQueueFamilyIndex = readback.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        readback.buffer                                             = samples.handle;
        readback.size                                               = samples.size;
        r.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &readback, 0, nullptr);
        check(r.EndCommandBuffer(command), "vkEndCommandBuffer");
        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        check(r.CreateFence(r.device, &fenceInfo, nullptr, &r.fence), "vkCreateFence");
        VkSubmitInfo submission{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submission.commandBufferCount = 1;
        submission.pCommandBuffers    = &command;
        check(r.QueueSubmit(r.queue, 1, &submission, r.fence), "vkQueueSubmit");
        const auto completed = r.WaitForFences(r.device, 1, &r.fence, VK_TRUE, 30'000'000'000ULL);
        if (completed != VK_SUCCESS && completed != VK_ERROR_DEVICE_LOST)
        {
            // No submitted object is released until completion or device loss.
            // Keep the owner through process exit if the healthy device stalls.
            (void)owner.release();
            throw std::runtime_error("Vulkan fence did not complete: VkResult " + std::to_string(completed) + "; submitted resources retained until process exit");
        }
        check(completed, "vkWaitForFences");
        output.sample0.resize(static_cast<std::size_t>(planeBytes));
        output.sample3.resize(static_cast<std::size_t>(planeBytes));
        void* mapped = nullptr;
        check(r.MapMemory(r.device, samples.memory, 0, samples.size, 0, &mapped), "vkMapMemory samples");
        const auto* bytes = static_cast<const std::uint8_t*>(mapped);
        std::memcpy(output.sample0.data(), bytes, output.sample0.size());
        std::memcpy(output.sample3.data(), bytes + planeBytes, output.sample3.size());
        r.UnmapMemory(r.device, samples.memory);
        return true;
    }
    catch (const std::exception& exception)
    {
        output.sample0.clear();
        output.sample3.clear();
        error = exception.what();
        return false;
    }
}

bool ReplayVulkanOffscreen(const NativeDrawReplayRecipe&     recipe,
                           const std::vector<std::uint32_t>& vertexSpirv,
                           const std::vector<std::uint32_t>& pixelSpirv,
                           const std::vector<std::uint32_t>& sampleSpirv,
                           VulkanDrawOutput&                 output,
                           std::string&                      error)
{
    // The bounded pass path admits BC2 per draw; retain the legacy
    // single-draw format boundary and result for the existing CLI path.
    if (!recipe.textures.empty() && (recipe.textures[0].format == NativeDrawReplayTextureFormat::Bc2 || recipe.textures[0].format == NativeDrawReplayTextureFormat::Bc3))
    {
        output = {};
        error  = "Vulkan offscreen requires a CPU RGBA8 or BC1 single-mip 2D texture";
        return false;
    }
    VulkanDrawPassOptions options;
    for (std::size_t component = 0; component < options.clearColor.size(); ++component)
    {
        options.clearColor[component] = static_cast<float>(recipe.clearColor[component]) / 255.0F;
    }
    options.clearDepth   = recipe.depth.initialClear;
    options.clearStencil = 0;
    return ReplayVulkanOffscreenPass(std::span<const NativeDrawReplayRecipe>(&recipe, 1), vertexSpirv, pixelSpirv, sampleSpirv, options, output, error);
}

} // namespace rerevved::gpu
