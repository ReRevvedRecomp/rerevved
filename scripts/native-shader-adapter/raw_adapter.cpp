
#include "pch.h"
#include "shader_recompiler.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static std::vector<uint8_t> read(const char* path, size_t maximum)
{
    auto size = std::filesystem::file_size(path);
    if (size > maximum) throw std::runtime_error("raw adapter input exceeds byte bound");
    std::vector<uint8_t> bytes(size);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("raw adapter input could not be read");
    return bytes;
}
int main(int argc, char** argv)
{
    try
    {
        if (argc != 8 || std::string_view(argv[1]) != "--raw" ||
            (std::string_view(argv[2]) != "vs" && std::string_view(argv[2]) != "ps"))
            throw std::runtime_error("usage: adapter --raw vs|ps words.le registers.le output.hlsl common.h hash");
        auto bytes = read(argv[3], 65536);
        auto registers = read(argv[4], 0x5003 * 4);
        if (bytes.empty() || bytes.size() % 12 || registers.size() != 0x5003 * 4)
            throw std::runtime_error("raw adapter instruction or register extent is invalid");
        auto header = read(argv[6], 1024 * 1024);
        std::vector<uint32_t> words(bytes.size() / 4);
        for (size_t i = 0; i < words.size(); ++i)
            words[i] = bytes[i * 4] | uint32_t(bytes[i * 4 + 1]) << 8 |
                       uint32_t(bytes[i * 4 + 2]) << 16 | uint32_t(bytes[i * 4 + 3]) << 24;
        std::vector<uint32_t> registerWords(registers.size() / 4);
        for (size_t i = 0; i < registerWords.size(); ++i)
            registerWords[i] = registers[i * 4] | uint32_t(registers[i * 4 + 1]) << 8 |
                               uint32_t(registers[i * 4 + 2]) << 16 | uint32_t(registers[i * 4 + 3]) << 24;
        ShaderRecompiler recompiler;
        recompiler.recompileRaw(words, std::string_view(argv[2]) == "ps",
            std::string_view(reinterpret_cast<const char*>(header.data()), header.size()), &registerWords);
        if (std::filesystem::exists(argv[5])) throw std::runtime_error("raw adapter output must be fresh");
        std::ofstream output(argv[5], std::ios::binary);
        output << recompiler.out;
        output.close();
        if (!output) throw std::runtime_error("raw adapter output could not be written");
        std::cout << "raw " << argv[2] << " " << argv[7] << ": " << words.size() << " words\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
