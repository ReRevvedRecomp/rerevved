#include "gpu/d3d12/native_texture_upload.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <toml++/toml.hpp>

using namespace rerevved::gpu;

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "native_texture_upload_test: " << message << '\n';
        std::exit(1);
    }
}

NativeTextureFetch fetch(std::uint32_t format, std::uint32_t endian, std::uint32_t width, std::uint32_t height, bool tiled = false)
{
    return { 2U | (1U << 22) | (tiled ? 0x80000000U : 0),
             0x00100000U | (endian << 6) | format,
             (width - 1) | ((height - 1) << 13),
             0xD10,
             0,
             0x200 };
}

void linearRgba()
{
    auto descriptor = fetch(6, 2, 3, 2);
    descriptor[3]   = 0xC14; // BGRA selection.
    NativeTextureMemoryRange range;
    NativeDrawReplayTexture  texture;
    std::string              error;
    require(GetNativeTextureMemoryRange(descriptor, range, error), error);
    require(range.address == 0x100000 && range.size == 140, "linear rows preserve guest pitch");
    std::vector<std::uint8_t>       input(range.size, 0xEE);
    const std::vector<std::uint8_t> expected = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24
    };
    for (std::size_t row = 0; row < 2; ++row)
        for (std::size_t byte = 0; byte < 12; ++byte)
            input[row * 128 + (byte ^ 3)] = expected[row * 12 + byte];
    require(DecodeNativeTexture(descriptor, input, texture, error), error);
    require(texture.bytes == expected, "RGBA rows decoded without pitch padding");
    require(texture.swizzle == std::array<std::uint8_t, 4>{ 2, 1, 0, 3 }, "guest BGRA swizzle applied once");
    input.pop_back();
    require(!DecodeNativeTexture(descriptor, input, texture, error) && texture.bytes.empty(),
            "truncated input cannot retain an earlier successful texture");
}

void tiledRgba()
{
    const auto               descriptor = fetch(6, 2, 32, 32, true);
    NativeTextureMemoryRange range;
    NativeDrawReplayTexture  texture;
    std::string              error;
    require(GetNativeTextureMemoryRange(descriptor, range, error), error);
    std::vector<std::uint8_t> input(range.size);
    // Independent locations cover microtile row order, bank selection, and the
    // last texel of a 32x32 RGBA tile in the Xenos address layout.
    const std::array<std::array<std::size_t, 3>, 4> samples = {
        { { 0, 0, 0 }, { 8, 0, 64 }, { 0, 1, 16 }, { 31, 31, 3964 } }
    };
    for (std::size_t i = 0; i < samples.size(); ++i)
        for (std::size_t channel = 0; channel < 4; ++channel)
            input[samples[i][2] + (channel ^ 3)] = static_cast<std::uint8_t>(4 * i + channel + 1);
    require(DecodeNativeTexture(descriptor, input, texture, error), error);
    for (std::size_t i = 0; i < samples.size(); ++i)
        for (std::size_t channel = 0; channel < 4; ++channel)
            require(texture.bytes[(samples[i][1] * 32 + samples[i][0]) * 4 + channel] == 4 * i + channel + 1,
                    "tiled RGBA coordinates and endian");
}

void compressedAndFont()
{
    std::string              error;
    NativeTextureMemoryRange range;
    NativeDrawReplayTexture  texture;
    for (const auto format : { 18U, 19U })
    {
        const auto descriptor = fetch(format, 1, 4, 4);
        require(GetNativeTextureMemoryRange(descriptor, range, error), error);
        require(range.size == (format == 18 ? 8U : 16U), "compressed block extent");
        std::vector<std::uint8_t> input(range.size);
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i ^ 1] = static_cast<std::uint8_t>(i);
        require(DecodeNativeTexture(descriptor, input, texture, error), error);
        require(texture.format == (format == 18 ? NativeDrawReplayTextureFormat::Bc1 : NativeDrawReplayTextureFormat::Bc2),
                "block compression remains compressed for native upload");
        for (std::size_t i = 0; i < input.size(); ++i)
            require(texture.bytes[i] == i, "compressed block byte order");
    }
    auto descriptor = fetch(2, 0, 3, 2);
    descriptor[3]   = (5U | (5U << 3) | (5U << 6)) << 1; // White RGB, sampled red alpha.
    require(GetNativeTextureMemoryRange(descriptor, range, error), error);
    require(range.size == 35, "font rows use byte pitch");
    std::vector<std::uint8_t> input(range.size, 0xEE);
    input[0]  = 1;
    input[1]  = 2;
    input[2]  = 3;
    input[32] = 4;
    input[33] = 5;
    input[34] = 6;
    require(DecodeNativeTexture(descriptor, input, texture, error), error);
    require(texture.bytes == std::vector<std::uint8_t>{ 1, 2, 3, 4, 5, 6 }, "font pitch padding removed");
    require(texture.swizzle == std::array<std::uint8_t, 4>{ 5, 5, 5, 0 }, "font alpha and constant RGB preserved");
}

void rejectedInputs()
{
    NativeTextureMemoryRange range;
    std::string              error;
    auto                     descriptor = fetch(7, 0, 32, 32);
    require(!GetNativeTextureMemoryRange(descriptor, range, error), "unsupported format rejected");
    descriptor = fetch(6, 2, 32, 32, true);
    descriptor[5] |= 0x800;
    require(!GetNativeTextureMemoryRange(descriptor, range, error), "packed mip tail rejected");
    descriptor    = fetch(6, 2, 1024, 1024, true);
    descriptor[0] = 2U | (32U << 22) | 0x80000000;
    descriptor[1] = 0x1FFFF086;
    require(!GetNativeTextureMemoryRange(descriptor, range, error) && range.size == 0,
            "physical memory wrap rejected before source access");
}

void capturedDescriptors(const std::filesystem::path& directory)
{
    const auto  manifest = toml::parse_file((directory / "manifest.toml").string());
    const auto& textures = *manifest["textures"].as_array();
    std::size_t checked  = 0;
    for (const auto& eventNode : *manifest["events"].as_array())
    {
        const auto& event = *eventNode.as_table();
        const auto* uses  = event["texture_uses"].as_array();
        if (!uses || uses->empty())
            continue;
        const auto path = directory / event["registers"]["file"].value<std::string>().value();
        require(std::filesystem::file_size(path) == 0x5003 * 4, "captured register extent");
        std::vector<std::uint8_t> registers(0x5003 * 4);
        std::ifstream             file(path, std::ios::binary);
        require(bool(file.read(reinterpret_cast<char*>(registers.data()), registers.size())), "captured registers readable");
        for (const auto& useNode : *uses)
        {
            const auto& use = *useNode.as_table();
            if (!use["available"].value_or(false) || use["signed"].value_or(false))
                continue;
            const auto ordinal = use["fetch"].value<std::uint32_t>().value();
            require(ordinal < 32, "texture fetch ordinal");
            NativeTextureFetch descriptor;
            for (std::size_t i = 0; i < descriptor.size(); ++i)
            {
                const auto offset = (0x4800 + ordinal * 6 + i) * 4;
                descriptor[i]     = registers[offset] | (std::uint32_t(registers[offset + 1]) << 8) |
                                    (std::uint32_t(registers[offset + 2]) << 16) | (std::uint32_t(registers[offset + 3]) << 24);
            }
            NativeTextureMemoryRange range;
            NativeDrawReplayTexture  actual;
            std::string              error;
            require(GetNativeTextureMemoryRange(descriptor, range, error), error);
            // This checks captured layout and binding metadata. Pixel fidelity
            // requires real guest source bytes from the matching draw boundary.
            const std::vector<std::uint8_t> source(range.size);
            require(DecodeNativeTexture(descriptor, source, actual, error), error);
            const auto& expected = *textures[use["texture"].value<std::size_t>().value()].as_table();
            require(range.address == (expected["guest_base_page"].value<std::uint32_t>().value() << 12), "captured physical texture base");
            require(actual.width == expected["width"].value<std::uint32_t>().value() &&
                        actual.height == expected["height"].value<std::uint32_t>().value() &&
                        actual.bytes.size() == expected["data"]["bytes"].value<std::size_t>().value(),
                    "captured host extent and tight payload size");
            auto mapping = use["component_mapping"].value<std::uint32_t>().value();
            for (std::size_t i = 0; i < 4; ++i)
                require(actual.swizzle[i] == ((mapping >> (3 * i)) & 7), "captured host component mapping");
            ++checked;
        }
    }
    require(checked > 0, "captured texture bindings present");
    std::cout << "native texture descriptors checked: " << checked << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    linearRgba();
    tiledRgba();
    compressedAndFont();
    rejectedInputs();
    if (argc == 2)
        capturedDescriptors(argv[1]);
    return 0;
}
