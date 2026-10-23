#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace rerevved::gpu
{

// Idle default-heap textures kept for reuse. The census-0013 world window
// created 109 textures per swap, 48.2 MB of uploads (peak window 59.6 MB);
// this holds about two swaps of that churn, as both frame contexts share it.
inline constexpr std::uint64_t kTexturePoolIdleCapBytes = 128ULL * 1024ULL * 1024ULL;
// census-0020: 160 MiB pool held 164.6/167.8 MB idle but evicted 4.8
// targets/swap, 6 steady. If 1280x720x4-byte targets are 3.74 MB, about 22 MB
// more retains them with 11 MB spare.
inline constexpr std::uint64_t kAttachmentPoolIdleCapBytes = 192ULL * 1024ULL * 1024ULL;
// World frame: up to 19 targets use 52.9 MiB (median 37.4 MiB), with 64 KiB
// alignment.
inline constexpr std::uint64_t kResolvePoolIdleCapBytes = 64ULL * 1024ULL * 1024ULL;

struct ResourcePoolStats
{
    std::uint64_t hits          = 0; // takes served by an idle resource
    std::uint64_t misses        = 0; // takes that found none
    std::uint64_t returns       = 0; // resources handed back, kept or not
    std::uint64_t evictions     = 0; // idle resources released over the cap
    std::uint64_t idleBytes     = 0;
    std::uint64_t pendingBytes  = 0;
    std::uint64_t peakHeldBytes = 0; // idle plus pending
    std::uint64_t evictedBytes  = 0; // bytes of those evictions
    std::uint64_t pendingMisses = 0; // misses while the key waited only on fences
};

// A pool's diagnostic report is due after this many misses since the last,
// at most kPoolReportLimit times per pool.
inline constexpr std::uint64_t kPoolReportMisses = 128;
inline constexpr std::uint64_t kPoolReportLimit  = 64;

// One-thread fence-retired pool of exact-key move-only resources. A resource
// is a move-only handle whose destruction releases it; the owner creates and
// returns them, while other threads may read each atomic stat individually.
template <class Key, class Resource>
class FencedResourcePool
{
    struct Entry
    {
        Key           key{};
        Resource      resource{};
        std::uint64_t bytes = 0;
    };

public:
    struct Taken
    {
        Resource      resource{};
        std::uint64_t bytes = 0;

        explicit operator bool() const noexcept
        {
            return bool(resource);
        }
    };

    explicit FencedResourcePool(std::uint64_t idleCapBytes)
    : idleCapBytes_(idleCapBytes)
    {
    }

    FencedResourcePool(const FencedResourcePool&)            = delete;
    FencedResourcePool& operator=(const FencedResourcePool&) = delete;

    // Reclaims retirements through completedFence, then hands out the newest
    // idle resource of key, or an empty one. A miss is pending when the key
    // has retirements whose fences have not completed, else absent.
    Taken Take(const Key& key, std::uint64_t completedFence) noexcept
    {
        Reclaim(completedFence);
        auto* const record       = recordOf(key);
        const auto [first, last] = byKey_.equal_range(key);
        if (first == last)
        {
            const bool pending = std::any_of(pending_.begin(), pending_.end(), [&](const auto& retired)
                                             {
                                                 return retired.second.key == key;
                                             });
            add(stats_.misses, 1);
            if (pending)
                add(stats_.pendingMisses, 1);
            if (record)
                ++(pending ? record->pendingMisses : record->absentMisses);
            ++missesSinceReport_;
            return {};
        }
        if (record)
            ++record->hits;
        const auto newest = std::prev(last);
        auto       idle   = idle_.find(newest->second);
        Taken      taken{ std::move(idle->second.resource), idle->second.bytes };
        idle_.erase(idle);
        byKey_.erase(newest);
        sub(stats_.idleBytes, taken.bytes);
        add(stats_.hits, 1);
        return taken;
    }

    // Hold through fenceValue(0=no GPU use); caller promises no later
    // recorded/submitted use. Idle returns always succeed, releasing
    // over-cap/OOM storage. Failed pending return keeps caller resource, false.
    bool Return(const Key& key, Resource&& resource, std::uint64_t bytes, std::uint64_t fenceValue) noexcept
    {
        if (!resource)
            return true;
        if (fenceValue == 0)
        {
            add(stats_.returns, 1);
            countReturn(key);
            Entry entry{ key, std::move(resource), bytes };
            if (bytes <= idleCapBytes_)
                keep(std::move(entry));
            return true;
        }
        if (bytes > idleCapBytes_)
            return false;
        try
        {
            // The node exists before the resource moves into it.
            const auto node       = pending_.emplace(fenceValue, Entry{ key, Resource{}, bytes });
            node->second.resource = std::move(resource);
        }
        catch (...)
        {
            return false;
        }
        add(stats_.returns, 1);
        countReturn(key);
        add(stats_.pendingBytes, bytes);
        notePeak();
        return true;
    }

    // Moves retirements through completedFence to the idle set, then releases
    // the oldest idle resources over the cap.
    void Reclaim(std::uint64_t completedFence) noexcept
    {
        while (!pending_.empty() && pending_.begin()->first <= completedFence)
        {
            auto entry = std::move(pending_.begin()->second);
            pending_.erase(pending_.begin());
            sub(stats_.pendingBytes, entry.bytes);
            keep(std::move(entry));
        }
    }

    // Releases every idle resource, for storage pressure or shutdown.
    void ReleaseIdle() noexcept
    {
        while (!idle_.empty())
            evictOldest(true);
    }

    // Releases every held resource, idle and pending, for a shutdown after
    // the queue drained.
    void ReleaseAll() noexcept
    {
        ReleaseIdle();
        pending_.clear();
        stats_.pendingBytes.store(0, std::memory_order_relaxed);
    }

    // Visits every held resource, idle then pending, then forgets them
    // without releasing, for a device loss that must not free queue-visible
    // storage. visit must not throw.
    template <class Visit>
    void DetachAll(Visit&& visit) noexcept
    {
        static_assert(noexcept(visit(std::declval<Resource&>())), "a detach visitor must not throw");
        for (auto& [serial, entry] : idle_)
            visit(entry.resource);
        for (auto& [fence, entry] : pending_)
            visit(entry.resource);
        idle_.clear();
        byKey_.clear();
        pending_.clear();
        stats_.idleBytes.store(0, std::memory_order_relaxed);
        stats_.pendingBytes.store(0, std::memory_order_relaxed);
    }

    // Visits every held resource and its size, idle then pending.
    template <class Visit>
    void ForEachHeld(Visit&& visit) const
    {
        for (const auto& [serial, entry] : idle_)
            visit(entry.resource, entry.bytes);
        for (const auto& [fence, entry] : pending_)
            visit(entry.resource, entry.bytes);
    }

    // Idle and pending resources.
    std::size_t HeldCount() const noexcept
    {
        return idle_.size() + pending_.size();
    }

    // Host bytes of the bookkeeping tree nodes, each charged its value and
    // four pointers.
    std::uint64_t BookkeepingBytes() const noexcept
    {
        constexpr std::uint64_t node = 4 * sizeof(void*);
        return idle_.size() * (sizeof(typename decltype(idle_)::value_type) + node) + byKey_.size() * (sizeof(typename decltype(byKey_)::value_type) + node) +
               pending_.size() * (sizeof(typename decltype(pending_)::value_type) + node);
    }

    ResourcePoolStats Stats() const noexcept
    {
        const auto load = [](const std::atomic<std::uint64_t>& value)
        {
            return value.load(std::memory_order_relaxed);
        };
        return { load(stats_.hits), load(stats_.misses), load(stats_.returns), load(stats_.evictions), load(stats_.idleBytes), load(stats_.pendingBytes), load(stats_.peakHeldBytes), load(stats_.evictedBytes), load(stats_.pendingMisses) };
    }

    std::uint64_t IdleCapBytes() const noexcept
    {
        return idleCapBytes_;
    }

    // True once per kPoolReportMisses misses, at most kPoolReportLimit times;
    // the owner then logs Report().
    bool ReportDue() noexcept
    {
        if (missesSinceReport_ < kPoolReportMisses || reports_ >= kPoolReportLimit)
            return false;
        missesSinceReport_ = 0;
        ++reports_;
        return true;
    }

    // Reports list totals and tracked keys by misses, evictions, and idle
    // resources; allocation failure returns empty.
    std::string Report(std::size_t maxKeys = 12) const noexcept
    {
        try
        {
            std::array<std::uint64_t, kTrackedKeys> idleCount{}, idleBytes{};
            for (const auto& [serial, entry] : idle_)
                for (std::size_t index = 0; index < trackedKeys_; ++index)
                    if (records_[index].key == entry.key)
                    {
                        ++idleCount[index];
                        idleBytes[index] += entry.bytes;
                        break;
                    }
            const auto score = [&](std::size_t index)
            {
                const auto& record = records_[index];
                return record.pendingMisses + record.absentMisses + record.evictions + idleCount[index];
            };
            const auto stats = Stats();
            const auto mb    = [](std::uint64_t bytes)
            {
                const auto tenths = (bytes * 10 + 524288) / 1048576;
                return std::to_string(tenths / 10) + '.' + std::to_string(tenths % 10);
            };
            std::string                    line = "cap_mib=" + std::to_string(idleCapBytes_ >> 20) + " hits=" + std::to_string(stats.hits) + " misses=" + std::to_string(stats.misses) +
                                                  " pending_misses=" + std::to_string(stats.pendingMisses) + " absent_misses=" + std::to_string(stats.misses - stats.pendingMisses) +
                                                  " returns=" + std::to_string(stats.returns) + " evictions=" + std::to_string(stats.evictions) + " idle_mb=" + mb(stats.idleBytes) +
                                                  " pending_mb=" + mb(stats.pendingBytes) + " keys=" + std::to_string(trackedKeys_) + " untracked_events=" + std::to_string(untrackedEvents_);
            std::array<bool, kTrackedKeys> listed{};
            for (std::size_t slot = 0; slot < maxKeys; ++slot)
            {
                std::size_t best = trackedKeys_;
                for (std::size_t index = 0; index < trackedKeys_; ++index)
                    if (!listed[index] && score(index) && (best == trackedKeys_ || score(index) > score(best)))
                        best = index;
                if (best == trackedKeys_)
                    break;
                listed[best]       = true;
                const auto& record = records_[best];
                line += " k_";
                appendKeyText(line, record.key, best);
                line += '=' + std::to_string(record.hits) + '/' + std::to_string(record.pendingMisses) + '/' + std::to_string(record.absentMisses) + '/' +
                        std::to_string(record.returns) + '/' + std::to_string(record.evictions) + '/' + std::to_string(idleCount[best]) + '/' + mb(idleBytes[best]);
            }
            return line;
        }
        catch (...)
        {
            return {};
        }
    }

private:
    struct Counters
    {
        std::atomic<std::uint64_t> hits{ 0 }, misses{ 0 }, returns{ 0 }, evictions{ 0 };
        std::atomic<std::uint64_t> idleBytes{ 0 }, pendingBytes{ 0 }, peakHeldBytes{ 0 }, evictedBytes{ 0 };
        std::atomic<std::uint64_t> pendingMisses{ 0 };
    };

    // Diagnostic counts of one key, in a fixed table so recording never
    // allocates.
    struct KeyRecord
    {
        Key           key{};
        std::uint64_t hits = 0, pendingMisses = 0, absentMisses = 0, returns = 0, evictions = 0;
    };

    static constexpr std::size_t kTrackedKeys = 64;

    KeyRecord* recordOf(const Key& key) noexcept
    {
        for (std::size_t index = 0; index < trackedKeys_; ++index)
            if (records_[index].key == key)
                return &records_[index];
        if (trackedKeys_ == kTrackedKeys)
        {
            ++untrackedEvents_;
            return nullptr;
        }
        records_[trackedKeys_].key = key;
        return &records_[trackedKeys_++];
    }

    void countReturn(const Key& key) noexcept
    {
        if (auto* record = recordOf(key))
            ++record->returns;
    }

    // Texture keys encode dimensions, samples, format, flags, castability,
    // layout, quality, mip, and array count.
    static void appendKeyText(std::string& line, const Key& key, std::size_t slot)
    {
        if constexpr (requires { key.width; key.height; key.samples; key.format; key.flags; })
        {
            constexpr char digits[] = "0123456789abcdef";
            std::string    flags;
            for (auto value = static_cast<std::uint64_t>(key.flags); flags.empty() || value; value >>= 4)
                flags.insert(flags.begin(), digits[value & 15]);
            line += std::to_string(key.width) + 'x' + std::to_string(key.height) + 'x' + std::to_string(key.samples) + "_f" + std::to_string(key.format) + "_g" + flags;
            if constexpr (requires { key.castable; })
                if (key.castable)
                    line += "_c";
            // D3D12_RESOURCE_DIMENSION_TEXTURE2D is 3.
            if constexpr (requires { key.dimension; })
                if (key.dimension != 3)
                    line += "_d" + std::to_string(key.dimension);
            if constexpr (requires { key.layout; })
                if (key.layout != 0)
                    line += "_l" + std::to_string(key.layout);
            if constexpr (requires { key.quality; })
                if (key.quality != 0)
                    line += "_q" + std::to_string(key.quality);
            if constexpr (requires { key.mipLevels; })
                if (key.mipLevels != 1)
                    line += "_m" + std::to_string(key.mipLevels);
            if constexpr (requires { key.depthOrArraySize; })
                if (key.depthOrArraySize != 1)
                    line += "_a" + std::to_string(key.depthOrArraySize);
        }
        else if constexpr (std::is_integral_v<Key>)
            line += std::to_string(key);
        else
            line += "slot" + std::to_string(slot);
    }

    // Single writer: the owning thread.
    static void add(std::atomic<std::uint64_t>& value, std::uint64_t delta) noexcept
    {
        value.store(value.load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
    }

    static void sub(std::atomic<std::uint64_t>& value, std::uint64_t delta) noexcept
    {
        value.store(value.load(std::memory_order_relaxed) - delta, std::memory_order_relaxed);
    }

    void notePeak() noexcept
    {
        const auto held = stats_.idleBytes.load(std::memory_order_relaxed) + stats_.pendingBytes.load(std::memory_order_relaxed);
        if (held > stats_.peakHeldBytes.load(std::memory_order_relaxed))
            stats_.peakHeldBytes.store(held, std::memory_order_relaxed);
    }

    // Bookkeeping that cannot allocate releases the resource instead.
    void keep(Entry&& entry) noexcept
    {
        const auto bytes = entry.bytes;
        try
        {
            const auto serial = serial_ + 1;
            const auto index  = byKey_.emplace(entry.key, serial);
            try
            {
                idle_.emplace(serial, std::move(entry));
            }
            catch (...)
            {
                byKey_.erase(index);
                throw;
            }
            serial_ = serial;
            add(stats_.idleBytes, bytes);
            while (stats_.idleBytes.load(std::memory_order_relaxed) > idleCapBytes_)
                evictOldest(true);
            notePeak();
        }
        catch (...)
        {
        }
    }

    void evictOldest(bool counted) noexcept
    {
        const auto oldest = idle_.begin();
        const auto bytes  = oldest->second.bytes;
        if (counted)
            if (auto* record = recordOf(oldest->second.key))
                ++record->evictions;
        for (auto [first, last] = byKey_.equal_range(oldest->second.key); first != last; ++first)
            if (first->second == oldest->first)
            {
                byKey_.erase(first);
                break;
            }
        idle_.erase(oldest);
        sub(stats_.idleBytes, bytes);
        if (counted)
        {
            add(stats_.evictions, 1);
            add(stats_.evictedBytes, bytes);
        }
    }

    std::uint64_t idleCapBytes_ = 0;
    std::uint64_t serial_       = 0;
    // Idle resources by return order, oldest first, and their keys.
    std::map<std::uint64_t, Entry>    idle_;
    std::multimap<Key, std::uint64_t> byKey_;
    // Retired resources by the fence value that releases them.
    std::multimap<std::uint64_t, Entry> pending_;
    Counters                            stats_;
    std::array<KeyRecord, kTrackedKeys> records_{};
    std::size_t                         trackedKeys_     = 0;
    std::uint64_t                       untrackedEvents_ = 0, missesSinceReport_ = 0, reports_ = 0;
};

// Cross-thread releases are posted to the pool owner, which drains them under a
// fence covering work recorded before the drain.
template <class Key, class Resource>
class ResourceReturnQueue
{
    struct Entry
    {
        Key           key{};
        Resource      resource{};
        std::uint64_t bytes = 0;
    };

public:
    ResourceReturnQueue()                                      = default;
    ResourceReturnQueue(const ResourceReturnQueue&)            = delete;
    ResourceReturnQueue& operator=(const ResourceReturnQueue&) = delete;

    // Any thread. Queues resource for the next drain. A closed queue, or
    // bookkeeping that cannot allocate, refuses it: false leaves resource
    // with the caller, which releases it as before.
    bool Post(const Key& key, Resource& resource, std::uint64_t bytes) noexcept
    {
        if (!resource)
            return true;
        std::lock_guard lock(mutex_);
        if (closed_)
            return false;
        try
        {
            entries_.push_back(Entry{ key, Resource{}, bytes });
        }
        catch (...)
        {
            return false;
        }
        entries_.back().resource = std::move(resource);
        bytes_ += bytes;
        return true;
    }

    // Owner thread. Returns each queued resource that admit accepts to pool
    // at fenceValue and releases the rest, outside the lock. Returns how many
    // the pool accepted; an idle one over its cap is still released there.
    template <class Admit>
    std::size_t Drain(FencedResourcePool<Key, Resource>& pool, std::uint64_t fenceValue, Admit&& admit) noexcept
    {
        static_assert(noexcept(admit(std::declval<Resource&>())), "an admit check must not throw");
        std::vector<Entry> drained;
        {
            std::lock_guard lock(mutex_);
            drained.swap(entries_);
            bytes_ = 0;
        }
        std::size_t pooled = 0;
        for (auto& entry : drained)
            if (admit(entry.resource) && pool.Return(entry.key, std::move(entry.resource), entry.bytes, fenceValue))
                ++pooled;
        return pooled;
    }

    // Refuses later posts and releases queued resources, for a shutdown
    // after the GPU went idle.
    void Close() noexcept
    {
        std::vector<Entry> released;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            released.swap(entries_);
            bytes_ = 0;
        }
    }

    // Refuses later posts and forgets queued resources without releasing
    // them, for a device loss. visit must not throw.
    template <class Visit>
    void DetachAll(Visit&& visit) noexcept
    {
        static_assert(noexcept(visit(std::declval<Resource&>())), "a detach visitor must not throw");
        std::lock_guard lock(mutex_);
        closed_ = true;
        for (auto& entry : entries_)
            visit(entry.resource);
        entries_.clear();
        bytes_ = 0;
    }

    // Visits every queued resource and its size, under the lock; visit must
    // not post to this queue.
    template <class Visit>
    void ForEachQueued(Visit&& visit) const
    {
        std::lock_guard lock(mutex_);
        for (const auto& entry : entries_)
            visit(entry.resource, entry.bytes);
    }

    std::size_t QueuedCount() const noexcept
    {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }

    std::uint64_t QueuedBytes() const noexcept
    {
        std::lock_guard lock(mutex_);
        return bytes_;
    }

    // Host bytes of the queue's storage.
    std::uint64_t BookkeepingBytes() const noexcept
    {
        std::lock_guard lock(mutex_);
        return entries_.capacity() * sizeof(Entry);
    }

private:
    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::uint64_t      bytes_  = 0;
    bool               closed_ = false;
};

// Upper bounds of the gap buckets a PoolMissLog counts misses in; past them
// come 64 and more, then keys the pool never took back.
inline constexpr std::array<std::uint64_t, 7> kPoolMissGapBounds  = { 0, 1, 3, 7, 15, 31, 63 };
inline constexpr std::size_t                  kPoolMissGapBuckets = kPoolMissGapBounds.size() + 2;

// A diagnostic of a keyed pool's misses: per key, the misses by the epochs
// since the pool last took that key back. A fixed table keeps recording free
// of allocation; misses of keys past it count as untracked.
template <class Key, std::size_t Capacity = 256>
class PoolMissLog
{
public:
    struct Entry
    {
        Key                                            key{};
        std::uint64_t                                  lastReturn = 0, misses = 0;
        bool                                           returned = false;
        std::array<std::uint64_t, kPoolMissGapBuckets> gaps{};
    };

    void Returned(const Key& key, std::uint64_t epoch) noexcept
    {
        std::lock_guard lock(mutex_);
        if (auto* entry = find(key))
        {
            entry->lastReturn = epoch;
            entry->returned   = true;
        }
    }

    void Missed(const Key& key, std::uint64_t epoch) noexcept
    {
        std::lock_guard lock(mutex_);
        auto*           entry = find(key);
        if (!entry)
        {
            ++untracked_;
            return;
        }
        ++entry->misses;
        std::size_t bucket = kPoolMissGapBuckets - 1;
        if (entry->returned)
        {
            const auto gap = epoch - entry->lastReturn;
            bucket         = kPoolMissGapBounds.size();
            for (std::size_t index = 0; index < kPoolMissGapBounds.size(); ++index)
                if (gap <= kPoolMissGapBounds[index])
                {
                    bucket = index;
                    break;
                }
        }
        ++entry->gaps[bucket];
    }

    // The keys with the most misses, most first, and the untracked misses;
    // unused slots of `top` hold no misses.
    template <std::size_t N>
    void Top(std::array<Entry, N>& top, std::uint64_t& untracked) const noexcept
    {
        std::lock_guard lock(mutex_);
        top       = {};
        untracked = untracked_;
        std::array<bool, Capacity> taken{};
        for (auto& slot : top)
        {
            std::size_t best = used_;
            for (std::size_t index = 0; index < used_; ++index)
                if (!taken[index] && entries_[index].misses && (best == used_ || entries_[index].misses > entries_[best].misses))
                    best = index;
            if (best == used_)
                break;
            taken[best] = true;
            slot        = entries_[best];
        }
    }

private:
    Entry* find(const Key& key) noexcept
    {
        for (std::size_t index = 0; index < used_; ++index)
            if (entries_[index].key == key)
                return &entries_[index];
        if (used_ == Capacity)
            return nullptr;
        entries_[used_].key = key;
        return &entries_[used_++];
    }

    mutable std::mutex          mutex_;
    std::array<Entry, Capacity> entries_{};
    std::size_t                 used_      = 0;
    std::uint64_t               untracked_ = 0;
};

} // namespace rerevved::gpu
