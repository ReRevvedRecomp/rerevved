#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace rerevved::gpu::diagnostics
{

class NativeMenuFrameShadow
{
public:
    NativeMenuFrameShadow();
    ~NativeMenuFrameShadow();
    NativeMenuFrameShadow(const NativeMenuFrameShadow&)            = delete;
    NativeMenuFrameShadow& operator=(const NativeMenuFrameShadow&) = delete;

    bool Start(const std::filesystem::path& outputDirectory,
               const std::filesystem::path& shaderDirectory,
               std::string&                 error);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rerevved::gpu::diagnostics
