#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace rerevved::gpu::diagnostics
{

struct ScenePayloadBinding
{
    std::uint64_t frame = 0, event = 0, submission = 0, parent = 0, generation = 0, version = 0;
    std::uint64_t address = 0, size = 0;
    std::string   resource, role, sha256, error;
    bool          physical             = false;
    std::uint64_t allocationGeneration = 0;
};

struct ScenePayloadRange
{
    std::uint64_t address = 0, size = 0;
};

// Only observed allocate/free pairs establish these identities. Descriptor
// epochs remain separate. Overlap implicitly ends the older live allocations.
class SceneAllocationTracker
{
public:
    explicit SceneAllocationTracker(std::size_t capacity = 65536)
    : capacity(capacity)
    {
    }

    std::uint64_t Allocate(ScenePayloadRange range);
    std::uint64_t Free(std::uint64_t address);
    std::uint64_t Find(ScenePayloadRange range) const;
    void          Invalidate();
    // The live allocations in address order, as (range, generation).
    std::vector<std::pair<ScenePayloadRange, std::uint64_t>> Live() const;

    std::uint64_t Overflow() const
    {
        return overflow.load(std::memory_order_relaxed);
    }

    std::uint64_t Overlaps() const
    {
        return overlaps.load(std::memory_order_relaxed);
    }

private:
    struct Allocation
    {
        ScenePayloadRange range;
        std::uint64_t     generation;
    };

    std::map<std::uint64_t, Allocation> live;
    std::size_t                         capacity;
    std::uint64_t                       next    = 0;
    bool                                stopped = false;
    std::atomic<std::uint64_t>          overflow{ 0 };
    std::atomic<std::uint64_t>          overlaps{ 0 };
};

// Returns the intersection without wrapping either input. An invalid range is empty.
ScenePayloadRange ClipScenePayloadRange(ScenePayloadRange request, ScenePayloadRange allocation) noexcept;

// Guest readers run synchronously. Only Flush performs filesystem work. The byte
// budget bounds all unique retained bytes, including unpublished pre-arm history.
class ScenePayloadStore
{
public:
    using Reader = std::function<std::span<const std::uint8_t>()>;
    ScenePayloadStore(bool enabled, std::uint64_t budget, std::string captureId);
    ScenePayloadBinding Capture(ScenePayloadBinding binding, const Reader& reader, bool publish = true);
    void                Publish(ScenePayloadBinding binding);
    bool                Active() const;
    std::uint64_t       Bytes() const;
    std::uint64_t       Overflow() const;
    std::uint64_t       Failures() const;
    void                Flush(const std::filesystem::path& directory);

    const std::string& CaptureId() const noexcept
    {
        return captureId;
    }

    std::uint64_t Budget() const noexcept
    {
        return budget;
    }

private:
    struct Blob
    {
        std::vector<std::uint8_t> bytes;
        bool                      published = false, written = false;
    };

    void                                  Queue(ScenePayloadBinding binding);
    mutable std::mutex                    gate;
    std::mutex                            flushGate;
    bool                                  enabled, stopped = false, manifestStarted = false;
    std::uint64_t                         budget, bytes = 0, overflow = 0, failures = 0, bindings = 0;
    std::string                           captureId;
    std::unordered_map<std::string, Blob> blobs;
    std::vector<ScenePayloadBinding>      pending;
};

} // namespace rerevved::gpu::diagnostics
