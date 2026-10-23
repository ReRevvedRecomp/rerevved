#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace rerevved::gpu::vertex_residency
{

// Residency stores metadata only; the renderer owns GPU buffers, copies,
// barriers, submissions, fences, and failures.

using VertexOwner     = std::shared_ptr<const std::vector<std::uint8_t>>;
using WeakVertexOwner = std::weak_ptr<const std::vector<std::uint8_t>>;

inline constexpr std::uint32_t kNone      = ~std::uint32_t(0);
inline constexpr std::uint64_t kNever     = ~std::uint64_t(0);
inline constexpr std::uint64_t kAlignment = 256;
inline constexpr std::uint32_t kMaxChunks = 64;

enum class Mode : std::uint8_t
{
    Off,
    Owner,
    Exact
};

// Why a draw used the ordinary upload, in kReasonNames order.
enum class Reason : std::uint8_t
{
    OutsideFrame,
    Owner,
    Bounds,
    Generation,
    Mutated,
    Cap,
    Records,
    Allocation,
    Scheduling,
    DisabledAfterInitFailure
};

inline constexpr std::size_t kReasonCount = 10;

inline constexpr std::array<const char*, kReasonCount> kReasonNames{ "outside_frame", "owner", "bounds", "generation", "mutated", "cap", "records", "allocation", "scheduling", "disabled_after_init_failure" };

struct Limits
{
    std::uint64_t chunkBytes = 16ULL << 20;
    std::uint32_t chunkCount = 4;
    std::uint32_t maxEntries = 4096;
    std::uint32_t buckets    = 8192;
    // Command batches that may record at once.
    std::uint32_t batches = 2;
    // Exact mode's CPU snapshot ceiling.
    std::uint64_t snapshotBytes = 64ULL << 20;
    // A full table may reclaim a chunk of recent use if it holds fewer live
    // bytes than this and no command list or fence needs it.
    std::uint64_t hotChunkBytes = 4ULL << 20;
    // Test hooks: replace the key hash, and fail snapshot allocation.
    std::uint64_t (*hash)(const void* object, std::uint64_t offset, std::uint64_t length) noexcept = nullptr;
    bool (*allocationFails)() noexcept                                                             = nullptr;
};

inline std::uint64_t HashKey(const void* object, std::uint64_t offset, std::uint64_t length) noexcept
{
    std::uint64_t hash = reinterpret_cast<std::uintptr_t>(object) * 0x9E3779B97F4A7C15ULL;
    for (const auto value : { offset, length })
    {
        hash ^= value + 0x9E3779B97F4A7C15ULL + (hash << 6) + (hash >> 2);
        hash *= 0xFF51AFD7ED558CCDULL;
        hash ^= hash >> 33;
    }
    return hash;
}

struct Request
{
    // Null for inline or unknown vertex bytes.
    const VertexOwner* owner  = nullptr;
    std::uint64_t      offset = 0, length = 0;
    // The device generation the caller records for.
    std::uint64_t generation = 0;
    std::uint32_t batch      = 0;
    bool          frameDraw = false, frameBatch = false;
};

struct Placement
{
    std::uint32_t chunk  = kNone;
    std::uint64_t offset = 0;
};

struct Decision
{
    enum class Kind : std::uint8_t
    {
        Off,
        Fallback,
        Hit,
        Miss
    };

    Kind          kind   = Kind::Off;
    Reason        reason = Reason::Owner;
    Placement     where; // the resident span of a hit, or the span a miss reserved
    std::uint64_t length = 0;
    std::uint32_t record = kNone, serial = 0;
};

struct ChunkInfo
{
    enum class State : std::uint8_t
    {
        Empty,
        Active,
        Retired
    };

    State         state  = State::Empty;
    std::uint64_t cursor = 0, liveBytes = 0, lastUseFence = 0, lastUsedSwap = 0;
    std::uint32_t pins = 0, reuseSerial = 0;
};

struct Counters
{
    // Whole-chunk logical evictions, owners that expired, clears that removed
    // keys, and retired chunks reused.
    std::uint64_t evictions = 0, expiries = 0, clears = 0, chunkReuses = 0;
};

class VertexResidency
{
public:
    explicit VertexResidency(Mode mode, const Limits& limits = {})
    : mode_(mode)
    , limits_(Clamp(limits))
    {
        if (mode_ == Mode::Off)
            return;
        records_.resize(limits_.maxEntries);
        buckets_.assign(limits_.buckets, kNone);
        chunks_.resize(limits_.chunkCount);
        batches_.resize(limits_.batches);
        for (auto& batch : batches_)
            batch.pinSerial.assign(limits_.chunkCount, 0);
        RebuildFreeList();
    }

    VertexResidency(const VertexResidency&)            = delete;
    VertexResidency& operator=(const VertexResidency&) = delete;

    Mode mode() const noexcept
    {
        return mode_;
    }

    const Limits& limits() const noexcept
    {
        return limits_;
    }

    // The bytes of CPU metadata, counting each expired owner's control block
    // until its record is released. Exact snapshots are separate.
    std::uint64_t MetadataBytes() const noexcept
    {
        if (mode_ == Mode::Off)
            return 0;
        constexpr std::uint64_t controlBlock = sizeof(std::vector<std::uint8_t>) + 4 * sizeof(void*);
        return std::uint64_t(limits_.maxEntries) * (sizeof(Record) + controlBlock) + std::uint64_t(limits_.buckets) * sizeof(std::uint32_t) + limits_.chunkCount * sizeof(ChunkSlot) + limits_.batches * (sizeof(Batch) + limits_.chunkCount * sizeof(std::uint32_t));
    }

    std::uint64_t SnapshotCeilingBytes() const noexcept
    {
        return mode_ == Mode::Exact ? limits_.snapshotBytes : 0;
    }

    // The fixed chunk footprint, whatever the chunks hold.
    std::uint64_t FootprintBytes() const noexcept
    {
        return mode_ == Mode::Off ? 0 : std::uint64_t(limits_.chunkCount) * limits_.chunkBytes;
    }

    // Changes the device generation and drops everything resident.
    void SetGeneration(std::uint64_t generation)
    {
        ResetAll();
        generation_ = generation;
    }

    // Changes only the generation that keys compare with, so a test can show
    // that the key guard holds without any clear.
    void SetGenerationForTesting(std::uint64_t generation) noexcept
    {
        generation_ = generation;
    }

    std::uint64_t Generation() const noexcept
    {
        return generation_;
    }

    // A recording packet starts or ends. A start allows reuse again and ends
    // the previous packet's reuse hold; both sweep owners that expired and
    // reap retired chunks the fence has passed.
    void BeginPacket(std::uint64_t completed)
    {
        if (mode_ == Mode::Off)
            return;
        packetReuseDisabled_ = false;
        Reap(completed);
        SweepExpired();
    }

    void EndPacket(std::uint64_t completed)
    {
        if (mode_ == Mode::Off)
            return;
        Reap(completed);
        SweepExpired();
    }

    // The terminal presentation draw has recorded, whether or not it
    // succeeded. Chunks stay protected through the current and previous swap.
    void AdvanceSwap() noexcept
    {
        ++swap_;
    }

    std::uint64_t Swap() const noexcept
    {
        return swap_;
    }

    // Looks the span up, or reserves a destination for it. `completed` is
    // called only for a miss and returns the queue's completed fence value.
    template <class Completed>
    Decision Plan(const Request& request, Completed&& completed)
    {
        Decision decision;
        if (mode_ == Mode::Off)
            return decision;
        decision.kind   = Decision::Kind::Fallback;
        const auto fall = [&](Reason reason)
        {
            decision.reason = reason;
            return decision;
        };
        if (!request.frameDraw || !request.frameBatch)
            return fall(Reason::OutsideFrame);
        if (!request.owner || !*request.owner || EmptyControlBlock(*request.owner))
            return fall(Reason::Owner);
        if (request.generation != generation_)
            return fall(Reason::Generation);
        const auto size = (*request.owner)->size();
        if (!request.length || request.offset > size || request.length > size - request.offset)
            return fall(Reason::Bounds);
        if (request.batch >= batches_.size() || !batches_[request.batch].open)
            return fall(Reason::Scheduling);
        if (packetReuseDisabled_)
            return fall(Reason::Mutated);

        const auto hash  = limits_.hash ? limits_.hash(request.owner->get(), request.offset, request.length) : HashKey(request.owner->get(), request.offset, request.length);
        const auto found = Find(request, hash);
        if (found != kNone)
        {
            Record&     record  = records_[found];
            const auto* current = (*request.owner)->data();
            if (record.data != current || record.size != size || record.capacity != (*request.owner)->capacity() ||
                (mode_ == Mode::Exact && std::memcmp(record.snapshot.get(), current + request.offset, request.length) != 0))
            {
                Unlink(found);
                packetReuseDisabled_ = true;
                return fall(Reason::Mutated);
            }
            if (!Ready(record, request.batch))
                return fall(Reason::Scheduling);
            ChunkSlot& chunk = chunks_[record.chunk];
            Touch(chunk);
            Pin(request.batch, record.chunk);
            // A buffer in COMMON promotes to a vertex buffer at its first read.
            batches_[request.batch].vertexMask |= std::uint64_t(1) << record.chunk;
            decision.kind   = Decision::Kind::Hit;
            decision.where  = { record.chunk, record.chunkOffset };
            decision.length = request.length;
            return decision;
        }

        const auto done = NeverEvidence(completed());
        Reap(done);
        if (freeHead_ == kNone)
            SweepExpired();
        if (freeHead_ == kNone)
            return fall(Reason::Records);
        const std::uint64_t             span = (request.length + kAlignment - 1) & ~(kAlignment - 1);
        std::unique_ptr<std::uint8_t[]> snapshot;
        if (mode_ == Mode::Exact)
        {
            if (request.length > limits_.snapshotBytes || snapshotUsed_ > limits_.snapshotBytes - request.length)
                return fall(Reason::Cap);
            try
            {
                if (limits_.allocationFails && limits_.allocationFails())
                    throw std::bad_alloc{};
                snapshot = std::make_unique_for_overwrite<std::uint8_t[]>(request.length);
            }
            catch (const std::bad_alloc&)
            {
                return fall(Reason::Allocation);
            }
        }
        Placement where;
        Reason    why = Reason::Cap;
        if (!Allocate(span, done, where, why))
            return fall(why);

        const auto index   = freeHead_;
        Record&    record  = records_[index];
        freeHead_          = record.next;
        record.state       = Record::State::Reserved;
        record.owner       = *request.owner;
        record.object      = request.owner->get();
        record.data        = (*request.owner)->data();
        record.size        = size;
        record.capacity    = (*request.owner)->capacity();
        record.offset      = request.offset;
        record.length      = request.length;
        record.span        = span;
        record.hash        = hash;
        record.bucket      = std::uint32_t(hash % limits_.buckets);
        record.generation  = generation_;
        record.chunk       = where.chunk;
        record.chunkOffset = where.offset;
        if (snapshot)
        {
            std::memcpy(snapshot.get(), record.data + request.offset, request.length);
            snapshotUsed_ += request.length;
        }
        record.snapshot = std::move(snapshot);
        decision.kind   = Decision::Kind::Miss;
        decision.where  = where;
        decision.length = request.length;
        decision.record = index;
        decision.serial = record.serial;
        return decision;
    }

    // Whether a miss's copy needs a barrier from the vertex-buffer state to
    // the copy destination state first: the chunk already served a read or
    // received a copy in this list. A chunk in COMMON promotes with none.
    bool NeedsPreCopyBarrier(std::uint32_t batch, std::uint32_t chunk) const noexcept
    {
        return (batches_[batch].vertexMask >> chunk) & 1;
    }

    // The copy and its barriers are recorded in `batch`'s list: pin the
    // chunk and publish the entry for ordered use.
    bool Commit(const Decision& decision, std::uint32_t batch)
    {
        if (decision.kind != Decision::Kind::Miss || decision.record >= records_.size())
            return false;
        Record& record = records_[decision.record];
        if (record.state != Record::State::Reserved || record.serial != decision.serial || batch >= batches_.size() || !batches_[batch].open)
            return false;
        ChunkSlot& chunk = chunks_[record.chunk];
        Pin(batch, record.chunk);
        batches_[batch].vertexMask |= std::uint64_t(1) << record.chunk;
        Touch(chunk);
        record.state            = Record::State::Live;
        record.initBatch        = batch;
        record.initSerial       = batches_[batch].serial;
        record.next             = buckets_[record.bucket];
        buckets_[record.bucket] = decision.record;
        chunk.liveBytes += record.span;
        payload_ += record.length;
        ++entries_;
        return true;
    }

    // The copy was never recorded: release the reservation.
    void Abandon(const Decision& decision)
    {
        if (decision.kind != Decision::Kind::Miss || decision.record >= records_.size())
            return;
        Record& record = records_[decision.record];
        if (record.state != Record::State::Reserved || record.serial != decision.serial)
            return;
        ChunkSlot& chunk = chunks_[record.chunk];
        if (chunk.cursor == record.chunkOffset + record.span)
            chunk.cursor = record.chunkOffset;
        Free(decision.record);
    }

    // A command list starts recording. A list that was reset without having
    // been submitted is discarded: its pins end and the entries it
    // initialized leave the table.
    void BeginBatch(std::uint32_t batch)
    {
        if (mode_ == Mode::Off || batch >= batches_.size())
            return;
        Batch& list = batches_[batch];
        if (list.open && !list.submitted)
            Discard(batch);
        ++list.serial;
        list.open       = true;
        list.submitted  = false;
        list.vertexMask = 0;
    }

    // The list executed and its signal at `fence` succeeded.
    void SubmitBatch(std::uint32_t batch, std::uint64_t fence)
    {
        if (mode_ == Mode::Off || batch >= batches_.size())
            return;
        Batch& list = batches_[batch];
        StampPins(batch, fence);
        list.open      = false;
        list.submitted = true;
    }

    // The list executed but its signal failed, so its completion is unknown:
    // its chunks are never reused and nothing resident remains addressable.
    void QuarantineBatch(std::uint32_t batch)
    {
        if (mode_ == Mode::Off || batch >= batches_.size())
            return;
        StampPins(batch, kNever);
        ClearLive();
        batches_[batch].open = false;
    }

    // Removes every live key. Chunks that held keys retire; each is reused
    // only after its pins end and its last-use fence completes.
    void ClearLive()
    {
        if (mode_ == Mode::Off)
            return;
        const bool had = entries_ != 0;
        for (std::uint32_t index = 0; index < records_.size(); ++index)
            if (records_[index].state == Record::State::Live)
                Unlink(index);
        for (auto& chunk : chunks_)
            if (chunk.state == ChunkInfo::State::Active)
            {
                chunk.state     = ChunkInfo::State::Retired;
                chunk.liveBytes = 0;
            }
        active_ = kNone;
        if (had)
            ++counters_.clears;
    }

    // A failed recording packet: no key survives and reuse stays off for it.
    void FailPacket()
    {
        ClearLive();
        packetReuseDisabled_ = true;
    }

    // Abandons the device's objects without releasing them: nothing is
    // addressable and no chunk is ever reused.
    void DetachAll()
    {
        if (mode_ == Mode::Off)
            return;
        ClearLive();
        for (auto& chunk : chunks_)
        {
            chunk.state        = ChunkInfo::State::Retired;
            chunk.lastUseFence = kNever;
        }
    }

    // Drops all state for a new device or after a drained teardown. Pending
    // references of earlier lists no longer match any chunk.
    void ResetAll()
    {
        if (mode_ == Mode::Off)
            return;
        for (auto& record : records_)
        {
            record.owner.reset();
            record.snapshot.reset();
            record.state = Record::State::Free;
            ++record.serial;
        }
        RebuildFreeList();
        std::fill(buckets_.begin(), buckets_.end(), kNone);
        for (auto& chunk : chunks_)
        {
            const auto serial = chunk.reuseSerial + 1;
            chunk             = {};
            chunk.reuseSerial = serial;
        }
        for (auto& batch : batches_)
            batch.vertexMask = 0;
        active_  = kNone;
        entries_ = payload_ = snapshotUsed_ = 0;
        packetReuseDisabled_                = false;
    }

    // Retired chunks whose pins ended and whose last use the fence passed
    // become reusable. UINT64_MAX is a removed device, never completion.
    void Reap(std::uint64_t completed)
    {
        if (mode_ == Mode::Off || completed == kNever)
            return;
        for (auto& chunk : chunks_)
            if (chunk.state == ChunkInfo::State::Retired && chunk.pins == 0 && chunk.lastUseFence != kNever && chunk.lastUseFence <= completed)
            {
                chunk.state     = ChunkInfo::State::Empty;
                chunk.cursor    = 0;
                chunk.liveBytes = 0;
                ++chunk.reuseSerial;
                ++counters_.chunkReuses;
            }
    }

    // Removes the keys of owners that expired. Their spans stay in their
    // chunks until the chunk is reclaimed.
    void SweepExpired()
    {
        if (mode_ == Mode::Off || !entries_)
            return;
        for (std::uint32_t index = 0; index < records_.size(); ++index)
            if (records_[index].state == Record::State::Live && records_[index].owner.expired())
            {
                Unlink(index);
                ++counters_.expiries;
            }
    }

    // Live keys, their payload bytes, and the chunk bytes retired chunks hold.
    std::uint64_t Entries() const noexcept
    {
        return entries_;
    }

    std::uint64_t PayloadBytes() const noexcept
    {
        return payload_;
    }

    std::uint64_t SnapshotBytes() const noexcept
    {
        return snapshotUsed_;
    }

    std::uint64_t RetiredBytes() const noexcept
    {
        std::uint64_t retired = 0;
        for (const auto& chunk : chunks_)
            retired += chunk.state == ChunkInfo::State::Retired ? limits_.chunkBytes : 0;
        return retired;
    }

    // Command batch references not yet submitted or discarded.
    std::uint64_t PendingPins() const noexcept
    {
        std::uint64_t pins = 0;
        for (const auto& chunk : chunks_)
            pins += chunk.pins;
        return pins;
    }

    std::uint64_t PendingLists() const noexcept
    {
        std::uint64_t lists = 0;
        for (const auto& batch : batches_)
            lists += batch.open && !batch.submitted && batch.pinMask != 0;
        return lists;
    }

    const Counters& counters() const noexcept
    {
        return counters_;
    }

    ChunkInfo Chunk(std::uint32_t chunk) const noexcept
    {
        const auto& value = chunks_[chunk];
        return { value.state, value.cursor, value.liveBytes, value.lastUseFence, value.lastUsedSwap, value.pins, value.reuseSerial };
    }

    std::uint32_t chunkCount() const noexcept
    {
        return limits_.chunkCount;
    }

private:
    struct Record
    {
        enum class State : std::uint8_t
        {
            Free,
            Reserved,
            Live
        };

        State               state = State::Free;
        WeakVertexOwner     owner;
        const void*         object = nullptr;
        const std::uint8_t* data   = nullptr;
        std::size_t         size = 0, capacity = 0;
        std::uint64_t       offset = 0, length = 0, span = 0, hash = 0, generation = 0, chunkOffset = 0;
        std::uint32_t       bucket = 0, chunk = 0, next = kNone, serial = 0;
        // The list that recorded the copy, and which instance of it.
        std::uint32_t                   initBatch = 0, initSerial = 0;
        std::unique_ptr<std::uint8_t[]> snapshot;
    };

    struct ChunkSlot : ChunkInfo
    {
        // Admission order, so the oldest chunk is the one used least recently.
        std::uint64_t tick = 0;
    };

    struct Batch
    {
        bool          open = false, submitted = false;
        std::uint32_t serial = 0;
        // Chunks this list references, and each chunk's reuse serial then.
        std::uint64_t              pinMask = 0;
        std::vector<std::uint32_t> pinSerial;
        // Chunks this list has used or written, so tracked in the vertex
        // buffer state; every list starts with its chunks in COMMON.
        std::uint64_t vertexMask = 0;
    };

    static Limits Clamp(Limits limits) noexcept
    {
        limits.chunkCount = std::clamp<std::uint32_t>(limits.chunkCount, 1, kMaxChunks);
        limits.buckets    = std::max<std::uint32_t>(limits.buckets, 1);
        limits.batches    = std::max<std::uint32_t>(limits.batches, 1);
        return limits;
    }

    static bool EmptyControlBlock(const VertexOwner& owner) noexcept
    {
        const WeakVertexOwner none;
        return !owner.owner_before(none) && !none.owner_before(owner);
    }

    static bool SameOwner(const WeakVertexOwner& stored, const VertexOwner& owner) noexcept
    {
        return !stored.owner_before(owner) && !owner.owner_before(stored);
    }

    static std::uint64_t NeverEvidence(std::uint64_t completed) noexcept
    {
        return completed == kNever ? 0 : completed;
    }

    void RebuildFreeList() noexcept
    {
        freeHead_ = records_.empty() ? kNone : 0;
        for (std::uint32_t index = 0; index < records_.size(); ++index)
            records_[index].next = index + 1 < records_.size() ? index + 1 : kNone;
    }

    // Returns the live record of the request's key, dropping owners that
    // expired along its chain.
    std::uint32_t Find(const Request& request, std::uint64_t hash)
    {
        for (auto index = buckets_[hash % limits_.buckets]; index != kNone;)
        {
            const auto next = records_[index].next;
            if (records_[index].owner.expired())
            {
                Unlink(index);
                ++counters_.expiries;
            }
            else if (Matches(records_[index], request, hash))
                return index;
            index = next;
        }
        return kNone;
    }

    static bool Matches(const Record& record, const Request& request, std::uint64_t hash) noexcept
    {
        return record.hash == hash &&
               record.generation == request.generation &&
               record.object == request.owner->get() &&
               record.offset == request.offset &&
               record.length == request.length &&
               SameOwner(record.owner, *request.owner);
    }

    // A copy another list recorded is ordered before this one only once that
    // list was submitted. The same open list orders its own copies.
    bool Ready(const Record& record, std::uint32_t consumer) const noexcept
    {
        const Batch& init = batches_[record.initBatch];
        if (record.initBatch == consumer && record.initSerial == init.serial && init.open)
            return true;
        return record.initSerial < init.serial || (record.initSerial == init.serial && init.submitted);
    }

    void Touch(ChunkSlot& chunk) noexcept
    {
        chunk.lastUsedSwap = swap_;
        chunk.tick         = ++tick_;
    }

    void Pin(std::uint32_t batch, std::uint32_t chunk)
    {
        Batch& list = batches_[batch];
        if (((list.pinMask >> chunk) & 1) && list.pinSerial[chunk] == chunks_[chunk].reuseSerial)
            return;
        list.pinMask |= std::uint64_t(1) << chunk;
        list.pinSerial[chunk] = chunks_[chunk].reuseSerial;
        ++chunks_[chunk].pins;
    }

    // Ends a list's references. A reference of an older chunk generation
    // is gone with that generation.
    void StampPins(std::uint32_t batch, std::uint64_t fence)
    {
        Batch& list = batches_[batch];
        for (std::uint32_t chunk = 0; chunk < chunks_.size(); ++chunk)
        {
            if (!((list.pinMask >> chunk) & 1) || list.pinSerial[chunk] != chunks_[chunk].reuseSerial)
                continue;
            auto& value        = chunks_[chunk];
            value.lastUseFence = std::max(value.lastUseFence, fence);
            --value.pins;
        }
        list.pinMask = 0;
    }

    void Discard(std::uint32_t batch)
    {
        Batch& list = batches_[batch];
        for (std::uint32_t index = 0; index < records_.size(); ++index)
            if (records_[index].state == Record::State::Live && records_[index].initBatch == batch && records_[index].initSerial == list.serial)
                Unlink(index);
        for (std::uint32_t chunk = 0; chunk < chunks_.size(); ++chunk)
            if (((list.pinMask >> chunk) & 1) && list.pinSerial[chunk] == chunks_[chunk].reuseSerial)
                --chunks_[chunk].pins;
        list.pinMask = 0;
    }

    void Free(std::uint32_t index) noexcept
    {
        Record& record = records_[index];
        if (record.snapshot)
            snapshotUsed_ -= record.length;
        record.snapshot.reset();
        record.owner.reset();
        record.state = Record::State::Free;
        ++record.serial;
        record.next = freeHead_;
        freeHead_   = index;
    }

    void Unlink(std::uint32_t index) noexcept
    {
        Record& record = records_[index];
        if (record.state == Record::State::Live)
        {
            auto* link = &buckets_[record.bucket];
            while (*link != index)
                link = &records_[*link].next;
            *link = record.next;
            chunks_[record.chunk].liveBytes -= record.span;
            payload_ -= record.length;
            --entries_;
        }
        Free(index);
    }

    void EvictChunk(std::uint32_t index)
    {
        for (std::uint32_t record = 0; record < records_.size(); ++record)
            if (records_[record].state == Record::State::Live && records_[record].chunk == index)
                Unlink(record);
        chunks_[index].state     = ChunkInfo::State::Retired;
        chunks_[index].liveBytes = 0;
        if (active_ == index)
            active_ = kNone;
        ++counters_.evictions;
    }

    bool UsedRecently(const ChunkSlot& chunk) const noexcept
    {
        return chunk.lastUsedSwap + 1 >= swap_;
    }

    bool Bump(std::uint32_t index, std::uint64_t span, Placement& where) noexcept
    {
        ChunkSlot& chunk = chunks_[index];
        where            = { index, chunk.cursor };
        chunk.cursor += span;
        return true;
    }

    void Activate(std::uint32_t index) noexcept
    {
        ChunkSlot& chunk   = chunks_[index];
        chunk.state        = ChunkInfo::State::Active;
        chunk.cursor       = 0;
        chunk.liveBytes    = 0;
        chunk.lastUsedSwap = swap_;
        chunk.tick         = ++tick_;
        active_            = index;
    }

    // Take aligned space from filling then empty chunks. If full, evict oldest
    // nonrecent chunks whole; recent chunks need few live bytes and no pending
    // need before reclamation. Never wait.
    bool Allocate(std::uint64_t span, std::uint64_t completed, Placement& where, Reason& why)
    {
        if (span > limits_.chunkBytes)
        {
            why = Reason::Cap;
            return false;
        }
        if (active_ != kNone && chunks_[active_].state == ChunkInfo::State::Active && chunks_[active_].cursor + span <= limits_.chunkBytes)
            return Bump(active_, span, where);
        for (std::uint32_t index = 0; index < chunks_.size(); ++index)
            if (chunks_[index].state == ChunkInfo::State::Empty)
            {
                Activate(index);
                return Bump(index, span, where);
            }
        SweepExpired();
        std::uint32_t oldest  = kNone;
        bool          retired = false;
        for (std::uint32_t index = 0; index < chunks_.size(); ++index)
        {
            retired = retired || chunks_[index].state == ChunkInfo::State::Retired;
            if (chunks_[index].state == ChunkInfo::State::Active && (oldest == kNone || chunks_[index].tick < chunks_[oldest].tick))
                oldest = index;
        }
        why = Reason::Cap;
        if (oldest == kNone)
            return false;
        std::uint32_t victim = oldest;
        if (UsedRecently(chunks_[oldest]))
        {
            victim = kNone;
            for (std::uint32_t index = 0; index < chunks_.size(); ++index)
            {
                const ChunkSlot& chunk = chunks_[index];
                if (chunk.state != ChunkInfo::State::Active || chunk.pins != 0 || chunk.lastUseFence > completed || chunk.liveBytes >= limits_.hotChunkBytes)
                    continue;
                if (victim == kNone || chunk.liveBytes < chunks_[victim].liveBytes || (chunk.liveBytes == chunks_[victim].liveBytes && chunk.tick < chunks_[victim].tick))
                    victim = index;
            }
            if (victim == kNone)
                return false;
        }
        else if (retired)
            return false;
        EvictChunk(victim);
        Reap(completed);
        if (chunks_[victim].state != ChunkInfo::State::Empty)
            return false;
        Activate(victim);
        return Bump(victim, span, where);
    }

    Mode                       mode_;
    Limits                     limits_;
    std::vector<Record>        records_;
    std::vector<std::uint32_t> buckets_;
    std::vector<ChunkSlot>     chunks_;
    std::vector<Batch>         batches_;
    std::uint32_t              freeHead_ = kNone, active_ = kNone;
    std::uint64_t              generation_ = 0, swap_ = 0, tick_ = 0;
    std::uint64_t              entries_ = 0, payload_ = 0, snapshotUsed_ = 0;
    bool                       packetReuseDisabled_ = false;
    Counters                   counters_;
};

} // namespace rerevved::gpu::vertex_residency
