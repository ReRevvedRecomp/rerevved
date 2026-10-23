#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace rerevved::gpu::diagnostics
{

struct SceneTargetRecord
{
    std::string kind, resource, aspect, format, sampleLayout, resolve;
    // The title joins the SDK guest physical range and cache identity to its
    // observed allocation history before assigning resource and generation.
    std::uint64_t                frame = 0, generation = 0, completion = 0;
    std::array<std::uint32_t, 4> rectangle{};
    std::uint32_t                rowBytes = 0, rows = 0, samples = 0, bytesPerSample = 0;
};

struct SceneTargetStats
{
    std::uint64_t bytes = 0, records = 0, failures = 0, gaps = 0, overflow = 0;
};

// Ready SDK bytes/interpretation are owned/lossless at ordered processor
// marker/swap, never call-time. Pending/failed/cancelled give none; cancel
// never blocks guest. Unjoined identities remain gaps.
class SceneTargetReadback
{
public:
    SceneTargetReadback(std::string captureId, std::uint64_t byteBudget, std::uint32_t recordBudget = 256);
    bool             Capture(SceneTargetRecord record, std::span<const std::uint8_t> bytes);
    void             Gap(SceneTargetRecord record, std::string reason);
    void             Flush(const std::filesystem::path& directory);
    SceneTargetStats Stats() const;

    std::uint64_t ByteBudget() const
    {
        return byteBudget;
    }

    std::uint32_t RecordBudget() const
    {
        return recordBudget;
    }

private:
    struct Receipt
    {
        SceneTargetRecord         record;
        std::vector<std::uint8_t> bytes;
        std::string               error;
    };

    bool                 Admit();
    mutable std::mutex   gate;
    std::string          captureId;
    std::uint64_t        byteBudget;
    std::uint32_t        recordBudget;
    SceneTargetStats     stats;
    std::vector<Receipt> receipts;
};

} // namespace rerevved::gpu::diagnostics
