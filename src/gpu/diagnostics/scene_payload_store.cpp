#include "scene_payload_store.h"

#include <algorithm>
#include <fstream>
#include <rex/crypto/sha256.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace rerevved::gpu::diagnostics
{

std::uint64_t SceneAllocationTracker::Allocate(ScenePayloadRange range)
{
    if (stopped)
        return 0;
    if (!range.size || range.size > UINT64_MAX - range.address)
        return 0;
    // A missed free ends the entire old generation, including its nonoverlapping tail.
    auto first = live.lower_bound(range.address);
    if (first != live.begin())
        --first;
    for (auto it = first; it != live.end() && it->first < range.address + range.size;)
        if (ClipScenePayloadRange(range, it->second.range).size)
        {
            it = live.erase(it);
            ++overlaps;
        }
        else
            ++it;
    if (live.size() >= capacity)
    {
        Invalidate();
        return 0;
    }
    live.emplace(range.address, Allocation{ range, ++next });
    return next;
}

void SceneAllocationTracker::Invalidate()
{
    if (stopped)
        return;
    live.clear();
    stopped = true;
    ++overflow;
}

std::uint64_t SceneAllocationTracker::Free(std::uint64_t address)
{
    if (const auto it = live.find(address); it != live.end())
    {
        const auto generation = it->second.generation;
        live.erase(it);
        return generation;
    }
    return 0;
}

std::uint64_t SceneAllocationTracker::Find(ScenePayloadRange range) const
{
    if (!range.size)
        return 0;
    auto it = live.upper_bound(range.address);
    if (it != live.begin())
    {
        const auto& allocation = (--it)->second;
        if (ClipScenePayloadRange(range, allocation.range).size == range.size)
            return allocation.generation;
    }
    return 0;
}

std::vector<std::pair<ScenePayloadRange, std::uint64_t>> SceneAllocationTracker::Live() const
{
    std::vector<std::pair<ScenePayloadRange, std::uint64_t>> result;
    result.reserve(live.size());
    for (const auto& [address, allocation] : live)
        result.emplace_back(allocation.range, allocation.generation);
    return result;
}

ScenePayloadRange ClipScenePayloadRange(ScenePayloadRange r, ScenePayloadRange a) noexcept
{
    if (r.size > UINT64_MAX - r.address || a.size > UINT64_MAX - a.address)
        return {};
    const auto begin = std::max(r.address, a.address), end = std::min(r.address + r.size, a.address + a.size);
    return end > begin ? ScenePayloadRange{ begin, end - begin } : ScenePayloadRange{};
}

ScenePayloadStore::ScenePayloadStore(bool enabled, std::uint64_t budget, std::string captureId)
: enabled(enabled)
, budget(budget)
, captureId(std::move(captureId))
{
}

bool ScenePayloadStore::Active() const
{
    std::lock_guard lock(gate);
    return enabled && !stopped;
}

std::uint64_t ScenePayloadStore::Bytes() const
{
    std::lock_guard lock(gate);
    return bytes;
}

std::uint64_t ScenePayloadStore::Overflow() const
{
    std::lock_guard lock(gate);
    return overflow;
}

std::uint64_t ScenePayloadStore::Failures() const
{
    std::lock_guard lock(gate);
    return failures;
}

void ScenePayloadStore::Queue(ScenePayloadBinding binding)
{
    // Bound metadata independently; the terminal count makes dropped bindings visible.
    if (++bindings > 1000000)
    {
        ++overflow;
        stopped = true;
        return;
    }
    if (!binding.sha256.empty())
        blobs.at(binding.sha256).published = true;
    pending.push_back(std::move(binding));
}

ScenePayloadBinding ScenePayloadStore::Capture(ScenePayloadBinding b, const Reader& reader, bool publish)
{
    std::lock_guard lock(gate);
    if (!enabled)
        return b;
    if (stopped)
        b.error = "payload_retention_stopped";
    else
    {
        try
        {
            const auto input = reader();
            if (input.size() != b.size)
                throw std::runtime_error("payload_read_size");
            const auto hash = rex::crypto::sha256(input.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(input.data()), input.size()));
            if (!blobs.contains(hash))
            {
                if (input.size() > budget - bytes || blobs.size() >= 262144)
                {
                    stopped = true;
                    ++overflow;
                    b.error = input.size() > budget - bytes ? "payload_byte_budget" : "payload_blob_capacity";
                }
                else
                {
                    blobs.emplace(hash, Blob{ { input.begin(), input.end() } });
                    bytes += input.size();
                }
            }
            if (b.error.empty())
                b.sha256 = hash;
        }
        catch (const std::exception& e)
        {
            b.error = e.what();
            ++failures;
        }
    }
    if (publish)
        Queue(b);
    return b;
}

void ScenePayloadStore::Publish(ScenePayloadBinding b)
{
    std::lock_guard lock(gate);
    if (enabled)
        Queue(std::move(b));
}

void ScenePayloadStore::Flush(const std::filesystem::path& directory)
{
    std::lock_guard  flushLock(flushGate);
    std::unique_lock lock(gate);
    if (!enabled)
        return;
    if (manifestStarted && pending.empty())
        return;
    const bool                       start = !manifestStarted;
    std::vector<ScenePayloadBinding> rows;
    rows.swap(pending);
    // Blob nodes and their immutable byte vectors survive map rehashes.
    std::vector<std::pair<std::string, Blob*>> output;
    for (auto& [hash, blob] : blobs)
        if (blob.published && !blob.written)
            output.emplace_back(hash, &blob);
    lock.unlock();
    if (start)
    {
        if (!std::filesystem::create_directory(directory / "payloads"))
            throw std::runtime_error("payload_directory_not_fresh");
        std::ofstream f(directory / "payload-manifest.toml", std::ios::binary | std::ios::noreplace);
        f << toml::table{ { "schema", 1 }, { "capture_id", captureId }, { "byte_budget", static_cast<std::int64_t>(budget) }, { "hash", "SHA-256" }, { "byte_order", "unmodified guest bytes; state delta words are BE32" }, { "pre_arm_policy", "only creation and updates of live k_8 1024x1024 and k_32_FLOAT 512x512 candidates; published on first sampled use; charged to byte budget" } } << '\n';
        f.close();
        if (!f)
            throw std::runtime_error("payload_manifest_create");
        std::lock_guard update(gate);
        manifestStarted = true;
    }
    for (const auto& [hash, blob] : output)
    {
        std::ofstream f(directory / "payloads" / (hash + ".bin"), std::ios::binary | std::ios::noreplace);
        f.write(reinterpret_cast<const char*>(blob->bytes.data()), blob->bytes.size());
        f.close();
        if (!f)
            throw std::runtime_error("payload_blob_write");
        std::lock_guard update(gate);
        blob->written = true;
    }
    std::ofstream f(directory / "payload-manifest.toml", std::ios::binary | std::ios::app);
    for (const auto& b : rows)
    {
        toml::table row{ { "capture_id", captureId }, { "frame", static_cast<std::int64_t>(b.frame) }, { "event", static_cast<std::int64_t>(b.event) }, { "submission", static_cast<std::int64_t>(b.submission) }, { "parent_call", static_cast<std::int64_t>(b.parent) }, { "resource", b.resource }, { "generation", static_cast<std::int64_t>(b.generation) }, { "version", static_cast<std::int64_t>(b.version) }, { "role", b.role }, { "address", static_cast<std::int64_t>(b.address) }, { "bytes", static_cast<std::int64_t>(b.size) }, { "physical", b.physical }, { "sha256", b.sha256 }, { "error", b.error } };
        if (!b.sha256.empty())
            row.insert("file", "payloads/" + b.sha256 + ".bin");
        row.insert("allocation_generation", static_cast<std::int64_t>(b.allocationGeneration));
        toml::array rows;
        rows.push_back(std::move(row));
        f << '\n'
          << toml::table{ { "bindings", std::move(rows) } } << '\n';
    }
    f.close();
    if (!f)
        throw std::runtime_error("payload_manifest_write");
}

} // namespace rerevved::gpu::diagnostics
