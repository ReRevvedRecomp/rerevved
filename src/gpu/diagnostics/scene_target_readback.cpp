#include "scene_target_readback.h"

#include <fstream>
#include <rex/crypto/sha256.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace rerevved::gpu::diagnostics
{

SceneTargetReadback::SceneTargetReadback(std::string captureId, std::uint64_t byteBudget, std::uint32_t recordBudget)
: captureId(std::move(captureId))
, byteBudget(byteBudget)
, recordBudget(recordBudget)
{
}

bool SceneTargetReadback::Admit()
{
    if (stats.records >= recordBudget)
    {
        ++stats.overflow;
        return false;
    }
    ++stats.records;
    return true;
}

bool SceneTargetReadback::Capture(SceneTargetRecord record, std::span<const std::uint8_t> bytes)
{
    std::lock_guard lock(gate);
    if (!Admit())
        return false;
    const auto& r = record;
    std::string error;
    if ((r.kind != "seed" && r.kind != "reference") || !r.generation || !r.completion || r.resource.empty() ||
        r.aspect.empty() || r.format.empty() || r.sampleLayout.empty() || r.resolve.empty() || !r.samples || r.samples > 16 ||
        r.rectangle[2] <= r.rectangle[0] || r.rectangle[3] <= r.rectangle[1] || !r.rowBytes || !r.bytesPerSample || r.bytesPerSample > 16 ||
        std::uint64_t(r.rectangle[2] - r.rectangle[0]) * r.samples * r.bytesPerSample != r.rowBytes ||
        r.rows != r.rectangle[3] - r.rectangle[1] || std::uint64_t(r.rowBytes) * r.rows != bytes.size())
    {
        error = "invalid_target_receipt";
        ++stats.failures;
    }
    else if (bytes.size() > byteBudget - stats.bytes)
    {
        error = "target_byte_budget";
        ++stats.overflow;
    }
    if (!error.empty())
    {
        receipts.push_back({ std::move(record), {}, std::move(error) });
        return false;
    }
    stats.bytes += bytes.size();
    receipts.push_back({ std::move(record), { bytes.begin(), bytes.end() }, {} });
    return true;
}

void SceneTargetReadback::Gap(SceneTargetRecord record, std::string reason)
{
    std::lock_guard lock(gate);
    ++stats.gaps;
    if (Admit())
        receipts.push_back({ std::move(record), {}, std::move(reason) });
}

SceneTargetStats SceneTargetReadback::Stats() const
{
    std::lock_guard lock(gate);
    return stats;
}

void SceneTargetReadback::Flush(const std::filesystem::path& directory)
{
    std::lock_guard lock(gate);
    try
    {
        if (!std::filesystem::create_directory(directory / "targets"))
            throw std::runtime_error("target_directory_not_fresh");
        toml::array rows;
        std::size_t index = 0;
        for (const auto& receipt : receipts)
        {
            const auto& r = receipt.record;
            toml::table row{ { "kind", r.kind }, { "resource", r.resource }, { "aspect", r.aspect }, { "format", r.format }, { "sample_layout", r.sampleLayout }, { "resolve", r.resolve }, { "frame", static_cast<std::int64_t>(r.frame) }, { "generation", static_cast<std::int64_t>(r.generation) }, { "completion", static_cast<std::int64_t>(r.completion) }, { "rectangle", toml::array{ r.rectangle[0], r.rectangle[1], r.rectangle[2], r.rectangle[3] } }, { "bytes_per_sample", r.bytesPerSample }, { "row_bytes", r.rowBytes }, { "rows", r.rows }, { "samples", r.samples }, { "bytes", static_cast<std::int64_t>(receipt.bytes.size()) }, { "error", receipt.error } };
            if (receipt.error.empty())
            {
                const auto    name = "targets/" + std::to_string(index) + ".bin";
                const auto    hash = rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(receipt.bytes.data()), receipt.bytes.size()));
                std::ofstream blob(directory / name, std::ios::binary | std::ios::noreplace);
                blob.write(reinterpret_cast<const char*>(receipt.bytes.data()), receipt.bytes.size());
                blob.close();
                if (!blob)
                    throw std::runtime_error("target_blob_write");
                row.insert("file", name);
                row.insert("sha256", hash);
            }
            rows.push_back(std::move(row));
            ++index;
        }
        std::ofstream f(directory / "target-manifest.toml", std::ios::binary | std::ios::noreplace);
        f << toml::table{ { "schema", 1 }, { "capture_id", captureId }, { "byte_budget", static_cast<std::int64_t>(byteBudget) }, { "record_budget", recordBudget }, { "records", std::move(rows) } };
        f.close();
        if (!f)
            throw std::runtime_error("target_manifest_write");
    }
    catch (...)
    {
        ++stats.failures;
        throw;
    }
}

} // namespace rerevved::gpu::diagnostics
