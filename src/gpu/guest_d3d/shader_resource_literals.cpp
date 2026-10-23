#include "shader_resource_literals.h"

#include <exception>
#include <stdexcept>

namespace rerevved::gpu::guest_d3d
{
namespace
{

// Stage header offsets inside a shader object, and the object field holding
// the resource base the record offsets are relative to.
constexpr std::uint32_t kPixelHeader = 40, kPixelResource = 24;
constexpr std::uint32_t kVertexHeader = 872, kVertexResource = 32;

void refuse(const char* code)
{
    throw std::runtime_error(code);
}

std::uint32_t word(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

std::uint32_t half(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 8) | bytes[offset + 1];
}

} // namespace

bool ReadShaderResourceLiterals(const ReadMemory& read, std::uint32_t object, bool pixel, std::vector<ShaderLiteralRange>& ranges, std::vector<std::pair<std::uint32_t, std::uint32_t>>& registers, std::string& error)
{
    ranges.clear();
    registers.clear();
    error.clear();
    try
    {
        const auto fetch = [&](std::uint64_t address, std::uint64_t size, bool physical)
        {
            if (address > UINT32_MAX || size > UINT32_MAX)
                refuse("shader_literal_address");
            std::vector<std::uint8_t> bytes;
            try
            {
                bytes = read(address, size, physical);
            }
            catch (const std::exception&)
            {
                refuse("shader_literal_unreadable");
            }
            if (bytes.size() != size)
                refuse("shader_literal_unreadable");
            return bytes;
        };

        const auto header = std::uint64_t(object) + (pixel ? kPixelHeader : kVertexHeader);
        const auto offset = word(fetch(header + 20, 4, false), 0);
        if (!offset)
            return true;
        const auto table = header + offset;
        const auto size  = word(fetch(table + 16, 4, false), 0);
        if (size < 8 || size > kShaderLiteralMaxTableBytes)
            refuse("shader_literal_extent");
        const auto records = fetch(table + 20, size, false);

        std::vector<std::uint8_t> resource;
        // 0x826ADB98 runs while the cursor is below the table end and reads a
        // whole 8-byte record, so a record head or body past the end is refused.
        for (std::uint32_t i = 0; i < size; i += 8)
        {
            if (i + 4 > size)
                refuse("shader_literal_record_extent");
            const auto first = half(records, i), count = half(records, i + 2);
            if (!count)
                break;
            if (i + 8 > size)
                refuse("shader_literal_record_extent");
            if (resource.empty())
                resource = fetch(std::uint64_t(object) + (pixel ? kPixelResource : kVertexResource), 4, false);
            const auto raw = std::uint64_t(word(resource, 0)) + word(records, i + 4);
            if (raw > UINT32_MAX)
                refuse("shader_literal_address");
            const auto begin = kShaderLiteralRegisterBase + first * 4;
            if (std::uint64_t(begin) + count > kShaderLiteralRegisterEnd)
                refuse("shader_literal_range");
            // The stage's file is c0..c255 of its half; a pixel record must start
            // in the pixel half and a vertex record must end in the vertex half.
            if (pixel ? first < kShaderLiteralPixelFirst : first * 4 + count > kShaderLiteralPixelFirst * 4)
                refuse("shader_literal_stage");
            // The loader's physical alias: the low 29 bits, plus 0x1000 for sums
            // at or above 0xE0000000.
            const auto physical = static_cast<std::uint32_t>((raw & 0x1FFFFFFFULL) + (((raw >> 20) + 512) & 0x1000));
            const auto values   = fetch(physical, std::uint64_t(count) * 4, true);
            ranges.push_back({ static_cast<std::uint16_t>(first), static_cast<std::uint16_t>(count), physical });
            for (std::uint32_t k = 0; k < count; ++k)
                registers.emplace_back(begin + k, word(values, k * 4));
        }
        return true;
    }
    catch (const std::exception& failure)
    {
        ranges.clear();
        registers.clear();
        error = failure.what();
        return false;
    }
}

} // namespace rerevved::gpu::guest_d3d
