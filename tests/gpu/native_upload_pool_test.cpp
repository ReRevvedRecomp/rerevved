#include "allocation_harness.hpp"
#include "gpu/d3d12/native_resource_pool.h"
#include "gpu/d3d12/native_upload_buffer_d3d12.h"
#include "gpu/d3d12/native_upload_pool.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace rerevved::gpu;

namespace
{

// The old per-test countdown on the shared allocation harness: store(n) fails
// the allocation after n successes, once, on any thread; store(-1) disarms. No
// assertion runs while it is armed.
class FailureCountdown
{
public:
    void store(int successfulAllocations)
    {
        scope_.reset();
        if (successfulAllocations >= 0)
            scope_.emplace(successfulAllocations,
                           rerevved::tests::allocation::ScopedCountdown::Scope::AllThreads,
                           rerevved::tests::allocation::ScopedCountdown::Mode::OneShot);
    }

private:
    std::optional<rerevved::tests::allocation::ScopedCountdown> scope_;
};

FailureCountdown allocationsBeforeFailure;

// Disarms the countdown when a case ends, however it ends.
struct DisarmCountdown
{
    ~DisarmCountdown()
    {
        allocationsBeforeFailure.store(-1);
    }
};

} // namespace

namespace
{

constexpr std::uint64_t KiB = 1024;
constexpr std::uint64_t MiB = 1024 * KiB;

// A buffer whose release the test observes through a weak pointer.
struct FakeBuffer
{
    std::shared_ptr<int> id;

    explicit operator bool() const noexcept
    {
        return bool(id);
    }
};

using Pool  = FencedUploadPool<FakeBuffer>;
using Lease = Pool::Lease;

// Mirrors the pool's private entry layout so the bookkeeping charge is exact.
struct MirrorEntry
{
    FakeBuffer    buffer{};
    std::uint64_t bytes  = 0;
    bool          pooled = false;
};

constexpr std::uint64_t kNodeBytes = 4 * sizeof(void*);
constexpr std::uint64_t kEntryNode = sizeof(std::pair<const std::uint64_t, MirrorEntry>) + kNodeBytes;
constexpr std::uint64_t kClassNode = sizeof(std::pair<const std::uint64_t, std::uint64_t>) + kNodeBytes;

static_assert(!std::is_copy_constructible_v<Lease> && !std::is_copy_assignable_v<Lease>, "a lease owns its buffer uniquely");
static_assert(std::is_nothrow_move_constructible_v<Lease> && std::is_nothrow_move_assignable_v<Lease>, "a lease moves without throwing");

struct Factory
{
    int                             next  = 0;
    int                             fails = 0; // creations still to fail
    std::vector<std::uint64_t>      sizes;
    std::vector<std::weak_ptr<int>> made;

    Pool::Factory bind()
    {
        return [this](std::uint64_t bytes)
        {
            sizes.push_back(bytes);
            if (fails > 0)
            {
                --fails;
                return FakeBuffer{};
            }
            FakeBuffer buffer{ std::make_shared<int>(++next) };
            made.push_back(buffer.id);
            return buffer;
        };
    }
};

void classes()
{
    UNSCOPED_INFO("no class for zero bytes");
    REQUIRE(UploadPoolClassBytes(0) == 0);
    UNSCOPED_INFO("64 KiB is the smallest class");
    REQUIRE((UploadPoolClassBytes(1) == 64 * KiB && UploadPoolClassBytes(64 * KiB) == 64 * KiB));
    UNSCOPED_INFO("64 KiB steps through 256 KiB");
    REQUIRE((UploadPoolClassBytes(64 * KiB + 1) == 128 * KiB && UploadPoolClassBytes(200 * KiB) == 256 * KiB));
    UNSCOPED_INFO("quarter-octave steps above 256 KiB");
    REQUIRE((UploadPoolClassBytes(256 * KiB + 1) == 320 * KiB && UploadPoolClassBytes(300 * KiB) == 320 * KiB));
    UNSCOPED_INFO("classes scale with the octave");
    REQUIRE((UploadPoolClassBytes(MiB + 1) == MiB + 256 * KiB && UploadPoolClassBytes(128 * MiB) == 128 * MiB));
    UNSCOPED_INFO("sizes beyond 2^62 have no class");
    REQUIRE((UploadPoolClassBytes(1ULL << 62) == 1ULL << 62 && UploadPoolClassBytes((1ULL << 62) + 1) == 0));
    std::uint64_t previous = 0;
    for (std::uint64_t bytes = 1; bytes < 600 * MiB; bytes += bytes / 7 + 4093)
    {
        const auto size = UploadPoolClassBytes(bytes);
        UNSCOPED_INFO("a class holds its request in whole placement units, monotonically");
        REQUIRE((size >= bytes && size % (64 * KiB) == 0 && size >= previous));
        UNSCOPED_INFO("a class wastes under a quarter of a request");
        REQUIRE((bytes <= 256 * KiB ? size - bytes < 64 * KiB : (size - bytes) * 4 < bytes));
        UNSCOPED_INFO("a class is its own class");
        REQUIRE(UploadPoolClassBytes(size) == size);
        previous = size;
    }
}

void fencedReuse()
{
    Factory factory;
    Pool    pool(factory.bind());
    auto    first = pool.Acquire(100 * KiB, 0);
    UNSCOPED_INFO("a request creates its class");
    REQUIRE((first && first.pooled() && !first.reused() && first.bytes() == 128 * KiB && factory.sizes.back() == 128 * KiB));
    const auto* firstId = first.buffer().id.get();
    pool.Retire(std::move(first), 5);
    UNSCOPED_INFO("retiring moves the buffer out of the lease");
    REQUIRE((!first && pool.Stats().pendingBytes == 128 * KiB));
    auto second = pool.Acquire(100 * KiB, 4);
    UNSCOPED_INFO("a buffer stays unused until its fence completes");
    REQUIRE((second && second.buffer().id.get() != firstId && factory.next == 2));
    auto third = pool.Acquire(120 * KiB, 5);
    UNSCOPED_INFO("a completed fence makes the buffer reusable");
    REQUIRE((third && third.reused() && third.buffer().id.get() == firstId));
    auto stats = pool.Stats();
    UNSCOPED_INFO("hits, creations and live bytes count leases");
    REQUIRE((stats.hits == 1 && stats.creations == 2 && stats.liveBytes == 256 * KiB && stats.idleBytes == 0 && stats.pendingBytes == 0));
    pool.Retire(std::move(second), 0);
    UNSCOPED_INFO("fence 0 retires straight to idle");
    REQUIRE(pool.Stats().idleBytes == 128 * KiB);
    {
        auto scoped = std::move(third);
        UNSCOPED_INFO("moving transfers the buffer");
        REQUIRE((!third && scoped));
    }
    stats = pool.Stats();
    UNSCOPED_INFO("a destroyed lease returns its buffer to idle");
    REQUIRE((stats.idleBytes == 256 * KiB && stats.liveBytes == 256 * KiB && !factory.made[0].expired()));
    auto reassigned = pool.Acquire(128 * KiB, 0);
    auto other      = pool.Acquire(128 * KiB, 0);
    reassigned      = std::move(other);
    UNSCOPED_INFO("move assignment returns the replaced buffer to idle");
    REQUIRE((!other && reassigned && pool.Stats().idleBytes == 128 * KiB));
    reassigned.reset();
    UNSCOPED_INFO("reset returns the buffer to idle");
    REQUIRE((!reassigned && pool.Stats().idleBytes == 256 * KiB));
}

void classSpan()
{
    Factory factory;
    Pool    pool(factory.bind());
    auto    big = pool.Acquire(MiB, 0);
    pool.Retire(std::move(big), 0);
    auto closer = pool.Acquire(600 * KiB, 0);
    UNSCOPED_INFO("a buffer serves requests up to one octave smaller");
    REQUIRE((closer && closer.bytes() == MiB && pool.Stats().hits == 1));
    pool.Retire(std::move(closer), 0);
    auto farther = pool.Acquire(400 * KiB, 0);
    UNSCOPED_INFO("a request more than an octave smaller creates its own class");
    REQUIRE((farther && farther.bytes() == 448 * KiB && pool.Stats().creations == 2));
    pool.Retire(std::move(farther), 0);
    auto exact = pool.Acquire(400 * KiB, 0);
    UNSCOPED_INFO("the smallest fitting class wins");
    REQUIRE((exact.bytes() == 448 * KiB && pool.Stats().hits == 2));
    pool.Discard(std::move(exact));
    // The smallest classes: 128 KiB serves 64 KiB, 192 KiB does not.
    auto twice  = pool.Acquire(128 * KiB, 0);
    auto thrice = pool.Acquire(192 * KiB, 0);
    pool.Retire(std::move(thrice), 0);
    auto tiny = pool.Acquire(1, 0);
    UNSCOPED_INFO("a 192 KiB buffer is more than an octave above 64 KiB");
    REQUIRE((tiny.bytes() == 64 * KiB && !tiny.reused()));
    pool.Retire(std::move(twice), 0);
    auto served = pool.Acquire(1, 0);
    UNSCOPED_INFO("a 128 KiB buffer serves a 64 KiB request");
    REQUIRE((served.bytes() == 128 * KiB && served.reused()));
}

void newestFirstAndCap()
{
    Factory            factory;
    Pool               pool(factory.bind(), MiB);
    std::vector<Lease> leases;
    for (int index = 0; index < 4; ++index)
        leases.push_back(pool.Acquire(300 * KiB, 0));
    for (auto& lease : leases)
        pool.Retire(std::move(lease), 0);
    // Four 320 KiB buffers exceed the 1 MiB cap: the oldest is released.
    UNSCOPED_INFO("the oldest idle buffer is evicted over the cap");
    REQUIRE((factory.made[0].expired() && !factory.made[1].expired()));
    auto stats = pool.Stats();
    UNSCOPED_INFO("eviction lowers idle and live bytes");
    REQUIRE((stats.evictions == 1 && stats.idleBytes == 960 * KiB && stats.liveBytes == 960 * KiB && stats.peakLiveBytes == 1280 * KiB));
    auto newest = pool.Acquire(300 * KiB, 0);
    UNSCOPED_INFO("the most recently retired buffer is reused first");
    REQUIRE(newest.buffer().id == factory.made[3].lock());
    pool.Discard(std::move(newest));
    UNSCOPED_INFO("discarding releases the buffer");
    REQUIRE((!newest && factory.made[3].expired() && pool.Stats().liveBytes == 640 * KiB));
    pool.ReleaseIdle();
    stats = pool.Stats();
    UNSCOPED_INFO("releasing idle buffers frees them all");
    REQUIRE((factory.made[1].expired() && factory.made[2].expired() && stats.idleBytes == 0 && stats.liveBytes == 0 && stats.evictions == 3));
}

void bypassAndOrdering()
{
    Factory factory;
    Pool    pool(factory.bind(), MiB);
    auto    huge = pool.Acquire(3 * MiB, 0);
    UNSCOPED_INFO("a class above the cap bypasses the pool");
    REQUIRE((huge && !huge.pooled() && huge.bytes() == 3 * MiB && pool.Stats().bypasses == 1));
    auto tiny = pool.Acquire(64 * KiB, 0);
    pool.Retire(std::move(huge), 7);
    pool.Retire(std::move(tiny), 3);
    pool.Reclaim(5);
    auto stats = pool.Stats();
    UNSCOPED_INFO("only completed fences reclaim");
    REQUIRE((!factory.made[0].expired() && stats.idleBytes == 64 * KiB && stats.pendingBytes == 3 * MiB));
    pool.Reclaim(7);
    stats = pool.Stats();
    UNSCOPED_INFO("a bypass buffer is released after its fence, never kept");
    REQUIRE((factory.made[0].expired() && stats.idleBytes == 64 * KiB && stats.pendingBytes == 0 && stats.liveBytes == 64 * KiB));
    {
        auto dropped = pool.Acquire(2 * MiB, 0);
    }
    UNSCOPED_INFO("a destroyed bypass lease releases its buffer");
    REQUIRE((factory.made[2].expired() && pool.Stats().liveBytes == 64 * KiB));
}

void failures()
{
    Factory factory;
    Pool    pool(factory.bind());
    auto    kept = pool.Acquire(64 * KiB, 0);
    pool.Retire(std::move(kept), 0);
    factory.fails = 1;
    auto attempts = factory.sizes.size();
    auto retried  = pool.Acquire(MiB * 4, 0);
    UNSCOPED_INFO("a failed creation releases idle buffers and retries exactly once");
    REQUIRE((retried && factory.sizes.size() == attempts + 2 && factory.made[0].expired() && pool.Stats().failures == 0 && pool.Stats().idleBytes == 0));
    factory.fails = 1;
    attempts      = factory.sizes.size();
    auto failed   = pool.Acquire(MiB, 0);
    UNSCOPED_INFO("with nothing idle a failed creation is not retried");
    REQUIRE((!failed && factory.sizes.size() == attempts + 1 && pool.Stats().failures == 1));
    auto spare = pool.Acquire(64 * KiB, 0);
    pool.Retire(std::move(spare), 0);
    factory.fails = 2;
    attempts      = factory.sizes.size();
    auto twice    = pool.Acquire(MiB, 0);
    UNSCOPED_INFO("two failed creations return an empty lease after releasing idle buffers");
    REQUIRE((!twice && factory.sizes.size() == attempts + 2 && pool.Stats().failures == 2 && pool.Stats().idleBytes == 0));
    UNSCOPED_INFO("zero bytes is no request");
    REQUIRE((!pool.Acquire(0, 0) && pool.Stats().failures == 2));
    // A budget refuses the first creation until the idle buffer is released.
    spare = pool.Acquire(64 * KiB, 0);
    pool.Retire(std::move(spare), 0);
    std::vector<std::uint64_t> admitted;
    attempts      = factory.sizes.size();
    auto budgeted = pool.Acquire(2 * MiB, 0, [&](std::uint64_t bytes)
                                 {
                                     admitted.push_back(bytes);
                                     return pool.Stats().idleBytes == 0;
                                 });
    UNSCOPED_INFO("a refused creation releases idle buffers and asks again before creating once");
    REQUIRE((budgeted && admitted.size() == 2 && admitted[0] == 2 * MiB && factory.sizes.size() == attempts + 1 && pool.Stats().idleBytes == 0));
    attempts     = factory.sizes.size();
    auto refused = pool.Acquire(MiB, 0, [](std::uint64_t)
                                {
                                    return false;
                                });
    UNSCOPED_INFO("a refused creation never reaches the factory");
    REQUIRE((!refused && factory.sizes.size() == attempts && pool.Stats().failures == 3));
    // The budget check may itself return a lease and release idle storage.
    auto       returned = pool.Acquire(512 * KiB, 0);
    auto       inner    = pool.Acquire(3 * MiB, 0, [&](std::uint64_t)
                                       {
                                  returned.reset();
                                  pool.ReleaseIdle();
                                  return true;
                                       });
    const auto stats    = pool.Stats();
    UNSCOPED_INFO("an admit that re-enters the pool keeps the counts exact");
    REQUIRE((inner && !returned && stats.idleBytes == 0 && stats.liveBytes == 4 * MiB + 2 * MiB + 3 * MiB));
    pool.Discard(std::move(retried));
    pool.Discard(std::move(budgeted));
    pool.Discard(std::move(inner));
    UNSCOPED_INFO("releasing every lease leaves nothing live");
    REQUIRE((pool.Stats().liveBytes == 0 && pool.HeldCount() == 0 && pool.BookkeepingBytes() == 0));
}

void detach()
{
    Factory factory;
    Pool    pool(factory.bind());
    auto    idle    = pool.Acquire(64 * KiB, 0);
    auto    pending = pool.Acquire(128 * KiB, 0);
    auto    leased  = pool.Acquire(256 * KiB, 0);
    auto    lost    = pool.Acquire(320 * KiB, 0);
    pool.Retire(std::move(idle), 0);
    UNSCOPED_INFO("an idle buffer charges its entry and class nodes exactly");
    REQUIRE(pool.BookkeepingBytes() == kEntryNode + kClassNode);
    pool.Retire(std::move(pending), 9);
    std::uint64_t held = 0;
    pool.ForEachHeld([&](const FakeBuffer& buffer, std::uint64_t bytes)
                     {
                         UNSCOPED_INFO("held buffers are valid");
                         REQUIRE(bool(buffer));
                         held += bytes;
                     });
    UNSCOPED_INFO("held buffers are the idle and pending ones");
    REQUIRE((held == 192 * KiB && pool.HeldCount() == 2));
    UNSCOPED_INFO("an idle and a pending buffer charge their entry and class nodes exactly");
    REQUIRE(pool.BookkeepingBytes() == 2 * kEntryNode + kClassNode);
    std::vector<std::shared_ptr<int>> leaked;
    leaked.reserve(2);
    pool.DetachAll([&](FakeBuffer& buffer) noexcept
                   {
                       leaked.push_back(buffer.id);
                   });
    auto stats = pool.Stats();
    UNSCOPED_INFO("detaching hands over the idle and the pending buffer");
    REQUIRE((leaked.size() == 2 && leaked[0] && leaked[0] == factory.made[0].lock() && leaked[1] && leaked[1] == factory.made[1].lock()));
    UNSCOPED_INFO("detaching forgets held buffers and keeps outstanding leases live");
    REQUIRE((pool.HeldCount() == 0 && stats.liveBytes == 576 * KiB && stats.idleBytes == 0 && stats.pendingBytes == 0));
    std::shared_ptr<int> kept;
    lost.Detach([&](FakeBuffer& buffer) noexcept
                {
                    kept = buffer.id;
                });
    UNSCOPED_INFO("a detached lease hands over its buffer and returns nothing");
    REQUIRE((!lost && kept && kept == factory.made[3].lock() && pool.Stats().liveBytes == 256 * KiB && pool.HeldCount() == 0));
    pool.Discard(std::move(leased));
    UNSCOPED_INFO("a lease outstanding at detach is still discarded exactly");
    REQUIRE(pool.Stats().liveBytes == 0);
}

void bookkeepingFailures()
{
    Factory factory;
    Pool    pool(factory.bind());
    auto    lease = pool.Acquire(64 * KiB, 0);
    bool    threw = false;
    allocationsBeforeFailure.store(0);
    try
    {
        pool.Retire(std::move(lease), 9);
    }
    catch (const std::bad_alloc&)
    {
        threw = true;
    }
    allocationsBeforeFailure.store(-1);
    auto stats = pool.Stats();
    UNSCOPED_INFO("a retirement whose bookkeeping cannot allocate leaves the lease intact");
    REQUIRE((threw && lease && lease.buffer().id && lease.buffer().id == factory.made[0].lock() && stats.pendingBytes == 0 && stats.liveBytes == 64 * KiB));
    allocationsBeforeFailure.store(0);
    lease.reset();
    allocationsBeforeFailure.store(-1);
    stats = pool.Stats();
    UNSCOPED_INFO("a returned buffer whose bookkeeping cannot allocate is released");
    REQUIRE((!lease && factory.made[0].expired() && stats.idleBytes == 0 && stats.liveBytes == 0 && pool.HeldCount() == 0));
    auto second = pool.Acquire(64 * KiB, 0);
    allocationsBeforeFailure.store(1);
    second.reset();
    allocationsBeforeFailure.store(-1);
    UNSCOPED_INFO("a failure in the second bookkeeping node rolls back the first");
    REQUIRE((factory.made[1].expired() && pool.Stats().liveBytes == 0 && pool.HeldCount() == 0 && pool.BookkeepingBytes() == 0));
}

void retireToOwner()
{
    Factory factory, otherFactory;
    Pool    pool(factory.bind());
    Pool    other(otherFactory.bind());
    auto    foreign = other.Acquire(64 * KiB, 0);
    pool.Retire(std::move(foreign), 4);
    UNSCOPED_INFO("a lease retires to its own pool");
    REQUIRE((other.Stats().pendingBytes == 64 * KiB && pool.Stats().pendingBytes == 0 && pool.Stats().liveBytes == 0));
}

// Overlapping frame contexts return buffers two swaps after use.
void steadyFrames()
{
    Factory       factory;
    Pool          pool(factory.bind());
    std::uint32_t seed = 12345;
    const auto    next = [&](std::uint64_t mean)
    {
        seed = seed * 1664525U + 1013904223U;
        // Uniform in [mean / 4, 7 mean / 4): mean preserved, a wide spread.
        return mean / 4 + (std::uint64_t(seed >> 8) * (3 * mean / 2) >> 24);
    };
    std::uint64_t createdAfterWarmup = 0, hitsAfterWarmup = 0;
    // Census-0013 world per swap: 109 uploads totaling 48 MB on average, plus
    // about 70 arenas; two frame contexts overlap.
    for (std::uint64_t frame = 1; frame <= 60; ++frame)
    {
        const auto         before = pool.Stats();
        std::vector<Lease> used;
        for (int texture = 0; texture < 109; ++texture)
            used.push_back(pool.Acquire(next(440 * KiB), frame > 2 ? frame - 2 : 0));
        for (int arena = 0; arena < 70; ++arena)
            used.push_back(pool.Acquire(next(430 * KiB), frame > 2 ? frame - 2 : 0));
        for (auto& lease : used)
        {
            UNSCOPED_INFO("every steady request is served");
            REQUIRE(bool(lease));
            pool.Retire(std::move(lease), frame);
        }
        const auto after = pool.Stats();
        if (frame > 10)
        {
            createdAfterWarmup += after.creations - before.creations;
            hitsAfterWarmup += after.hits - before.hits;
        }
    }
    const auto stats = pool.Stats();
    std::cout << "steady frames: hits " << hitsAfterWarmup << " creations " << createdAfterWarmup << " peak MiB " << stats.peakLiveBytes / MiB
              << " evictions " << stats.evictions << '\n';
    UNSCOPED_INFO("steady frames reuse at least 95 percent of buffers");
    REQUIRE(hitsAfterWarmup * 100 >= (hitsAfterWarmup + createdAfterWarmup) * 95);
    UNSCOPED_INFO("live bytes stay within two frames plus the idle cap");
    REQUIRE(stats.peakLiveBytes <= 3 * kUploadPoolIdleCapBytes);
}

// Keyed resources whose releases the test counts.
int keyedReleases = 0;

struct CountedRelease
{
    void operator()(int* value) const noexcept
    {
        ++keyedReleases;
        delete value;
    }
};

using Counted   = std::unique_ptr<int, CountedRelease>;
using KeyedPool = FencedResourcePool<int, Counted>;

// Mirrors the keyed pool's private entry layout.
struct KeyedMirror
{
    int           key{};
    Counted       resource{};
    std::uint64_t bytes = 0;
};

constexpr std::uint64_t kKeyedEntryNode = sizeof(std::pair<const std::uint64_t, KeyedMirror>) + kNodeBytes;
constexpr std::uint64_t kKeyedKeyNode   = sizeof(std::pair<const int, std::uint64_t>) + kNodeBytes;

Counted counted(int id)
{
    return Counted(new int(id));
}

void keyedExactReuse()
{
    keyedReleases = 0;
    KeyedPool pool(4 * MiB);
    UNSCOPED_INFO("an empty keyed pool misses");
    REQUIRE((!pool.Take(1, 0) && pool.Stats().misses == 1));
    pool.Return(1, counted(10), MiB, 0);
    pool.Return(1, counted(11), MiB, 0);
    UNSCOPED_INFO("another key never takes an idle resource");
    REQUIRE(!pool.Take(2, 0));
    auto taken = pool.Take(1, 0);
    UNSCOPED_INFO("the newest idle resource of a key is taken first");
    REQUIRE((taken && *taken.resource == 11 && taken.bytes == MiB));
    const auto stats = pool.Stats();
    UNSCOPED_INFO("a take moves one resource out of the idle set");
    REQUIRE((stats.hits == 1 && stats.misses == 2 && stats.returns == 2 && stats.idleBytes == MiB && keyedReleases == 0));
    pool.Return(1, Counted{}, MiB, 0);
    UNSCOPED_INFO("an empty resource is not a return");
    REQUIRE((pool.Stats().returns == 2 && pool.HeldCount() == 1));
}

void keyedFence()
{
    keyedReleases = 0;
    KeyedPool pool(4 * MiB);
    pool.Return(3, counted(30), MiB, 7);
    UNSCOPED_INFO("a retired resource stays pending until its fence completes");
    REQUIRE((!pool.Take(3, 6) && pool.Stats().pendingBytes == MiB && pool.Stats().idleBytes == 0));
    auto taken = pool.Take(3, 7);
    UNSCOPED_INFO("a completed fence makes a retired resource available");
    REQUIRE((taken && *taken.resource == 30 && pool.Stats().pendingBytes == 0));
    pool.Return(3, counted(31), MiB, 9);
    pool.Return(4, counted(40), MiB, 0);
    pool.ReleaseIdle();
    UNSCOPED_INFO("releasing idle resources keeps pending ones");
    REQUIRE((keyedReleases == 1 && pool.HeldCount() == 1 && pool.Stats().pendingBytes == MiB && pool.Stats().evictions == 1 && pool.Stats().evictedBytes == MiB));
    pool.Reclaim(8);
    UNSCOPED_INFO("an earlier fence does not reclaim a later retirement");
    REQUIRE(pool.Stats().pendingBytes == MiB);
    pool.Reclaim(9);
    UNSCOPED_INFO("reclaiming a fence moves its retirements to the idle set");
    REQUIRE((pool.Stats().pendingBytes == 0 && pool.Stats().idleBytes == MiB && keyedReleases == 1));
    pool.Return(5, counted(50), MiB, 12);
    pool.ReleaseAll();
    UNSCOPED_INFO("releasing all frees idle and pending resources");
    REQUIRE((keyedReleases == 3 && pool.HeldCount() == 0 && pool.Stats().idleBytes == 0 && pool.Stats().pendingBytes == 0));
}

void keyedCap()
{
    keyedReleases = 0;
    KeyedPool pool(3 * MiB);
    for (int id = 0; id < 4; ++id)
        pool.Return(id, counted(id), MiB, 0);
    auto stats = pool.Stats();
    UNSCOPED_INFO("the oldest idle resource is released over the cap");
    REQUIRE((keyedReleases == 1 && stats.evictions == 1 && stats.evictedBytes == MiB && stats.idleBytes == 3 * MiB && stats.peakHeldBytes == 3 * MiB));
    // The taken resource is released with its temporary.
    UNSCOPED_INFO("eviction removes the oldest key only");
    REQUIRE((!pool.Take(0, 0) && pool.Take(1, 0)));
    UNSCOPED_INFO("an idle resource over the idle cap is released at once");
    REQUIRE((pool.Return(9, counted(9), 4 * MiB, 0) && keyedReleases == 3));
    auto large = counted(10);
    UNSCOPED_INFO("a pending resource over the idle cap stays with its owner");
    REQUIRE((!pool.Return(9, std::move(large), 4 * MiB, 5) && large && *large == 10 && keyedReleases == 3));
    stats = pool.Stats();
    UNSCOPED_INFO("a refused return holds nothing");
    REQUIRE((stats.idleBytes == 2 * MiB && stats.pendingBytes == 0 && stats.returns == 5));
}

void keyedDetachAndBookkeeping()
{
    keyedReleases = 0;
    KeyedPool pool(8 * MiB);
    pool.Return(1, counted(1), MiB, 0);
    pool.Return(1, counted(2), 2 * MiB, 0);
    pool.Return(2, counted(3), MiB, 5);
    UNSCOPED_INFO("bookkeeping charges every tree node");
    REQUIRE(pool.BookkeepingBytes() == 3 * kKeyedEntryNode + 2 * kKeyedKeyNode);
    std::vector<int> visited;
    std::uint64_t    heldBytes = 0;
    pool.ForEachHeld([&](const Counted& resource, std::uint64_t bytes)
                     {
                         visited.push_back(*resource);
                         heldBytes += bytes;
                     });
    UNSCOPED_INFO("held resources are visited idle then pending");
    REQUIRE((visited == std::vector<int>{ 1, 2, 3 } && heldBytes == 4 * MiB && pool.HeldCount() == 3));
    std::vector<int*> detached;
    pool.DetachAll([&](Counted& resource) noexcept
                   {
                       detached.push_back(resource.release());
                   });
    const auto stats = pool.Stats();
    UNSCOPED_INFO("detaching forgets every held resource without releasing it");
    REQUIRE((detached.size() == 3 && keyedReleases == 0 && pool.HeldCount() == 0 && stats.idleBytes == 0 && stats.pendingBytes == 0 && pool.BookkeepingBytes() == 0));
    for (auto* value : detached)
        delete value;
}

void keyedBookkeepingFailures()
{
    keyedReleases = 0;
    KeyedPool  pool(8 * MiB);
    const auto failNext = [](int succeeding)
    {
        allocationsBeforeFailure.store(succeeding);
    };
    for (const int succeeding : { 0, 1 })
    {
        auto resource = counted(succeeding);
        failNext(succeeding);
        pool.Return(1, std::move(resource), MiB, 0);
        failNext(-1);
    }
    UNSCOPED_INFO("an idle return that cannot allocate releases its resource");
    REQUIRE((keyedReleases == 2 && pool.HeldCount() == 0 && pool.BookkeepingBytes() == 0 && pool.Stats().idleBytes == 0));
    auto pending = counted(7);
    failNext(0);
    const bool held = pool.Return(1, std::move(pending), MiB, 5);
    failNext(-1);
    UNSCOPED_INFO("a pending return that cannot allocate leaves the resource with its owner");
    REQUIRE((!held && pending && *pending == 7 && keyedReleases == 2 && pool.HeldCount() == 0 && pool.Stats().pendingBytes == 0));
    pending.reset();
    // Once its fence completed, a retired resource may be released.
    pool.Return(1, counted(8), MiB, 5);
    failNext(0);
    pool.Reclaim(5);
    failNext(-1);
    UNSCOPED_INFO("a reclaim that cannot allocate releases its resource");
    REQUIRE((keyedReleases == 4 && pool.HeldCount() == 0 && pool.Stats().pendingBytes == 0 && pool.Stats().idleBytes == 0));
}

using KeyedReturns = ResourceReturnQueue<int, Counted>;

constexpr auto admitAll = [](Counted&) noexcept
{
    return true;
};

// Last-holder returns wait for an owner drain whose fence covers prior work.
void returnQueueFence()
{
    keyedReleases = 0;
    KeyedPool    pool(8 * MiB);
    KeyedReturns returns;
    auto         first = counted(1), second = counted(2);
    UNSCOPED_INFO("a post takes the resource");
    REQUIRE((returns.Post(1, first, MiB) && !first && returns.Post(1, second, 2 * MiB) && !second));
    std::uint64_t queued = 0;
    returns.ForEachQueued([&](const Counted&, std::uint64_t bytes)
                          {
                              queued += bytes;
                          });
    UNSCOPED_INFO("a posted resource is visible as queued and waits for the drain");
    REQUIRE((queued == 3 * MiB && returns.QueuedCount() == 2 && returns.QueuedBytes() == 3 * MiB && pool.HeldCount() == 0 && !pool.Take(1, 100)));
    UNSCOPED_INFO("queued posts charge their storage");
    REQUIRE(returns.BookkeepingBytes() >= 2 * (sizeof(int) + sizeof(Counted) + sizeof(std::uint64_t)));
    UNSCOPED_INFO("a drain returns every post at its fence");
    REQUIRE((returns.Drain(pool, 9, admitAll) == 2 && returns.QueuedCount() == 0 && returns.QueuedBytes() == 0 && pool.Stats().pendingBytes == 3 * MiB));
    UNSCOPED_INFO("a drain hands the queue's storage over with its posts");
    REQUIRE(returns.BookkeepingBytes() == 0);
    UNSCOPED_INFO("a drained resource stays pending until the drain's fence completes");
    REQUIRE(!pool.Take(1, 8));
    auto taken = pool.Take(1, 9);
    UNSCOPED_INFO("the newest drained resource is taken first");
    REQUIRE((taken && *taken.resource == 2 && taken.bytes == 2 * MiB && keyedReleases == 0));
    Counted empty;
    UNSCOPED_INFO("an empty resource is not queued");
    REQUIRE((returns.Post(1, empty, MiB) && returns.QueuedCount() == 0));
}

// Drain admits only unreferenced resources; refused/pending returns release.
void returnQueueAdmit()
{
    keyedReleases = 0;
    KeyedPool    pool(8 * MiB);
    KeyedReturns returns;
    for (int id = 1; id <= 3; ++id)
    {
        auto resource = counted(id);
        returns.Post(id, resource, MiB);
    }
    const auto pooled = returns.Drain(pool, 0, [](Counted& resource) noexcept
                                      {
                                          return *resource != 2;
                                      });
    UNSCOPED_INFO("a resource the admit check rejects is released, not pooled");
    REQUIRE((pooled == 2 && keyedReleases == 1 && pool.HeldCount() == 2 && !pool.Take(2, 0)));
    auto large = counted(9);
    returns.Post(9, large, 16 * MiB);
    UNSCOPED_INFO("a drained pending return over the cap is released");
    REQUIRE((returns.Drain(pool, 5, admitAll) == 0 && keyedReleases == 2 && pool.HeldCount() == 2));
}

// Holders on other threads post while the owner drains.
void returnQueueThreads()
{
    keyedReleases = 0;

    constexpr int            kPosters = 4, kPosts = 64;
    KeyedPool                pool(1024 * MiB);
    KeyedReturns             returns;
    std::atomic<int>         refused{ 0 };
    std::atomic<bool>        start{ false };
    std::vector<std::thread> posters;
    for (int poster = 0; poster < kPosters; ++poster)
        posters.emplace_back([&, poster]()
                             {
                                 while (!start.load())
                                     std::this_thread::yield();
                                 for (int post = 0; post < kPosts; ++post)
                                 {
                                     auto resource = counted(poster * kPosts + post);
                                     if (!returns.Post(poster, resource, MiB))
                                         refused.fetch_add(1);
                                 }
                             });
    start.store(true);
    // The owner drains while the posters run, until every post arrived.
    std::size_t pooled = 0;
    while (pooled < kPosters * kPosts && refused.load() == 0)
        pooled += returns.Drain(pool, 0, admitAll);
    for (auto& poster : posters)
        poster.join();
    UNSCOPED_INFO("every post from other threads reaches the pool once");
    REQUIRE((refused.load() == 0 && pooled == kPosters * kPosts && pool.HeldCount() == kPosters * kPosts && keyedReleases == 0));
}

void returnQueueCloseAndDetach()
{
    keyedReleases = 0;
    KeyedPool pool(8 * MiB);
    {
        KeyedReturns returns;
        auto         queued = counted(1);
        returns.Post(1, queued, MiB);
        returns.Close();
        UNSCOPED_INFO("closing releases queued resources");
        REQUIRE((keyedReleases == 1 && returns.QueuedCount() == 0 && returns.QueuedBytes() == 0));
        auto late = counted(2);
        UNSCOPED_INFO("a closed queue leaves a late resource with its holder");
        REQUIRE((!returns.Post(1, late, MiB) && late && *late == 2 && keyedReleases == 1));
    }
    KeyedReturns lost;
    auto         queued = counted(3);
    lost.Post(1, queued, MiB);
    std::vector<int*> detached;
    lost.DetachAll([&](Counted& resource) noexcept
                   {
                       detached.push_back(resource.release());
                   });
    auto late = counted(4);
    UNSCOPED_INFO("a device loss forgets queued resources and refuses later posts");
    REQUIRE((detached.size() == 1 && *detached[0] == 3 && keyedReleases == 2 && lost.QueuedCount() == 0 && !lost.Post(1, late, MiB) && late));
    for (auto* value : detached)
        delete value;
    UNSCOPED_INFO("a detached queue drains nothing");
    REQUIRE((lost.Drain(pool, 0, admitAll) == 0 && pool.HeldCount() == 0));
}

void returnQueueBookkeeping()
{
    keyedReleases = 0;
    KeyedPool    pool(8 * MiB);
    KeyedReturns returns;
    auto         resource = counted(1);
    allocationsBeforeFailure.store(0);
    const bool posted = returns.Post(1, resource, MiB);
    allocationsBeforeFailure.store(-1);
    UNSCOPED_INFO("a post that cannot allocate leaves the resource with its holder");
    REQUIRE((!posted && resource && *resource == 1 && returns.QueuedCount() == 0 && returns.QueuedBytes() == 0));
    UNSCOPED_INFO("a later post succeeds");
    REQUIRE((returns.Post(1, resource, MiB) && !resource));
    allocationsBeforeFailure.store(0);
    const auto pooled = returns.Drain(pool, 5, admitAll);
    allocationsBeforeFailure.store(-1);
    UNSCOPED_INFO("a drained pending return that cannot allocate is released");
    REQUIRE((pooled == 0 && keyedReleases == 1 && pool.HeldCount() == 0 && pool.Stats().pendingBytes == 0));
}

// Miss log buckets reuse age and lists the most missed keys first.
void missLog()
{
    PoolMissLog<int, 2> log;
    allocationsBeforeFailure.store(0);
    log.Missed(1, 0);
    log.Returned(1, 5);
    for (const std::uint64_t epoch : { 5, 6, 7, 8, 9, 12, 13, 20, 21, 36, 37, 68, 69, 200 })
        log.Missed(1, epoch);
    log.Returned(2, 0);
    log.Missed(2, 2);
    log.Missed(3, 2);
    log.Returned(3, 2);
    allocationsBeforeFailure.store(-1);
    std::array<PoolMissLog<int, 2>::Entry, 3> top;
    std::uint64_t                             untracked = 0;
    log.Top(top, untracked);
    // Gaps 0 1 2 3 4 7 8 15 16 31 32 63 64 195, then the miss before any return.
    const std::array<std::uint64_t, kPoolMissGapBuckets> gaps{ 1, 1, 2, 2, 2, 2, 2, 2, 1 };
    UNSCOPED_INFO("the miss log buckets one key's misses by return gap");
    REQUIRE((untracked == 1 && top[0].key == 1 && top[0].misses == 15 && top[0].gaps == gaps));
    UNSCOPED_INFO("the miss log lists keys by misses and leaves unused slots empty");
    REQUIRE((top[1].key == 2 && top[1].misses == 1 && top[1].gaps[2] == 1 && top[2].misses == 0));
}

// A texture-shaped key, printed by its allocation fields.
struct ReportTextureKey
{
    std::uint32_t dimension = 3, height = 0, format = 0, samples = 1, quality = 0, layout = 0, flags = 0;
    std::uint64_t width            = 0;
    std::uint16_t depthOrArraySize = 1, mipLevels = 1;
    bool          castable = false;

    auto operator<=>(const ReportTextureKey&) const = default;
};

// Reports are due each kPoolReportMisses misses, capped at kPoolReportLimit.
void keyedReport()
{
    keyedReleases = 0;
    KeyedPool pool(2 * MiB);
    pool.Return(1, counted(10), MiB, 7);
    UNSCOPED_INFO("a fenced key and an unknown key miss");
    REQUIRE((!pool.Take(1, 6) && !pool.Take(2, 6)));
    auto taken = pool.Take(1, 7);
    UNSCOPED_INFO("the completed retirement is taken");
    REQUIRE(bool(taken));
    pool.Return(1, std::move(taken.resource), MiB, 0);
    pool.Return(3, counted(30), MiB, 0);
    pool.Return(4, counted(40), MiB, 0);
    UNSCOPED_INFO("the evicted key misses");
    REQUIRE((keyedReleases == 1 && !pool.Take(1, 7)));
    const auto stats = pool.Stats();
    UNSCOPED_INFO("only the miss behind a fence is pending");
    REQUIRE((stats.misses == 3 && stats.pendingMisses == 1 && stats.evictions == 1));
    UNSCOPED_INFO("the report splits misses and lists keys by misses, evictions and idle holdings: " + pool.Report());
    REQUIRE(pool.Report() == "cap_mib=2 hits=1 misses=3 pending_misses=1 absent_misses=2 returns=4 evictions=1 idle_mb=2.0 pending_mb=0.0 keys=4 untracked_events=0"
                             " k_1=1/1/1/2/1/0/0.0 k_2=0/0/1/0/0/0/0.0 k_3=0/0/0/1/0/1/1.0 k_4=0/0/0/1/0/1/1.0");
    UNSCOPED_INFO("the report lists at most the requested keys");
    REQUIRE(pool.Report(2).ends_with(" k_2=0/0/1/0/0/0/0.0"));

    for (std::uint64_t miss = 3; miss + 1 < kPoolReportMisses; ++miss)
        (void)pool.Take(9, 7);
    UNSCOPED_INFO("a report is not due one miss before the interval");
    REQUIRE(!pool.ReportDue());
    (void)pool.Take(9, 7);
    UNSCOPED_INFO("a report is due once at the interval");
    REQUIRE((pool.ReportDue() && !pool.ReportDue()));
    std::uint64_t due = 1;
    for (std::uint64_t miss = 1; miss <= kPoolReportMisses * (kPoolReportLimit + 1); ++miss)
    {
        (void)pool.Take(9, 7);
        const bool expected = miss % kPoolReportMisses == 0 && due < kPoolReportLimit;
        UNSCOPED_INFO(std::format("a report is due exactly every {} misses up to the limit (miss {})", kPoolReportMisses, miss));
        REQUIRE(pool.ReportDue() == expected);
        due += expected ? 1 : 0;
    }
    UNSCOPED_INFO("reports stop at the limit");
    REQUIRE(due == kPoolReportLimit);

    FencedResourcePool<ReportTextureKey, Counted> textures(8 * MiB);
    ReportTextureKey                              color{ .height = 720, .format = 28, .flags = 1, .width = 1280 };
    ReportTextureKey                              other{ .height = 720, .format = 24, .samples = 4, .flags = 0x14, .width = 1280, .mipLevels = 2, .castable = true };
    (void)textures.Take(color, 0);
    textures.Return(other, counted(1), MiB, 0);
    UNSCOPED_INFO("texture keys print their allocation: " + textures.Report());
    REQUIRE(textures.Report().ends_with(" k_1280x720x1_f28_g1=0/0/1/0/0/0/0.0 k_1280x720x4_f24_g14_c_m2=0/0/0/1/0/1/1.0"));
}

void resolveFrameSets()
{
    keyedReleases = 0;

    constexpr std::uint64_t kAligned = 64 * KiB;
    const auto              aligned  = [](std::uint64_t width, std::uint64_t height)
    {
        return (width * height * 4 + kAligned - 1) / kAligned * kAligned;
    };

    struct Target
    {
        int           key;
        std::uint64_t bytes;
    };

    std::vector<Target> set;
    for (int index = 0; index < 6; ++index)
        set.push_back({ 5, aligned(900, 675) });
    for (int index = 0; index < 2; ++index)
        set.push_back({ 1, aligned(1024, 1024) });
    for (int index = 0; index < 8; ++index)
        set.push_back({ 2, aligned(1280, 720) });
    for (int index = 0; index < 2; ++index)
        set.push_back({ 3, aligned(640, 360) });
    set.push_back({ 4, aligned(320, 180) });
    std::uint64_t setBytes = 0;
    for (const auto& target : set)
        setBytes += target.bytes;
    // Heaviest world frame: 52.875 MiB of 64 KiB-aligned resolve destinations.
    UNSCOPED_INFO("the heaviest frame's set fits the resolve cap");
    REQUIRE((setBytes == 52 * MiB + 896 * KiB && setBytes <= kResolvePoolIdleCapBytes));
    KeyedPool     pool(kResolvePoolIdleCapBytes);
    KeyedReturns  returns;
    std::uint64_t hits = 0, created = 0;
    for (std::uint64_t frame = 1; frame <= 10; ++frame)
    {
        std::vector<Counted> live;
        for (const auto& target : set)
        {
            auto taken = pool.Take(target.key, frame - 1);
            if (taken)
                ++hits;
            else
            {
                ++created;
                taken.resource = counted(target.key);
            }
            live.push_back(std::move(taken.resource));
        }
        for (std::size_t index = 0; index < set.size(); ++index)
            returns.Post(set[index].key, live[index], set[index].bytes);
        returns.Drain(pool, frame, admitAll);
    }
    const auto stats = pool.Stats();
    UNSCOPED_INFO("after the first frame every resolve destination is reused without eviction");
    REQUIRE((created == set.size() && hits == 9 * set.size() && stats.evictions == 0 && keyedReleases == 0 && stats.peakHeldBytes == setBytes));
}

void d3d12Check()
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
    {
        SKIP("no D3D12 device");
    }
    FencedUploadPool<NativeUploadBuffer> pool([&](std::uint64_t bytes)
                                              {
                                                  return CreateNativeUploadBuffer(device.Get(), bytes);
                                              });
    auto                                 lease = pool.Acquire(100 * KiB, 0);
    UNSCOPED_INFO("a pooled upload buffer is mapped with a GPU address");
    REQUIRE((lease && lease.buffer().mapped && lease.buffer().gpu != 0));
    const auto desc = lease.buffer().resource->GetDesc();
    UNSCOPED_INFO("the buffer has its class size");
    REQUIRE((desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && desc.Width == lease.bytes() && lease.bytes() == 128 * KiB));
    D3D12_HEAP_PROPERTIES heap{};
    UNSCOPED_INFO("the buffer is on the upload heap");
    REQUIRE((SUCCEEDED(lease.buffer().resource->GetHeapProperties(&heap, nullptr)) && heap.Type == D3D12_HEAP_TYPE_UPLOAD));
    std::memset(lease.buffer().mapped, 0x5A, static_cast<std::size_t>(lease.bytes()));
    const auto* resource = lease.buffer().resource.Get();
    auto*       mapped   = lease.buffer().mapped;
    pool.Retire(std::move(lease), 0);
    auto again = pool.Acquire(64 * KiB, 0);
    UNSCOPED_INFO("reuse keeps the resource and its persistent mapping");
    REQUIRE((again.reused() && again.buffer().resource.Get() == resource && again.buffer().mapped == mapped && again.buffer().mapped[again.bytes() - 1] == 0x5A));
    pool.Discard(std::move(again));
    UNSCOPED_INFO("discarding releases the device buffer");
    REQUIRE(pool.Stats().liveBytes == 0);
}

} // namespace

TEST_CASE("native_upload_pool", "[ctest][native-upload-pool][timeout-120]")
{
    const DisarmCountdown disarm;
    classes();
    fencedReuse();
    classSpan();
    newestFirstAndCap();
    bypassAndOrdering();
    failures();
    detach();
    bookkeepingFailures();
    retireToOwner();
    steadyFrames();
    keyedExactReuse();
    keyedFence();
    keyedCap();
    keyedDetachAndBookkeeping();
    keyedBookkeepingFailures();
    returnQueueFence();
    returnQueueAdmit();
    returnQueueThreads();
    returnQueueCloseAndDetach();
    returnQueueBookkeeping();
    resolveFrameSets();
    missLog();
    keyedReport();
}

TEST_CASE("native_upload_pool_d3d12", "[ctest][release-smoke][native-upload-pool][native_gpu][timeout-120]")
{
    d3d12Check();
}
