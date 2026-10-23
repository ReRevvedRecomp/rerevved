#pragma once

#include <filesystem>
#include <string>

enum class SolidAdapterDestination
{
    TransparentBlack,
    ReferenceBefore,
};

bool CompareSolidAdapterCapture(const std::filesystem::path& capture,
                                const std::filesystem::path& shaderDirectory,
                                const std::filesystem::path& output,
                                std::string&                 error,
                                const std::filesystem::path& referenceDirectory = {},
                                SolidAdapterDestination      destination        = SolidAdapterDestination::TransparentBlack);

bool IngestSolidAdapterReference(const std::filesystem::path& capture,
                                 const std::filesystem::path& output,
                                 std::string&                 error);
