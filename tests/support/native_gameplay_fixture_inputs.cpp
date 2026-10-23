#include "native_gameplay_fixture_inputs.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>

#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <toml++/toml.hpp>

namespace rerevved::gpu::diagnostics
{
namespace
{

using namespace rex::graphics::diagnostic;
constexpr std::size_t   kMaxFrameBytes       = NativeGameplayLiveInputs::MaxOwnedBytes;
constexpr std::size_t   kMaxEvents           = 4096;
constexpr std::size_t   kRegisterCount       = 0x5003;
constexpr std::uint32_t kSurfaceMain         = 0x0A010280;
constexpr std::uint32_t kCopyClearBits       = (1U << 8) | (1U << 9);
constexpr std::uint32_t kRg16FloatFormat     = 31;
constexpr std::uint8_t  kRg16FloatSigns      = 0x55;
constexpr std::uint32_t kRg16FloatMapping    = 6984;
constexpr std::uint32_t kRg16FloatWidth      = 1280;
constexpr std::uint32_t kRg16FloatHeight     = 720;
constexpr std::uint32_t kRg16FloatPitch      = 40;
constexpr std::uint32_t kRg16FloatRawSwizzle = 0xB48;

bool isPreparedCpuCube(const DrawCaptureFrameInputTextureView& source,
                       std::span<const std::uint32_t>          registers)
{
    using namespace rex::graphics;
    const bool bc3Size  = source.format == 20;
    const auto size     = bc3Size ? 128U : 64U;
    const auto pitch    = bc3Size ? 4U : 2U;
    const auto maxLevel = bc3Size ? 7U : 6U;
    if (source.dimension != 3 || source.depth != 6 || source.width != size || source.height != size ||
        source.pitch != pitch || source.tiled || !source.packed_mips || source.mip_min_level != 0 ||
        source.mip_max_level != maxLevel || source.fetch_words.size() != 6 || source.fetch_constant >= 32 ||
        registers.size() != kRegisterCount || !source.available || source.gpu_written ||
        source.unavailable_reason != DrawCaptureReason::kNone ||
        !std::equal(source.fetch_words.begin(), source.fetch_words.end(), registers.begin() + 0x4800 + 6 * source.fetch_constant))
        return false;
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, source.fetch_words.data(), sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture || fetch.dimension != xenos::DataDimension::kCube ||
        fetch.stacked || fetch.size_2d.stack_depth != 5 || fetch.size_2d.width != size - 1 || fetch.size_2d.height != size - 1 ||
        fetch.pitch != pitch || fetch.tiled || !fetch.packed_mips || fetch.mip_min_level != 0 || fetch.mip_max_level != maxLevel ||
        fetch.exp_adjust || fetch.force_bc_w_to_max || !fetch.base_address || !fetch.mip_address ||
        fetch.base_address == fetch.mip_address)
        return false;
    const auto signs   = (source.fetch_words[0] >> 2) & 255U;
    const bool rgba8   = source.format == 6 && fetch.format == xenos::TextureFormat::k_8_8_8_8 &&
                         source.endianness == 2 && fetch.endianness == xenos::Endian::k8in32 &&
                         fetch.swizzle == 0xA0A && !fetch.num_format && !signs &&
                         !source.is_signed && !source.swizzled_signs && source.component_mapping == 0x1A0A;
    const bool rgba16f = source.format == 32 && fetch.format == xenos::TextureFormat::k_16_16_16_16_FLOAT &&
                         source.endianness == 1 && fetch.endianness == xenos::Endian::k8in16 &&
                         fetch.swizzle == 0x688 && fetch.num_format == 1 && signs == 0x55 &&
                         source.is_signed && source.swizzled_signs == 0x55 && source.component_mapping == 0x1688;
    const bool bc3     = source.format == 20 && fetch.format == xenos::TextureFormat::k_DXT4_5 &&
                         source.endianness == 1 && fetch.endianness == xenos::Endian::k8in16 &&
                         fetch.swizzle == 0x688 && !fetch.num_format && !signs &&
                         !source.is_signed && !source.swizzled_signs && source.component_mapping == 0x1688;
    return (rgba8 || rgba16f || bc3) && source.base_address == (fetch.base_address << 12) &&
           source.mip_address == (fetch.mip_address << 12) && source.base_size == (rgba16f ? 196608U : 98304U) &&
           source.mip_size == (bc3 ? 280192U : rgba8 ? 94080U
                                                     : 94208U) &&
           std::uint64_t(source.base_address) + source.base_size <= 0x20000000ULL &&
           std::uint64_t(source.mip_address) + source.mip_size <= 0x20000000ULL;
}

bool isPreparedMainLeftClearState(std::span<const std::uint32_t> registers)
{
    // Keep Prepared admission limited to the observed main-left Rgba8 clear form.
    return registers.size() == kRegisterCount && registers[0x2000] == kSurfaceMain && registers[0x2001] == 0 &&
           registers[0x2002] == 0x530 && registers[0x2205] == 0x10000 && registers[0x2208] == 6 &&
           registers[0x2080] == 0 && registers[0x2081] == 0 && registers[0x2082] == 0x02D00280 &&
           registers[0x2318] == 0x00100340 && registers[0x2319] && (registers[0x2319] & 0xFFFU) == 0 &&
           registers[0x231A] == 0x02D00500 && registers[0x231B] == 0x01000300 && registers[0x231C] == 0xFFF &&
           registers[0x231D] == 0xFFFFFF00 && registers[0x231E] == 0 && registers[0x231F] == 0;
}

bool isPreparedMainLeftGeometry(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != 24)
        return false;
    const std::array<std::uint32_t, 6> expected = {
        std::bit_cast<std::uint32_t>(-0.5F),
        std::bit_cast<std::uint32_t>(-0.5F),
        std::bit_cast<std::uint32_t>(639.5F),
        std::bit_cast<std::uint32_t>(-0.5F),
        std::bit_cast<std::uint32_t>(639.5F),
        std::bit_cast<std::uint32_t>(719.5F),
    };
    for (std::size_t index = 0; index != expected.size(); ++index)
    {
        const auto word = (std::uint32_t(bytes[index * 4]) << 24) | (std::uint32_t(bytes[index * 4 + 1]) << 16) |
                          (std::uint32_t(bytes[index * 4 + 2]) << 8) | bytes[index * 4 + 3];
        if (word != expected[index])
            return false;
    }
    return true;
}

std::size_t textureOwnedBytes(const NativeDrawReplayTexture& texture)
{
    return texture.payloadBytes().size() + (texture.textureVersion ? texture.textureVersion->MetadataBytes() : 0);
}

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

DrawCaptureFrameEvent swapEvent(const DrawCaptureFrameInputSwapView& view, bool prepared = false)
{
    DrawCaptureFrameEvent event;
    event.id                 = view.id;
    event.frame              = view.frame;
    event.submission         = view.submission;
    event.kind               = DrawCaptureFrameEvent::Kind::kSwap;
    event.frontbuffer_ptr    = view.frontbuffer_ptr;
    event.frontbuffer_width  = view.frontbuffer_width;
    event.frontbuffer_height = view.frontbuffer_height;
    event.success            = !prepared;
    event.execution_status   = prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued;
    event.draw.draw.registers.assign(view.registers.begin(), view.registers.end());
    return event;
}

} // namespace

struct NativeGameplayLiveInputs::Impl
{
    struct TextureVersion
    {
        NativeTextureFetch        fetch;
        std::vector<std::uint8_t> base;
        std::vector<std::uint8_t> mips;
        std::uint32_t             index            = 0;
        bool                      generated        = false;
        bool                      nativeSource     = false;
        std::uint64_t             memoryIdentity   = 0;
        std::uint64_t             memoryGeneration = 0;
    };

    NativeGameplaySnapshot                           snapshot;
    std::map<std::uint32_t, NativeDrawReplayTexture> textures;
    std::vector<TextureVersion>                      versions;
    using TextureKey = std::tuple<NativeTextureFetch, std::uint64_t, std::uint64_t>;
    std::map<TextureKey, NativeDrawReplayTexture>                                          previousTextures;
    std::size_t                                                                            previousBytes = 0;
    std::map<std::uint64_t, std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>> vertexPayloads;
    using BufferKey = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::uint32_t>;
    std::map<BufferKey, std::shared_ptr<const std::vector<std::uint8_t>>> bufferVersions;
    std::set<std::uint32_t>                                               gpuCopyAddresses;
    std::size_t                                                           bytes = 0;

    std::vector<std::uint8_t>               baseScratch, mipScratch;
    bool                                    finished = false, preparedBoundary = false;
    std::uint64_t                           maximumFrame = 0, maximumSubmission = 0;
    EventConsumer                           consumer;
    std::size_t                             budget = kMaxFrameBytes;
    std::function<std::size_t(std::size_t)> reclaim;
    EventTimings                            timings;
    std::chrono::steady_clock::time_point   timingBegin;
    bool                                    timingActive = false, timingCollected = false, timingEncoded = false;

    void discardPrevious()
    {
        previousTextures.clear();
        previousBytes = 0;
    }

    void trimPrevious(std::size_t size)
    {
        const auto used = bytes + snapshot.events.StoredBytes();
        require(used <= budget, "live guest input owner exceeds its byte allowance");
        auto available = budget - used;
        if (previousBytes > available || size > available - previousBytes)
            discardPrevious();
        if (size > available && reclaim)
        {
            std::chrono::steady_clock::time_point reclaimBegin;
            if (timingActive)
            {
                reclaimBegin = std::chrono::steady_clock::now();
                ++timings.clockReads;
                ++timings.reclaims;
            }
            budget = reclaim(used + size);
            if (timingActive)
            {
                timings.reclaimNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - reclaimBegin).count();
                ++timings.clockReads;
            }
            require(budget <= kMaxFrameBytes && used <= budget, "live guest input reclamation returned an invalid allowance");
        }
    }

    void reserve(std::size_t size)
    {
        trimPrevious(size);
        require(size <= budget - bytes - snapshot.events.StoredBytes(), "live guest inputs exceed the owned frame byte bound");
        bytes += size;
    }

    void append(DrawCaptureFrameEvent event, std::span<const std::uint32_t> registers = {}, bool prepare = true)
    {
        require(snapshot.events.empty() || snapshot.events.LastId() < event.id,
                "live guest input event identities are not ordered");
        // A full register checkpoint and working bank bound compact storage.
        // Evict unused prior pixels before allocating an event near the limit.
        trimPrevious(sizeof(event) + sizeof(std::vector<std::uint32_t>) + alignof(decltype(event)) +
                     2 * kRegisterCount * sizeof(std::uint32_t) +
                     event.draw.draw.vertex_fetches.capacity() * sizeof(DrawCaptureVertexFetch) +
                     event.draw.texture_uses.capacity() * sizeof(DrawCaptureTextureUse) + event.copy_mode.size());
        std::string                           preparationError;
        bool                                  prepared = true;
        std::chrono::steady_clock::time_point consumeBegin;
        if (timingActive)
        {
            consumeBegin = std::chrono::steady_clock::now();
            timings.collectionNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(consumeBegin - timingBegin).count();
            ++timings.clockReads;
            timingCollected = true;
        }
        if (consumer && prepare)
        {
            if (timingActive)
                ++timings.consumerCalls;
            try
            {
                prepared = consumer(event, registers.empty() ? std::span(event.draw.draw.registers) : registers, snapshot.textures, textures, preparationError);
            }
            catch (const std::exception& exception)
            {
                preparationError = exception.what();
                prepared         = false;
            }
        }
        std::chrono::steady_clock::time_point encodingBegin;
        if (timingActive)
        {
            encodingBegin = std::chrono::steady_clock::now();
            timings.consumeNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(encodingBegin - consumeBegin).count();
            ++timings.clockReads;
        }
        // Preserve the rejected event as well as earlier inputs for bounded
        // failure diagnostics. Normal rendering never walks this journal.
        const auto eventFrame      = event.frame;
        const auto eventSubmission = event.submission;
        if (registers.empty())
            snapshot.events.push_back(std::move(event), budget - bytes - previousBytes);
        else
            snapshot.events.push_back(std::move(event), registers, budget - bytes - previousBytes);
        maximumFrame      = std::max(maximumFrame, eventFrame);
        maximumSubmission = std::max(maximumSubmission, eventSubmission);
        if (timingActive)
        {
            timings.encodingNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - encodingBegin).count();
            ++timings.clockReads;
            ++timings.encodedEvents;
            timingEncoded = true;
        }
        if (!prepared)
            throw std::runtime_error(preparationError.empty() ? "native event preparation failed" : preparationError);
    }

    void begin(DrawCaptureFrameEvent boundary)
    {
        preparedBoundary = !boundary.success && !boundary.host_issued && boundary.execution_status == DrawCaptureExecutionStatus::kUnfinished;
        discardPrevious();
        if (finished)
            for (const auto& version : versions)
                if (!version.generated && version.memoryIdentity && version.memoryGeneration)
                    if (auto found = textures.find(version.index); found != textures.end())
                    {
                        auto [entry, inserted] = previousTextures.emplace(
                            TextureKey{ version.fetch, version.memoryIdentity, version.memoryGeneration },
                            std::move(found->second));
                        if (inserted)
                            previousBytes += textureOwnedBytes(entry->second);
                    }
        snapshot = {};
        finished = false;
        textures.clear();
        versions.clear();
        vertexPayloads.clear();
        bufferVersions.clear();
        bytes        = 0;
        maximumFrame = maximumSubmission = 0;
        snapshot.start_frame             = boundary.frame;
        snapshot.start_submission        = boundary.submission;
        append(std::move(boundary));
    }

    std::vector<std::uint8_t> read(const DrawCaptureGuestMemoryReader& reader,
                                   std::uint32_t                       address,
                                   std::uint32_t                       size)
    {
        require(size <= kMaxFrameBytes && std::uint64_t(address) + size <= 0x20000000ULL,
                "live guest input memory range is invalid");
        std::vector<std::uint8_t> result(size);
        require(!size || reader.Copy(address, result), "live guest input memory could not be copied");
        return result;
    }

    std::shared_ptr<const std::vector<std::uint8_t>> vertex(const DrawCaptureFrameInputVertexFetchView& view)
    {
        const auto&     version = view.memory_version;
        const bool      current = bufferCurrent(version, view.physical_address, view.size);
        const BufferKey versionKey{ version.identity, version.generation, view.physical_address, view.size };
        if (current)
            if (const auto found = bufferVersions.find(versionKey); found != bufferVersions.end())
                if (version.IsCurrent())
                    return found->second;
        auto                                             data      = read(view.memory, view.physical_address, view.size);
        const bool                                       unchanged = current && version.IsCurrent();
        const auto                                       key       = XXH3_64bits(data.data(), data.size());
        const auto                                       found     = vertexPayloads.find(key);
        std::shared_ptr<const std::vector<std::uint8_t>> owned;
        if (found != vertexPayloads.end())
            for (const auto& candidate : found->second)
                if (*candidate == data)
                {
                    owned = candidate;
                    break;
                }
        if (!owned)
        {
            reserve(data.size());
            owned = std::make_shared<const std::vector<std::uint8_t>>(std::move(data));
            vertexPayloads[key].push_back(owned);
        }
        // Store only scalar versions with immutable owned bytes. Account for
        // the record and tree links within the existing frame byte budget.
        constexpr auto entryBytes = sizeof(decltype(bufferVersions)::value_type) + 4 * sizeof(void*);
        if (unchanged && bufferVersions.size() < kMaxEvents &&
            entryBytes <= budget - bytes - snapshot.events.StoredBytes())
        {
            if (bufferVersions.emplace(versionKey, owned).second)
                reserve(entryBytes);
        }
        return owned;
    }

    static bool bufferCurrent(const DrawCaptureGuestMemoryVersion& version,
                              std::uint32_t                        address,
                              std::uint32_t                        size)
    {
        return size && version.base_address == address && version.base_size == size &&
               !version.mip_address && !version.mip_size && version.IsCurrent();
    }

    std::span<const std::uint8_t> scratch(const DrawCaptureGuestMemoryReader& reader,
                                          std::uint32_t                       address,
                                          std::uint32_t                       size,
                                          std::vector<std::uint8_t>&          storage)
    {
        require(size <= kMaxFrameBytes && std::uint64_t(address) + size <= 0x20000000ULL,
                "live guest input memory range is invalid");
        if (storage.size() < size)
            storage.resize(size);
        auto data = std::span(storage).first(size);
        require(!size || reader.Copy(address, data), "live guest input memory could not be copied");
        return data;
    }

    std::uint32_t texture(const DrawCaptureFrameInputTextureView& view, bool prepared)
    {
        require(view.fetch_words.size() == 6, "live texture fetch words are incomplete");
        NativeTextureFetch fetch;
        std::copy(view.fetch_words.begin(), view.fetch_words.end(), fetch.begin());
        const bool                             generated    = view.gpu_written;
        const bool                             nativeSource = prepared && !view.available &&
                                                              view.unavailable_reason == DrawCaptureReason::kTextureUnresolved;
        std::span<const std::uint8_t>          base, mips;
        std::optional<NativeDrawReplayTexture> previous;
        const auto&                            version = view.memory_version;
        bool                                   current = false;
        if (nativeSource)
        {
            NativeTextureMemoryRanges ranges;
            std::string               error;
            const bool                unsignedRgba8   = !view.is_signed && view.swizzled_signs == 0 && view.format == 6;
            const bool                signedRg16Float = view.is_signed && view.swizzled_signs == kRg16FloatSigns &&
                                                        view.component_mapping == kRg16FloatMapping && view.format == kRg16FloatFormat &&
                                                        view.dimension == 1 && view.depth == 1 && view.width == kRg16FloatWidth &&
                                                        view.height == kRg16FloatHeight && view.pitch == kRg16FloatPitch && view.tiled &&
                                                        !view.packed_mips && view.mip_min_level == 0 && view.mip_max_level == 0 &&
                                                        ((fetch[0] >> 2) & 0xFFU) == kRg16FloatSigns &&
                                                        ((fetch[3] >> 1) & 0xFFFU) == kRg16FloatRawSwizzle;
            require(view.required && (unsignedRgba8 || signedRg16Float) &&
                        !view.memory_version.identity && !view.memory_version.generation &&
                        GetNativeTextureMemoryRanges(fetch, ranges, error) &&
                        view.base_address == ranges.base.address && view.base_size == ranges.base.size &&
                        view.mip_address == ranges.mips.address && view.mip_size == ranges.mips.size,
                    error.empty() ? "prepared native texture source range is not exact" : error.c_str());
            require(snapshot.textures.size() < kMaxEvents, "live guest textures exceed the identity bound");
            const auto         index = static_cast<std::uint32_t>(snapshot.textures.size());
            DrawCaptureTexture metadata;
            metadata.availability              = DrawCaptureAvailability::kUnavailable;
            metadata.unavailable_reason        = DrawCaptureReason::kTextureUnresolved;
            metadata.guest_base_page           = view.base_address >> 12;
            metadata.guest_mip_page            = view.mip_address >> 12;
            metadata.guest_width               = view.width;
            metadata.guest_height              = view.height;
            metadata.guest_depth_or_array_size = view.dimension == 3 ? 6 : view.depth;
            metadata.guest_pitch               = view.pitch;
            metadata.guest_format              = view.format;
            metadata.guest_endianness          = view.endianness;
            metadata.guest_tiled               = view.tiled;
            metadata.guest_packed_mips         = view.packed_mips;
            // Native candidates carry the host layout needed for the later
            // resource-backed admission, while retaining no CPU payload.
            metadata.host_width               = view.width;
            metadata.host_height              = view.height;
            metadata.host_depth_or_array_size = view.dimension == 3 ? 6 : 1;
            metadata.host_mip_levels          = 1;
            metadata.host_format              = signedRg16Float ? 33 : 27; // R16G16_FLOAT or R8G8B8A8_TYPELESS.
            snapshot.textures.push_back(std::move(metadata));
            versions.push_back({ fetch, {}, {}, index, false, true, 0, 0 });
            return index;
        }
        if (!generated)
        {
            NativeTextureMemoryRanges ranges;
            std::string               error;
            if (!GetNativeTextureMemoryRanges(fetch, ranges, error))
                throw std::runtime_error(error);
            if (prepared)
                require(view.base_address == ranges.base.address && view.base_size >= ranges.base.size &&
                            std::uint64_t(view.base_address) + view.base_size <= 0x20000000ULL &&
                            view.mip_address == ranges.mips.address && view.mip_size >= ranges.mips.size &&
                            std::uint64_t(view.mip_address) + view.mip_size <= 0x20000000ULL,
                        "prepared live texture does not name its complete CPU footprint");
            // Match the decoder's entire footprint, including every mip. The
            // callback is borrowed; retain only scalar IDs with owned bytes.
            current = ranges.base.size == version.base_size && ranges.mips.size == version.mip_size &&
                      (!ranges.base.size || ranges.base.address == version.base_address) &&
                      (!ranges.mips.size || ranges.mips.address == version.mip_address) && version.IsCurrent();
            if (current)
                for (const auto& old : versions)
                    if (!old.generated && !old.nativeSource && old.fetch == fetch &&
                        old.memoryIdentity == version.identity &&
                        old.memoryGeneration == version.generation)
                        return old.index;
            if (current)
                if (auto found = previousTextures.find({ fetch, version.identity, version.generation });
                    found != previousTextures.end())
                {
                    previousBytes -= textureOwnedBytes(found->second);
                    previous = std::move(found->second);
                    previousTextures.erase(found);
                }
            if (!previous)
            {
                base    = scratch(view.memory, ranges.base.address, ranges.base.size, baseScratch);
                mips    = scratch(view.memory, ranges.mips.address, ranges.mips.size, mipScratch);
                current = current && version.IsCurrent();
            }
        }
        else
        {
            // An observed GPU owner cannot supply cached CPU pixels, even if
            // a later view happens to repeat the earlier scalar token.
            for (auto it = previousTextures.begin(); it != previousTextures.end();)
                if (std::get<0>(it->first) == fetch)
                {
                    previousBytes -= textureOwnedBytes(it->second);
                    it = previousTextures.erase(it);
                }
                else
                    ++it;
            for (auto& old : versions)
                if (old.fetch == fetch)
                    old.memoryIdentity = old.memoryGeneration = 0;
        }
        if (!previous)
            for (auto& old : versions)
                if (!old.nativeSource && old.fetch == fetch && old.generated == generated &&
                    std::equal(old.base.begin(), old.base.end(), base.begin(), base.end()) &&
                    std::equal(old.mips.begin(), old.mips.end(), mips.begin(), mips.end()))
                {
                    if (current)
                    {
                        old.memoryIdentity   = version.identity;
                        old.memoryGeneration = version.generation;
                    }
                    return old.index;
                }

        require(snapshot.textures.size() < kMaxEvents, "live guest textures exceed the identity bound");
        const auto         index = static_cast<std::uint32_t>(snapshot.textures.size());
        DrawCaptureTexture metadata;
        // This DTO contains no host pixels; CPU payloads live in the decoded
        // map. The host DTO's default complete status would falsely retire a
        // native resolve when the live converter checks CPU ownership.
        metadata.availability              = DrawCaptureAvailability::kUnavailable;
        metadata.unavailable_reason        = DrawCaptureReason::kTextureSource;
        metadata.guest_base_page           = view.base_address >> 12;
        metadata.guest_mip_page            = view.mip_address >> 12;
        metadata.guest_width               = view.width;
        metadata.guest_height              = view.height;
        metadata.guest_depth_or_array_size = view.dimension == 3 ? 6 : view.depth;
        metadata.guest_pitch               = view.pitch;
        metadata.guest_format              = view.format;
        metadata.guest_endianness          = view.endianness;
        metadata.guest_tiled               = view.tiled;
        metadata.guest_packed_mips         = view.packed_mips;
        if (!generated)
        {
            NativeDrawReplayTexture decoded;
            std::string             error;
            if (previous)
                decoded = std::move(*previous);
            else if (!DecodeNativeTextureVersioned(fetch, base, mips, decoded, error))
                throw std::runtime_error(error);
            reserve(textureOwnedBytes(decoded));
            reserve(base.size());
            reserve(mips.size());
            textures.emplace(index, std::move(decoded));
        }
        snapshot.textures.push_back(std::move(metadata));
        versions.push_back({ fetch, { base.begin(), base.end() }, { mips.begin(), mips.end() }, index, generated, false, current ? version.identity : 0, current ? version.generation : 0 });
        return index;
    }
};

NativeGameplayLiveInputs::NativeGameplayLiveInputs()
: impl(std::make_unique<Impl>())
{
}

NativeGameplayLiveInputs::~NativeGameplayLiveInputs()                                              = default;
NativeGameplayLiveInputs::NativeGameplayLiveInputs(NativeGameplayLiveInputs&&) noexcept            = default;
NativeGameplayLiveInputs& NativeGameplayLiveInputs::operator=(NativeGameplayLiveInputs&&) noexcept = default;

void NativeGameplayLiveInputs::SetOwnedByteBudget(std::size_t bytes, std::function<std::size_t(std::size_t)> reclaim)
{
    require(bytes <= MaxOwnedBytes && OwnedBytes() <= bytes, "live guest input budget is smaller than its owner");
    impl->budget  = bytes;
    impl->reclaim = std::move(reclaim);
}

std::size_t NativeGameplayLiveInputs::BoundaryByteBound()
{
    return sizeof(DrawCaptureFrameEvent) + sizeof(std::vector<std::uint32_t>) + alignof(DrawCaptureFrameEvent) +
           2 * kRegisterCount * sizeof(std::uint32_t);
}

void NativeGameplayLiveInputs::SetEventConsumer(EventConsumer consumer)
{
    require(impl->snapshot.events.empty(), "live event consumer must be set before collection");
    impl->consumer = std::move(consumer);
}

void NativeGameplayLiveInputs::ResetEventTimings(bool enabled)
{
    impl->timings         = {};
    impl->timings.enabled = enabled;
    impl->timingActive    = false;
}

void NativeGameplayLiveInputs::BeginEventTiming(std::chrono::steady_clock::time_point begin, bool draw)
{
    if (!impl->timings.enabled)
        return;
    impl->timingBegin     = begin;
    impl->timingActive    = true;
    impl->timingCollected = impl->timingEncoded = false;
    if (draw)
        ++impl->timings.drawCallbacks;
    else
        ++impl->timings.copyCallbacks;
}

void NativeGameplayLiveInputs::EndEventTiming(std::chrono::steady_clock::time_point end)
{
    if (!impl->timingActive)
        return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - impl->timingBegin).count();
    impl->timings.callbackNanoseconds += elapsed;
    if (!impl->timingCollected)
        impl->timings.collectionNanoseconds += elapsed;
    else if (!impl->timingEncoded)
        ++impl->timings.incompleteEvents;
    impl->timingActive = false;
}

const NativeGameplayLiveInputs::EventTimings& NativeGameplayLiveInputs::Timings() const
{
    return impl->timings;
}

bool NativeGameplayLiveInputs::Begin(const DrawCaptureFrameInputSwapView& boundary, std::string& error)
{
    error.clear();
    try
    {
        require(boundary.registers.size() == kRegisterCount, "live swap registers are incomplete");
        impl->begin(swapEvent(boundary));
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool NativeGameplayLiveInputs::BeginPrepared(const DrawCaptureFrameInputSwapView& boundary, std::string& error)
{
    error.clear();
    try
    {
        require(impl->snapshot.events.empty(), "prepared live input boundary is already owned");
        require(boundary.id && !boundary.original_succeeded && boundary.registers.size() == kRegisterCount,
                "prepared live boundary has invalid provenance or incomplete registers");
        impl->begin(swapEvent(boundary, true));
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool NativeGameplayLiveInputs::BeginAfterCompletion(std::string& error)
{
    error.clear();
    try
    {
        require(impl->finished && !impl->snapshot.events.empty(), "live input frame is not complete");
        impl->begin(impl->snapshot.events.ReleaseFinalEvent());
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool NativeGameplayLiveInputs::BeginAfterCompletion(const NativeGameplayLiveInputs& completed, std::string& error)
{
    error.clear();
    try
    {
        require(this != &completed && completed.impl->finished && !completed.impl->snapshot.events.empty(), "live input source frame is not complete");
        impl->gpuCopyAddresses = completed.impl->gpuCopyAddresses;
        impl->begin(completed.impl->snapshot.events.back());
        for (const auto& version : completed.impl->versions)
            if (!version.generated && version.memoryIdentity && version.memoryGeneration)
                if (auto found = completed.impl->textures.find(version.index); found != completed.impl->textures.end())
                {
                    auto [entry, inserted] = impl->previousTextures.emplace(
                        Impl::TextureKey{ version.fetch, version.memoryIdentity, version.memoryGeneration }, found->second);
                    if (inserted)
                        impl->previousBytes += textureOwnedBytes(entry->second);
                }
        impl->trimPrevious(0);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool NativeGameplayLiveInputs::Draw(const DrawCaptureFrameInputDrawView& view, std::string& error)
{
    return CollectDraw(view, error, false);
}

bool NativeGameplayLiveInputs::PreparedDraw(const DrawCaptureFrameInputDrawView& view, std::string& error)
{
    return CollectDraw(view, error, true);
}

bool NativeGameplayLiveInputs::CollectDraw(const DrawCaptureFrameInputDrawView& view, std::string& error, bool prepared)
{
    error.clear();
    try
    {
        require(!impl->finished, "live input frame is already complete");
        if (!prepared && view.execution_status == DrawCaptureExecutionStatus::kNoEffect && !view.original_host_issued)
            return true;
        if (prepared)
            require(!view.original_host_issued && view.execution_status == DrawCaptureExecutionStatus::kUnfinished &&
                        view.execution_reason == DrawCaptureReason::kNone &&
                        (view.unavailable_reason == DrawCaptureReason::kNone ||
                         view.unavailable_reason == DrawCaptureReason::kTextureUnresolved),
                    "prepared live draw has invalid provenance or texture inputs");
        else
            require(view.execution_status == DrawCaptureExecutionStatus::kIssued && view.original_host_issued,
                    "live draw was not issued successfully by the original command processor");
        const bool native_source_candidate =
            prepared && !view.cpu_data_complete &&
            view.unavailable_reason == DrawCaptureReason::kTextureUnresolved;
        require((view.cpu_data_complete || native_source_candidate) && view.vertex_shader.present &&
                    view.pixel_shader.present,
                "live draw has incomplete CPU inputs or unsupported GPU memory exports");
        require(view.registers.size() == kRegisterCount && view.vertex_fetches.size() <= 96 &&
                    view.textures.size() <= 128,
                "live draw input state is incomplete or oversized");
        if (native_source_candidate)
        {
            for (const auto& fetch : view.vertex_fetches)
                require(fetch.size && fetch.memory.copy,
                        "prepared native draw has incomplete vertex input");
            if (view.index.present)
                require(view.index.memory.copy && view.index.size && view.index.count >= view.requested_index_count &&
                            (view.index.format == 0 || view.index.format == 1),
                        "prepared native draw has incomplete index input");
        }
        if (prepared)
            for (const auto& source : view.textures)
                if (source.required)
                {
                    const bool native_source = native_source_candidate && !source.available &&
                                               source.unavailable_reason == DrawCaptureReason::kTextureUnresolved;
                    if (isPreparedCpuCube(source, view.registers))
                        continue;
                    require((native_source || (source.available && !source.gpu_written &&
                                               source.unavailable_reason == DrawCaptureReason::kNone)) &&
                                source.dimension == 1 && source.depth == 1 && source.fetch_words.size() == 6 &&
                                (!native_source ||
                                 (((!source.is_signed && source.swizzled_signs == 0 && source.format == 6) ||
                                   (source.is_signed && source.swizzled_signs == kRg16FloatSigns &&
                                    source.component_mapping == kRg16FloatMapping && source.format == kRg16FloatFormat &&
                                    source.width == kRg16FloatWidth && source.height == kRg16FloatHeight &&
                                    source.pitch == kRg16FloatPitch && source.tiled && !source.packed_mips &&
                                    source.mip_min_level == 0 && source.mip_max_level == 0 &&
                                    ((source.fetch_words[0] >> 2) & 0xFFU) == kRg16FloatSigns &&
                                    ((source.fetch_words[3] >> 1) & 0xFFFU) == kRg16FloatRawSwizzle)) &&
                                  ((source.fetch_words[4] >> 2) & 0xFU) == 0 &&
                                  ((source.fetch_words[4] >> 6) & 0xFU) == 0 &&
                                  !(source.fetch_words[5] & (1U << 11)))) &&
                                ((source.fetch_words[5] >> 9) & 3U) == 1 && !(source.fetch_words[1] & (1U << 10)) &&
                                !(source.fetch_words[2] >> 26),
                            "prepared live draw requires an available CPU or native texture source");
                }
        DrawCaptureFrameEvent event;
        event.id                            = view.id;
        event.frame                         = view.frame;
        event.submission                    = view.submission;
        event.kind                          = DrawCaptureFrameEvent::Kind::kDraw;
        event.success                       = !prepared;
        event.host_issued                   = view.original_host_issued;
        event.execution_status              = prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued;
        event.draw.guest_vertex_shader_hash = view.vertex_shader.hash;
        event.draw.guest_pixel_shader_hash  = view.pixel_shader.hash;
        auto& draw                          = event.draw.draw;
        draw.frame                          = view.frame;
        draw.submission                     = view.submission;
        draw.vertex_shader_hash             = view.vertex_shader.hash;
        draw.pixel_shader_hash              = view.pixel_shader.hash;
        draw.primitive_type                 = view.primitive_type;
        draw.requested_index_count          = view.requested_index_count;
        draw.major_mode_explicit            = view.major_mode_explicit;
        draw.indexed                        = view.indexed;
        impl->reserve(view.vertex_shader.words.size_bytes());
        impl->reserve(view.pixel_shader.words.size_bytes());
        draw.vertex_ucode.assign(view.vertex_shader.words.begin(), view.vertex_shader.words.end());
        draw.pixel_ucode.assign(view.pixel_shader.words.begin(), view.pixel_shader.words.end());
        for (const auto& source : view.vertex_fetches)
        {
            DrawCaptureVertexFetch fetch;
            fetch.fetch_constant = source.fetch_constant;
            fetch.base           = source.physical_address;
            fetch.shared_bytes   = impl->vertex(source);
            draw.vertex_fetches.push_back(std::move(fetch));
        }
        draw.index.present    = view.index.present;
        draw.index.guest_base = view.index.physical_address;
        draw.index.format     = view.index.format;
        draw.index.endianness = view.index.endianness;
        draw.index.count      = view.requested_index_count;
        draw.index.dma_count  = view.index.count;
        draw.index.dma_length = view.index.size;
        if (view.index.present)
        {
            const std::uint64_t elementSize = view.index.format == 0 ? 2 : view.index.format == 1 ? 4
                                                                                                  : 0;
            const auto          indexBytes  = std::uint64_t(view.requested_index_count) * elementSize;
            require(elementSize && view.requested_index_count && view.requested_index_count <= view.index.count &&
                        indexBytes <= view.index.size,
                    "live draw index range exceeds its DMA capacity");
            // DMA capacity may include padding beyond the submitted indices.
            // Preserve its metadata while owning only the consumed range.
            impl->reserve(indexBytes);
            if (Impl::bufferCurrent(view.index.memory_version, view.index.physical_address, indexBytes))
            {
                DrawCaptureFrameInputVertexFetchView prefix;
                prefix.physical_address = view.index.physical_address;
                prefix.size             = indexBytes;
                prefix.memory           = view.index.memory;
                prefix.memory_version   = view.index.memory_version;
                draw.index.bytes        = *impl->vertex(prefix);
            }
            else
                draw.index.bytes = impl->read(view.index.memory, view.index.physical_address, indexBytes);
        }
        for (const auto& source : view.textures)
        {
            DrawCaptureTextureUse use;
            use.stage                = source.stage;
            use.shader_binding_index = source.shader_binding_index;
            use.fetch_constant       = source.fetch_constant;
            use.dimension            = source.dimension;
            use.sampler_value        = source.sampler_value;
            use.component_mapping    = source.component_mapping;
            use.swizzled_signs       = source.swizzled_signs;
            use.is_signed            = source.is_signed;
            use.required             = source.required;
            if (source.required)
            {
                const bool native_source = native_source_candidate && !source.available &&
                                           source.unavailable_reason == DrawCaptureReason::kTextureUnresolved;
                require(native_source || source.available,
                        "live draw has an unavailable required texture binding");
                try
                {
                    use.texture_index = impl->texture(source, prepared);
                }
                catch (const std::exception& exception)
                {
                    throw std::runtime_error("texture fetch " + std::to_string(source.fetch_constant) +
                                             " format " + std::to_string(source.format) + ": " + exception.what());
                }
                use.data_available     = !native_source;
                use.availability       = native_source ? DrawCaptureAvailability::kUnavailable
                                                       : DrawCaptureAvailability::kComplete;
                use.unavailable_reason = native_source ? DrawCaptureReason::kTextureUnresolved
                                                       : DrawCaptureReason::kNone;
            }
            event.draw.texture_uses.push_back(use);
        }
        impl->append(std::move(event), view.registers);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool NativeGameplayLiveInputs::Copy(const DrawCaptureFrameInputCopyView& view,
                                    bool                                 collecting,
                                    std::string&                         error,
                                    bool                                 captureClearOnly)
{
    return CollectCopy(view, collecting, error, captureClearOnly, false);
}

bool NativeGameplayLiveInputs::PreparedCopy(const DrawCaptureFrameInputCopyView& view, std::string& error)
{
    return CollectCopy(view, true, error, false, true);
}

bool NativeGameplayLiveInputs::CollectCopy(const DrawCaptureFrameInputCopyView& view,
                                           bool                                 collecting,
                                           std::string&                         error,
                                           bool                                 captureClearOnly,
                                           bool                                 prepared)
{
    error.clear();
    try
    {
        require(!collecting || !impl->finished, "live input frame is already complete");
        require(view.registers.size() == kRegisterCount, "live copy registers are incomplete");
        if (prepared)
        {
            require(view.cpu_data_complete && view.unavailable_reason == DrawCaptureReason::kNone &&
                        !view.original_succeeded && !view.written_address && !view.written_length && view.copy_mode == "resolve",
                    "prepared live copy has incomplete inputs or invalid provenance");
            const auto clearBits = view.registers[0x2318] & kCopyClearBits;
            const bool selectedMainLeftClear =
                clearBits == kCopyClearBits && isPreparedMainLeftClearState(view.registers) && view.source_rect_present &&
                view.source_x == 0 && view.source_y == 0 && view.source_width == 640 && view.source_height == 720;
            require(!clearBits || selectedMainLeftClear,
                    "prepared live copy has an unsupported clear combination");
            require(view.planned_address && view.planned_length && view.planned_length <= kMaxFrameBytes &&
                        std::uint64_t(view.planned_address) + view.planned_length <= 0x20000000ULL &&
                        view.source_rect_present && view.source_width && view.source_height &&
                        std::uint64_t(view.source_x) + view.source_width <= 8192 && std::uint64_t(view.source_y) + view.source_height <= 8192 &&
                        view.registers[0x2208] == 6 && view.registers[0x2319] && view.registers[0x2319] < 0x20000000U &&
                        ((view.registers[0x2318] >> 20) & 3U) <= 1 &&
                        ((view.registers[0x2318] & 7U) == 0 || (view.registers[0x2318] & 7U) == 4) &&
                        (!(view.registers[0x2318] & kCopyClearBits) ||
                         ((view.registers[0x2318] & kCopyClearBits) == kCopyClearBits && isPreparedMainLeftClearState(view.registers) &&
                          view.source_x == 0 && view.source_y == 0 && view.source_width == 640 && view.source_height == 720)) &&
                        !(view.registers[0x231B] & (1U << 3)),
                    "prepared live copy has an invalid planned range or source rectangle");
            require(view.source_geometry.fetch_constant == 0 && view.source_geometry.size == 24,
                    "prepared live copy requires exact vf0 rectangle geometry");
        }
        else if (!view.original_succeeded || !view.written_length)
        {
            if (!collecting)
                return true;
            require(view.original_succeeded, "original guest resolve failed");
            require(view.written_length != 0 || captureClearOnly, "live clear-only resolves are unsupported");
        }
        require(std::uint64_t(view.written_address) + view.written_length <= 0x20000000ULL,
                "live resolve wrote an invalid physical range");
        if (!prepared && view.written_length)
        {
            require(impl->gpuCopyAddresses.size() < kMaxEvents ||
                        impl->gpuCopyAddresses.contains(view.registers[0x2319]),
                    "live copy destinations exceed the identity bound");
            impl->gpuCopyAddresses.insert(view.registers[0x2319]);
        }
        if (!collecting)
            return true;
        DrawCaptureFrameEvent event;
        event.id                                                = view.id;
        event.frame                                             = view.frame;
        event.submission                                        = view.submission;
        event.kind                                              = DrawCaptureFrameEvent::Kind::kCopy;
        event.success                                           = !prepared;
        event.execution_status                                  = prepared ? DrawCaptureExecutionStatus::kUnfinished : DrawCaptureExecutionStatus::kIssued;
        event.copy_mode                                         = prepared || view.written_length ? std::string(view.copy_mode) : "clear";
        const auto&                                      source = view.source_geometry;
        std::shared_ptr<const std::vector<std::uint8_t>> sourcePayload;
        if (source.size)
        {
            sourcePayload = impl->vertex(source);
            if (prepared && (view.registers[0x2318] & kCopyClearBits) == kCopyClearBits)
                require(isPreparedMainLeftGeometry(*sourcePayload),
                        "prepared live copy requires the captured main-left rectangle geometry");
            DrawCaptureVertexFetch fetch;
            fetch.fetch_constant = source.fetch_constant;
            fetch.base           = source.physical_address;
            fetch.shared_bytes   = std::move(sourcePayload);
            event.draw.draw.vertex_fetches.push_back(std::move(fetch));
        }
        impl->append(std::move(event), view.registers);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

bool NativeGameplayLiveInputs::Finish(const DrawCaptureFrameInputSwapView& swap, std::string& error)
{
    return CollectSwap(swap, error, false);
}

bool NativeGameplayLiveInputs::PreparedSwap(const DrawCaptureFrameInputSwapView& swap, std::string& error)
{
    return CollectSwap(swap, error, true);
}

bool NativeGameplayLiveInputs::CollectSwap(const DrawCaptureFrameInputSwapView& swap, std::string& error, bool prepared)
{
    error.clear();
    try
    {
        require(!impl->finished, "live input frame is already complete");
        require(!impl->preparedBoundary || prepared, "prepared live boundary requires prepared terminal admission");
        if (prepared)
        {
            require(impl->preparedBoundary && !impl->snapshot.events.empty() && swap.id && !swap.original_succeeded,
                    "prepared live swap has invalid provenance or no prepared boundary");
            require(swap.frame >= impl->maximumFrame && swap.submission >= impl->maximumSubmission,
                    "prepared live swap does not close the owned event interval");
        }
        require(swap.gamma_256.size() == 256 && !swap.use_pwl_gamma_ramp && !swap.use_fxaa && swap.is_8bpc,
                "live gameplay output requires the supported 8-bit source and table gamma");
        require(swap.registers.size() == kRegisterCount, "live swap registers are incomplete");
        require(swap.frame != std::numeric_limits<std::uint64_t>::max(), "live guest frame identity overflow");
        impl->append(swapEvent(swap, prepared), {}, false);
        impl->snapshot.end_frame      = swap.frame + 1;
        impl->snapshot.end_submission = swap.submission;
        auto& output                  = impl->snapshot.final_swap;
        output.frontbuffer_ptr        = swap.frontbuffer_ptr;
        output.output_width           = swap.frontbuffer_width;
        output.output_height          = swap.frontbuffer_height;
        output.gamma.is_8bpc          = true;
        output.gamma.gamma_256.assign(swap.gamma_256.begin(), swap.gamma_256.end());
        // Version comparison storage is needed only while collecting. Keep
        // the decoded immutable inputs when handing a completed frame off.
        for (auto& version : impl->versions)
        {
            impl->bytes -= version.base.size() + version.mips.size();
            std::vector<std::uint8_t>().swap(version.base);
            std::vector<std::uint8_t>().swap(version.mips);
        }
        std::vector<std::uint8_t>().swap(impl->baseScratch);
        std::vector<std::uint8_t>().swap(impl->mipScratch);
        impl->discardPrevious();
        impl->finished = true;
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

const NativeGameplaySnapshot& NativeGameplayLiveInputs::Snapshot() const
{
    return impl->snapshot;
}

std::size_t NativeGameplayLiveInputs::OwnedBytes() const
{
    return impl->bytes + impl->previousBytes + impl->snapshot.events.StoredBytes();
}

const std::map<std::uint32_t, NativeDrawReplayTexture>& NativeGameplayLiveInputs::Textures() const
{
    return impl->textures;
}

bool NativeGameplayLiveInputs::WriteOwnership(const std::filesystem::path& path, std::string& error) const
{
    error.clear();
    try
    {
        toml::array copies, textures, events;
        for (const auto address : impl->gpuCopyAddresses)
            copies.push_back(address);
        for (std::size_t index = 0; index < impl->snapshot.textures.size(); ++index)
        {
            const auto& texture = impl->snapshot.textures[index];
            toml::array fetch;
            for (const auto word : impl->versions[index].fetch)
                fetch.push_back(word);
            textures.push_back(toml::table{
                { "index", static_cast<std::int64_t>(index) },
                { "cpu_owned", impl->textures.contains(static_cast<std::uint32_t>(index)) },
                { "base_page", texture.guest_base_page },
                { "mip_page", texture.guest_mip_page },
                { "format", texture.guest_format },
                { "fetch", std::move(fetch) },
            });
        }
        for (const auto& event : impl->snapshot.events)
        {
            toml::array uses;
            for (const auto& use : event.draw.texture_uses)
                if (use.required)
                    uses.push_back(toml::table{ { "fetch", use.fetch_constant }, { "texture", use.texture_index } });
            if (uses.empty() && event.kind != DrawCaptureFrameEvent::Kind::kCopy)
                continue;
            toml::table row{ { "event", static_cast<std::int64_t>(event.id) }, { "uses", std::move(uses) } };
            if (event.kind == DrawCaptureFrameEvent::Kind::kCopy && event.copy_mode != "clear")
                row.insert("copy_destination", event.draw.draw.registers[0x2319]);
            events.push_back(std::move(row));
        }
        std::ofstream file(path, std::ios::out | std::ios::noreplace);
        file << toml::table{
            { "schema_version", 1 },
            { "gpu_copy_addresses", std::move(copies) },
            { "textures", std::move(textures) },
            { "events", std::move(events) },
        };
        file.flush();
        if (!file)
            throw std::runtime_error("could not write live input ownership record");
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

} // namespace rerevved::gpu::diagnostics
