#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <utility>

namespace rerevved::gpu
{

// Committed D3D12 buffers occupy whole 64 KiB placement units, so smaller
// classes would save no memory.
inline constexpr std::uint64_t kUploadPoolMinClassBytes = 64ULL * 1024ULL;
// census-0019 uploaded 53-54 MB textures/swap; arenas add an estimated 30 MB.
// Two in-flight frames plus 25% texture class rounding:2 x (54 x1.25 +30) MB,
// about 195 MB.
inline constexpr std::uint64_t kUploadPoolIdleCapBytes = 192ULL * 1024ULL * 1024ULL;
// An idle buffer serves a request whose class is at least 1 / this of its own
// (one octave).
inline constexpr std::uint64_t kUploadPoolReuseFactor = 2;

// The smallest size class holding bytes: 64 KiB multiples through 256 KiB,
// then four classes per octave, so a class wastes under a quarter of a request.
// Returns 0 for 0 bytes or for sizes beyond 2^62.
std::uint64_t UploadPoolClassBytes(std::uint64_t bytes) noexcept;

struct UploadPoolStats
{
    std::uint64_t hits          = 0; // requests served by an idle buffer
    std::uint64_t creations     = 0; // buffers created, bypasses included
    std::uint64_t bypasses      = 0; // requests above the idle cap, never kept
    std::uint64_t evictions     = 0; // idle buffers released over the cap
    std::uint64_t failures      = 0; // requests refused or failed after a retry
    std::uint64_t liveBytes     = 0; // created and not released: leased, pending or idle
    std::uint64_t idleBytes     = 0;
    std::uint64_t pendingBytes  = 0;
    std::uint64_t peakLiveBytes = 0;
};

// One-thread fenced size-class pool. Retire holds a unique lease through its
// fence; reset/destruction returns it idle, promising no GPU use. Stats fields
// are atomic, not a snapshot.
template <class Buffer>
class FencedUploadPool
{
    struct Entry
    {
        Buffer        buffer{};
        std::uint64_t bytes  = 0;
        bool          pooled = false;
    };

public:
    using Factory = std::function<Buffer(std::uint64_t bytes)>;

    // Leases own buffers uniquely; the pool must outlive every lease.
    class Lease
    {
    public:
        Lease()                        = default;
        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr))
        , entry_(std::exchange(other.entry_, Entry{}))
        , reused_(other.reused_)
        {
        }

        Lease& operator=(Lease&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                pool_   = std::exchange(other.pool_, nullptr);
                entry_  = std::exchange(other.entry_, Entry{});
                reused_ = other.reused_;
            }
            return *this;
        }

        ~Lease()
        {
            reset();
        }

        // Returns the buffer to the idle set; its GPU use must be complete.
        void reset() noexcept
        {
            if (auto* pool = std::exchange(pool_, nullptr))
                pool->keep(std::exchange(entry_, Entry{}));
        }

        // Hands the buffer to visit, which must not throw, and forgets it
        // without returning it, for a device loss that must not free
        // queue-visible storage.
        template <class Visit>
        void Detach(Visit&& visit) noexcept
        {
            static_assert(noexcept(visit(std::declval<Buffer&>())), "a detach visitor must not throw");
            if (auto* pool = std::exchange(pool_, nullptr))
            {
                visit(entry_.buffer);
                sub(pool->stats_.liveBytes, entry_.bytes);
                entry_ = Entry{};
            }
        }

        explicit operator bool() const noexcept
        {
            return pool_ != nullptr;
        }

        Buffer& buffer() noexcept
        {
            return entry_.buffer;
        }

        const Buffer& buffer() const noexcept
        {
            return entry_.buffer;
        }

        // The buffer's size, at least the request.
        std::uint64_t bytes() const noexcept
        {
            return entry_.bytes;
        }

        bool pooled() const noexcept
        {
            return entry_.pooled;
        }

        // Whether the request was served by an idle buffer.
        bool reused() const noexcept
        {
            return reused_;
        }

    private:
        friend class FencedUploadPool;

        Lease(FencedUploadPool* pool, Entry&& entry, bool reused)
        : pool_(pool)
        , entry_(std::move(entry))
        , reused_(reused)
        {
        }

        FencedUploadPool* pool_ = nullptr;
        Entry             entry_;
        bool              reused_ = false;
    };

    explicit FencedUploadPool(Factory factory, std::uint64_t idleCapBytes = kUploadPoolIdleCapBytes)
    : factory_(std::move(factory))
    , idleCapBytes_(idleCapBytes)
    {
    }

    FencedUploadPool(const FencedUploadPool&)            = delete;
    FencedUploadPool& operator=(const FencedUploadPool&) = delete;

    // Reclaim fences, then newest idle of smallest class within octave, else
    // create. admit(classBytes) may refuse/return leases/ReleaseIdle. Failure
    // drops all idle, retries once; empty means bytes 0 or both fail.
    template <class Admit>
    Lease Acquire(std::uint64_t bytes, std::uint64_t completedFence, Admit&& admit)
    {
        Reclaim(completedFence);
        const auto size = UploadPoolClassBytes(bytes);
        if (!size)
            return {};
        const bool pooled = size <= idleCapBytes_;
        if (pooled)
            if (const auto found = byClass_.lower_bound(size); found != byClass_.end() && found->first / kUploadPoolReuseFactor <= size)
            {
                const auto newest = std::prev(byClass_.upper_bound(found->first));
                auto       idle   = idle_.find(newest->second);
                auto       entry  = std::move(idle->second);
                idle_.erase(idle);
                byClass_.erase(newest);
                sub(stats_.idleBytes, entry.bytes);
                add(stats_.hits, 1);
                return { this, std::move(entry), true };
            }
        const auto create = [&]
        {
            return admit(size) ? factory_(size) : Buffer{};
        };
        Buffer buffer = create();
        if (!buffer && !idle_.empty())
        {
            // Idle memory may be what the device or the budget lacks.
            while (!idle_.empty())
                evictOldest(false);
            buffer = create();
        }
        if (!buffer)
        {
            add(stats_.failures, 1);
            return {};
        }
        add(stats_.creations, 1);
        if (!pooled)
            add(stats_.bypasses, 1);
        add(stats_.liveBytes, size);
        if (const auto live = stats_.liveBytes.load(std::memory_order_relaxed); live > stats_.peakLiveBytes.load(std::memory_order_relaxed))
            stats_.peakLiveBytes.store(live, std::memory_order_relaxed);
        return { this, Entry{ std::move(buffer), size, pooled }, false };
    }

    Lease Acquire(std::uint64_t bytes, std::uint64_t completedFence)
    {
        return Acquire(bytes, completedFence, [](std::uint64_t)
                       {
                           return true;
                       });
    }

    // Hold through fenceValue; 0 means no pending GPU use. Bypass releases then
    // rather than pooling; foreign leases retire to their own pool. Allocation
    // failure preserves the lease.
    void Retire(Lease&& lease, std::uint64_t fenceValue)
    {
        auto* pool = lease.pool_;
        if (!pool)
            return;
        if (fenceValue == 0)
        {
            lease.reset();
            return;
        }
        const auto node = pool->pending_.emplace(fenceValue, Entry{});
        node->second    = std::exchange(lease.entry_, Entry{});
        lease.pool_     = nullptr;
        add(pool->stats_.pendingBytes, node->second.bytes);
    }

    // Releases a lease's buffer instead of keeping it.
    void Discard(Lease&& lease)
    {
        auto* pool = std::exchange(lease.pool_, nullptr);
        if (!pool)
            return;
        sub(pool->stats_.liveBytes, lease.entry_.bytes);
        lease.entry_ = Entry{};
    }

    // Moves retirements through completedFence to the idle set, then releases
    // the oldest idle buffers over the cap.
    void Reclaim(std::uint64_t completedFence)
    {
        while (!pending_.empty() && pending_.begin()->first <= completedFence)
        {
            auto entry = std::move(pending_.begin()->second);
            pending_.erase(pending_.begin());
            sub(stats_.pendingBytes, entry.bytes);
            keep(std::move(entry));
        }
    }

    // Releases every idle buffer, for storage pressure or shutdown.
    void ReleaseIdle() noexcept
    {
        while (!idle_.empty())
            evictOldest(true);
    }

    // DetachAll visits idle and pending buffers without releasing queue-visible
    // storage; the visitor must not throw.
    template <class Visit>
    void DetachAll(Visit&& visit) noexcept
    {
        static_assert(noexcept(visit(std::declval<Buffer&>())), "a detach visitor must not throw");
        for (auto& [serial, entry] : idle_)
            visit(entry.buffer);
        for (auto& [fence, entry] : pending_)
            visit(entry.buffer);
        sub(stats_.liveBytes, stats_.idleBytes.load(std::memory_order_relaxed) + stats_.pendingBytes.load(std::memory_order_relaxed));
        idle_.clear();
        byClass_.clear();
        pending_.clear();
        stats_.idleBytes.store(0, std::memory_order_relaxed);
        stats_.pendingBytes.store(0, std::memory_order_relaxed);
    }

    // Visits every held buffer and its size, idle then pending.
    template <class Visit>
    void ForEachHeld(Visit&& visit) const
    {
        for (const auto& [serial, entry] : idle_)
            visit(entry.buffer, entry.bytes);
        for (const auto& [fence, entry] : pending_)
            visit(entry.buffer, entry.bytes);
    }

    // Idle and pending buffers.
    std::size_t HeldCount() const noexcept
    {
        return idle_.size() + pending_.size();
    }

    // Host bytes of the bookkeeping tree nodes, each charged its value and
    // four pointers.
    std::uint64_t BookkeepingBytes() const noexcept
    {
        constexpr std::uint64_t node = 4 * sizeof(void*);
        return idle_.size() * (sizeof(typename decltype(idle_)::value_type) + node) + byClass_.size() * (sizeof(typename decltype(byClass_)::value_type) + node) +
               pending_.size() * (sizeof(typename decltype(pending_)::value_type) + node);
    }

    UploadPoolStats Stats() const noexcept
    {
        const auto load = [](const std::atomic<std::uint64_t>& value)
        {
            return value.load(std::memory_order_relaxed);
        };
        return { load(stats_.hits), load(stats_.creations), load(stats_.bypasses), load(stats_.evictions), load(stats_.failures), load(stats_.liveBytes), load(stats_.idleBytes), load(stats_.pendingBytes), load(stats_.peakLiveBytes) };
    }

    std::uint64_t IdleCapBytes() const noexcept
    {
        return idleCapBytes_;
    }

private:
    struct Counters
    {
        std::atomic<std::uint64_t> hits{ 0 }, creations{ 0 }, bypasses{ 0 }, evictions{ 0 }, failures{ 0 };
        std::atomic<std::uint64_t> liveBytes{ 0 }, idleBytes{ 0 }, pendingBytes{ 0 }, peakLiveBytes{ 0 };
    };

    // Single writer: the owning thread.
    static void add(std::atomic<std::uint64_t>& value, std::uint64_t delta) noexcept
    {
        value.store(value.load(std::memory_order_relaxed) + delta, std::memory_order_relaxed);
    }

    static void sub(std::atomic<std::uint64_t>& value, std::uint64_t delta) noexcept
    {
        value.store(value.load(std::memory_order_relaxed) - delta, std::memory_order_relaxed);
    }

    // Bookkeeping that cannot allocate releases the buffer instead.
    void keep(Entry&& entry) noexcept
    {
        const auto bytes = entry.bytes;
        if (entry.pooled)
            try
            {
                const auto serial = serial_ + 1;
                const auto index  = byClass_.emplace(bytes, serial);
                try
                {
                    idle_.emplace(serial, std::move(entry));
                }
                catch (...)
                {
                    byClass_.erase(index);
                    throw;
                }
                serial_ = serial;
                add(stats_.idleBytes, bytes);
                while (stats_.idleBytes.load(std::memory_order_relaxed) > idleCapBytes_)
                    evictOldest(true);
                return;
            }
            catch (...)
            {
            }
        sub(stats_.liveBytes, bytes);
        entry = Entry{};
    }

    void evictOldest(bool counted) noexcept
    {
        const auto oldest = idle_.begin();
        const auto bytes  = oldest->second.bytes;
        for (auto [first, last] = byClass_.equal_range(bytes); first != last; ++first)
            if (first->second == oldest->first)
            {
                byClass_.erase(first);
                break;
            }
        idle_.erase(oldest);
        sub(stats_.idleBytes, bytes);
        sub(stats_.liveBytes, bytes);
        if (counted)
            add(stats_.evictions, 1);
    }

    Factory       factory_;
    std::uint64_t idleCapBytes_ = 0;
    std::uint64_t serial_       = 0;
    // Idle buffers by retirement order, oldest first, and their classes.
    std::map<std::uint64_t, Entry>              idle_;
    std::multimap<std::uint64_t, std::uint64_t> byClass_;
    // Retired buffers by the fence value that releases them.
    std::multimap<std::uint64_t, Entry> pending_;
    Counters                            stats_;
};

} // namespace rerevved::gpu
