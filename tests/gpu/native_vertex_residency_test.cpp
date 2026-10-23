#include "gpu/d3d12/native_vertex_residency.h"
#include "manual_cases.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace rerevved::gpu::vertex_residency;

namespace
{

constexpr std::uint64_t KiB = 1024, MiB = 1024 * KiB;

VertexOwner makeOwner(std::size_t bytes, std::uint8_t seed = 1)
{
    auto data = std::make_shared<std::vector<std::uint8_t>>(bytes);
    for (std::size_t index = 0; index < bytes; ++index)
        (*data)[index] = static_cast<std::uint8_t>(seed + index);
    return data;
}

// A small table whose chunk geometry the cases can fill by hand.
Limits smallLimits()
{
    Limits limits;
    limits.chunkBytes    = 4 * KiB;
    limits.chunkCount    = 4;
    limits.maxEntries    = 64;
    limits.buckets       = 16;
    limits.snapshotBytes = 8 * KiB;
    limits.hotChunkBytes = KiB;
    return limits;
}

struct Fixture
{
    VertexResidency res;
    std::uint64_t   completed = 0;

    explicit Fixture(Mode mode = Mode::Owner, const Limits& limits = smallLimits())
    : res(mode, limits)
    {
        res.BeginBatch(0);
    }

    Request request(const VertexOwner& owner, std::uint64_t offset = 0, std::uint64_t length = ~std::uint64_t(0), std::uint32_t batch = 0) const
    {
        Request request;
        request.owner      = &owner;
        request.offset     = offset;
        request.length     = length == ~std::uint64_t(0) ? owner->size() - offset : length;
        request.generation = res.Generation();
        request.batch      = batch;
        request.frameDraw = request.frameBatch = true;
        return request;
    }

    Decision plan(const Request& request)
    {
        return res.Plan(request, [this]
                        {
                            return completed;
                        });
    }

    Decision plan(const VertexOwner& owner, std::uint64_t offset = 0, std::uint64_t length = ~std::uint64_t(0), std::uint32_t batch = 0)
    {
        return plan(request(owner, offset, length, batch));
    }

    // Plans a span that must miss and publishes it from `batch`.
    Decision admit(const VertexOwner& owner, std::uint64_t offset = 0, std::uint64_t length = ~std::uint64_t(0), std::uint32_t batch = 0)
    {
        const auto decision = plan(owner, offset, length, batch);
        UNSCOPED_INFO("a new span misses");
        CHECK(decision.kind == Decision::Kind::Miss);
        UNSCOPED_INFO("a reserved span commits");
        CHECK(res.Commit(decision, batch));
        return decision;
    }
};

void testDefaults()
{
    const Limits limits;
    UNSCOPED_INFO("the documented chunk, record, bucket and snapshot bounds");
    CHECK((limits.chunkBytes == 16 * MiB && limits.chunkCount == 4 && limits.maxEntries == 4096 && limits.buckets == 8192 && limits.snapshotBytes == 64 * MiB && limits.batches == 2 && limits.hotChunkBytes == 4 * MiB));
    VertexResidency off(Mode::Off);
    UNSCOPED_INFO("an Off table holds and charges nothing");
    CHECK((off.FootprintBytes() == 0 && off.MetadataBytes() == 0 && off.SnapshotCeilingBytes() == 0));
    const auto owner = makeOwner(64);
    Request    request;
    request.owner     = &owner;
    request.length    = 64;
    request.frameDraw = request.frameBatch = true;
    UNSCOPED_INFO("an Off table plans nothing");
    CHECK(off.Plan(request, []
                   {
                       return std::uint64_t(0);
                   })
              .kind == Decision::Kind::Off);
    UNSCOPED_INFO("an Off table reports no state");
    CHECK((off.Entries() == 0 && off.PendingPins() == 0 && off.RetiredBytes() == 0));
    VertexResidency owner_mode(Mode::Owner);
    VertexResidency exact(Mode::Exact);
    UNSCOPED_INFO("the footprint is the fixed chunk bytes");
    CHECK((owner_mode.FootprintBytes() == 64 * MiB && exact.FootprintBytes() == 64 * MiB));
    UNSCOPED_INFO("only Exact charges the snapshot ceiling");
    CHECK((owner_mode.SnapshotCeilingBytes() == 0 && exact.SnapshotCeilingBytes() == 64 * MiB));
    UNSCOPED_INFO("metadata is a fixed charge");
    CHECK((owner_mode.MetadataBytes() == exact.MetadataBytes() && owner_mode.MetadataBytes() > 0));
    UNSCOPED_INFO("the reason names");
    CHECK((kReasonNames.size() == 10 && std::string_view(kReasonNames[0]) == "outside_frame" && std::string_view(kReasonNames[9]) == "disabled_after_init_failure"));
}

void testKeyIdentity()
{
    Fixture    f;
    const auto owner = makeOwner(1000);
    const auto first = f.admit(owner);
    const auto again = f.plan(owner);
    UNSCOPED_INFO("the same owner and span hit its resident span");
    CHECK((again.kind == Decision::Kind::Hit && again.where.chunk == first.where.chunk && again.where.offset == first.where.offset));
    UNSCOPED_INFO("a hit reports the span length");
    CHECK(again.length == 1000);

    const auto equalBytes = makeOwner(1000);
    UNSCOPED_INFO("equal bytes under another owner miss");
    CHECK((*equalBytes == *owner && f.plan(equalBytes).kind == Decision::Kind::Miss));

    // Two control blocks designating the same object.
    const std::vector<std::uint8_t> object(1000, 7);
    const auto                      blockA = std::make_shared<int>(1);
    const auto                      blockB = std::make_shared<int>(2);
    const VertexOwner               viaA(blockA, &object), viaB(blockB, &object), viaAOther(blockA, &object);
    f.admit(viaA);
    UNSCOPED_INFO("a copy of the owner hits");
    CHECK(f.plan(viaAOther).kind == Decision::Kind::Hit);
    UNSCOPED_INFO("the same pointer under another control block misses");
    CHECK(f.plan(viaB).kind == Decision::Kind::Miss);

    // One control block designating two objects.
    const std::vector<std::uint8_t> other(1000, 9);
    const VertexOwner               aliasOther(blockA, &other);
    UNSCOPED_INFO("another object under the same control block misses");
    CHECK(f.plan(aliasOther).kind == Decision::Kind::Miss);

    // Subspans are distinct keys.
    const auto big = makeOwner(2048, 3);
    f.admit(big, 0, 512);
    UNSCOPED_INFO("the span hits");
    CHECK(f.plan(big, 0, 512).kind == Decision::Kind::Hit);
    UNSCOPED_INFO("a longer span from the same offset misses");
    CHECK(f.plan(big, 0, 768).kind == Decision::Kind::Miss);
    UNSCOPED_INFO("the same length at another offset misses");
    CHECK(f.plan(big, 256, 512).kind == Decision::Kind::Miss);
    const auto sub = f.admit(big, 256, 512);
    UNSCOPED_INFO("subspans occupy distinct spans");
    CHECK((f.plan(big, 0, 512).where.offset != sub.where.offset || f.plan(big, 0, 512).where.chunk != sub.where.chunk));
}

std::uint64_t constantHash(const void*, std::uint64_t, std::uint64_t) noexcept
{
    return 12345;
}

void testCollisions()
{
    auto limits = smallLimits();
    limits.hash = constantHash;
    Fixture    f(Mode::Owner, limits);
    const auto a = makeOwner(300, 1), b = makeOwner(300, 2), c = makeOwner(300, 3);
    const auto da = f.admit(a), db = f.admit(b);
    UNSCOPED_INFO("colliding keys keep distinct spans");
    CHECK((da.where.offset != db.where.offset || da.where.chunk != db.where.chunk));
    UNSCOPED_INFO("colliding keys each hit their own span");
    CHECK((f.plan(a).where.offset == da.where.offset && f.plan(b).where.offset == db.where.offset));
    UNSCOPED_INFO("a third colliding key misses and evicts neither");
    CHECK(f.plan(c).kind == Decision::Kind::Miss);
    UNSCOPED_INFO("collision does not evict a live key");
    CHECK(f.res.Entries() == 2);
    Limits one  = smallLimits();
    one.buckets = 1;
    Fixture    g(Mode::Owner, one);
    const auto x = makeOwner(256, 5), y = makeOwner(256, 6);
    const auto dx = g.admit(x), dy = g.admit(y);
    UNSCOPED_INFO("one bucket chains every key");
    CHECK((g.plan(x).where.offset == dx.where.offset && g.plan(y).where.offset == dy.where.offset));
}

// With every key colliding, only the key comparison separates owners and spans.
void testKeyUnderCollision()
{
    auto limits = smallLimits();
    limits.hash = constantHash;
    Fixture                         f(Mode::Owner, limits);
    const auto                      big = makeOwner(2048, 3);
    const std::vector<std::uint8_t> object(2048, 7), other(2048, 9);
    const auto                      blockA = std::make_shared<int>(1);
    const auto                      blockB = std::make_shared<int>(2);
    const VertexOwner               viaA(blockA, &object), viaB(blockB, &object), aliasOther(blockA, &other);
    f.admit(big, 0, 512);
    f.admit(viaA, 0, 512);
    UNSCOPED_INFO("each colliding key hits its own span");
    CHECK((f.plan(big, 0, 512).kind == Decision::Kind::Hit && f.plan(viaA, 0, 512).kind == Decision::Kind::Hit));
    UNSCOPED_INFO("a longer span misses");
    CHECK(f.plan(big, 0, 768).kind == Decision::Kind::Miss);
    UNSCOPED_INFO("another offset misses");
    CHECK(f.plan(big, 256, 512).kind == Decision::Kind::Miss);
    UNSCOPED_INFO("equal bytes under another owner miss");
    CHECK(f.plan(makeOwner(2048, 3), 0, 512).kind == Decision::Kind::Miss);
    UNSCOPED_INFO("another control block over the same object misses");
    CHECK(f.plan(viaB, 0, 512).kind == Decision::Kind::Miss);
    UNSCOPED_INFO("another object under the same control block misses");
    CHECK(f.plan(aliasOther, 0, 512).kind == Decision::Kind::Miss);
}

void testEligibility()
{
    Fixture    f;
    const auto owner  = makeOwner(512);
    auto       reason = [&](Request request)
    {
        const auto decision = f.plan(request);
        UNSCOPED_INFO("the request falls back");
        CHECK(decision.kind == Decision::Kind::Fallback);
        return decision.reason;
    };
    auto standalone      = f.request(owner);
    standalone.frameDraw = false;
    UNSCOPED_INFO("a standalone draw falls back as outside_frame");
    CHECK(reason(standalone) == Reason::OutsideFrame);
    standalone            = f.request(owner);
    standalone.frameBatch = false;
    UNSCOPED_INFO("a draw without a frame batch falls back as outside_frame");
    CHECK(reason(standalone) == Reason::OutsideFrame);

    auto inlineBytes  = f.request(owner);
    inlineBytes.owner = nullptr;
    UNSCOPED_INFO("inline bytes fall back as owner");
    CHECK(reason(inlineBytes) == Reason::Owner);
    const VertexOwner nullOwner;
    inlineBytes.owner = &nullOwner;
    UNSCOPED_INFO("a null owner falls back as owner");
    CHECK(reason(inlineBytes) == Reason::Owner);
    const std::vector<std::uint8_t> object(512, 1);
    const VertexOwner               aliasWithoutBlock(std::shared_ptr<void>{}, &object);
    UNSCOPED_INFO("the alias has a pointer and no control block");
    CHECK((aliasWithoutBlock.get() && !aliasWithoutBlock.use_count()));
    inlineBytes.owner = &aliasWithoutBlock;
    UNSCOPED_INFO("an empty-owner alias falls back as owner");
    CHECK(reason(inlineBytes) == Reason::Owner);

    UNSCOPED_INFO("an empty span falls back as bounds");
    CHECK(reason(f.request(owner, 0, 0)) == Reason::Bounds);
    UNSCOPED_INFO("an offset past the end falls back as bounds");
    CHECK(reason(f.request(owner, 513, 1)) == Reason::Bounds);
    UNSCOPED_INFO("a span past the end falls back as bounds");
    CHECK(reason(f.request(owner, 100, 413)) == Reason::Bounds);
    auto overflow = f.request(owner, ~std::uint64_t(0) - 3, 8);
    UNSCOPED_INFO("an overflowing span falls back as bounds");
    CHECK(reason(overflow) == Reason::Bounds);
    UNSCOPED_INFO("an empty span at the end falls back");
    CHECK(f.plan(owner, 512, 0).kind == Decision::Kind::Fallback);
    UNSCOPED_INFO("the last byte is a valid span");
    CHECK(f.plan(owner, 511, 1).kind == Decision::Kind::Miss);

    auto generation = f.request(owner);
    generation.generation += 1;
    UNSCOPED_INFO("another device generation falls back as generation");
    CHECK(reason(generation) == Reason::Generation);

    UNSCOPED_INFO("a batch that is not recording falls back as scheduling");
    CHECK(reason(f.request(owner, 0, 512, 1)) == Reason::Scheduling);
    f.res.BeginBatch(1);
    UNSCOPED_INFO("a recording batch plans");
    CHECK(f.plan(owner, 0, 512, 1).kind == Decision::Kind::Miss);
    UNSCOPED_INFO("an unknown batch falls back as scheduling");
    CHECK(reason(f.request(owner, 0, 512, 7)) == Reason::Scheduling);
}

void testGenerationKey()
{
    Fixture    f;
    const auto owner = makeOwner(512);
    f.admit(owner);
    UNSCOPED_INFO("the owner hits in its generation");
    CHECK(f.plan(owner).kind == Decision::Kind::Hit);
    // The same owner, epoch and addresses in a new generation, with no clear.
    f.res.SetGenerationForTesting(f.res.Generation() + 1);
    UNSCOPED_INFO("nothing was cleared");
    CHECK(f.res.Entries() == 1);
    const auto decision = f.plan(owner);
    UNSCOPED_INFO("a key from another generation never hits");
    CHECK(decision.kind == Decision::Kind::Miss);
    f.res.Abandon(decision);

    // A request still carrying the old generation is refused outright.
    Fixture    stale;
    const auto old = stale.request(owner);
    stale.admit(owner);
    stale.res.SetGenerationForTesting(stale.res.Generation() + 1);
    const auto refused = stale.plan(old);
    UNSCOPED_INFO("a request from another generation falls back");
    CHECK((refused.kind == Decision::Kind::Fallback && refused.reason == Reason::Generation));

    Fixture g;
    g.admit(owner);
    g.res.SetGeneration(9);
    UNSCOPED_INFO("a new generation drops everything");
    CHECK((g.res.Entries() == 0 && g.res.Generation() == 9));
    g.res.BeginBatch(0);
    UNSCOPED_INFO("nothing survives a generation");
    CHECK(g.plan(owner).kind == Decision::Kind::Miss);
}

void testBarrierState()
{
    Fixture    f;
    const auto owner = makeOwner(512, 1), second = makeOwner(512, 2), third = makeOwner(512, 3);
    auto       missing = f.plan(owner);
    UNSCOPED_INFO("a first copy into a chunk in COMMON needs no barrier before it");
    CHECK((missing.kind == Decision::Kind::Miss && !f.res.NeedsPreCopyBarrier(0, missing.where.chunk)));
    UNSCOPED_INFO("commit");
    CHECK(f.res.Commit(missing, 0));
    UNSCOPED_INFO("after a copy and its barrier the chunk is a vertex buffer");
    CHECK(f.res.NeedsPreCopyBarrier(0, missing.where.chunk));
    // Two admissions into the same chunk with a draw between them.
    const auto next = f.plan(second);
    UNSCOPED_INFO("the second copy transitions from the vertex state first");
    CHECK((next.kind == Decision::Kind::Miss && next.where.chunk == missing.where.chunk && f.res.NeedsPreCopyBarrier(0, next.where.chunk)));
    f.res.Commit(next, 0);
    // A hit promotes the chunk in a list that has not touched it.
    f.res.SubmitBatch(0, 1);
    f.res.BeginBatch(1);
    UNSCOPED_INFO("each list starts with its chunks in COMMON");
    CHECK(!f.res.NeedsPreCopyBarrier(1, missing.where.chunk));
    UNSCOPED_INFO("a hit promotes the chunk for vertex use");
    CHECK((f.plan(owner, 0, 512, 1).kind == Decision::Kind::Hit && f.res.NeedsPreCopyBarrier(1, missing.where.chunk)));
    f.res.BeginBatch(0);
    UNSCOPED_INFO("a new list forgets the previous list's state");
    CHECK(!f.res.NeedsPreCopyBarrier(0, missing.where.chunk));
    (void)third;
}

void testAlignmentAndFill()
{
    Fixture                  f(Mode::Owner, Limits{});
    std::vector<VertexOwner> owners;
    std::uint64_t            expectedOffset = 0;
    std::uint32_t            chunk          = 0;
    // Four chunks of sixteen 1 MiB spans, plus an unaligned one.
    for (int index = 0; index < 64; ++index)
    {
        owners.push_back(makeOwner(MiB - (index == 3 ? 100 : 0), std::uint8_t(index)));
        const auto decision = f.admit(owners.back());
        UNSCOPED_INFO("spans bump through a chunk and then the next");
        CHECK((decision.where.chunk == chunk && decision.where.offset == expectedOffset));
        UNSCOPED_INFO("every span is 256-byte aligned");
        CHECK(decision.where.offset % 256 == 0);
        expectedOffset += MiB;
        if (expectedOffset == 16 * MiB)
        {
            expectedOffset = 0;
            ++chunk;
        }
    }
    UNSCOPED_INFO("the footprint stays fixed");
    CHECK(f.res.FootprintBytes() == 64 * MiB);
    UNSCOPED_INFO("live payload excludes alignment padding");
    CHECK((f.res.Entries() == 64 && f.res.PayloadBytes() == 64 * MiB - 100));
    // Every chunk was used in the current swap: a new span falls back.
    auto extra = makeOwner(MiB);
    auto cap   = f.plan(extra);
    UNSCOPED_INFO("a full table with only recent chunks falls back as cap");
    CHECK((cap.kind == Decision::Kind::Fallback && cap.reason == Reason::Cap));
    f.res.AdvanceSwap();
    cap = f.plan(extra);
    UNSCOPED_INFO("chunks of the previous swap are still protected");
    CHECK((cap.kind == Decision::Kind::Fallback && cap.reason == Reason::Cap && f.res.counters().evictions == 0));
    UNSCOPED_INFO("protection keeps every key");
    CHECK(f.res.Entries() == 64);
    // A span larger than a chunk can never fit.
    const auto huge = makeOwner(16 * MiB + 1);
    f.res.AdvanceSwap();
    f.res.AdvanceSwap();
    const auto tooBig = f.plan(huge);
    UNSCOPED_INFO("a span larger than a chunk falls back as cap without evicting");
    CHECK((tooBig.kind == Decision::Kind::Fallback && tooBig.reason == Reason::Cap && f.res.counters().evictions == 0));
}

void testOldestFirstEviction()
{
    Fixture                  f;
    std::vector<VertexOwner> owners;
    for (int index = 0; index < 8; ++index)
    {
        owners.push_back(makeOwner(2 * KiB, std::uint8_t(index)));
        f.admit(owners.back()); // two per chunk
    }
    f.res.SubmitBatch(0, 1);
    f.completed = 1;
    f.res.BeginBatch(0);
    // Chunk 0 is the oldest, chunk 3 the newest.
    f.res.AdvanceSwap();
    f.res.AdvanceSwap();
    // Refresh chunk 1 so chunk 0 stays the oldest and chunk 1 becomes the newest but one.
    UNSCOPED_INFO("chunk 1 hits");
    CHECK(f.plan(owners[2]).kind == Decision::Kind::Hit);
    f.res.AdvanceSwap();
    f.res.AdvanceSwap();
    const auto extra = makeOwner(2 * KiB, 99);
    const auto miss  = f.plan(extra);
    UNSCOPED_INFO("the oldest chunk is evicted whole and reused");
    CHECK((miss.kind == Decision::Kind::Miss && miss.where.chunk == 0 && miss.where.offset == 0));
    UNSCOPED_INFO("one chunk was evicted");
    CHECK(f.res.counters().evictions == 1);
    UNSCOPED_INFO("every key of the evicted chunk left the table");
    CHECK((f.plan(owners[0]).kind == Decision::Kind::Miss && f.plan(owners[1]).kind == Decision::Kind::Miss));
    UNSCOPED_INFO("other chunks keep their keys");
    CHECK((f.plan(owners[2]).kind == Decision::Kind::Hit && f.plan(owners[6]).kind == Decision::Kind::Hit));
    f.res.Abandon(miss);
}

void testPendingFences()
{
    Fixture                  f;
    std::vector<VertexOwner> owners;
    for (int index = 0; index < 8; ++index)
    {
        owners.push_back(makeOwner(2 * KiB, std::uint8_t(index)));
        f.admit(owners.back());
    }
    // The first list is still recording: every chunk is pinned.
    UNSCOPED_INFO("each chunk the list wrote is pinned once");
    CHECK(f.res.PendingPins() == 4);
    f.res.AdvanceSwap();
    f.res.AdvanceSwap();
    f.completed = 100;
    auto extra  = makeOwner(2 * KiB, 77);
    auto pinned = f.plan(extra);
    UNSCOPED_INFO("a pinned oldest chunk is evicted logically but not reused");
    CHECK((pinned.kind == Decision::Kind::Fallback && pinned.reason == Reason::Cap));
    UNSCOPED_INFO("its keys left the table");
    CHECK((f.res.Entries() == 6 && f.res.counters().evictions == 1));
    UNSCOPED_INFO("its bytes stay held");
    CHECK((f.res.RetiredBytes() == 4 * KiB && f.res.FootprintBytes() == 4 * 4 * KiB));
    // A newer chunk is not skipped to.
    pinned = f.plan(extra);
    UNSCOPED_INFO("a pending chunk blocks reuse without skipping to a newer one");
    CHECK((pinned.kind == Decision::Kind::Fallback && pinned.reason == Reason::Cap && f.res.counters().evictions == 1));
    // Submit: the last-use fence now protects the chunk.
    f.res.SubmitBatch(0, 50);
    f.completed = 49;
    f.res.BeginBatch(0);
    pinned = f.plan(extra);
    UNSCOPED_INFO("one less than the fence cannot recycle the chunk");
    CHECK((pinned.kind == Decision::Kind::Fallback && pinned.reason == Reason::Cap && f.res.RetiredBytes() == 4 * KiB));
    UNSCOPED_INFO("the chunk carries its last-use fence");
    CHECK((f.res.Chunk(0).state == ChunkInfo::State::Retired && f.res.Chunk(0).lastUseFence == 50));
    f.completed = 50;
    f.res.BeginBatch(0);
    pinned = f.plan(extra);
    UNSCOPED_INFO("at the fence the chunk is recycled");
    CHECK((pinned.kind == Decision::Kind::Miss && pinned.where.chunk == 0 && pinned.where.offset == 0));
    UNSCOPED_INFO("reuse advances the chunk's serial");
    CHECK((f.res.Chunk(0).reuseSerial == 1 && f.res.counters().chunkReuses == 1));
    f.res.Abandon(pinned);
}

void testLastUseIsNotInsertionFence()
{
    Fixture    f;
    const auto owner = makeOwner(2 * KiB, 1);
    f.admit(owner);
    f.res.SubmitBatch(0, 10);
    f.completed = 10;
    // A later list hits the resident span, extending the chunk's last use.
    f.res.BeginBatch(1);
    UNSCOPED_INFO("a later list hits");
    CHECK(f.plan(owner, 0, 2 * KiB, 1).kind == Decision::Kind::Hit);
    UNSCOPED_INFO("the hit pins the chunk");
    CHECK(f.res.Chunk(0).pins == 1);
    f.res.SubmitBatch(1, 20);
    UNSCOPED_INFO("the hit's fence is the chunk's last use");
    CHECK((f.res.Chunk(0).lastUseFence == 20 && f.res.Chunk(0).pins == 0));
    // Retire the chunk: its keys go, its bytes stay until fence 20.
    f.res.FailPacket();
    f.completed = 10;
    f.res.Reap(f.completed);
    UNSCOPED_INFO("the insertion fence alone does not release the chunk");
    CHECK(f.res.Chunk(0).state == ChunkInfo::State::Retired);
    f.res.Reap(UINT64_MAX);
    f.res.BeginPacket(UINT64_MAX);
    UNSCOPED_INFO("UINT64_MAX is never completion for a pending chunk");
    CHECK((f.res.Chunk(0).state == ChunkInfo::State::Retired && f.res.counters().chunkReuses == 0));
    f.completed = 20;
    f.res.Reap(f.completed);
    UNSCOPED_INFO("the last-use fence releases the chunk");
    CHECK(f.res.Chunk(0).state == ChunkInfo::State::Empty);
    f.res.Reap(UINT64_MAX);
    UNSCOPED_INFO("UINT64_MAX is never completion");
    CHECK(f.res.counters().chunkReuses == 1);
}

void testHotChunkReclaim()
{
    Fixture                  f(Mode::Owner, Limits{});
    std::vector<VertexOwner> cold, filler;
    const auto               hot = makeOwner(MiB, 200);
    f.admit(hot);
    for (int index = 0; index < 15; ++index)
    {
        cold.push_back(makeOwner(MiB, std::uint8_t(index)));
        f.admit(cold.back());
    }
    for (int index = 0; index < 12; ++index)
    {
        filler.push_back(makeOwner(4 * MiB, std::uint8_t(index)));
        f.admit(filler.back());
    }
    f.res.SubmitBatch(0, 1);
    f.completed = 1;
    f.res.BeginBatch(0);
    const auto extra = makeOwner(MiB, 55);
    // Every chunk is recent and chunk 0 holds many live bytes.
    auto full = f.plan(extra);
    UNSCOPED_INFO("a full table of live recent chunks falls back as cap");
    CHECK((full.kind == Decision::Kind::Fallback && full.reason == Reason::Cap));
    // The cold owners expire: chunk 0 now holds one hot entry and 15 MiB of expired ones.
    cold.clear();
    const auto reclaimed = f.plan(extra);
    UNSCOPED_INFO("a recent chunk with few live bytes is reclaimed");
    CHECK((reclaimed.kind == Decision::Kind::Miss && reclaimed.where.chunk == 0 && reclaimed.where.offset == 0));
    UNSCOPED_INFO("one eviction after fifteen expiries");
    CHECK((f.res.counters().evictions == 1 && f.res.counters().expiries == 15));
    f.res.Abandon(reclaimed);
    UNSCOPED_INFO("the hot entry is re-admitted on its next miss");
    CHECK(f.plan(hot).kind == Decision::Kind::Miss);

    // A chunk the queue still needs is not reclaimed.
    Fixture                  g(Mode::Owner, smallLimits());
    std::vector<VertexOwner> small;
    for (int index = 0; index < 4; ++index)
    {
        small.push_back(makeOwner(4 * KiB, std::uint8_t(index)));
        g.admit(small.back());
    }
    small[0].reset();
    const auto blocked = g.plan(makeOwner(KiB, 9));
    UNSCOPED_INFO("a pinned chunk is not reclaimed");
    CHECK((blocked.kind == Decision::Kind::Fallback && blocked.reason == Reason::Cap && g.res.counters().evictions == 0));
    g.res.SubmitBatch(0, 5);
    g.completed = 4;
    g.res.BeginBatch(0);
    UNSCOPED_INFO("a chunk whose fence has not completed is not reclaimed");
    CHECK((g.plan(makeOwner(KiB, 9)).kind == Decision::Kind::Fallback && g.res.counters().evictions == 0));
    g.completed     = 5;
    const auto okay = g.plan(makeOwner(KiB, 9));
    UNSCOPED_INFO("its fence allows the reclaim");
    CHECK((okay.kind == Decision::Kind::Miss && okay.where.chunk == 0));

    // Of several reclaimable chunks the one with the fewest live bytes goes,
    // even when an older chunk qualifies.
    Fixture                  h(Mode::Owner, smallLimits());
    const auto               keep = makeOwner(512, 1);
    std::vector<VertexOwner> rest;
    VertexOwner              padding = makeOwner(3584, 2), vanishing = makeOwner(4 * KiB, 3);
    h.admit(keep);
    h.admit(padding);
    h.admit(vanishing);
    for (int index = 0; index < 2; ++index)
    {
        rest.push_back(makeOwner(4 * KiB, std::uint8_t(10 + index)));
        h.admit(rest.back());
    }
    padding.reset();
    vanishing.reset();
    h.res.SubmitBatch(0, 1);
    h.completed = 1;
    h.res.BeginBatch(0);
    const auto fewest = h.plan(makeOwner(KiB, 9));
    UNSCOPED_INFO("the chunk with the fewest live bytes is reclaimed");
    CHECK((fewest.kind == Decision::Kind::Miss && fewest.where.chunk == 1 && h.res.counters().evictions == 1));
    UNSCOPED_INFO("the older chunk with more live bytes keeps its key");
    CHECK(h.plan(keep).kind == Decision::Kind::Hit);
}

void testExpiry()
{
    Fixture                                        f;
    auto                                           owner = makeOwner(1024);
    std::weak_ptr<const std::vector<std::uint8_t>> watch = owner;
    f.admit(owner);
    f.res.SubmitBatch(0, 3);
    UNSCOPED_INFO("the entry is live");
    CHECK(f.res.Entries() == 1);
    f.res.BeginBatch(0);
    owner.reset();
    UNSCOPED_INFO("the entry kept no strong owner");
    CHECK(watch.expired());
    f.res.EndPacket(0);
    UNSCOPED_INFO("a sweep removes the key of an expired owner");
    CHECK((f.res.Entries() == 0 && f.res.PayloadBytes() == 0 && f.res.counters().expiries == 1));
    UNSCOPED_INFO("the chunk keeps the expired span");
    CHECK((f.res.Chunk(0).state == ChunkInfo::State::Active && f.res.Chunk(0).cursor == 1024 && f.res.Chunk(0).liveBytes == 0));

    // Expiry while the chunk is pending keeps its storage.
    Fixture g;
    auto    pending = makeOwner(1024);
    g.admit(pending);
    pending.reset();
    g.res.BeginPacket(0);
    UNSCOPED_INFO("expiry leaves the pin");
    CHECK((g.res.Entries() == 0 && g.res.PendingPins() == 1));
    g.res.SubmitBatch(0, 4);
    g.res.ClearLive();
    g.res.Reap(3);
    UNSCOPED_INFO("the footprint stays through the last-use fence");
    CHECK((g.res.Chunk(0).state == ChunkInfo::State::Retired && g.res.FootprintBytes() == 16 * KiB));

    // A new owner at the old object's address does not inherit the entry.
    Fixture     h;
    auto        original = makeOwner(512, 1);
    const void* address  = original->data();
    h.admit(original);
    original.reset();
    h.res.BeginPacket(0);
    auto replacement = makeOwner(512, 1);
    UNSCOPED_INFO("a new owner misses");
    CHECK(h.plan(replacement).kind == Decision::Kind::Miss);
    (void)address;

    // A lookup drops an expired owner on its chain.
    Limits one  = smallLimits();
    one.buckets = 1;
    Fixture    chain(Mode::Owner, one);
    auto       a = makeOwner(256, 1);
    const auto b = makeOwner(256, 2);
    chain.admit(b);
    chain.admit(a);
    a.reset();
    UNSCOPED_INFO("a lookup removes an expired entry it passes");
    CHECK((chain.plan(b).kind == Decision::Kind::Hit && chain.res.Entries() == 1 && chain.res.counters().expiries == 1));
}

void testBatchLifecycle()
{
    Fixture    f;
    const auto owner = makeOwner(512);
    // A failed copy leaves no entry.
    auto reserved = f.plan(owner);
    f.res.Abandon(reserved);
    UNSCOPED_INFO("an abandoned reservation publishes nothing");
    CHECK((f.res.Entries() == 0 && f.res.PendingPins() == 0));
    UNSCOPED_INFO("an abandoned reservation returns its span");
    CHECK(f.res.Chunk(0).cursor == 0);
    UNSCOPED_INFO("a retry misses and succeeds");
    CHECK(f.plan(owner).kind == Decision::Kind::Miss);
    auto tiny       = smallLimits();
    tiny.maxEntries = 2;
    Fixture records(Mode::Owner, tiny);
    for (int attempt = 0; attempt < 5; ++attempt)
        records.res.Abandon(records.plan(owner));
    UNSCOPED_INFO("abandoned reservations return their records");
    CHECK(records.plan(owner).kind == Decision::Kind::Miss);

    // A copy recorded in an unsubmitted list is not ready for another list.
    Fixture g;
    g.res.BeginBatch(1);
    g.admit(owner, 0, 512, 0);
    const auto notReady = g.plan(owner, 0, 512, 1);
    UNSCOPED_INFO("another list cannot use an unsubmitted copy");
    CHECK((notReady.kind == Decision::Kind::Fallback && notReady.reason == Reason::Scheduling));
    g.res.SubmitBatch(0, 7);
    UNSCOPED_INFO("after the producer list is submitted the copy is ready");
    CHECK(g.plan(owner, 0, 512, 1).kind == Decision::Kind::Hit);

    // Its own open list orders its own copy.
    Fixture h;
    h.admit(owner);
    UNSCOPED_INFO("a later draw in the same list hits");
    CHECK(h.plan(owner).kind == Decision::Kind::Hit);

    // A list reset without submission is discarded.
    Fixture d;
    d.admit(owner);
    UNSCOPED_INFO("the list holds its copy");
    CHECK((d.res.PendingPins() == 1 && d.res.Entries() == 1));
    d.res.BeginBatch(0);
    UNSCOPED_INFO("a discarded list releases its pins and entries");
    CHECK((d.res.PendingPins() == 0 && d.res.Entries() == 0));
    UNSCOPED_INFO("a discarded initialization is never ready");
    CHECK(d.plan(owner).kind == Decision::Kind::Miss);

    // A submitted list keeps its entries when the next list begins.
    Fixture s;
    s.admit(owner);
    s.res.SubmitBatch(0, 2);
    s.res.BeginBatch(0);
    UNSCOPED_INFO("a submitted list's entries survive its reuse");
    CHECK((s.res.Entries() == 1 && s.plan(owner).kind == Decision::Kind::Hit));

    // A signal failure quarantines the list's chunks.
    Fixture q;
    q.admit(owner);
    q.res.QuarantineBatch(0);
    UNSCOPED_INFO("quarantine removes every key");
    CHECK((q.res.Entries() == 0 && q.res.PendingPins() == 0));
    q.completed = 1000;
    q.res.Reap(q.completed);
    UNSCOPED_INFO("a chunk of unknown completion is never reused");
    CHECK((q.res.Chunk(0).state == ChunkInfo::State::Retired && q.res.Chunk(0).lastUseFence == kNever));
    UNSCOPED_INFO("quarantined bytes stay retired");
    CHECK(q.res.RetiredBytes() == 4 * KiB);

    // A list reference from before a reset cannot stamp the new chunk.
    Fixture r;
    r.admit(owner);
    r.res.ResetAll();
    r.res.BeginBatch(1);
    const auto fresh = r.plan(owner, 0, 512, 1);
    UNSCOPED_INFO("the new generation admits again");
    CHECK((fresh.kind == Decision::Kind::Miss && r.res.Commit(fresh, 1)));
    r.res.SubmitBatch(0, 99);
    UNSCOPED_INFO("an old reference neither stamps nor releases the new chunk");
    CHECK((r.res.Chunk(0).lastUseFence == 0 && r.res.Chunk(0).pins == 1));
    r.res.SubmitBatch(1, 5);
    UNSCOPED_INFO("the new reference does");
    CHECK((r.res.Chunk(0).lastUseFence == 5 && r.res.Chunk(0).pins == 0));
}

void testClearSeams()
{
    Fixture    f;
    const auto a = makeOwner(512, 1), b = makeOwner(512, 2);
    f.admit(a);
    f.admit(b);
    f.res.SubmitBatch(0, 4);
    f.res.BeginBatch(0);
    f.res.ClearLive();
    UNSCOPED_INFO("a clear removes every key");
    CHECK((f.res.Entries() == 0 && f.res.PayloadBytes() == 0 && f.res.counters().clears == 1));
    UNSCOPED_INFO("a clear holds the chunk for its fence");
    CHECK((f.res.Chunk(0).state == ChunkInfo::State::Retired && f.res.RetiredBytes() == 4 * KiB));
    f.res.Reap(3);
    UNSCOPED_INFO("a clear does not release storage before completion");
    CHECK(f.res.Chunk(0).state == ChunkInfo::State::Retired);
    f.res.Reap(4);
    UNSCOPED_INFO("completion releases it");
    CHECK(f.res.Chunk(0).state == ChunkInfo::State::Empty);

    Fixture g;
    g.admit(a);
    g.res.FailPacket();
    UNSCOPED_INFO("a failed packet removes every key");
    CHECK(g.res.Entries() == 0);
    const auto held = g.plan(a);
    UNSCOPED_INFO("a failed packet disables reuse");
    CHECK((held.kind == Decision::Kind::Fallback && held.reason == Reason::Mutated));
    g.res.BeginPacket(0);
    g.res.BeginBatch(0);
    UNSCOPED_INFO("a new packet may admit again");
    CHECK(g.plan(a).kind == Decision::Kind::Miss);

    Fixture d;
    d.admit(a);
    d.res.SubmitBatch(0, 4);
    d.res.DetachAll();
    d.completed = UINT64_MAX - 1;
    d.res.Reap(d.completed);
    UNSCOPED_INFO("abandonment holds every chunk");
    CHECK((d.res.Entries() == 0 && d.res.RetiredBytes() == 4 * 4 * KiB));
    for (std::uint32_t chunk = 0; chunk < 4; ++chunk)
    {
        UNSCOPED_INFO("a detached chunk is never reused");
        CHECK(d.res.Chunk(chunk).state == ChunkInfo::State::Retired);
    }

    Fixture r;
    r.admit(a);
    r.res.ResetAll();
    UNSCOPED_INFO("a reset empties everything");
    CHECK((r.res.Entries() == 0 && r.res.PendingPins() == 0 && r.res.RetiredBytes() == 0));
}

void testRecordsAndSnapshots()
{
    auto limits       = smallLimits();
    limits.maxEntries = 3;
    Fixture                  f(Mode::Owner, limits);
    std::vector<VertexOwner> owners;
    for (int index = 0; index < 3; ++index)
    {
        owners.push_back(makeOwner(256, std::uint8_t(index)));
        f.admit(owners.back());
    }
    const auto extra = makeOwner(256, 9);
    auto       full  = f.plan(extra);
    UNSCOPED_INFO("no free record falls back as records");
    CHECK((full.kind == Decision::Kind::Fallback && full.reason == Reason::Records));
    owners[1].reset();
    full = f.plan(extra);
    UNSCOPED_INFO("an expired owner frees its record");
    CHECK(full.kind == Decision::Kind::Miss);
    f.res.Abandon(full);

    auto exactLimits          = smallLimits();
    exactLimits.snapshotBytes = 1024;
    Fixture    g(Mode::Exact, exactLimits);
    const auto big      = makeOwner(2000);
    auto       decision = g.plan(big);
    UNSCOPED_INFO("a snapshot beyond the ceiling falls back as cap");
    CHECK((decision.kind == Decision::Kind::Fallback && decision.reason == Reason::Cap));
    const auto small = makeOwner(600), more = makeOwner(600, 5);
    g.admit(small);
    UNSCOPED_INFO("a snapshot is held");
    CHECK(g.res.SnapshotBytes() == 600);
    decision = g.plan(more);
    UNSCOPED_INFO("no room for a second snapshot");
    CHECK((decision.kind == Decision::Kind::Fallback && decision.reason == Reason::Cap));

    static bool fail;
    fail                        = true;
    exactLimits.allocationFails = []() noexcept
    {
        return fail;
    };
    Fixture h(Mode::Exact, exactLimits);
    decision = h.plan(small);
    UNSCOPED_INFO("a failed snapshot allocation falls back as allocation");
    CHECK((decision.kind == Decision::Kind::Fallback && decision.reason == Reason::Allocation));
    fail = false;
    UNSCOPED_INFO("a retry succeeds");
    CHECK(h.plan(small).kind == Decision::Kind::Miss);
}

void testExactMode()
{
    // A same-size mutable alias of an owner the table already holds.
    std::vector<std::uint8_t> bytes(1024, 3);
    const VertexOwner         owner(std::make_shared<int>(0), &bytes);
    for (const auto mode : { Mode::Owner, Mode::Exact })
    {
        Fixture f(mode);
        std::fill(bytes.begin(), bytes.end(), 3);
        f.admit(owner);
        bytes[500]          = 99;
        const auto decision = f.plan(owner);
        if (mode == Mode::Exact)
        {
            UNSCOPED_INFO("Exact rejects a mutated same-size owner");
            CHECK((decision.kind == Decision::Kind::Fallback && decision.reason == Reason::Mutated));
            UNSCOPED_INFO("the entry leaves and its span stays");
            CHECK((f.res.Entries() == 0 && f.res.Chunk(0).cursor == 1024));
            // Reuse is off for the rest of the packet, even for a new span.
            const auto next = makeOwner(256);
            const auto held = f.plan(next);
            UNSCOPED_INFO("reuse stays off for the packet");
            CHECK((held.kind == Decision::Kind::Fallback && held.reason == Reason::Mutated));
            f.res.BeginPacket(0);
            UNSCOPED_INFO("a new packet may retry with a new copy");
            CHECK(f.plan(owner).kind == Decision::Kind::Miss);
        }
        else
        {
            UNSCOPED_INFO("Owner mode trusts an unchanged owner");
            CHECK(decision.kind == Decision::Kind::Hit);
        }
    }
    // The data pointer, size and capacity are rechecked in either mode.
    Fixture           g;
    auto              mutableVector = std::make_shared<std::vector<std::uint8_t>>(512, 1);
    const VertexOwner viaConst      = mutableVector;
    g.admit(viaConst, 0, 256);
    mutableVector->resize(1024);
    const auto changed = g.plan(viaConst, 0, 256);
    UNSCOPED_INFO("a resized owner falls back as mutated");
    CHECK((changed.kind == Decision::Kind::Fallback && changed.reason == Reason::Mutated));
}

void testMetadataIsFixed()
{
    VertexResidency before(Mode::Owner);
    const auto      fixed = before.MetadataBytes();
    Fixture         f(Mode::Owner, Limits{});
    const auto      owner = makeOwner(1024);
    f.admit(owner);
    UNSCOPED_INFO("occupancy changes neither the metadata charge nor the footprint");
    CHECK((f.res.MetadataBytes() == fixed && f.res.FootprintBytes() == 64 * MiB));
    UNSCOPED_INFO("live bytes are counted per chunk");
    CHECK((f.res.Chunk(0).liveBytes == 1024 && f.res.chunkCount() == 4));
}

struct Case
{
    const char* name;
    void (*run)();
};

const Case cases[] = {
    { "testDefaults", testDefaults },
    { "testKeyIdentity", testKeyIdentity },
    { "testCollisions", testCollisions },
    { "testKeyUnderCollision", testKeyUnderCollision },
    { "testEligibility", testEligibility },
    { "testGenerationKey", testGenerationKey },
    { "testBarrierState", testBarrierState },
    { "testAlignmentAndFill", testAlignmentAndFill },
    { "testOldestFirstEviction", testOldestFirstEviction },
    { "testPendingFences", testPendingFences },
    { "testLastUseIsNotInsertionFence", testLastUseIsNotInsertionFence },
    { "testHotChunkReclaim", testHotChunkReclaim },
    { "testExpiry", testExpiry },
    { "testBatchLifecycle", testBatchLifecycle },
    { "testClearSeams", testClearSeams },
    { "testRecordsAndSnapshots", testRecordsAndSnapshots },
    { "testExactMode", testExactMode },
    { "testMetadataIsFixed", testMetadataIsFixed },
};

// Runs every case, or only the one named by `selected`; returns how many ran.
unsigned runCases(std::string_view selected)
{
    unsigned ran = 0;
    for (const auto& c : cases)
    {
        if (!selected.empty() && selected != c.name)
            continue;
        INFO("case " << c.name);
        c.run();
        ++ran;
    }
    return ran;
}

} // namespace

TEST_CASE("native_vertex_residency", "[ctest][native-vertex-residency][timeout-120]")
{
    CHECK(runCases({}) == std::size(cases));
}

// Runs one named case: native-vertex-residency.case <name>.
TEST_CASE("native_vertex_residency_case", "[.][manual][native-vertex-residency]")
{
    auto& args = rerevved::tests::CurrentManualArgv();
    if (args.argc() == 0)
    {
        SKIP("pass a case name through --helper native-vertex-residency.case");
    }
    UNSCOPED_INFO("pass exactly one case name");
    REQUIRE(args.argc() == 1);
    UNSCOPED_INFO(args[1]);
    CHECK(runCases(args[1]) == 1);
}

REREVVED_MANUAL_HELPER(nativeVertexResidencyCase, "native-vertex-residency.case", "native_vertex_residency_case")
