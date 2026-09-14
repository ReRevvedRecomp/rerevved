#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace rerevved::gpu::diagnostics
{

// One diagnostic draw from the running Xenos consumer is rendered on an
// independent native device. The guest keeps its original presentation path.
class NativeMenuShadow
{
public:
    NativeMenuShadow();
    ~NativeMenuShadow();

    NativeMenuShadow(const NativeMenuShadow&)            = delete;
    NativeMenuShadow& operator=(const NativeMenuShadow&) = delete;

    bool Start(const std::filesystem::path& outputDirectory,
               const std::filesystem::path& shaderDirectory,
               std::string&                 error);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace rerevved::gpu::diagnostics
