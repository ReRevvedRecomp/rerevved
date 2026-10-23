#pragma once
#include <filesystem>
#include <string>

bool ReplayGameplaySequence(const std::filesystem::path& recipe,
                            const std::filesystem::path& output,
                            std::string&                 error);
