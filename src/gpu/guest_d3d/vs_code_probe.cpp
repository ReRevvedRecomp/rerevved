#include "vs_code_probe.h"

#include "indexed_buffer_draw_inputs.h"

#include <algorithm>
#include <exception>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

namespace
{

// The big-endian word a four-byte read returned; zero for any other size.
std::uint32_t wordOf(const GuestBytes& bytes)
{
    if (bytes.size() != 4)
        return 0;
    return static_cast<std::uint32_t>(bytes[0]) << 24 | static_cast<std::uint32_t>(bytes[1]) << 16 | static_cast<std::uint32_t>(bytes[2]) << 8 | bytes[3];
}

} // namespace

VsCodeReading ReadVsCodeState(std::uint32_t device, std::uint32_t strideBytes, const ReadMemory& read)
{
    VsCodeReading reading;
    try
    {
        const auto objects = read(std::uint64_t(device) + 0x3198, 4, false);
        if (objects.size() != 4)
        {
            reading.error = "shader_object_unreadable";
            return reading;
        }
        const auto vertex = wordOf(objects);
        if (!vertex)
        {
            reading.error = "shader_object_unset";
            return reading;
        }
        IndexedBufferDrawInputs inputs;
        bool                    patches = false;
        if (!ReadHostPatchInputs(device, strideBytes, read, inputs, patches, reading.error, true))
            return reading;
        // readVertexStage selected the active variant for both sides. Backing
        // storage never receives the inline linkage applied to execution code.
        reading.readDigest = IndexedBufferMicrocodeWordDigest(inputs.vertexCode);
        reading.hostDigest = reading.readDigest;
        if (patches)
        {
            std::vector<std::uint32_t> offsets{ 0 };
            for (std::size_t element = 0; element + 12 <= inputs.declaration.size(); element += 12)
            {
                const auto* bytes  = inputs.declaration.data() + element;
                const auto  offset = std::uint32_t(bytes[2]) << 8 | bytes[3];
                if (std::ranges::find(offsets, offset) == offsets.end())
                    offsets.push_back(offset);
            }
            const auto identity = MemoizedVertexShaderIdentity(inputs, offsets, false, strideBytes);
            if (!identity->ok)
            {
                reading.error = identity->error;
                return reading;
            }
            reading.hostDigest = identity->digest;
        }
        reading.state = reading.readDigest == reading.hostDigest ? VsCodeState::Fresh : VsCodeState::Stale;
    }
    catch (const std::exception& exception)
    {
        reading.error = exception.what();
        reading.state = VsCodeState::Unreadable;
    }
    return reading;
}

VsKeyReading ReadVsKeyState(std::uint32_t device, std::uint32_t strideBytes, const ReadMemory& read)
{
    VsKeyReading reading;
    try
    {
        const auto stored  = read(std::uint64_t(device) + 0x30F0, 16, false);
        const auto cache   = read(std::uint64_t(device) + 0x2E38, 16, false);
        const auto pixel   = wordOf(read(std::uint64_t(device) + 0x3194, 4, false));
        const auto objects = read(std::uint64_t(device) + 0x3198, 4, false);
        if (stored.size() != 16 || cache.size() != 16 || objects.size() != 4)
        {
            reading.error = "key_unreadable";
            return reading;
        }
        const auto vertex = wordOf(objects);
        if (!vertex)
        {
            reading.error = "shader_object_unset";
            return reading;
        }
        // 0x826ADD88 takes variant one without a pixel shader when VS header word +872 has bit 0x20.
        const auto header  = read(std::uint64_t(vertex) + 872, 4, false);
        const auto variant = !pixel && header.size() == 4 && (wordOf(header) & 0x20) ? 1U : 0U;
        const auto record  = read(std::uint64_t(vertex) + 48 + 416ULL * variant, 16, false);
        if (header.size() != 4 || record.size() != 16)
        {
            reading.error = "key_unreadable";
            return reading;
        }
        std::vector<std::uint8_t> key(stored.begin(), stored.end());
        key[0]                = static_cast<std::uint8_t>(strideBytes >> 2);
        reading.cacheDiffers  = !std::equal(key.begin(), key.end(), cache.begin());
        reading.recordDiffers = !std::equal(key.begin(), key.end(), record.begin());
        reading.readable      = true;
    }
    catch (const std::exception& exception)
    {
        reading.error = exception.what();
    }
    return reading;
}

} // namespace rerevved::gpu::guest_d3d
