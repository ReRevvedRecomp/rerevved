#include "resource_mirror_registry.h"

#include "gpu/d3d12/native_texture_upload.h"
#include "gpu/guest_d3d/guest_resource_observers.h"

#include <algorithm>
#include <exception>
#include <format>

namespace rerevved::gpu::guest_d3d
{
namespace
{

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
{
    return (std::uint32_t(bytes[offset]) << 24) | (std::uint32_t(bytes[offset + 1]) << 16) | (std::uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

bool contains(const GuestResourceRange& range, std::uint64_t address, std::uint64_t size) noexcept
{
    return range.size && address >= range.address && address + size <= std::uint64_t(range.address) + range.size;
}

// Storage-defining fetch bits (dwords 0..5): type, pitch and tiling; format,
// endian, stacking and base; size; dimension, packed mips and mip base.
constexpr std::array<std::uint32_t, 6> kTextureStorageMask{ 0xFFC00003U, 0xFFFFF4FFU, 0xFFFFFFFFU, 0, 0, 0xFFFFFE00U };

std::mutex                          publishedMutex;
std::weak_ptr<GuestResourceMirrors> published;

} // namespace

std::atomic<bool> guestResourceMirrorsEnabled{ false };

std::size_t GuestResourceHeaderBytes(GuestResourceKind kind) noexcept
{
    return kind == GuestResourceKind::Texture ? kGuestTextureHeaderBytes : kind == GuestResourceKind::Surface ? kGuestSurfaceHeaderBytes
                                                                                                              : kGuestBufferHeaderBytes;
}

bool GuestResourceWords(GuestResourceKind kind, std::span<const std::uint8_t> bytes, std::array<std::uint32_t, 6>& words) noexcept
{
    words = {};
    if (bytes.size() < GuestResourceHeaderBytes(kind))
        return false;
    switch (kind)
    {
        case GuestResourceKind::Texture:
            for (std::size_t i = 0; i < words.size(); ++i)
                words[i] = word(bytes, 0x1C + i * 4);
            // 0x8269AF10 stores the base and mip base as physical aliases.
            words[1] = GuestPhysicalAddress(words[1]);
            words[5] = GuestPhysicalAddress(words[5] & ~0x1FFU) | (words[5] & 0x1FFU);
            return true;
        case GuestResourceKind::VertexBuffer:
            // +0x18 is the CPU address | 3, +0x1C the encoded fetch size word;
            // 0x8268FB08 binds them as the physical fetch pair.
            words = { GuestPhysicalAddress(word(bytes, 0x18)), word(bytes, 0x1C) };
            return true;
        case GuestResourceKind::IndexBuffer:
            // +0x18 is the CPU address of index zero, +0x1C the byte size.
            words = { word(bytes, 0x18), word(bytes, 0x1C), word(bytes, 0) };
            return true;
        case GuestResourceKind::Surface:
            return true;
    }
    return false;
}

bool DescribeGuestResource(GuestResourceKind kind, std::uint32_t header, std::span<const std::uint8_t> bytes, GuestResourceRegistration& registration)
{
    registration        = {};
    registration.kind   = kind;
    registration.header = header;
    if (!GuestResourceWords(kind, bytes, registration.words))
        return false;
    switch (kind)
    {
        case GuestResourceKind::Texture:
        {
            const auto&               fetch = registration.words;
            NativeTextureMemoryRanges ranges;
            NativeTextureMemoryRange  base;
            std::string               error;
            if (GetNativeTextureMemoryRanges(fetch, ranges, error))
                registration.ranges = { GuestResourceRange{ ranges.base.address, ranges.base.size }, GuestResourceRange{ ranges.mips.address, ranges.mips.size } };
            else if (GetNativeTextureMemoryRange(fetch, base, error))
                registration.ranges[0] = { base.address, base.size };
            return true;
        }
        case GuestResourceKind::VertexBuffer:
        {
            const auto [address, size] = std::pair{ registration.words[0], registration.words[1] };
            registration.ranges[0]     = { address & ~3U, ((size >> 2) & 0xFFFFFFU) * 4 };
            return true;
        }
        case GuestResourceKind::IndexBuffer:
            registration.ranges[0] = { GuestPhysicalAddress(registration.words[0]), registration.words[1] };
            return true;
        case GuestResourceKind::Surface:
            // A surface holds no storage of its own; its locks name its texture.
            return false;
    }
    return false;
}

bool GuestTextureStorageMatches(const std::array<std::uint32_t, 6>& fetch, const std::array<std::uint32_t, 6>& registered) noexcept
{
    for (std::size_t i = 0; i < fetch.size(); ++i)
        if ((fetch[i] ^ registered[i]) & kTextureStorageMask[i])
            return false;
    return true;
}

bool GuestResourceWordsMatch(GuestResourceKind kind, const std::array<std::uint32_t, 6>& words, const std::array<std::uint32_t, 6>& registered) noexcept
{
    return kind == GuestResourceKind::Texture ? GuestTextureStorageMatches(words, registered) : words == registered;
}

std::string FormatGuestResourceMirrorCounts(const GuestResourceMirrorCounts& counts)
{
    static constexpr std::array<const char*, kGuestResourceKindCount> kKinds{ "texture", "vertex_buffer", "index_buffer", "surface" };

    auto fields = std::format(" mirror_hits={} mirror_fallbacks={} mirror_uploads={} mirror_upload_mb={:.1f} mirror_resolved={} mirror_watch_dirty={} mirror_bind_registrations={} "
                              "mirror_view_evictions={}",
                              counts.hits,
                              counts.fallbacks,
                              counts.uploads,
                              double(counts.uploadBytes) / 1e6,
                              counts.resolved,
                              counts.watchDirty,
                              counts.bindRegistrations,
                              counts.viewEvictions);
    if (counts.freeUnsized)
        fields += std::format(" mirror_free_unsized={}", counts.freeUnsized);
    for (std::size_t kind = 0; kind < kKinds.size(); ++kind)
        if (counts.locks[kind])
            fields += std::format(" mirror_locks_{}={}", kKinds[kind], counts.locks[kind]);
    return fields;
}

GuestResourceMirrors::GuestResourceMirrors(Config config)
: config(std::move(config))
, token([]
        {
            static std::atomic<std::uint64_t> registries{ 0 };
            return registries.fetch_add(1, std::memory_order_relaxed) + 1;
        }())
{
}

GuestResourceMirrors::~GuestResourceMirrors()
{
    // Unregisters the write notice before the maps it reaches are destroyed.
    handle.reset();
}

void GuestResourceMirrors::Hold(std::shared_ptr<void> held)
{
    std::lock_guard lock(mutex);
    handle = std::move(held);
}

bool GuestResourceMirrors::Holds(GuestResourceKind kind, std::uint32_t header, const std::array<std::uint32_t, 6>& words) const
{
    std::lock_guard lock(mutex);
    const auto      found = headers.find(header);
    if (found == headers.end())
        return false;
    const auto& object = objects.at(found->second);
    return !object.released && object.registration.kind == kind && GuestResourceWordsMatch(kind, words, object.registration.words);
}

std::uint64_t GuestResourceMirrors::Register(const GuestResourceRegistration& registration, bool bound)
{
    if (bound)
        bindRegistrations.fetch_add(1, std::memory_order_relaxed);
    Retired         retired; // freed after the lock
    std::lock_guard lock(mutex);
    drain(retired);
    // A new object may be a newer match for any cached bind.
    memo.clear();
    ++findGeneration;
    // Locks opened before this registration, or on the live object it
    // replaces, stay open until their unlocks. A released object moved its
    // open locks to the pending set at its release.
    std::uint32_t open = 0;
    if (const auto pending = pendingLocks.find(registration.header); pending != pendingLocks.end())
    {
        open = pending->second.locks;
        pendingLocks.erase(pending);
    }
    if (const auto found = headers.find(registration.header); found != headers.end())
    {
        if (const auto& replaced = objects.at(found->second); !replaced.released)
            open += replaced.locks;
        erase(found->second, retired);
    }
    const auto serial            = ++serials;
    auto&      object            = objects[serial];
    object.registration          = registration;
    object.locks                 = open;
    headers[registration.header] = serial;
    for (std::size_t i = 0; i < object.ranges.size(); ++i)
    {
        object.ranges[i].range = registration.ranges[i];
        if (!registration.ranges[i].size)
            continue;
        addresses.emplace(registration.ranges[i].address, std::pair{ serial, int(i) });
        longest = std::max(longest, registration.ranges[i].size);
    }
    return serial;
}

void GuestResourceMirrors::drop(Range& range, Retired& retired) noexcept
{
    if (range.bytes)
        held -= range.bytes->size();
    retired.push_back(std::move(range.bytes));
    for (auto& view : range.views)
    {
        held -= view.size;
        retired.push_back(std::move(view.bytes));
    }
    range.views.clear();
}

void GuestResourceMirrors::erase(std::uint64_t serial, Retired& retired)
{
    const auto found = objects.find(serial);
    if (found == objects.end())
        return;
    auto& object = found->second;
    for (std::size_t i = 0; i < object.ranges.size(); ++i)
    {
        auto& range = object.ranges[i];
        drop(range, retired);
        if (!range.range.size)
            continue;
        const auto [first, last] = addresses.equal_range(range.range.address);
        for (auto it = first; it != last; ++it)
            if (it->second == std::pair{ serial, int(i) })
            {
                addresses.erase(it);
                break;
            }
    }
    if (const auto header = headers.find(object.registration.header); header != headers.end() && header->second == serial)
        headers.erase(header);
    objects.erase(found);
}

void GuestResourceMirrors::release(std::uint64_t serial, Object& object)
{
    if (object.released)
        return;
    object.released = true;
    releasedQueue.push_back(serial);
    // Locks still open move to the pending set, where their unlocks go and a
    // successor with this header inherits them until a free over this
    // object's storage ends them.
    if (object.locks)
    {
        try
        {
            auto& pending = pendingLocks[object.registration.header];
            pending.locks += object.locks;
            pending.inherited += object.locks;
            pending.ranges = object.registration.ranges;
        }
        catch (const std::exception&)
        {
            degraded = true;
        }
        object.locks = 0;
    }
}

void GuestResourceMirrors::drain(Retired& retired)
{
    // Buffers a frame in flight shares stay alive in its recipes until the
    // producer retires that frame by fence; the entry itself goes now.
    for (const auto serial : releasedQueue)
        if (const auto found = objects.find(serial); found != objects.end() && found->second.released)
            erase(serial, retired);
    releasedQueue.clear();
}

void GuestResourceMirrors::dirty(Object& object) noexcept
{
    for (auto& range : object.ranges)
        ++range.generation;
}

void GuestResourceMirrors::Locked(GuestResourceKind kind, std::uint32_t header) noexcept
{
    lockCounts[static_cast<std::size_t>(kind)].fetch_add(1, std::memory_order_relaxed);
    if (!header)
        return;
    std::lock_guard lock(mutex);
    if (const auto found = headers.find(header); found != headers.end() && !objects.at(found->second).released)
    {
        auto& object = objects.at(found->second);
        ++object.locks;
        dirty(object);
        return;
    }
    // A lock before the object's first registration: the registration starts
    // with it open. One that cannot be recorded makes every bind fall back.
    try
    {
        ++pendingLocks[header].locks;
    }
    catch (const std::exception&)
    {
        degraded = true;
    }
}

void GuestResourceMirrors::Unlocked(std::uint32_t header) noexcept
{
    if (!header)
        return;
    std::lock_guard lock(mutex);
    // The same target the lock chose: a live object, else the pending set.
    if (const auto found = headers.find(header); found != headers.end() && !objects.at(found->second).released)
    {
        // The lock window wrote the storage; the next bind captures it.
        auto& object = objects.at(found->second);
        object.locks -= object.locks ? 1 : 0;
        dirty(object);
        return;
    }
    if (const auto pending = pendingLocks.find(header); pending != pendingLocks.end())
    {
        // An unlock closes an inherited lock first, so a free ends only locks
        // that are certainly inherited; the worst case is an extra fallback.
        auto& locks = pending->second;
        locks.locks -= locks.locks ? 1 : 0;
        locks.inherited -= locks.inherited ? 1 : 0;
        if (!locks.locks)
            pendingLocks.erase(pending);
    }
}

void GuestResourceMirrors::Freed(std::uint32_t physical, std::uint32_t size) noexcept
{
    std::lock_guard lock(mutex);
    std::uint32_t   begin = physical;
    std::uint64_t   end   = std::uint64_t(physical) + size;
    if (!size)
    {
        // No heap extent: the base page or, when longer, the largest
        // registered range starting in it.
        freeUnsized.fetch_add(1, std::memory_order_relaxed);
        begin = physical & ~0xFFFU;
        end   = std::uint64_t(begin) + 0x1000;
        for (auto it = addresses.lower_bound(begin); it != addresses.end() && it->first < std::uint64_t(begin) + 0x1000; ++it)
            end = std::max(end, std::uint64_t(it->first) + objects.at(it->second.first).ranges[it->second.second].range.size);
    }
    const auto first = begin > longest ? begin - longest : 0;
    for (auto it = addresses.lower_bound(first); it != addresses.end() && it->first < end; ++it)
    {
        auto&       object = objects.at(it->second.first);
        const auto& range  = object.ranges[it->second.second].range;
        if (std::uint64_t(range.address) + range.size > begin)
            release(it->second.first, object);
    }
    // Locks a released object held end with its storage; locks opened before
    // any registration stay.
    for (auto it = pendingLocks.begin(); it != pendingLocks.end();)
    {
        auto& locks = it->second;
        if (locks.inherited && std::ranges::any_of(locks.ranges,
                                                   [&](const GuestResourceRange& range)
                                                   {
                                                       return range.size && range.address < end && std::uint64_t(range.address) + range.size > begin;
                                                   }))
        {
            locks.locks -= locks.inherited;
            locks.inherited = 0;
        }
        it = locks.locks ? std::next(it) : pendingLocks.erase(it);
    }
}

void GuestResourceMirrors::Released(std::uint32_t header) noexcept
{
    std::lock_guard lock(mutex);
    if (const auto found = headers.find(header); found != headers.end())
        release(found->second, objects.at(found->second));
}

void GuestResourceMirrors::ReleasedAll() noexcept
{
    std::lock_guard lock(mutex);
    for (auto& [serial, object] : objects)
        release(serial, object);
    pendingLocks.clear();
    degraded = false;
}

void GuestResourceMirrors::Written(std::uint64_t begin, std::uint64_t end, bool resolve) noexcept
{
    written(begin, end, resolve, false);
}

void GuestResourceMirrors::written(std::uint64_t begin, std::uint64_t end, bool resolve, bool notice) noexcept
{
    if (begin >= end)
        return;
    std::lock_guard lock(mutex);
    const auto      first = std::min<std::uint64_t>(begin - std::min<std::uint64_t>(begin, longest), UINT32_MAX);
    for (auto it = addresses.lower_bound(std::uint32_t(first)); it != addresses.end() && it->first < end; ++it)
    {
        auto& range = objects.at(it->second.first).ranges[it->second.second];
        if (std::uint64_t(range.range.address) + range.range.size <= begin)
            continue;
        // Guest writes reach the watch only outside the lock windows that
        // already dirtied the range.
        if (notice && range.bytes && range.captured == range.generation)
            watchDirty.fetch_add(1, std::memory_order_relaxed);
        ++range.generation;
        range.resolved |= resolve;
    }
}

std::pair<std::uint32_t, std::uint32_t> GuestResourceMirrors::WriteNotice(void* context, std::uint32_t physical, std::uint32_t length, bool) noexcept
{
    static_cast<GuestResourceMirrors*>(context)->written(physical, std::uint64_t(physical) + length, false, true);
    return { physical, length };
}

std::size_t GuestResourceMirrors::FindKeyHash::operator()(const FindKey& key) const noexcept
{
    std::uint64_t hash = (std::uint64_t(key.address) << 32 | key.size) ^ (std::uint64_t(key.kind) << 60);
    const auto    mix  = [&](std::uint64_t value)
    {
        hash = (hash ^ value) * 0x9E3779B97F4A7C15ULL;
        hash ^= hash >> 29;
    };
    mix(key.header);
    for (const auto value : key.words)
        mix(value);
    return std::size_t(hash);
}

std::optional<GuestResourceMirrors::Found> GuestResourceMirrors::find(const GuestResourceRead& request) const
{
    const FindKey key{ request.kind, request.address, request.size, request.header, request.words };
    if (const auto cached = memo.find(key); cached != memo.end())
    {
        // A released object drops out of the answer; any other change to the
        // registry that could alter it cleared the memo.
        if (!cached->second || (objects.contains(cached->second->serial) && !objects.at(cached->second->serial).released))
            return cached->second;
    }
    const auto result = scan(request);
    if (memo.size() >= kMaxMemo)
        memo.clear();
    memo.insert_or_assign(key, result);
    return result;
}

std::optional<GuestResourceMirrors::Found> GuestResourceMirrors::hinted(const GuestResourceRead& request) const
{
    auto&      hint = *request.hint;
    const bool same = hint.registry == token && hint.generation == findGeneration && hint.kind == request.kind && hint.address == request.address && hint.size == request.size &&
                      hint.header == request.header && hint.words == request.words;
    // As find's memo: a released object drops out of the answer, and any
    // other change that could alter it advanced the generation.
    if (same && !hint.found)
        return std::nullopt;
    if (same)
        if (const auto object = objects.find(hint.serial); object != objects.end() && !object->second.released)
            return Found{ hint.serial, hint.range };
    const auto result = find(request);
    hint              = { token, findGeneration, result ? result->serial : 0, result ? result->range : 0, result.has_value(), request.kind, request.address, request.size, request.header, request.words };
    return result;
}

std::optional<GuestResourceMirrors::Found> GuestResourceMirrors::scan(const GuestResourceRead& request) const
{
    // An index buffer draw names its object; binds go by that header.
    if (request.kind == GuestResourceKind::IndexBuffer && request.header)
    {
        const auto found = headers.find(request.header);
        if (found == headers.end())
            return std::nullopt;
        const auto& object = objects.at(found->second);
        const auto& words  = object.registration.words;
        if (object.released || object.registration.kind != GuestResourceKind::IndexBuffer || words[0] != request.words[0] || words[1] != request.words[1] ||
            !contains(object.ranges[0].range, request.address, request.size))
            return std::nullopt;
        return Found{ found->second, 0 };
    }
    // Otherwise by storage: the newest live object whose range holds the
    // read and whose words name it.
    std::optional<Found> best;
    const auto           first = request.address > longest ? request.address - longest : 0;
    for (auto it = addresses.lower_bound(first); it != addresses.end() && it->first <= request.address; ++it)
    {
        const auto [serial, index] = it->second;
        const auto& object         = objects.at(serial);
        const auto& range          = object.ranges[index].range;
        if (object.released || object.registration.kind != request.kind || !contains(range, request.address, request.size))
            continue;
        if (request.kind == GuestResourceKind::VertexBuffer)
        {
            // 0x8268FB08 offsets the base and shortens the size by the same
            // bytes, so a stream ends where its buffer does.
            if ((request.words[0] & 3) != 3 || std::uint64_t(request.address) + request.size != std::uint64_t(range.address) + range.size)
                continue;
        }
        else if (request.kind == GuestResourceKind::Texture && !GuestTextureStorageMatches(request.words, object.registration.words))
            continue;
        if (!best || serial > best->serial)
            best = Found{ serial, std::size_t(index) };
    }
    return best;
}

GuestBytes GuestResourceMirrors::view(Object& object, Range& range, const GuestResourceRead& request, Retired& retired)
{
    const auto offset = request.address - range.range.address;
    if (offset == 0 && request.size == range.range.size)
        return GuestBytes::Shared(range.bytes);
    for (const auto& view : range.views)
        if (view.offset == offset && view.size == request.size)
            return GuestBytes::Shared(view.bytes);
    // A sub-range, such as an index range or an offset stream, keeps its own
    // copy so later binds share one buffer. The object's oldest view goes when
    // it holds kMaxObjectViews, or while the budget cannot hold the new one.
    const auto evictOldest = [&]
    {
        Range*      owner  = nullptr;
        std::size_t oldest = 0;
        for (auto& candidate : object.ranges)
            for (std::size_t i = 0; i < candidate.views.size(); ++i)
                if (!owner || candidate.views[i].order < owner->views[oldest].order)
                {
                    owner  = &candidate;
                    oldest = i;
                }
        if (!owner)
            return false;
        held -= owner->views[oldest].size;
        retired.push_back(std::move(owner->views[oldest].bytes));
        owner->views.erase(owner->views.begin() + std::ptrdiff_t(oldest));
        viewEvictions.fetch_add(1, std::memory_order_relaxed);
        return true;
    };
    if (object.ranges[0].views.size() + object.ranges[1].views.size() >= kMaxObjectViews)
        evictOldest();
    while (held + request.size > config.budget && evictOldest())
    {
    }
    auto bytes = std::make_shared<const std::vector<std::uint8_t>>(range.bytes->begin() + offset, range.bytes->begin() + offset + request.size);
    if (held + request.size > config.budget)
    {
        // Served from the copy, but not kept.
        viewEvictions.fetch_add(1, std::memory_order_relaxed);
        return GuestBytes::Shared(std::move(bytes));
    }
    held += request.size;
    range.views.push_back(View{ offset, request.size, ++object.viewOrder, bytes });
    return GuestBytes::Shared(std::move(bytes));
}

std::optional<GuestBytes> GuestResourceMirrors::Bind(const GuestResourceRead& request, const RangeRead& read)
{
    if (!request.size)
        return std::nullopt;
    Retired            retired; // freed after the lock
    Found              found;
    GuestResourceRange range;
    std::uint64_t      generation = 0;
    {
        std::lock_guard lock(mutex);
        drain(retired);
        const auto bound = degraded ? std::nullopt : request.hint ? hinted(request)
                                                                  : find(request);
        if (!bound)
        {
            fallbacks.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        found        = *bound;
        auto& object = objects.at(found.serial);
        auto& entry  = object.ranges[found.range];
        if (object.locks)
        {
            // The guest is writing it; the unlock dirties it again.
            fallbacks.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        if (entry.resolved)
        {
            // Resolve destinations stay on the bytes path.
            resolvedReads.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        if (entry.bytes && entry.captured == entry.generation)
        {
            hits.fetch_add(1, std::memory_order_relaxed);
            return view(object, entry, request, retired);
        }
        range      = entry.range;
        generation = entry.generation;
    }
    // Armed before the read, so a write after it is reported and one before
    // it is in the bytes read.
    if (!config.watch || !config.watch(range.address, range.size))
    {
        fallbacks.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    auto bytes = read(range.address, range.size);
    if (bytes.size() != range.size)
    {
        fallbacks.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }
    auto captured = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(std::move(bytes)));
    {
        std::lock_guard lock(mutex);
        const auto      object = objects.find(found.serial);
        if (object != objects.end() && !object->second.released && !object->second.locks)
        {
            auto&       entry = object->second.ranges[found.range];
            std::size_t freed = entry.bytes ? entry.bytes->size() : 0;
            for (const auto& slot : entry.views)
                freed += slot.size;
            if (entry.generation == generation && !entry.resolved && held - freed + range.size <= config.budget)
            {
                drop(entry, retired);
                entry.bytes = captured;
                held += range.size;
                entry.captured = generation;
                uploads.fetch_add(1, std::memory_order_relaxed);
                uploadBytes.fetch_add(range.size, std::memory_order_relaxed);
                hits.fetch_add(1, std::memory_order_relaxed);
                return view(object->second, entry, request, retired);
            }
        }
    }
    // The range changed while it was read, or does not fit: the read's bytes
    // are what the bytes path returns now.
    fallbacks.fetch_add(1, std::memory_order_relaxed);
    const auto offset = request.address - range.address;
    return GuestBytes(std::vector<std::uint8_t>(captured->begin() + offset, captured->begin() + offset + request.size));
}

GuestResourceMirrorCounts GuestResourceMirrors::TakeCounts() noexcept
{
    GuestResourceMirrorCounts counts;
    counts.hits              = hits.exchange(0, std::memory_order_relaxed);
    counts.fallbacks         = fallbacks.exchange(0, std::memory_order_relaxed);
    counts.resolved          = resolvedReads.exchange(0, std::memory_order_relaxed);
    counts.uploads           = uploads.exchange(0, std::memory_order_relaxed);
    counts.uploadBytes       = uploadBytes.exchange(0, std::memory_order_relaxed);
    counts.watchDirty        = watchDirty.exchange(0, std::memory_order_relaxed);
    counts.bindRegistrations = bindRegistrations.exchange(0, std::memory_order_relaxed);
    counts.viewEvictions     = viewEvictions.exchange(0, std::memory_order_relaxed);
    counts.freeUnsized       = freeUnsized.exchange(0, std::memory_order_relaxed);
    for (std::size_t kind = 0; kind < counts.locks.size(); ++kind)
        counts.locks[kind] = lockCounts[kind].exchange(0, std::memory_order_relaxed);
    return counts;
}

std::size_t GuestResourceMirrors::Objects() const
{
    std::lock_guard lock(mutex);
    return std::size_t(std::ranges::count_if(objects, [](const auto& item)
                                             {
                                                 return !item.second.released;
                                             }));
}

std::size_t GuestResourceMirrors::Entries() const
{
    std::lock_guard lock(mutex);
    return objects.size();
}

std::size_t GuestResourceMirrors::HeldBytes() const
{
    std::lock_guard lock(mutex);
    return held;
}

std::optional<std::uint64_t> GuestResourceMirrors::SerialOf(std::uint32_t header) const
{
    std::lock_guard lock(mutex);
    if (const auto found = headers.find(header); found != headers.end())
        return found->second;
    return std::nullopt;
}

void PublishGuestResourceMirrors(const std::shared_ptr<GuestResourceMirrors>& mirrors)
{
    std::lock_guard lock(publishedMutex);
    published = mirrors;
    guestResourceMirrorsEnabled.store(mirrors != nullptr, std::memory_order_relaxed);
    SetGuestResourceObserver(kGuestResourceMirrors, mirrors != nullptr);
}

std::shared_ptr<GuestResourceMirrors> PublishedGuestResourceMirrors()
{
    std::lock_guard lock(publishedMutex);
    auto            mirrors = published.lock();
    if (!mirrors)
    {
        guestResourceMirrorsEnabled.store(false, std::memory_order_relaxed);
        SetGuestResourceObserver(kGuestResourceMirrors, false);
    }
    return mirrors;
}

} // namespace rerevved::gpu::guest_d3d
