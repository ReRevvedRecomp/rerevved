#include "native_draw_shader_words.h"

#include "indexed_buffer_draw_inputs.h"
#include "native_frame_assembler.h"

#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

namespace
{

std::uint32_t word(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != 4)
        throw std::runtime_error("shader_words_extent");
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

} // namespace

bool HostPatchedVertexDigest(std::uint32_t device, std::uint32_t stride, const ReadMemory& read, std::string& digest, std::string& error)
{
    digest.clear();
    IndexedBufferDrawInputs inputs;
    bool                    patches = false;
    if (!ReadHostPatchInputs(device, stride, read, inputs, patches, error))
        return false;
    if (!patches)
    {
        digest = IndexedBufferMicrocodeWordDigest(inputs.vertexCode);
        return true;
    }
    try
    {
        std::vector<std::uint32_t> offsets;
        for (std::size_t element = 0; element + 12 <= inputs.declaration.size(); element += 12)
            offsets.push_back(word(std::span(inputs.declaration).subspan(element, 4)) & 0xFFFF);
        if (std::ranges::find(offsets, 0U) == offsets.end())
            offsets.push_back(0);
        // Linkage applies with a pixel shader; the vertex-only variant one has no patch model.
        const auto identity = MemoizedVertexShaderIdentity(inputs, offsets, bool(inputs.pixelObject), stride);
        if (!identity->ok)
        {
            error = identity->error;
            return false;
        }
        digest = identity->digest;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool ReadNativeDrawShaderWords(const NativeDrawSubmission& submission, const ReadMemory& read, NativeDrawShaderWords& words, std::string& error)
{
    words = {};
    error.clear();
    try
    {
        const auto objects = read(std::uint64_t(submission.Graphics()) + 0x3194, 8, false);
        CountVertexShaderIdentityBytes(objects.size());
        if (objects.size() != 8)
            throw std::runtime_error("shader_objects_unreadable");
        const auto pixel = word(std::span(objects).first(4)), vertex = word(std::span(objects).subspan(4));
        if (!vertex || !pixel)
            throw std::runtime_error("shader_object_unset");
        // Stream 0's stride in bytes, where each hook's lowerer finds it. The VS
        // is digested as this draw's fetch patch leaves it: the words in guest
        // memory follow the guest's last patch, which a consumed draw skips.
        const auto strideWord = [&](std::uint64_t address)
        {
            const auto bytes = read(address, 4, false);
            CountVertexShaderIdentityBytes(bytes.size());
            if (bytes.size() != 4)
                throw std::runtime_error("shader_stride_unreadable");
            return word(bytes);
        };
        const auto strideBytes = [&]() -> std::uint32_t
        {
            if (submission.hook == NativeDrawHook::IndexedUp)
                return strideWord(std::uint64_t(submission.stack) + 84);
            if (submission.hook == NativeDrawHook::NonindexedUp)
                return submission.arguments[4];
            return (strideWord(std::uint64_t(submission.Graphics()) + 0x30F0) >> 24) * 4;
        };
        std::string patchError;
        if (!HostPatchedVertexDigest(submission.Graphics(), strideBytes(), read, words.vertex, patchError))
            throw std::runtime_error(patchError);
        words.pixel = PixelShaderWordDigest(pixel, read);
        return true;
    }
    catch (const std::exception& exception)
    {
        words = {};
        error = exception.what();
        return false;
    }
}

} // namespace rerevved::gpu::guest_d3d
