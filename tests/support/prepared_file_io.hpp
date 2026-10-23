#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace rerevved::tests
{

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path);
void                      writeBytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

} // namespace rerevved::tests
